#!/usr/bin/env python3
"""
espoke の Movie で見る動画を作る。mp4 などの動画を、Pico が microSD から読める .lcv にする。

Pico は mp4 (H.264 / AAC) をデコードできない。そこで PC で、YouTube の中継 (discord_relay.py) と同じ仕組みで
音を MP3 に、映像を LCD 全体 (16x2 文字、80x16 ドット) の白黒のコマにして、1 つのファイルにまとめる。
コマの作り方 (縮め方・大津のしきい値・自作文字 8 つへの寄せ方) は YouTube と同じもの (LcdVideo) を使う。
Pico は Wi-Fi も中継も使わずにこれを読み、イヤホンがつながっていれば音も鳴らす (つながっていなければ動画だけ)。

使い方 (microSD を PC に挿して):
    python3 tools/movie_convert.py /media/$USER/<カード>     # カードの中の動画を全部 (変換済みで新しいものは飛ばす)
    python3 tools/movie_convert.py movie.mp4                 # 1 本だけ。隣に movie.lcv を作る
    python3 tools/movie_convert.py --show movie.lcv          # 作った .lcv のコマを端末に出して確かめる

    ffmpeg (ffprobe も) が要る。題名の漢字を読み (カナ) にするには fugashi が要る (無ければ ? になる):
        pip install 'fugashi[unidic-lite]'

.lcv の形 (数はすべてリトルエンディアン):
    0     ヘッダ 512 バイト (使うのは頭の 100 バイト。残りは 0)
            0  "LCV1"
            4  u16 版 (1)
            6  u16 コマ 1 つの大きさ (128)
            8  u32 コマの数
           12  u16 1 秒あたりのコマ数。k 番目のコマは k * 1000 / fps ms から
           14  u8  幅 (文字の数。16 = LCD 全体)
           15  u8  0
           16  u32 コマの頭の位置 (512)
           20  u32 MP3 の頭の位置 (512 の倍数)。MP3 はファイルの終わりまで
           24  u32 MP3 の大きさ (音の無い動画なら 0)
           28  u16 MP3 のビットレート (kbps)。固定ビットレートなので、Pico は時刻から位置を求めて飛べる
           30  u16 0
           32  u32 長さ (ms)
           36  題名 64 バイト (LCD の文字コード。終わりは 0)
    512   コマ。128 バイトずつ:
            0  絵の数 n (8 まで)
            1  マス 32 バイト (上の行の左から。絵の何番目か、0x20 (空白) か 0xFF (全面黒)。YouTube の V と同じ)
           33  絵 8 バイト x n (自作文字と同じ形。上の行から、下位 5 bit が左から右。1 が明るいドット)
    MP3   44100Hz ステレオ、固定ビットレート。ID3 タグも Xing ヘッダも付けない
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discord_relay as relay  # noqa: E402  YouTube のコマの作り方 (LcdVideo) と題名の変換 (to_lcd) を使う

VIDEO_EXTS = (".mp4", ".m4v", ".mov", ".mkv", ".webm", ".avi")  # Pico も、変換していない動画としてこれを数える
COLS = relay.YT_FULL_COLS  # LCD 全体
MAGIC = b"LCV1"
VERSION = 1
HEADER = 512
REC = 128            # コマ 1 つ。絵の数 1 + マス 32 + 絵 64 = 97 バイトを、SD の 1 セクタ (512) をまたがないよう 128 に
HEAD_FMT = "<4sHHIHBBIIIHHI64s"
TITLE_MAX = 63       # 題名のバイト数 (終わりの 0 を残す)
FPS = 6              # YouTube と同じ。LCD の液晶は反応に 0.2 秒ほどかかるので、上げても見え方はあまり変わらない
FPS_MAX = 10         # 1 コマを LCD に送るのに最大 50ms ほどかかる (100kHz の I2C)
KBPS = 128


def probe(path):
    """(縦長か, 音があるか, 長さ (秒))。回転の指定 (スマホの縦の動画) があれば、見える向きで縦長かを決める"""
    r = subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr.strip() or "ffprobe failed")
    info = json.loads(r.stdout)
    streams = info.get("streams") or []
    video = next((s for s in streams if s.get("codec_type") == "video"
                  and not (s.get("disposition") or {}).get("attached_pic")), None)
    if video is None:
        raise RuntimeError("映像が無い")
    w, h = video.get("width") or 0, video.get("height") or 0
    rot = next((int(sd["rotation"]) for sd in video.get("side_data_list") or [] if "rotation" in sd), 0)
    rot = rot or int((video.get("tags") or {}).get("rotate") or 0)
    if rot % 180:
        w, h = h, w
    audio = any(s.get("codec_type") == "audio" for s in streams)
    duration = float((info.get("format") or {}).get("duration") or video.get("duration") or 0)
    return h > w > 0, audio, duration


def clock(sec):
    sec = int(sec)
    return "%d:%02d:%02d" % (sec // 3600, sec // 60 % 60, sec % 60) if sec >= 3600 else "%d:%02d" % (sec // 60, sec % 60)


def lcd_title(text):
    """題名を LCD の文字コードにする。漢字を読みにする fugashi が無ければ、漢字は ? になる"""
    try:
        import fugashi  # noqa: F401
    except ImportError:
        relay._reading = lambda t: t
        if relay._KANJI.search(text):
            print("  fugashi が無いので、題名の漢字は ? になる (pip install 'fugashi[unidic-lite]')", file=sys.stderr)
    return relay.to_lcd(text, TITLE_MAX)


def convert(src, dst, fps, kbps):
    portrait, audio, duration = probe(src)
    (w, h), _ = relay.yt_geometry(COLS)
    size = w * h
    title = lcd_title(os.path.splitext(os.path.basename(src))[0])
    print("%s -> %s (%s%s%s)" % (src, os.path.basename(dst), clock(duration), ", 縦長" if portrait else "",
                                  "" if audio else ", 音なし"))
    t0 = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="espoke-movie-") as tmp:
        mp3 = os.path.join(tmp, "audio.mp3")
        # 映像は YouTube と同じ大きさの灰色にしてパイプで受け取り、音は MP3 にして一時ファイルに書く
        # (0:V は表紙の画像を除いた映像)
        cmd = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", src,
               "-map", "0:V:0", "-vf", relay.yt_filter(COLS, portrait, fps), "-f", "rawvideo", "pipe:1"]
        if audio:
            cmd += ["-map", "0:a:0", "-ac", "2", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", "%dk" % kbps,
                    "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", mp3]
        proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
        conv, recs, shown = relay.LcdVideo(COLS), bytearray(), 0.0
        try:
            while True:
                gray = proc.stdout.read(size)
                if len(gray) < size:
                    break
                codes, glyphs = conv.frame(gray)
                rec = bytes([len(glyphs)]) + codes + b"".join(bytes(g) for g in glyphs)
                recs += rec.ljust(REC, b"\0")
                if sys.stderr.isatty() and time.monotonic() - shown >= 1:
                    shown = time.monotonic()
                    done = len(recs) // REC / fps
                    print("\r  %s / %s (実時間の %.1f 倍)" % (clock(done), clock(duration), done / (shown - t0)),
                          end="", file=sys.stderr, flush=True)
        finally:
            proc.stdout.close()
            proc.wait()
        if sys.stderr.isatty():
            print(file=sys.stderr)
        if proc.returncode:
            raise RuntimeError("ffmpeg が失敗した (%d)" % proc.returncode)
        frames = len(recs) // REC
        if not frames:
            raise RuntimeError("コマが 1 つもできなかった")
        audio_size = os.path.getsize(mp3) if audio else 0
        audio_at = (HEADER + len(recs) + 511) // 512 * 512
        length = max(frames * 1000 // fps, audio_size * 8 // kbps)
        head = struct.pack(HEAD_FMT, MAGIC, VERSION, REC, frames, fps, COLS, 0, HEADER, audio_at, audio_size,
                           kbps, 0, length, title)
        # 書きかけのファイルを Pico に読ませないよう、別の名前で書いてから置き換える
        part = dst + ".part"
        try:
            with open(part, "wb") as f:
                f.write(head.ljust(HEADER, b"\0"))
                f.write(recs)
                f.write(b"\0" * (audio_at - HEADER - len(recs)))
                if audio:
                    with open(mp3, "rb") as a:
                        shutil.copyfileobj(a, f, 1 << 20)
            os.replace(part, dst)
        except BaseException:
            if os.path.exists(part):
                os.remove(part)
            raise
    print("  %d コマ、MP3 %d KB、%.1f 秒で作った" % (frames, audio_size // 1024, time.monotonic() - t0))


def targets(path):
    """path が動画ならそれだけ、フォルダなら中の動画を全部 (隠しフォルダと、macOS の ._ のファイルは除く)"""
    if not os.path.isdir(path):
        yield path
        return
    for root, dirs, files in os.walk(path):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d != "System Volume Information")
        for name in sorted(files):
            if not name.startswith("._") and os.path.splitext(name)[1].lower() in VIDEO_EXTS:
                yield os.path.join(root, name)


def show(path, at):
    """.lcv のヘッダと、at 秒のコマ (無ければ 5 つ散らして) を端末に出す (# が点いているドット)"""
    with open(path, "rb") as f:
        data = f.read(HEADER)
        (magic, version, rec, frames, fps, cols, _, frame_at, audio_at, audio_size, kbps, _, length,
         title) = struct.unpack_from(HEAD_FMT, data)
        if magic != MAGIC:
            sys.exit("%s は .lcv ではない" % path)
        print("%s: %s / %d コマ (%d コマ/秒, %d 文字幅) / MP3 %d KB (%d kbps) / 題名 %s" % (
            path, clock(length / 1000), frames, fps, cols, audio_size // 1024, kbps,
            title.rstrip(b"\0").decode("cp932", "replace")))
        picks = [min(int(t * fps), frames - 1) for t in at] or [frames * i // 5 for i in range(5)]
        for k in picks:
            f.seek(frame_at + k * rec)
            r = f.read(rec)
            n, codes = r[0], r[1:1 + 2 * cols]
            glyphs = [tuple(r[1 + 2 * cols + 8 * i:9 + 2 * cols + 8 * i]) for i in range(n)]
            print("%s (%d)\n%s" % (clock(k / fps), k, relay.yt_art(codes, glyphs)))


def main():
    ap = argparse.ArgumentParser(description="espoke の Movie 用に、動画を microSD に置く .lcv にする")
    ap.add_argument("paths", nargs="+", metavar="PATH", help="動画か、動画の入ったフォルダ (microSD など)")
    ap.add_argument("--fps", type=int, default=FPS, help="1 秒あたりのコマ数 (既定 %d、%d まで)" % (FPS, FPS_MAX))
    ap.add_argument("--kbps", type=int, default=KBPS, help="MP3 のビットレート (既定 %d)" % KBPS)
    ap.add_argument("--force", action="store_true", help="変換済みでも作り直す")
    ap.add_argument("--show", action="store_true", help="PATH の .lcv の中身を端末に出す")
    ap.add_argument("--at", type=float, action="append", default=[], metavar="SEC",
                    help="--show で出すコマの時刻 (秒。何度でも)")
    args = ap.parse_args()
    if args.show:
        for p in args.paths:
            show(p, args.at)
        return
    if not 1 <= args.fps <= FPS_MAX:
        sys.exit("--fps は 1〜%d" % FPS_MAX)

    found, failed = 0, 0
    for src in (t for p in args.paths for t in targets(p)):
        found += 1
        dst = os.path.splitext(src)[0] + ".lcv"
        if not args.force and os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            print("%s: 変換済み" % src)
            continue
        try:
            convert(src, dst, args.fps, args.kbps)
        except (RuntimeError, OSError) as e:
            failed += 1
            print("%s: %s" % (src, e), file=sys.stderr)
    if not found:
        sys.exit("動画 (%s) が見つからない" % " ".join(VIDEO_EXTS))
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
