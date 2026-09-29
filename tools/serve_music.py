#!/usr/bin/env python3
"""
espoke 用の音楽サーバ。音楽ファイルを持っている PC (dynabook) の上で動かす。

Pico WH は 16bit PCM の WAV しか読めず、サンプリングレートも 44100 の整数分の1
(44100 / 22050 / 11025) しか受け付けない。mp3 や m4a をそのまま渡しても鳴らないので、
ここで ffmpeg に通し、PCM に変換しながら流す。ファイル全体を変換し終えるのを
待たずに、変換した先から少しずつ送る。

必要なもの:
    python3 (標準ライブラリだけ) と ffmpeg
    Ubuntu なら  sudo apt install --no-install-recommends ffmpeg

使い方 (音楽のある PC で):
    python3 serve_music.py                     # ~/media/music を 8000番で配る
    python3 serve_music.py --dir ~/music --port 9000
    python3 serve_music.py --stereo            # ステレオで配る (帯域が倍になる)
    python3 serve_music.py --shuffle           # 最初からシャッフル

Pico 側で URL を組み立てずに済むよう、「次の曲」「前の曲」はサーバが覚えている。

    GET /next.wav      次の曲へ進めて、その曲を返す
    GET /prev.wav      前の曲へ戻す
    GET /random.wav    ランダムに選ぶ
    GET /current.wav   今の曲をもう一度
    GET /track/5.wav   5番の曲
    GET /list          曲一覧 (番号 <TAB> 名前)
    GET /now           今の曲
    GET /shuffle       シャッフルの ON/OFF を切り替え
    GET /rescan        ディレクトリを読み直す
    GET /             ブラウザ用の一覧
"""

import argparse
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import unicodedata
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ffmpeg が読めて、かつ音楽ファイルとして置かれていそうな拡張子
AUDIO_EXTS = {
    ".mp3", ".m4a", ".mp4", ".aac", ".flac", ".ogg", ".oga", ".opus",
    ".wav", ".wma", ".aiff", ".aif", ".alac", ".ape", ".wv",
}

LCD_COLS = 16  # LCD1602A の1行の文字数。X-Track ヘッダはこの長さに切る

# 送信が詰まったまま何秒待つか。Pico は電源が落ちたり電波が切れたりすると
# FIN を返さずに消えるので、これを入れないとカーネルが再送を諦めるまで
# (15分ほど) ffmpeg とスレッドが居座る
STREAM_TIMEOUT = 30


# ---------------- 曲 ----------------

def clean(name):
    """ファイル名に UTF-8 でないバイトが混ざっていても落ちない文字列にする"""
    return name.encode("utf-8", "replace").decode("utf-8")


def short_name(rel):
    """LCD に出すための ASCII 16文字。'01-Blue_Valentine.mp3' -> 'Blue Valentine'"""
    base = os.path.splitext(os.path.basename(rel))[0]
    base = re.sub(r"^\s*\d{1,3}\s*[-._ ]\s*", "", base)   # 先頭のトラック番号を落とす
    base = base.replace("_", " ")
    # アクセント付き文字は ASCII に潰す (LCD の文字 ROM に無いため)
    base = unicodedata.normalize("NFKD", base).encode("ascii", "ignore").decode("ascii")
    base = re.sub(r"\s+", " ", base).strip()
    return base[:LCD_COLS] if base else "track"


class Track:
    __slots__ = ("path", "rel", "name", "short")

    def __init__(self, path, root):
        self.path = path                              # ffmpeg に渡す実パス
        self.rel = clean(os.path.relpath(path, root))  # 表示用の相対パス
        self.name = self.rel
        self.short = short_name(self.rel)


def scan(root):
    tracks = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for fn in sorted(filenames):
            if os.path.splitext(fn)[1].lower() in AUDIO_EXTS:
                tracks.append(Track(os.path.join(dirpath, fn), root))
    tracks.sort(key=lambda t: t.rel)
    return tracks


# ---------------- プレイリスト ----------------

class Playlist:
    """曲順と、今どこを再生しているかを持つ。Pico 側は /next.wav を叩くだけで済む"""

    def __init__(self, tracks, shuffle=False):
        self.lock = threading.Lock()
        self.tracks = tracks
        self.shuffle = shuffle
        self._reorder()
        self.pos = -1  # まだ1曲も再生していない。最初の next で 0 になる

    def _reorder(self):
        self.order = list(range(len(self.tracks)))
        if self.shuffle:
            random.shuffle(self.order)

    def _at(self, pos):
        idx = self.order[pos]
        return idx, self.tracks[idx]

    def next(self):
        with self.lock:
            if not self.order:
                return None, None
            self.pos = (self.pos + 1) % len(self.order)
            # ひと巡りしたらシャッフルし直す (同じ順で繰り返さないように)
            if self.pos == 0 and self.shuffle:
                self._reorder()
            return self._at(self.pos)

    def prev(self):
        with self.lock:
            if not self.order:
                return None, None
            self.pos = (self.pos - 1) % len(self.order)
            return self._at(self.pos)

    def current(self):
        with self.lock:
            if not self.order:
                return None, None
            if self.pos < 0:
                self.pos = 0
            return self._at(self.pos)

    def random(self):
        with self.lock:
            if not self.order:
                return None, None
            self.pos = random.randrange(len(self.order))
            return self._at(self.pos)

    def goto(self, index):
        with self.lock:
            if not (0 <= index < len(self.tracks)):
                return None, None
            self.pos = self.order.index(index)
            return index, self.tracks[index]

    def toggle_shuffle(self):
        with self.lock:
            cur = self.order[self.pos] if self.order and self.pos >= 0 else None
            self.shuffle = not self.shuffle
            self._reorder()
            # 今かかっている曲の位置を新しい並びの中で拾い直す
            self.pos = self.order.index(cur) if cur is not None else -1
            return self.shuffle

    def replace(self, tracks):
        with self.lock:
            self.tracks = tracks
            self._reorder()
            self.pos = -1
            return len(tracks)


# ---------------- WAV / ffmpeg ----------------

def wav_header(rate, channels, data_bytes=0):
    """44バイトの WAV ヘッダ。長さが分からないので data サイズは 0 にする。

    Pico 側は data サイズ 0 を「最後まで読む」と解釈し、TCP が閉じたところで
    再生を終える。変換し終えるのを待たずに流せるのでこうしている。
    """
    block = channels * 2
    return (
        b"RIFF" + struct.pack("<I", 36 + data_bytes) + b"WAVE"
        + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate,
                                rate * block, block, 16)
        + b"data" + struct.pack("<I", data_bytes)
    )


def ffmpeg_cmd(path, rate, channels, volume):
    cmd = [
        FFMPEG, "-v", "error", "-nostdin",
        "-i", path,
        "-vn",                       # ジャケット画像を無視する
        "-ar", str(rate),
        "-ac", str(channels),
    ]
    if volume != 1.0:
        cmd += ["-filter:a", "volume=%.3f" % volume]
    cmd += ["-acodec", "pcm_s16le", "-f", "s16le", "-"]
    return cmd


# ---------------- HTTP ----------------

class Handler(BaseHTTPRequestHandler):
    # HTTP/1.0 にすると応答のたびに接続が閉じる。Content-Length を付けずに
    # 「接続が切れたら終わり」で長さを伝えられるので、変換しながら流せる。
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    # --- 応答のヘルパ ---

    def _text(self, body, code=200):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # --- ルーティング ---

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        path = url.path

        if path == "/":
            return self._index()
        if path == "/list":
            return self._list()
        if path == "/now":
            return self._now()
        if path == "/shuffle":
            on = PLAYLIST.toggle_shuffle()
            return self._text("shuffle %s\n" % ("on" if on else "off"))
        if path == "/rescan":
            n = PLAYLIST.replace(scan(ROOT))
            return self._text("%d tracks\n" % n)

        if path == "/next.wav":
            return self._stream(*PLAYLIST.next())
        if path == "/prev.wav":
            return self._stream(*PLAYLIST.prev())
        if path == "/random.wav":
            return self._stream(*PLAYLIST.random())
        if path == "/current.wav":
            return self._stream(*PLAYLIST.current())

        m = re.fullmatch(r"/track/(\d+)\.wav", path)
        if m:
            idx, track = PLAYLIST.goto(int(m.group(1)))
            if track is None:
                return self._text("no such track\n", 404)
            return self._stream(idx, track)

        self._text("not found\n", 404)

    # --- 各エンドポイント ---

    def _index(self):
        lines = [
            "espoke music server",
            "%s  (%d tracks, shuffle %s)" % (ROOT, len(PLAYLIST.tracks),
                                             "on" if PLAYLIST.shuffle else "off"),
            "%dHz %s 16bit PCM" % (RATE, "stereo" if CHANNELS == 2 else "mono"),
            "",
            "/next.wav /prev.wav /random.wav /current.wav /track/N.wav",
            "/list /now /shuffle /rescan",
            "",
        ]
        lines += ["%3d  %s" % (i, t.name) for i, t in enumerate(PLAYLIST.tracks)]
        self._text("\n".join(lines) + "\n")

    def _list(self):
        # Pico のシリアルに流す。LCD と同じ ASCII 名も添える
        body = "".join("%d\t%s\t%s\n" % (i, t.short, t.name)
                       for i, t in enumerate(PLAYLIST.tracks))
        self._text(body)

    def _now(self):
        idx, track = PLAYLIST.current()
        if track is None:
            return self._text("no tracks\n", 503)
        self._text("%d\t%s\t%s\n" % (idx, track.short, track.name))

    def _stream(self, idx, track):
        if track is None:
            return self._text("no tracks\n", 503)
        if not os.path.exists(track.path):
            return self._text("missing file\n", 404)

        try:
            proc = subprocess.Popen(
                ffmpeg_cmd(track.path, RATE, CHANNELS, VOLUME),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as e:
            return self._text("ffmpeg: %s\n" % e, 500)

        sys.stderr.write("play [%d] %s\n" % (idx, track.name))
        self.connection.settimeout(STREAM_TIMEOUT)

        self.send_response(200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("X-Index", str(idx))
        self.send_header("X-Track", track.short)            # LCD 用 ASCII 16文字
        self.send_header("X-Track-Full", urllib.parse.quote(track.name))
        self.send_header("Connection", "close")
        self.end_headers()

        sent = 0
        try:
            self.wfile.write(wav_header(RATE, CHANNELS))
            while True:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    break
                self.wfile.write(chunk)
                sent += len(chunk)
        except (BrokenPipeError, ConnectionResetError):
            # Pico 側が next や stop で切った。異常ではない
            sys.stderr.write("  stopped by client (%d KB)\n" % (sent // 1024))
        except (TimeoutError, socket.timeout):
            # 相手が黙って消えた (電源断・電波切れ・USB を抜いた等)
            sys.stderr.write("  client gone, %ds no progress (%d KB)\n"
                             % (STREAM_TIMEOUT, sent // 1024))
        finally:
            if proc.poll() is None:
                proc.kill()
            err = proc.stderr.read().decode("utf-8", "replace").strip()
            proc.wait()
            if err:
                sys.stderr.write("  ffmpeg: %s\n" % err)


# ---------------- 起動 ----------------

def lan_ip():
    """デフォルトルート側の IP を調べる (実際には送信しない)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    global ROOT, RATE, CHANNELS, VOLUME, FFMPEG, PLAYLIST

    ap = argparse.ArgumentParser(
        description="espoke 用: 音楽ファイルを Pico WH が読める WAV にして配る")
    ap.add_argument("--dir", default="~/media/music", help="配信するディレクトリ")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--rate", type=int, default=44100, choices=[44100, 22050, 11025],
                    help="サンプリングレート (既定: 44100)")
    ap.add_argument("--stereo", action="store_true",
                    help="ステレオで配る。帯域が倍になり underflow しやすい")
    ap.add_argument("--volume", type=float, default=1.0,
                    help="音量の倍率。1.0 がそのまま")
    ap.add_argument("--shuffle", action="store_true", help="最初からシャッフルする")
    ap.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg のパス")
    args = ap.parse_args()

    # ログにリダイレクトしても起動メッセージがすぐ出るようにする
    sys.stdout.reconfigure(line_buffering=True)

    FFMPEG = shutil.which(args.ffmpeg) or args.ffmpeg
    if not shutil.which(args.ffmpeg):
        sys.exit("ffmpeg が見つからない。 sudo apt install --no-install-recommends ffmpeg")

    ROOT = os.path.abspath(os.path.expanduser(args.dir))
    if not os.path.isdir(ROOT):
        sys.exit("ディレクトリがない: %s" % ROOT)

    RATE = args.rate
    CHANNELS = 2 if args.stereo else 1
    VOLUME = args.volume

    tracks = scan(ROOT)
    if not tracks:
        sys.exit("音楽ファイルが1つも見つからない: %s" % ROOT)
    PLAYLIST = Playlist(tracks, shuffle=args.shuffle)

    ip = lan_ip()
    kbps = RATE * CHANNELS * 2 / 1000  # README の帯域表と単位を揃える
    print("%s  %d tracks" % (ROOT, len(tracks)))
    print("%dHz %s 16bit PCM  (%.0f KB/s)"
          % (RATE, "stereo" if CHANNELS == 2 else "mono", kbps))
    print()
    print("  http://%s:%d/" % (ip, args.port))
    print()
    print("この URL を espoke/arduino_secrets.h の MUSIC_URL に書く:")
    print('  #define MUSIC_URL "http://%s:%d"' % (ip, args.port))
    print()
    print("Ctrl-C で停止")

    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
