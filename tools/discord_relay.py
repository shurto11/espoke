#!/usr/bin/env python3
"""
espoke 用の Discord と Spotify と YouTube の中継。常時動かしておく PC (学校の dynabook) の上で動かす。

Discord の決めたサーバー (かチャンネル) に書き込まれた発言を受け取り、LCD1602A に出せる文字だけの
1行にして Pico WH へ流す。LCD の文字 ROM (A00) にあるのは英数字と半角カタカナだけで、
その並びは JIS X 0201 (cp932 の1バイト文字) と同じ。漢字は MeCab (fugashi + unidic-lite)
で読みに直してから半角カナにする。辞書は数十 MB あって Pico には載らないので、ここでやる。
逆向きに、Pico でモールス入力した文を Bot として Discord に送る。
Bot を入れられないサーバーでは --notify にする。同じ PC で公式の Discord アプリを動かし、
その出すデスクトップ通知を受け取る。送るのは webhook (下の「--notify」の節)。

Spotify は、同じ PC で動かす go-librespot (非公式の Spotify Connect クライアント) の音を MP3 にして
Pico へ流す。Pico は SD の曲と同じく libmad で鳴らす。プレイリストの一覧や曲送りは go-librespot の
API を呼ぶ (Spotify の Web API や開発者アプリは使わない)。使い方は下の「Spotify」の節。

YouTube は、yt-dlp で自分のアカウントの一覧 (おすすめ・登録チャンネル・後で見る・高評価・再生リスト) を読み、
選んだ動画を yt-dlp でダウンロードし、ffmpeg で MP3 と、LCD の 6x2 文字に出す白黒の小さな動画にして Pico へ流す。
下の「YouTube」の節。

Pico は Tailscale に入れないので、tailscale funnel でこのサーバを
https://<マシン名>.<tailnet>.ts.net:8443 として公開し、Pico から HTTPS でつながせる。
(dynabook の 443 番は tailnet 向けの tailscale serve で使っているので、Funnel は 8443 番にする)
    tailscale funnel --bg --https=8443 8001

必要なもの:
    python3 -m venv ~/espoke/venv
    ~/espoke/venv/bin/pip install discord.py 'fugashi[unidic-lite]'
    ~/espoke/venv/bin/pip install lameenc     # Spotify を使うときだけ
    ~/espoke/venv/bin/pip install "yt-dlp[default]"   # YouTube を使うときだけ (ffmpeg と deno も要る)

設定 (環境変数。--env で読むファイルにも書ける):
    DISCORD_TOKEN       Bot のトークン
    DISCORD_GUILD_ID    受け取るサーバーの ID。Bot が見られるチャンネル全部の発言を受け取る
    DISCORD_CHANNEL_ID  1チャンネルだけ受け取るときは、こちらにチャンネルの ID を書く (両方あればこちら)
    DISCORD_SEND_CHANNEL_ID  Pico からの文を送るチャンネルの ID。無ければ DISCORD_CHANNEL_ID、
                        それも無ければ最後に発言を受け取ったチャンネルに送る
    DISCORD_NOTIFY_CHANNEL  --notify のとき、このチャンネル名 (# は無くてよい) の通知だけを使う
    DISCORD_WEBHOOK_URL     --notify のとき、Pico からの文を送る webhook。無ければ受け取るだけ
    RELAY_KEY           Pico と決めておく合言葉。Funnel で誰でもつなげるので、これで弾く
    SPOTIFY_PIPE        go-librespot が音を書く FIFO のパス。無ければ Spotify は使わない (/spotify は 404)
    SPOTIFY_API         go-librespot の API。既定 http://127.0.0.1:3678
    SPOTIFY_KBPS        Pico へ流す MP3 のビットレート。既定 128 (途切れるなら 96)
    SPOTIFY_TLS_PORT    Spotify と YouTube の TLS を受けるポート (Funnel の 10000 番の転送先)。既定 8010
    YOUTUBE_COOKIES     ログインしたブラウザから書き出した cookie のファイル。無ければ YouTube は使わない (/youtube は 404)
    YOUTUBE_DENO        deno のパス。既定 ~/espoke/bin/deno (yt-dlp が YouTube の JS を解くのに使う)
    YOUTUBE_KBPS        Pico へ流す MP3 のビットレート。既定 128
    YOUTUBE_FPS         LCD に出す動画の 1 秒あたりのコマ数。既定 6 (Pico の音が途切れるなら下げる)
    YOUTUBE_FULL        0 なら、横長の動画は LCD 全体ではなく左の 6x2 文字に収める (右に題名と時間が出る)

使い方:
    ~/espoke/venv/bin/python discord_relay.py                 # ~/espoke/relay.env を読んで動かす
    ~/espoke/venv/bin/python discord_relay.py --notify        # Bot を使わず Discord アプリの通知から受け取る
    ~/espoke/venv/bin/python discord_relay.py --no-discord    # Discord につながず /post だけで試す
    ~/espoke/venv/bin/python discord_relay.py --convert "今日は雨"   # 変換結果だけ見る
    ~/espoke/venv/bin/python discord_relay.py --youtube-test URL     # YouTube の動画を LCD のコマにして端末に出す

    GET  /stream?after=<id>  after より新しい発言を1行ずつ流し続ける (X-Key ヘッダが要る)
    POST /stream?after=<id>  本文 (UTF-8) を Discord に送ってから、GET と同じく流す (X-Key ヘッダが要る)。
                             送った結果は X-Sent ヘッダ ("ok" か理由)。--no-discord なら差出人 espoke の発言として流し返す
    POST /post               本文 (UTF-8) を差出人 test の発言として流す。試験用 (X-Key ヘッダが要る)
    GET  /spotify?win=<n>    WebSocket。Spotify の音と操作をやりとりする (X-Key ヘッダが要る。下の「Spotify」の節)
    GET  /youtube?win=<n>    WebSocket。YouTube の音と動画と操作をやりとりする (X-Key ヘッダが要る。下の「YouTube」の節)

1行は  <メッセージ ID> TAB <HH:MM> TAB <名前> TAB <本文> LF  で、cp932 のバイト列。
サーバー全体を受け取るときは、本文の頭に "#チャンネル名 " を付ける。
ID は Discord のメッセージ ID (snowflake) で、時間とともに増えるので、Pico は最後に
受け取った ID を after に付けてつなぎ直せば取りこぼさない。

空行は2つの意味で送る。つないだ直後は、溜まっていた分のあとに1つ送り、「つなぐ前から
あった分はここまで」と知らせる (Pico は起動直後の古い発言で着信を知らせないようにする)。
その後は何もない間 HEARTBEAT 秒ごとに送り、Pico が「つながっているが新着がない」と分かるようにする。

--notify:
    Bot を使えないときは、公式の Discord アプリを画面なし (Xvfb) で動かしておき、この中継が D-Bus の
    通知サーバー (org.freedesktop.Notifications) になって、アプリの出す通知の題名と本文を受け取る。
    題名は "名前 (#チャンネル, カテゴリ)" で、本文は発言そのもの。題名にサーバー名は無いので、サーバーでは絞れない。
    アカウントは自分のものを使い、見たいサーバーかチャンネルの通知だけを「すべてのメッセージ」にしておく
    (他のサーバーはメンションのみかミュート。他のサーバーでメンションされた発言も届く)。DM は捨てる。
    アプリがそのチャンネルを開いていると通知が出ない。
    Bot と違って履歴は読めないので、立ち上げ直すとそれまでの発言は消える。編集や削除も届かない。
    自分の発言は通知されない。webhook で送った文は通知されて戻ってくるので、ECHO_WAIT 秒の間は捨てる。
        ~/espoke/venv/bin/pip install jeepney

Spotify:
    go-librespot (https://github.com/devgianlu/go-librespot) を同じ PC で動かし、音を FIFO に書かせる。
    ~/espoke/librespot/config.yml の例 (--config_dir ~/espoke/librespot で読ませる):
        device_name: espoke
        zeroconf_enabled: false
        credentials: {type: device_auth}       # 初回はログに出るコードを spotify.com/pair で入れる
        audio_backend: pipe
        audio_output_pipe: /home/<user>/espoke/spotify.pcm
        audio_output_pipe_format: s16le
        audio_output_pipe_wait_for_reader: true
        skip_debounce_ms: 0                    # 曲送りをすぐ効かせる (Pico は切り替わるまでの音を捨てて待つ)
        prefer_firewall_friendly_ports: true   # 学校のネットワークでは 4070 番が通らないことがある
        server: {enabled: true, address: 127.0.0.1, port: 3678}   # 外に開けない (POST /token でトークンが取れる)
    relay.env に SPOTIFY_PIPE=/home/<user>/espoke/spotify.pcm を書くと有効になる。

    Pico は Spotify のときだけ、Funnel の 10000 番に TCP のままつなぎ、TLS はこの中継が受ける:
        sudo tailscale set --operator=$USER                       # 一度だけ (tailscale cert と funnel を sudo なしで)
        tailscale funnel --bg --tcp=10000 tcp://127.0.0.1:8010
    証明書は tailscale cert で、Funnel の 8443 番と同じものを ~/espoke/tls.crt と tls.key に取り出す (起動時と1日ごと)。
    8443 番 (tailscaled が TLS を受ける) にしないのは、TLS のレコードの大きさをこちらで決めるため。Go の TLS は
    送った量が 128KB を超えるとレコードを最大 16KB にするが、Pico の受信バッファは 4KB しかない。
    Funnel の入口から dynabook までは往復 1 秒ほどかかる (実測) ので、Pico がまだ読んでいない量 (win) を
    受信バッファより小さくしてレコードを抑えると、1 秒に win しか届かず 128kbps (16KB/秒) に足りない。
    この中継で TLS を受ければ、1 フレーム (1KB) ずつ書くのでレコードは 1KB ほどになり、win は 48KB にできる。

    Pico とは WebSocket で1本つなぐ (GET /spotify?win=<n>)。フレームはすべてバイナリで、先頭1バイトが種類:
        下り A  MP3
             L  番号 TAB 総数 TAB プレイリスト名     (つないだ直後に全部。0 番はお気に入りの曲)
             T  曲名 TAB アーティスト
             I  プレイリストの番号 TAB 曲の番号 TAB 曲の数 TAB 曲名 TAB アーティスト
                (Q への返事。空のプレイリストは曲の番号が -1)
             S  playing|paused|stopped TAB シャッフル 0/1 TAB 再生中のプレイリストの番号 (無ければ -1)
                TAB それまでに受け取った操作 (P U N B K R) の数。Pico はこれで、自分の操作より前の古い知らせを見分ける
                TAB 再生中の曲がプレイリストの何番目か (分からなければ -1)
             Z  切れ目。選曲・曲送りより前の音はここまで (Pico はそれまでの A を捨てて待つ)
             H  生存確認 (HEARTBEAT 秒ごと)
             E  エラー ("librespot down" など)
        上り C<n> 読んだ量の累計 / P<n> n 番目のプレイリストを頭から再生 / P<n>:<i> その i 番目の曲から再生 /
             U<n> シャッフルで再生 (どの曲から鳴るかもランダム) / Q<n>:<i> i 番目から SP_PAGE 曲の名前を送らせる /
             N 次 / B 前 / K 一時停止 / R 再開 / F シャッフルの入り切り
    文字はすべて LCD の文字コード (cp932 の1バイト文字)。
    中継は Pico がまだ読んでいない量を win 以下に抑える。Pico は受信リングに空きが無ければ読まないので、
    流す速さは Pico が決める。win は、曲送りのときに捨てる前の曲の量の上限でもある

YouTube:
    relay.env に YOUTUBE_COOKIES=/home/<user>/espoke/yt-cookies.txt を書くと有効になる。cookie はログインした
    ブラウザから書き出す (yt-dlp で使うとアカウントが制限されることがあるので、サブのアカウントがよい)。
    yt-dlp は YouTube の JS を解くのに deno (2.3 以上) か node (22 以上) が要る。deno を YOUTUBE_DENO に置く。

    Pico とは Spotify と同じ形の WebSocket で1本つなぐ (GET /youtube?win=<n>。Funnel の 10000 番)。違うのは:
        下り L  0〜3 番は Home (おすすめ) / Subscriptions (登録チャンネル) / Watch later / Liked videos、
                そのあとに自分の再生リスト
             I  曲名とアーティストの代わりに、動画の題名とチャンネル。1 つの一覧は YT_ITEMS 本まで
             T  題名 TAB チャンネル TAB 長さ (秒。ライブなど分からなければ -1)
             V  LCD の動画のコマ: 位置 (4 バイト) 時刻 (4 バイト。どちらもリトルエンディアン) 幅 (1 バイト)
                マスの番号 (幅 x 2 バイト) 絵 x 種類数
                位置は、そのコマの時刻の音が、このセッションで送った MP3 (A の中身) の何バイト目か。時刻は動画の頭からの ms。
                幅は文字の数で、ふつうは 16 (LCD 全体)。横長の動画は上下を、縦長の動画 (ショート動画) は時計回りに 90 度回して
                元の左右を切る。YOUTUBE_FULL=0 なら、横長の動画は 6 (左の 6x2 文字に収める)。
                マスは上の行の左から。番号は絵の何番目 (0〜7) か、0x20 (空白) か 0xFF (全面黒)。
                絵は 8 バイトずつで、自作文字と同じ形 (上の行から、下位 5 bit が左から右。1 が明るいドット)
    yt-dlp と ffmpeg は、Pico がつないでいる間に今の動画の分だけ動かす。Pico が切れたら止め、つなぎ直して R が来たら
    切れたところから鳴らす。
"""

import argparse
import asyncio
import base64
import bisect
import collections
import concurrent.futures
import datetime
import hashlib
import hmac
import json
import os
import random
import re
import select
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAME_MAX = 10   # 名前のバイト数。LCD の1行目に、右端の HH:MM と並べて出す
CHANNEL_MAX = 8 # 本文の頭に付けるチャンネル名のバイト数
TEXT_MAX = 200  # 本文のバイト数。Pico は2行目に流して出す
KEEP = 20       # 覚えておく発言の数。つなぎ直した Pico に取りこぼした分を送るため
BACKLOG = 8     # after なしでつないできたときに送る数 (Pico が持っておける数)
HEARTBEAT = 30  # 新着がないときに空行を送る間隔 (秒)
SEND_MAX = 2000     # Pico から送る文の長さの上限 (Discord の上限)。Pico で打てるのは 64 文字まで
SEND_TIMEOUT = 10   # Discord に送り終わるのを待つ上限 (秒)。Pico は 15 秒まで返事を待つ
FUNNEL_PORT = 8443  # tailscale funnel で公開するポート (443 / 8443 / 10000 から選べる)
SPOTIFY_FUNNEL_PORT = 10000  # Spotify 用に TCP のまま公開するポート (TLS はこの中継で受ける。下の「Spotify」の節)
SPOTIFY_TLS_PORT = 8010      # その転送先。SPOTIFY_TLS_PORT で変えられる
CERT_CHECK = 24 * 3600       # tailscale cert で証明書を取り直す間隔 (秒)。期限が近ければ tailscaled が更新する

# 送信が詰まったまま何秒待つか。Pico は電源が落ちたり電波が切れたりすると
# FIN を返さずに消えるので、これを入れないとスレッドが居座る
STREAM_TIMEOUT = 30

JST = datetime.timezone(datetime.timedelta(hours=9))

# Spotify
SP_RATE = 44100 * 4        # go-librespot の出す PCM (s16le ステレオ) の 1 秒あたりのバイト数
SP_CHUNK = 1152 * 4        # 一度に MP3 にする量 (MP3 の 1 フレーム分)
SP_READY = SP_RATE // 2    # Pico に送れる状態で溜めておく PCM。これより少なくなったら FIFO から読む
SP_HOLD = SP_RATE * 10     # 溜める上限。Pico が止まっている間に go-librespot を操作すると、これまで溜まる
SP_FRAME = 1024            # 1 フレームに入れる MP3 のバイト数
SP_WIN_MIN = 1500          # Pico が win に書いてよい範囲
SP_WIN_MAX = 65536
SP_LISTS = 50              # Pico に送るプレイリストの数の上限
SP_TITLE_MAX = 64          # 曲名・アーティストのバイト数
SP_NAME_MAX = 24           # プレイリスト名のバイト数。Pico は 14 文字だけ出し、50 個を RAM に持つ
SP_PAGE = 16               # 曲の一覧を Pico に送るときの、1 回の曲の数 (Pico は 32 曲分だけ持つ)
SP_TRACK_MAX = 48          # 曲名のバイト数 (Pico は 16 文字に収まらなければ流して出す)
SP_ARTIST_MAX = 32         # 曲選択で出すアーティスト名のバイト数
SP_TRACKS_TTL = 300        # 曲の一覧を覚えておく時間 (秒)。go-librespot も 5 分は同じ一覧を返す
SP_TRACKS_WAIT = 10.0      # go-librespot が曲の一覧と曲名を集め終えるのを待つ上限 (秒)
LIKED_NAME = "Liked Songs"  # 一覧の先頭に足すお気に入りの曲の名前
SP_DISCARD_WAIT = 0.1      # 選曲・曲送りで go-librespot を止めたあと、FIFO に残った前の曲を読み捨てる時間 (秒)
SP_POLL = 1.0              # go-librespot の状態を見る間隔 (秒)
SP_HOLD_WAIT = 2.0         # Pico が一時停止を頼んでから、状態が「再生中」でも音を止めたままにする時間 (秒)
SP_TIMEOUT = 90            # Pico から何も来なければ切る (秒)。Pico は 30 秒ごとに C を送ってくる
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# YouTube
YT_FPS = 6                 # LCD に出す動画の 1 秒あたりのコマ数。YOUTUBE_FPS で変えられる
YT_KBPS = 128              # Pico へ流す MP3 のビットレート。YOUTUBE_KBPS で変えられる
YT_FULL_COLS = 16          # 動画は LCD 全体 (16x2 文字、80x16 ドット、実寸 57x11.5mm) に出す。はみ出た所は切る
YT_COLS = 6                # YOUTUBE_FULL=0 のときは左の 6x2 文字 (30x16 ドット、実寸 20.7x11.5mm でほぼ 16:9) に収め、
                           # 右に題名と時間を出す
YT_ROWS = 2
YT_GLYPHS = 8              # LCD の自作文字の数。空白 (0x20) と全面黒 (0xFF) のほかに出せる絵の種類
YT_MARGIN = 6              # ドットの明るさがしきい値からこれだけ以内なら、前のコマのままにする (ちらつかないように)
YT_KEEP_DIFF = 1           # 前のコマとこれだけのドットしか違わないマスは、前のままにする (自作文字の書き換えを減らす)
YT_LISTS = (("Home", ":ytrec"), ("Subscriptions", ":ytsubs"), ("Watch later", ":ytwatchlater"),
            ("Liked videos", ":ytfav"))  # 一覧の頭に置くもの。このあとに自分の再生リストが続く
YT_PLAYLISTS = "https://www.youtube.com/feed/playlists"
YT_ITEMS = 100             # 1 つの一覧から読む動画の数
# 映像は一番小さいもの (144p 前後。LCD には 30x16 ドットしか出さない)、音は一番よいもの
YT_FORMAT = "(wv[vcodec^=avc1][height>=120]/wv[height>=120]/wv)+ba/w[height>=120]/b"
YT_BUF_MAX = 64 << 20      # まだ送っていない MP3 をメモリに溜める上限 (1 時間ほど)。超えたら ffmpeg を待たせる
YT_TRIM = 1 << 20          # 送った MP3 は、これだけ溜まったら前から捨てる
YT_FAIL_MAX = 3            # 見られない動画がこれだけ続いたら止める
YT_PREFETCH_TTL = 3600     # 先に取っておいた次の動画の情報を使う期限 (秒)。URL は数時間で切れる
YT_RESUME_BACK = 3000      # つなぎ直して続きから鳴らすとき、送った所からこれだけ (ms) 戻す (Pico に溜まっていた分)

# ---------------- LCD 用の変換 ----------------

# 全角カタカナ → 半角カタカナ (JIS X 0201)。濁点・半濁点は NFD で分けてから
# 結合文字 (U+3099 / U+309A) を ﾞ ﾟ にする。ヮヵヶヰヱ は半角に無いので近い字で代用する
_KANA_FULL = ("ァアィイゥウェエォオカキクケコサシスセソタチッツテトナニヌネノ"
              "ハヒフヘホマミムメモャヤュユョヨラリルレロワヲンー。「」、・"
              "ヮヵヶヰヹ゚")
_KANA_HALF = ("ｧｱｨｲｩｳｪｴｫｵｶｷｸｹｺｻｼｽｾｿﾀﾁｯﾂﾃﾄﾅﾆﾇﾈﾉ"
              "ﾊﾋﾌﾍﾎﾏﾐﾑﾒﾓｬﾔｭﾕｮﾖﾗﾘﾙﾚﾛﾜｦﾝｰ｡｢｣､･"
              "ﾜｶｹｲｴﾞﾟ")
assert len(_KANA_FULL) == len(_KANA_HALF)
# 半角に無い記号のうち、よく使うもの。A00 ROM では ~ が → に化けるので - にする
_TO_HALF = str.maketrans(_KANA_FULL + "〜~“”‘’", _KANA_HALF + "--\"\"''")

_KANJI = re.compile(r"[々㐀-䶿一-鿿豈-﫿]")

_tagger = None
_tagger_lock = threading.Lock()  # MeCab の Tagger はスレッドから同時に使えない


def _reading(text):
    """漢字を含む語だけ読み (カタカナ) にする。読みが分からない語はそのまま残す"""
    global _tagger
    with _tagger_lock:
        if _tagger is None:
            import fugashi
            _tagger = fugashi.Tagger()
        out = []
        for word in _tagger(text):
            kana = word.feature.kana if _KANJI.search(word.surface) else None
            out.append(kana or word.surface)
            # MeCab は空白を捨てるので、語の前にあった空白を戻す ("hello world" を潰さない)
            if word.white_space:
                out[-1] = word.white_space + out[-1]
        return "".join(out)


# ハングル → カタカナ。ROM にハングルは無く、自作の字 (CGRAM) も 8 字しか持てないので、発音をカタカナで書く。
# 1 字は 初声 (19)・中声 (21)・終声 (28) の組み合わせで、字の番号から計算で分けられる。
# 日本語で韓国語の読みを書くときの慣例に寄せる: 連音 (한국어 → ハングゴ)、鼻音化 (합니다 → ハムニダ)、
# 口蓋音化 (같이 → カチ)、語中の ㄱㄷㅂㅈ の濁り (고기 → コギ)、
# 母音のあとの濃音と、ㄱㄷㅂㅈ の仲間の前の終声 k t p の促音 (오빠 → オッパ、학교 → ハッキョ。믹스 は ミクス)
_HG_INIT = "k k n t t r m p p s s - ch ch ch k t p h".split()  # ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ
_HG_VOICED = {0: "g", 3: "d", 7: "b", 12: "j"}                  # 語中で濁る ㄱㄷㅂㅈ
_HG_TENSE = {1, 4, 8, 10, 13}                                    # ㄲㄸㅃㅆㅉ。母音のあとでは前に ッ が入る
_HG_STOP = {0, 1, 3, 4, 7, 8, 12, 13, 14, 15, 16, 17}            # ㄱㄲㄷㄸㅂㅃㅈㅉㅊㅋㅌㅍ。この前の終声 k t p は ッ
_HG_VOWEL = "a e ya ye o e yo ye o wa we we yo u wo we wi yu u ui i".split()
# 終声の音。ㄳ などの二重子音もここでは1つの音にする
_HG_FINAL = "- k k k n n n t r k m r r r p r m p p t t ng t t k t p t".split()
# 連音: 次の字の初声が ㅇ のとき、終声を次の字の初声に移す。終声の番号 → (残る終声, 移る初声)
_HG_LINK = {1: (0, 0), 2: (0, 1), 3: (1, 9), 4: (0, 2), 5: (4, 12), 6: (0, 2), 7: (0, 3), 8: (0, 5),
            9: (8, 0), 10: (8, 6), 11: (8, 7), 12: (8, 9), 13: (8, 16), 14: (8, 17), 15: (0, 5),
            16: (0, 6), 17: (0, 7), 18: (17, 9), 19: (0, 9), 20: (0, 10), 22: (0, 12), 23: (0, 14),
            24: (0, 15), 25: (0, 16), 26: (0, 17), 27: (0, None)}  # ㅎ は消える (좋아 → 조아)
_HG_KANA = {row: kana.split() for row, kana in {
    "": "ア イ ウ エ オ", "k": "カ キ ク ケ コ", "g": "ガ ギ グ ゲ ゴ", "n": "ナ ニ ヌ ネ ノ",
    "t": "タ ティ トゥ テ ト", "d": "ダ ディ ドゥ デ ド", "r": "ラ リ ル レ ロ", "m": "マ ミ ム メ モ",
    "p": "パ ピ プ ペ ポ", "b": "バ ビ ブ ベ ボ", "s": "サ シ ス セ ソ",
    "ch": "チャ チ チュ チェ チョ", "j": "ジャ ジ ジュ ジェ ジョ", "h": "ハ ヒ フ ヘ ホ"}.items()}
# 字になっていない字母 (ㅋㅋ など)。子音は「その音 + ウ段」、母音は中声と同じ読み
_HG_JAMO = ("ク ク ク ヌ ヌ ヌ トゥ トゥ ル ル ル ル ル ル ル ル ム プ プ プ ス ス ン チュ チュ チュ ク トゥ プ フ "
            "ア エ ヤ イェ オ エ ヨ イェ オ ワ ウェ ウェ ヨ ウ ウォ ウェ ウィ ユ ウ ウイ イ").split()
assert len(_HG_INIT) == 19 and len(_HG_VOWEL) == 21 and len(_HG_FINAL) == 28 and len(_HG_JAMO) == 51


def _hg_kana(row, vowel):
    """初声の行と中声から、カタカナ1字分 (キャ などは2字) を作る"""
    a, i, u, e, o = _HG_KANA[row]
    base = {"a": a, "i": i, "u": u, "e": e, "o": o}
    if vowel in base:
        return base[vowel]
    if vowel == "ui":
        return i if row else "ウイ"  # 희 → ヒ
    if vowel[0] == "y":
        if not row:
            return {"ya": "ヤ", "yu": "ユ", "yo": "ヨ", "ye": "イェ"}[vowel]
        if row in ("ch", "j"):
            return base[vowel[1]]  # 쳐 → チョ
        return i + {"a": "ャ", "u": "ュ", "o": "ョ", "e": "ェ"}[vowel[1]]
    if not row:
        return {"wa": "ワ", "wi": "ウィ", "we": "ウェ", "wo": "ウォ"}[vowel]
    return u + {"a": "ァ", "i": "ィ", "e": "ェ", "o": "ォ"}[vowel[1]]  # 과 → クァ


def _hangul(text):
    """ハングルを、読みのカタカナにする。ほかの字はそのまま"""
    # 字は [初声, 中声, 終声]、それ以外の字はそのままの文字で並べる
    items = []
    for c in text:
        n = ord(c) - 0xAC00
        if 0 <= n < 11172:
            items.append([n // 588, n % 588 // 28, n % 28])
        elif "ㄱ" <= c <= "ㅣ":
            items.append(_HG_JAMO[ord(c) - 0x3131])
        else:
            items.append(c)
    syllable = [isinstance(x, list) for x in items]
    for k in range(len(items) - 1):
        if syllable[k] and syllable[k + 1] and items[k + 1][0] == 11 and items[k][2] in _HG_LINK:
            items[k][2], moved = _HG_LINK[items[k][2]]
            if moved in (3, 16) and items[k + 1][1] == 20:
                moved = {3: 12, 16: 14}[moved]  # ㄷ ㅌ + 이 は 지 치 になる
            if moved is not None:
                items[k + 1][0] = moved
    out = []
    for k, x in enumerate(items):
        if not syllable[k]:
            out.append(x)
            continue
        first, vowel, last = x
        before = _HG_FINAL[items[k - 1][2]] if k and syllable[k - 1] else None  # 語頭なら None
        after = items[k + 1][0] if k + 1 < len(items) and syllable[k + 1] else None
        if first in _HG_VOICED and before in ("-", "n", "r", "m", "ng"):
            row = _HG_VOICED[first]
        else:
            row = _HG_INIT[first].strip("-")
        if first in _HG_TENSE and before == "-":
            out.append("ッ")  # 母音のあとだけ (오빠 → オッパ。날씨 は ナルシ)
        out.append(_hg_kana(row, _HG_VOWEL[vowel]))
        sound = _HG_FINAL[last]
        if after in (2, 6):  # 次が ㄴ ㅁ なら鼻音になる
            sound = {"k": "ng", "t": "n", "p": "m"}.get(sound, sound)
        if sound in ("k", "t", "p"):
            sound = "ッ" if after in _HG_STOP else {"k": "ク", "t": "ッ", "p": "プ"}[sound]
        out.append({"-": "", "n": "ン", "ng": "ン", "m": "ム", "r": "ル"}.get(sound, sound))
    return "".join(out)


def to_lcd(text, limit):
    """LCD1602A (A00 ROM) にそのまま書ける cp932 のバイト列にする"""
    # NFKC より先にハングルを読む (NFKC は ㅋ などの字母を、字の部品の別の文字に変えてしまう)
    text = _hangul(unicodedata.normalize("NFC", text))
    text = unicodedata.normalize("NFKC", text)  # 全角英数は ASCII に、半角カナは全角に揃える
    text = _reading(text)
    out = []
    for c in text:
        if "ぁ" <= c <= "ゖ":
            c = chr(ord(c) + 0x60)  # ひらがな → カタカナ
        if c.isspace():
            out.append(" ")
            continue
        # 1文字ずつ NFD で分ける。ガ は カ と濁点に分かれて ｶﾞ に、é は e とアクセントに分かれて e になる
        kept = [d for d in unicodedata.normalize("NFD", c).translate(_TO_HALF)
                if " " <= d <= "~" or "｡" <= d <= "ﾟ"]
        if kept:
            out += kept
        elif unicodedata.category(c)[0] in "LN":
            out.append("?")  # 読めなかった漢字や、ROM に無い文字 (ハングルは _hangul がカタカナにしている)
        # 絵文字や記号は捨てる
    text = re.sub(r" +", " ", "".join(out)).strip()
    return text.encode("cp932")[:limit]


def short_name(name):
    """サーバーでの名前 "本名 / AKA" から AKA だけを取り出す。AKA が無ければ本名のまま。
    AKA の中にも / がある人がいる ("中村大翔 / >_< /") ので、最初の / で分ける"""
    real, _, aka = name.replace("／", "/").partition("/")
    return aka.strip() or real.strip() or name


def tidy_text(text):
    """Discord の書式を外して、LCD に出す文にする。Bot で受け取った発言にも、通知の本文にも使う"""
    # 通知の本文のメンションは "\u2068@本名 / AKA\u2069" と囲まれて来るので、名前と同じく AKA にする
    text = re.sub("\u2068@([^\u2069]*)\u2069", lambda m: "@" + short_name(m.group(1)), text)
    text = re.sub(r"<a?:(\w+):\d+>", r":\1:", text)  # カスタム絵文字
    text = re.sub(r"https?://[^\s)>]+", "URL", text)  # (https://...) の閉じ括弧は残す
    text = re.sub(r"\*\*|__|~~|\|\||`", "", text)    # 太字・下線・打ち消し・伏せ字・コード
    return re.sub(r"^ *(#{1,3}|-#|>{1,3}) +", "", text, flags=re.M)  # 行頭の見出し・小さい字・引用


def discord_text(message):
    """Discord の発言から、表示する文を取り出す"""
    text = tidy_text(message.clean_content)  # メンションは @名前 になっている
    if message.attachments:
        text += " [添付]"
    if message.stickers:
        text += " [スタンプ]"
    return text


def make_line(msg_id, when, author, text, channel=None):
    body = to_lcd(text, TEXT_MAX) or to_lcd("[表示できない]", TEXT_MAX)
    if channel is not None:
        # サーバー全体を受け取るときは、どのチャンネルの発言かを本文の頭に付ける。
        # LCD の1行目は名前と時刻でいっぱいなので、流れる2行目に入れる
        # チャンネル名の頭によくある絵文字や区切り ("💬｜雑談" の "💬｜") は落とす
        channel = re.sub(r"^[\W_]+", "", unicodedata.normalize("NFKC", channel))
        body = (b"#" + to_lcd(channel, CHANNEL_MAX) + b" " + body)[:TEXT_MAX]
    return b"\t".join([
        str(msg_id).encode(),
        when.astimezone(JST).strftime("%H:%M").encode(),
        to_lcd(author, NAME_MAX) or b"?",
        body,
    ]) + b"\n"


# ---------------- 発言の置き場 ----------------

class Store:
    """直近 KEEP 件の発言を ID 順に持つ。新着が来たら配信中のスレッドを起こす"""

    def __init__(self):
        self.cond = threading.Condition()
        self.ids = []
        self.lines = []

    def add(self, msg_id, line):
        with self.cond:
            if msg_id in self.ids:
                return  # 起動時の履歴の読み込みと on_message が重なったとき
            i = bisect.bisect(self.ids, msg_id)
            self.ids.insert(i, msg_id)
            self.lines.insert(i, line)
            del self.ids[:-KEEP], self.lines[:-KEEP]
            self.cond.notify_all()

    def since(self, after):
        """after より新しい (ID, 行) の並び。after が None なら直近 BACKLOG 件。cond を取ってから呼ぶ"""
        if after is None:
            return list(zip(self.ids, self.lines))[-BACKLOG:]
        i = bisect.bisect(self.ids, after)
        return list(zip(self.ids[i:], self.lines[i:]))

    def next_id(self):
        """試験用の発言に付ける ID。Discord の ID と同じく時刻から作り、今ある最大より大きくする"""
        ms = int(datetime.datetime.now().timestamp() * 1000) - 1420070400000  # Discord の紀元
        with self.cond:
            return max(ms << 22, (self.ids[-1] + 1) if self.ids else 0)


STORE = Store()


def add_message(msg_id, when, author, text, channel=None):
    line = make_line(msg_id, when, author, text, channel)
    STORE.add(msg_id, line)
    sys.stderr.write("msg %s\n" % line.rstrip(b"\n").decode("cp932"))


def send_test(text):
    """--no-discord のときの送り先。Discord の代わりに、差出人 espoke の発言として Pico へ流し返す"""
    add_message(STORE.next_id(), datetime.datetime.now(JST), "espoke", text)
    return "ok"


# Pico から来た文を送る関数。結果 ("ok" か、LCD に出す短い理由) を返す。run_discord が差し替える
SEND = send_test


# ---------------- Spotify ----------------

def ws_frame(payload, opcode=0x2):
    """WebSocket のフレーム (サーバーから送るので、マスクしない)"""
    n = len(payload)
    if n < 126:
        head = bytes([0x80 | opcode, n])
    elif n < 65536:
        head = bytes([0x80 | opcode, 126]) + n.to_bytes(2, "big")
    else:
        head = bytes([0x80 | opcode, 127]) + n.to_bytes(8, "big")
    return head + payload


def ws_read(read):
    """クライアントのフレームを1つ読む。read(n) はちょうど n バイト返す (切れたら EOFError)。
    (opcode, 中身)。おかしなフレームなら (None, b"")"""
    head = read(2)
    opcode, n = head[0] & 0x0F, head[1] & 0x7F
    if n == 126:
        n = int.from_bytes(read(2), "big")
    elif n == 127:
        n = int.from_bytes(read(8), "big")
    if n > 4096:  # Pico が送るのは短い命令だけ
        return None, b""
    mask = read(4) if head[1] & 0x80 else b""
    data = read(n) if n else b""
    if mask:
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(data))
    return opcode, data


class SessionClosed(Exception):
    pass


class StreamSession:
    """Pico との WebSocket 1本 (Spotify と YouTube で同じ)。送る側は、Pico がまだ読んでいない量を win 以下に抑える。
    音 (A) より、それ以外のフレーム (曲名や状態) を先に送る (音を送り続けると、状態の知らせがなかなか割り込めない)。
    wlock は書く順番をそろえる。cond は数と状態を守る"""

    def __init__(self, sock, win):
        self.sock = sock
        self.win = win
        self.cond = threading.Condition()
        self.sent = 0       # 送ったバイト数の累計 (フレームの頭も数える)
        self.acked = 0      # Pico が読んだと知らせてきた累計
        self.unread_max = 0
        self.audio = 0      # 送った MP3 のバイト数
        self.last_send = time.monotonic()
        self.urgent = 0     # 待っている、音以外のフレームの数。0 になるまで音は送らない
        self.closed = False
        self.wlock = threading.Lock()  # フレームが混ざらないように、書くのは1つずつ
        # 読むのと書くのを同時にしない。TLS (ssl.SSLSocket) は、別のスレッドから同時に読み書きすると壊れる
        # (全速で受け取らせると数秒で切れた)。読むのは届いているときだけにして、書く側をあまり待たせない
        self.io_lock = threading.Lock()
        self.inbuf = bytearray()
        self.uris = []      # L で送ったプレイリストの URI。P<n> の n はこの並び
        self.cmds = 0       # 受け取った操作 (P N B K R) の数。S に付けて送る

    def send(self, kind, payload=b"", opcode=0x2, held=None):
        """1フレーム送る。音は、held() が真なら (一時停止や曲送りの途中なら) 送らずに False を返す。
        書く順番は送ると決めた順番にそろえ、一時停止の知らせのあとに音が続かないようにする"""
        frame = ws_frame(kind + payload, opcode)
        audio = kind == b"A"
        with self.cond:
            if audio:
                self.cond.wait_for(lambda: self.closed or not self.urgent)  # 音以外が待っていたら先に通す
            else:
                self.urgent += 1
        try:
            with self.wlock:
                with self.cond:
                    while not self.closed and self.sent + len(frame) - self.acked > self.win:
                        self.cond.wait()
                    if self.closed:
                        raise SessionClosed
                    if held and held():
                        return False
                    self.sent += len(frame)
                    self.unread_max = max(self.unread_max, self.sent - self.acked)
                    self.last_send = time.monotonic()
                try:
                    with self.io_lock:
                        self.sock.sendall(frame)
                except OSError:
                    self.close()
                    raise SessionClosed
            return True
        finally:
            if not audio:
                with self.cond:
                    self.urgent -= 1
                    self.cond.notify_all()

    def read(self, n):
        """ちょうど n バイト読む。届いているときだけ io_lock を取って読む"""
        while len(self.inbuf) < n:
            pending = self.sock.pending() if isinstance(self.sock, ssl.SSLSocket) else 0
            if not pending and not select.select([self.sock], [], [], SP_TIMEOUT)[0]:
                raise TimeoutError
            with self.io_lock:
                # 待たずに読む (届いたのが TLS の制御用のレコードや、レコードの途中までなら、また select で待つ)
                self.sock.setblocking(False)
                try:
                    chunk = self.sock.recv(65536)
                except (ssl.SSLWantReadError, BlockingIOError):
                    continue
                finally:
                    self.sock.settimeout(SP_TIMEOUT)
            if not chunk:
                raise EOFError
            self.inbuf += chunk
        data = bytes(self.inbuf[:n])
        del self.inbuf[:n]
        return data

    def ack(self, n):
        with self.cond:
            self.acked = max(self.acked, min(n, self.sent))
            self.cond.notify_all()

    def wait_closed(self, timeout):
        with self.cond:
            self.cond.wait_for(lambda: self.closed, timeout)
        return self.closed

    def close(self):
        with self.cond:
            if self.closed:
                return
            self.closed = True
            self.cond.notify_all()
        try:
            self.sock.shutdown(socket.SHUT_RDWR)  # 命令を待っているスレッドを起こす
        except OSError:
            pass


class Spotify:
    """go-librespot の操作と、その音を Pico に流すところ"""

    def __init__(self, api, pipe, kbps):
        import lameenc  # 使うときだけ要る
        self.lameenc = lameenc
        self.api_url = api.rstrip("/")
        self.pipe = pipe
        self.kbps = kbps
        if not os.path.exists(pipe):
            os.mkfifo(pipe)
        elif not stat.S_ISFIFO(os.stat(pipe).st_mode):
            sys.exit("SPOTIFY_PIPE (%s) が FIFO ではない" % pipe)

        # FIFO から読んだ PCM。cond は buf とその下の状態を守る
        self.cond = threading.Condition()
        self.buf = bytearray()
        self.feeding = False   # Pico がつながっていて、読んだ分を buf に溜める
        self.draining = False  # go-librespot を操作している間。溜まり具合にかかわらず読む (_pump の説明)
        self.switching = False # 選曲・曲送りで次の曲に移るのを待っている間。送るのを止めておく
        self.discarding = False  # 選曲・曲送りの途中。FIFO に残った前の曲を、待たずに読んで捨てる
        # 一時停止中は音を送らない。止まったあとに送るのを状態の知らせだけにして、知らせが音の後ろで待たないように
        self.holding = False
        self.held_at = 0.0     # Pico が最後に一時停止を頼んだ時刻
        self.gen = 0           # 選曲・曲送りのたびに増やす。送る側はこれを見て、作りかけの MP3 を捨てて Z を送る

        self.ops = concurrent.futures.ThreadPoolExecutor(1)  # go-librespot の操作は1つずつ
        self.list_ops = concurrent.futures.ThreadPoolExecutor(1)  # 曲の一覧 (待つことがあるので操作とは別に)
        self.tracks = {}  # プレイリストの URI -> (覚えた時刻, [(曲の URI, 曲名 or None)])
        self.tracks_lock = threading.Lock()
        self.lock = threading.Lock()
        self.session = None
        threading.Thread(target=self._pump, daemon=True).start()

    # --- go-librespot ---

    def api(self, method, path, body=None):
        """go-librespot の API を呼ぶ。応答の JSON (中身が無ければ None)。つながらなければ OSError"""
        data = json.dumps(body).encode() if body is not None else (b"" if method == "POST" else None)
        req = urllib.request.Request(self.api_url + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=5) as r:
            raw = r.read()
        return json.loads(raw) if raw else None

    # --- FIFO ---

    def _pump(self):
        """FIFO から go-librespot の音を読む。

        Pico がつながっている間は、Pico に送るのが追いつく分だけ読む。go-librespot は FIFO に書けるまで
        待つので、Pico が鳴らす速さで進む。ただし go-librespot は FIFO に書いている間ロックを持ったままで、
        一時停止や曲送りはそのロックを待つ。読むのを止めたまま操作すると固まるので、操作の間 (draining) は
        溜まり具合にかかわらず読む。
        Pico がつながっていない間は読み捨てる。Pico が要らない分を読むときは、どちらも実時間の速さにする
        (go-librespot を詰まらせず、早送りもさせない)"""
        while True:
            try:
                fd = os.open(self.pipe, os.O_RDONLY)  # go-librespot が書く側を開くまで待つ
            except OSError as e:
                sys.stderr.write("spotify: %s\n" % e)
                time.sleep(5)
                continue
            t0, paced = time.monotonic(), 0  # 実時間の速さで読んだ分を数える
            try:
                while True:
                    with self.cond:
                        self.cond.wait_for(lambda: not self.feeding or self.draining or self.discarding
                                           or len(self.buf) < SP_READY)
                        slow = not self.discarding and (not self.feeding or len(self.buf) >= SP_READY)
                    data = os.read(fd, 8192)
                    if not data:
                        break  # go-librespot が閉じた。また開くのを待つ
                    with self.cond:
                        if self.discarding:
                            continue
                        if self.feeding:
                            self.buf += data
                            if len(self.buf) > SP_HOLD:
                                del self.buf[:(len(self.buf) - SP_HOLD + 3) & ~3]
                            self.cond.notify_all()
                    if not slow:
                        continue
                    now = time.monotonic()
                    if now - t0 > paced / SP_RATE + 1:
                        t0, paced = now, 0  # 止まっていた後は数え直す
                    paced += len(data)
                    ahead = paced / SP_RATE - (now - t0)
                    if ahead > 0:
                        time.sleep(ahead)
            finally:
                os.close(fd)

    def _set_feeding(self, on):
        with self.cond:
            self.feeding = on
            self.buf.clear()
            self.cond.notify_all()

    # --- 操作 ---

    def _control(self, s, method, path, body=None):
        """go-librespot を操作する。その間は FIFO を読み続ける"""
        with self.cond:
            self.draining = True
            self.cond.notify_all()
        try:
            self.api(method, path, body)
        except OSError as e:
            sys.stderr.write("spotify: %s %s: %s\n" % (method, path, e))
            self._error(s, "librespot down")
        finally:
            with self.cond:
                self.draining = False
                self.cond.notify_all()

    def _switch(self, s, path, body=None):
        """選曲・曲送り。go-librespot を一時停止し、FIFO に残った前の曲を読み捨ててから切り替えて再開する。
        go-librespot の FIFO 出力は、曲を変えても書いてしまった分を捨てない (Drop() が何もしない) ので、
        そのまま切り替えると前の曲が 1 秒ほど続いた。最後に Z を送らせ、Pico にもそれより前の音を捨てさせる"""
        t0 = time.monotonic()
        with self.cond:
            self.switching = self.discarding = True
            self.cond.notify_all()
        try:
            try:
                self.api("POST", "/player/pause")  # 止まっていれば何もしない
            except OSError as e:
                sys.stderr.write("spotify: pause: %s\n" % e)
            time.sleep(SP_DISCARD_WAIT)
            with self.cond:
                self.discarding = False
                self.buf.clear()
                self.cond.notify_all()
            self._control(s, "POST", path, body)
            self._control(s, "POST", "/player/resume")  # 曲送りは止めたままのことがある。選曲 (play) ならもう鳴っている
            sys.stderr.write("spotify: %s %.1fs\n" % (path, time.monotonic() - t0))
        finally:
            with self.cond:
                self.gen += 1
                self.switching = self.discarding = False
                self.cond.notify_all()

    def _shuffle(self, s):
        try:
            on = bool((self.api("GET", "/status") or {}).get("shuffle_context"))
        except (OSError, ValueError):
            return self._error(s, "librespot down")
        self._control(s, "POST", "/player/shuffle_context", {"shuffle_context": not on})

    def _hold(self, on, by_pico=False):
        with self.cond:
            if by_pico:
                self.held_at = time.monotonic()
            self.holding = on
            self.cond.notify_all()

    def _play(self, s, n, i):
        """n 番目のプレイリストを、i 番目の曲から (i が -1 なら頭から) 再生する"""
        body = {"uri": s.uris[n]}
        if i >= 0:
            try:
                items = self._context_tracks(s.uris[n], i, i + 1)
            except (OSError, ValueError):
                items = None
            if items and i < len(items):
                body["skip_to_uri"] = items[i][0]
        self._switch(s, "/player/play", body)
        self.list_ops.submit(self._context_tracks, s.uris[n])  # 今の曲が何番目かを S で知らせられるように

    def _shuffle_play(self, s, n):
        """シャッフルにしてから再生する。go-librespot は、どの曲から鳴らすかもランダムに選ぶ"""
        self._control(s, "POST", "/player/shuffle_context", {"shuffle_context": True})
        self._switch(s, "/player/play", {"uri": s.uris[n]})
        self.list_ops.submit(self._context_tracks, s.uris[n])

    def _context_tracks(self, uri, lo=0, hi=0):
        """プレイリストの曲 [(曲の URI, 曲名 or None, アーティスト)]。go-librespot は一覧と曲名を裏で集めるので、一覧ができて
        lo〜hi 番目の曲名がそろうまで (最大 SP_TRACKS_WAIT 秒) 待つ。一覧ができなければ None"""
        with self.tracks_lock:
            hit = self.tracks.get(uri)
        if hit and time.monotonic() - hit[0] < SP_TRACKS_TTL and all(t[1] for t in hit[1][lo:hi]):
            return hit[1]
        deadline = time.monotonic() + SP_TRACKS_WAIT
        while True:
            d = self.api("GET", "/context/tracks?uri=" + urllib.parse.quote(uri, safe=":")) or {}
            items = [(t["uri"], (t.get("track") or {}).get("name"),
                      ", ".join((t.get("track") or {}).get("artist_names") or [])) for t in d.get("tracks") or []]
            if d.get("ready") and all(t[1] for t in items[lo:hi]) or time.monotonic() >= deadline:
                break
            time.sleep(0.5)
        if not d.get("ready"):
            return None
        with self.tracks_lock:
            self.tracks[uri] = (time.monotonic(), items)
        return items

    def _send_tracks(self, s, n, start):
        """n 番目のプレイリストの、start 番目から SP_PAGE 曲の名前とアーティストを I で送る"""
        try:
            items = self._context_tracks(s.uris[n], start, start + SP_PAGE)
        except (OSError, ValueError) as e:  # 404 は go-librespot の metadata.enabled が false
            sys.stderr.write("spotify: tracks: %s\n" % e)
            return self._error(s, "no track list")
        if items is None:
            return self._error(s, "no track list")
        if not items:
            return s.send(b"I", b"%d\t-1\t0\t" % n)
        for i in range(start, min(start + SP_PAGE, len(items))):
            _, name, artist = items[i]
            s.send(b"I", b"%d\t%d\t%d\t" % (n, i, len(items)) + (to_lcd(name or "", SP_TRACK_MAX) or b"?")
                   + b"\t" + to_lcd(artist, SP_ARTIST_MAX))

    def _track_index(self, context, track):
        """覚えている一覧の中で、track が何番目か。分からなければ -1"""
        with self.tracks_lock:
            hit = self.tracks.get(context)
        for i, t in enumerate(hit[1] if hit else []):
            if t[0] == track:
                return i
        return -1

    def _error(self, s, text):
        try:
            s.send(b"E", text.encode())
        except SessionClosed:
            pass

    def _command(self, s, data):
        kind, arg = data[:1], data[1:].decode("ascii", "replace")
        if kind == b"C":
            if arg.isdigit():
                s.ack(int(arg))
            return
        sys.stderr.write("spotify: %s\n" % data.decode("ascii", "replace"))
        if kind in (b"K", b"R", b"P", b"U", b"N", b"B"):
            s.cmds += 1
            self._hold(kind == b"K", kind == b"K")  # 一時停止は今すぐ音を止める。再開・選曲・曲送りは鳴らす
        n, _, i = arg.partition(":")
        n = int(n) if n.isdigit() and int(n) < len(s.uris) else -1
        i = int(i) if i.isdigit() else -1
        if kind == b"P" and n >= 0:
            self._submit(self._play, s, n, i)
        elif kind == b"U" and n >= 0:
            self._submit(self._shuffle_play, s, n)
        elif kind == b"Q" and n >= 0 and i >= 0:
            def send_tracks():
                try:
                    self._send_tracks(s, n, i)
                except SessionClosed:
                    pass
            self.list_ops.submit(send_tracks)
        elif kind == b"N":
            self._submit(self._switch, s, "/player/next")
        elif kind == b"B":
            self._submit(self._switch, s, "/player/prev")
        elif kind == b"K":
            self._submit(self._control, s, "POST", "/player/pause")
        elif kind == b"R":
            self._submit(self._control, s, "POST", "/player/resume")
        elif kind == b"F":
            self._submit(self._shuffle, s)

    def _submit(self, fn, *args):
        """操作を順に1つずつする。失敗したらログに出す (投げっぱなしだと例外が消える)"""
        def done(f):
            if f.exception():
                sys.stderr.write("spotify: %s: %r\n" % (fn.__name__, f.exception()))
        self.ops.submit(fn, *args).add_done_callback(done)

    # --- Pico へ送る ---

    def _send_lists(self, s):
        """プレイリストの一覧を送る。先頭はお気に入りの曲 (Liked Songs)。go-librespot の一覧には入っていないので、
        ユーザー名から URI (spotify:user:<ユーザー名>:collection) を作って足す"""
        try:
            user = (self.api("GET", "/status") or {}).get("username")
            items = (self.api("GET", "/library/playlists?limit=%d" % (SP_LISTS - 1)) or {}).get("items")
        except (OSError, ValueError) as e:
            sys.stderr.write("spotify: playlists: %s\n" % e)
            return self._error(s, "librespot down")
        if not user or items is None:
            return self._error(s, "spotify login")  # 204: まだログインしていない
        lists = [("spotify:user:%s:collection" % user, LIKED_NAME)] + [(p["uri"], p.get("name", "")) for p in items]
        s.uris = [uri for uri, _ in lists]
        for i, (_, name) in enumerate(lists):
            s.send(b"L", b"%d\t%d\t" % (i, len(lists)) + (to_lcd(name, SP_NAME_MAX) or b"?"))

    def _send_audio(self, s):
        """FIFO から読んだ PCM を MP3 にして送る"""
        def encoder():
            e = self.lameenc.Encoder()
            e.set_bit_rate(self.kbps)
            e.set_in_sample_rate(44100)
            e.set_channels(2)
            e.set_quality(5)
            return e

        enc, out, gen = encoder(), bytearray(), self.gen
        while not s.closed:
            with self.cond:
                # 切れ目 (Z) は一時停止中でも送る。音は一時停止中は送らない
                self.cond.wait_for(lambda: s.closed or (not self.switching and (
                                   self.gen != gen or (not self.holding and len(self.buf) >= SP_CHUNK))), 1)
                if s.closed or self.switching:
                    continue
                if self.gen != gen:
                    gen = self.gen
                    pcm = None
                elif not self.holding and len(self.buf) >= SP_CHUNK:
                    pcm = bytes(self.buf[:SP_CHUNK])
                    del self.buf[:SP_CHUNK]
                    self.cond.notify_all()
                else:
                    continue
            if pcm is None:
                # 選曲・曲送り。前の曲の残りを捨て、ここから新しい曲だと知らせる
                enc, out = encoder(), bytearray()
                s.send(b"Z")
                continue
            out += enc.encode(pcm)
            while len(out) >= SP_FRAME:
                if not s.send(b"A", bytes(out[:SP_FRAME]), held=lambda: self.holding or self.switching):
                    break  # 一時停止中。残りは再開したら送る
                s.audio += SP_FRAME
                del out[:SP_FRAME]

    def _send_status(self, s):
        """曲と状態が変わったら知らせる。何も送っていなければ HEARTBEAT 秒ごとに H を送る"""
        last_track = last_state = None
        down = False
        while not s.wait_closed(SP_POLL):
            try:
                st = self.api("GET", "/status")
                down = False
            except (OSError, ValueError):
                if not down:
                    self._error(s, "librespot down")
                down = True
                st = None
            if st:
                # 曲が変わる間は一瞬 track が無くなる。そのときは何も知らせない
                t = st.get("track") or {}
                if t and t.get("uri") != last_track:
                    last_track = t.get("uri")
                    artists = ", ".join(t.get("artist_names") or [])
                    s.send(b"T", to_lcd(t.get("name", ""), SP_TITLE_MAX) + b"\t" + to_lcd(artists, SP_TITLE_MAX))
                context = st.get("context_uri")
                state = ("stopped" if st.get("stopped") else "paused" if st.get("paused") else "playing",
                         int(bool(st.get("shuffle_context"))),
                         s.uris.index(context) if context in s.uris else -1,
                         self._track_index(context, t.get("uri")) if t else -1)
                # 選曲・曲送りの途中は、こちらで一時停止しているので状態を見ない (Pico が止まってしまう)
                if self.switching:
                    continue
                # 一時停止したら音を止めてから知らせる (知らせのあとに音が続かないように)。Pico が止めた直後は、
                # 止まる前の「再生中」が見えることがあるので、少し待ってから鳴らす
                if state[0] == "paused":
                    self._hold(True)
                elif state[0] == "playing" and self.holding and time.monotonic() - self.held_at >= SP_HOLD_WAIT:
                    self._hold(False)
                if state != last_state:
                    last_state = state
                    s.send(b"S", ("%s\t%d\t%d\t%d\t%d" % (state[:3] + (s.cmds, state[3]))).encode())
            if time.monotonic() - s.last_send >= HEARTBEAT:
                s.send(b"H")

    def serve(self, handler, win):
        """Pico との WebSocket が開いてから閉じるまで"""
        s = StreamSession(handler.connection, win)
        with self.lock:
            old, self.session = self.session, s
        if old:
            old.close()
        self._set_feeding(True)
        sys.stderr.write("spotify: session start win=%d\n" % win)

        def run(target):
            def body():
                try:
                    target(s)
                except SessionClosed:
                    pass
                except Exception as e:  # 送る側が死んだら全体を閉じる (Pico はつなぎ直してくる)
                    sys.stderr.write("spotify: %s: %r\n" % (target.__name__, e))
                    s.close()
            threading.Thread(target=body, daemon=True).start()

        handler.connection.settimeout(SP_TIMEOUT)
        try:
            self._send_lists(s)
            run(self._send_audio)
            run(self._send_status)
            while not s.closed:
                op, data = ws_read(s.read)
                if op is None or op == 0x8:
                    break
                if op == 0x9:
                    s.send(b"", data, 0xA)  # ping には pong を返す
                elif op == 0x2 and data:
                    self._command(s, data)
        except (OSError, EOFError, SessionClosed) as e:
            if not s.closed:
                sys.stderr.write("spotify: %r\n" % e)
        finally:
            s.close()
            with self.lock:
                last = self.session is s
                if last:
                    self.session = None
            if last:
                # 聞く人がいなくなったので止める。読み捨てに切り替えてから止める (_pump の説明)
                self._set_feeding(False)
                self._submit(self._control, s, "POST", "/player/pause")
            sys.stderr.write("spotify: session end (%d KB audio, unread max %d/%d)\n"
                             % (s.audio // 1024, s.unread_max, win))


SPOTIFY = None  # SPOTIFY_PIPE があれば main() が作る


# ---------------- YouTube ----------------
#
# yt-dlp で自分のアカウントの一覧 (おすすめ・登録チャンネル・後で見る・高評価・自分の再生リスト) を読み、選んだ動画を
# yt-dlp でダウンロードして、ffmpeg で MP3 と小さな灰色の動画にする。動画は LCD の 6x2 文字 (30x16 ドット) の白黒にして V で送る。
# Pico との WebSocket もフレームの形も Spotify と同じで、V だけを足す。yt-dlp と ffmpeg は、Pico がつないでいる間に
# 今の動画の分だけ動かす

_BLANK, _FULL = (0,) * 8, (0x1F,) * 8  # LCD に元からある空白 (0x20) と全面黒 (0xFF) のマスの絵


def yt_options(cookies, deno):
    """yt-dlp に渡す設定。YouTube の JS を解くのに deno が要る (node なら 22 以上)"""
    opts = {"quiet": True, "no_warnings": True, "noprogress": True}
    if cookies:
        opts["cookiefile"] = cookies
    if deno and os.path.exists(deno):
        opts["js_runtimes"] = {"deno": {"path": deno}}
    return opts


def yt_reason(e):
    """yt-dlp の例外を、Pico の LCD に出す短い理由にする"""
    msg = str(e).lower()
    for words, reason in ((("sign in", "login", "cookies", "401"), "youtube login"), (("private",), "private video"),
                          (("members",), "members only"), (("premiere", "will begin", "upcoming"), "not started")):
        if any(w in msg for w in words):
            return reason
    return "unavailable"


def _dist(a, b):
    """2 つのマスの絵で、違うドットの数"""
    return sum((x ^ y).bit_count() for x, y in zip(a, b))


def _otsu(values):
    """明るさを 2 つに分けるしきい値 (大津の方法。分けた 2 つの平均の差が一番大きくなる所)"""
    hist = [0] * 256
    for v in values:
        hist[v] += 1
    total, total_sum = len(values), sum(i * h for i, h in enumerate(hist))
    best, th, n0, sum0 = -1.0, 127, 0, 0
    for t in range(255):
        n0 += hist[t]
        sum0 += t * hist[t]
        if n0 == 0:
            continue
        if n0 == total:
            break
        d = sum0 / n0 - (total_sum - sum0) / (total - n0)
        between = n0 * (total - n0) * d * d
        if between > best:
            best, th = between, t
    return th


def _merge_cells(cells):
    """空白と全面黒のほかに YT_GLYPHS 種類より多い絵があれば、数の少ない絵を近い絵 (空白と全面黒も含む) に寄せていき、
    YT_GLYPHS 種類にする。寄せる組は、変わるドットの数 (マスの数 x 違うドット) が一番少ないもの"""
    counts = collections.Counter(g for g in cells if g not in (_BLANK, _FULL))
    if len(counts) <= YT_GLYPHS:
        return cells
    # LCD 全体 (32 マス) では 20 回以上寄せるので、違うドットの数は先に数えておく
    dist = {(a, b): _dist(a, b) for a in counts for b in list(counts) + [_BLANK, _FULL] if a != b}
    into = {}
    while len(counts) > YT_GLYPHS:
        _, src, dst = min((counts[a] * d, a, b) for (a, b), d in dist.items()
                          if a in counts and (b in counts or b in (_BLANK, _FULL)))
        into[src] = dst
        if dst in counts:
            counts[dst] += counts[src]
        del counts[src]
    def final(g):
        while g in into:
            g = into[g]
        return g
    return [final(g) for g in cells]


def yt_geometry(cols):
    """cols 文字幅の動画の、ffmpeg に出させる灰色の大きさと、実寸の枠 (0.1mm 単位、すき間込み)。
    灰色は 1 マスを 5 ドット + すき間 1 列、1 行を 8 ドット + すき間 1 行とする。LCD のドットは横 0.6mm・縦 0.7mm 間隔で、
    すき間もほぼ 1 ドット分なので、この 1 つが 6x7 (0.1mm) になる。6 文字なら 35x17 と 210x119"""
    w, h = cols * 6 - 1, YT_ROWS * 9 - 1
    return (w, h), (w * 6, h * 7)


def yt_art(codes, glyphs):
    """マスの番号と絵を、端末に出す絵にする (# が点いているドット。マスの間は空ける)。幅はマスの数から"""
    cols = len(codes) // YT_ROWS
    lines = []
    for cy in range(YT_ROWS):
        for r in range(8):
            row = []
            for cx in range(cols):
                c = codes[cy * cols + cx]
                bits = 0 if c == 0x20 else 0x1F if c == 0xFF else glyphs[c][r]
                row.append("".join("#" if bits >> (4 - k) & 1 else "." for k in range(5)))
            lines.append(" ".join(row))
        lines.append("")
    return "\n".join(lines)


class LcdVideo:
    """動画のコマ (ffmpeg が出す yt_geometry() の灰色) を、LCD の cols x YT_ROWS 文字の絵にする。
    2 値化は大津のしきい値にする。網目のディザより形が分かりやすく、空白や全面黒のマスが増えるので自作文字も足りやすい。
    前のコマを覚えておき、ちらつきを抑える"""

    def __init__(self, cols):
        self.cols = cols
        self.th = None     # 前のコマのしきい値
        self.bits = None   # 前のコマのドット (行ごと)
        self.cells = None  # 前のコマのマスの絵

    def frame(self, gray):
        """灰色 1 コマ -> (マスの番号, 絵のリスト)。マスは上の行の左から。番号は絵の何番目か、0x20 (空白) か 0xFF (全面黒)。
        絵は上の行から 8 バイトで、下位 5 bit が左から右 (LCD の自作文字と同じ)。明るいドットを 1 にする"""
        (w, h), _ = yt_geometry(self.cols)
        # すき間の列 (6 列ごと) と行 (9 行目) を抜いて、6 文字なら 30x16、16 文字なら 80x16 にする
        dots = [[gray[y * w + x] for x in range(w) if x % 6 != 5] for y in range(h) if y != 8]
        flat = [v for row in dots for v in row]
        t = _otsu(flat) if max(flat) - min(flat) >= 32 else 127  # ほとんど一色なら、明るいか暗いかで決める
        # しきい値は前のコマから少しずつ動かす。場面が変わったときはすぐ合わせる
        self.th = t if self.th is None or abs(t - self.th) > 48 else (2 * self.th + t) / 3
        prev = self.bits
        self.bits = [[prev[y][x] if prev and abs(v - self.th) <= YT_MARGIN else int(v > self.th)
                      for x, v in enumerate(row)] for y, row in enumerate(dots)]
        cells = []
        for i in range(self.cols * YT_ROWS):
            cy, cx = divmod(i, self.cols)
            g = tuple(sum(self.bits[cy * 8 + r][cx * 5 + k] << (4 - k) for k in range(5)) for r in range(8))
            if self.cells and _dist(g, self.cells[i]) <= YT_KEEP_DIFF:
                g = self.cells[i]
            cells.append(g)
        self.cells = cells = _merge_cells(cells)
        glyphs, codes = [], []
        for g in cells:
            if g == _BLANK:
                codes.append(0x20)
            elif g == _FULL:
                codes.append(0xFF)
            else:
                if g not in glyphs:
                    glyphs.append(g)
                codes.append(glyphs.index(g))
        return bytes(codes), glyphs


class YtStream:
    """動画 1 本を MP3 (ffmpeg の pipe:1) と灰色の小さな動画 (もう 1 本のパイプ) にして、どちらもメモリに溜める。
    音と映像は yt-dlp がダウンロードしてパイプで ffmpeg に渡す。ffmpeg に URL を直接読ませると、dynabook では
    403 で断られた (手元の PC では通った)。yt-dlp は決まった大きさに区切って頼むので通る。
    yt-dlp も ffmpeg も実時間の十数倍で進むので、送るのに合わせて待たせない (一時停止しても YouTube との接続が止まらない)。
    cond は呼ぶ側 (YouTube) と同じものを使い、溜まるたびに知らせる"""

    def __init__(self, info, kbps, fps, cond, start_ms=0, full=True):
        import yt_dlp
        self.info, self.kbps, self.fps, self.cond = info, kbps, fps, cond
        self.start_ms = start_ms  # 動画のどこから鳴らすか (つなぎ直して続きから鳴らすとき)
        self.mp3 = bytearray()
        self.mp3_base = 0         # mp3[0] が、この動画の MP3 の何バイト目か (送った分は前から捨てる)
        self.frames = collections.deque()  # (動画の頭からの ms, 12 マスの番号, 絵) を古い順に
        self.audio_done = False
        self.closed = False

        fmts = info.get("requested_formats") or [info]
        video = next((f for f in fmts if f.get("vcodec") not in (None, "none")), None)
        audio = next((f for f in fmts if f.get("acodec") not in (None, "none")), fmts[0])
        srcs = [audio] if video is None or video is audio else [audio, video]
        # 調べた情報 (URL を含む) をファイルにして yt-dlp に読ませる。動画を調べ直さないので、すぐダウンロードが始まる
        fd, self.info_json = tempfile.mkstemp(prefix="espoke-yt-", suffix=".json")
        with os.fdopen(fd, "w") as f:
            json.dump(yt_dlp.YoutubeDL.sanitize_info(info), f)
        self.downloads, inputs = [], []
        for f in srcs:
            r, w = os.pipe()
            self.downloads.append(subprocess.Popen(
                [sys.executable, "-m", "yt_dlp", "--load-info-json", self.info_json, "-f", f["format_id"], "-o", "-",
                 "--quiet", "--no-warnings", "--no-progress"], stdin=subprocess.DEVNULL, stdout=w))
            os.close(w)
            inputs.append(r)
        # 続きから鳴らすときは、出す側で頭を捨てる (パイプは飛ばして読めない)
        skip = ["-ss", "%.3f" % (start_ms / 1000)] if start_ms else []
        cmd = ["ffmpeg", "-nostdin", "-loglevel", "error"]
        for r in inputs:
            cmd += ["-i", "pipe:%d" % r]
        cmd += ["-map", "0:a:0"] + skip + ["-ac", "2", "-ar", "44100", "-c:a", "libmp3lame", "-b:a", "%dk" % kbps,
                                         "-id3v2_version", "0", "-write_xing", "0", "-f", "mp3", "pipe:1"]
        fds = list(inputs)
        # 動画は LCD 全体に出す。横長の動画は上下を、縦長の動画 (ショート動画) は時計回りに 90 度回して (動画の上が右)
        # 元の左右を切り、中央の帯だけにする。full が偽なら、横長の動画は左の 6x2 文字に収める
        portrait = video is not None and (video.get("height") or 0) > (video.get("width") or 0) > 0
        self.cols = YT_FULL_COLS if video and (full or portrait) else YT_COLS
        if video:
            # 灰色にしてから縮める (yuv のままだと、奇数の幅で pad が失敗する)。実寸の枠に合わせて (LCD 全体なら、枠を
            # 覆うように広げてはみ出た所を切り、6x2 文字なら枠に収めて) から、すき間込みのドットの数に平均して縮める
            vr, vw = os.pipe()
            gray, box = yt_geometry(self.cols)
            fit = (("transpose=1," if portrait else "") +
                   "scale=%d:%d:force_original_aspect_ratio=increase,crop=%d:%d" % (box + box)
                   if self.cols == YT_FULL_COLS else
                   "scale=%d:%d:force_original_aspect_ratio=decrease,pad=%d:%d:-1:-1" % (box + box))
            vf = "fps=%d,format=gray,%s,scale=%d:%d:flags=area" % ((fps, fit) + gray)
            cmd += ["-map", "%d:v:0" % (len(srcs) - 1)] + skip + ["-vf", vf, "-f", "rawvideo", "pipe:%d" % vw]
            fds.append(vw)
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, pass_fds=fds)
        for r in inputs:
            os.close(r)
        self.video_done = not video
        if video:
            os.close(vw)
            threading.Thread(target=self._read_video, args=(vr,), daemon=True).start()
        threading.Thread(target=self._read_audio, daemon=True).start()

    def _read_audio(self):
        try:
            while True:
                data = self.proc.stdout.read1(65536)
                if not data:
                    break
                with self.cond:
                    self.mp3 += data
                    self.cond.notify_all()
                    # 送るのが追いつかないほど溜まったら (長いライブなど)、ffmpeg を待たせる
                    self.cond.wait_for(lambda: self.closed or len(self.mp3) < YT_BUF_MAX)
                    if self.closed:
                        break
        except (OSError, ValueError):
            pass
        finally:
            with self.cond:
                self.audio_done = True
                self.cond.notify_all()

    def _read_video(self, fd):
        (w, h), _ = yt_geometry(self.cols)
        conv, size = LcdVideo(self.cols), w * h
        try:
            with os.fdopen(fd, "rb") as f:
                k = 0
                while not self.closed:
                    gray = f.read(size)
                    if len(gray) < size:
                        break
                    codes, glyphs = conv.frame(gray)
                    with self.cond:
                        self.frames.append((self.start_ms + k * 1000 // self.fps, codes, glyphs))
                        self.cond.notify_all()
                    k += 1
        except (OSError, ValueError):
            pass
        finally:
            with self.cond:
                self.video_done = True
                self.cond.notify_all()

    def _next(self, sent):
        """MP3 を sent バイト送ったあと、次に送れる MP3 の量と、その終わりの時刻 (ms)。まだ溜まっていなければ None。
        コマはその時刻の音より先に送るので、その時刻までのコマが出そろうのも待つ。cond を持って呼ぶ"""
        n = min(len(self.mp3) - (sent - self.mp3_base), SP_FRAME)
        if n < SP_FRAME and not self.audio_done:
            return None
        end = self.start_ms + (sent + n) * 8 // self.kbps
        if not self.video_done and (not self.frames or self.frames[-1][0] <= end):
            return None
        return n, end

    def ready(self, sent):
        return self._next(sent) is not None

    def take(self, sent):
        """次に送るもの: (その MP3 の時刻までのコマ, MP3)。終わりなら ([], b"")。ready() のあとに cond を持って呼ぶ"""
        n, end = self._next(sent)
        frames = []
        while self.frames and self.frames[0][0] <= end:
            frames.append(self.frames.popleft())
        pos = sent - self.mp3_base
        return frames, bytes(self.mp3[pos:pos + n])

    def trim(self, sent):
        """送り終えた MP3 を前から捨てる。cond を持って呼ぶ"""
        if sent - self.mp3_base >= YT_TRIM:
            del self.mp3[:sent - self.mp3_base]
            self.mp3_base = sent
            self.cond.notify_all()  # 溜める上限で待っている読む側を起こす

    def close(self):
        with self.cond:
            self.closed = True
            self.cond.notify_all()
        for p in [self.proc] + self.downloads:
            p.kill()
            p.wait()
        self.proc.stdout.close()
        try:
            os.remove(self.info_json)
        except OSError:
            pass


class YouTube:
    """YouTube の一覧と再生の操作と、その音と動画を Pico に流すところ。Pico とのセッションは 1 本だけ"""

    def __init__(self, cookies, deno, kbps, fps, full):
        import yt_dlp  # 使うときだけ要る
        self.yt_dlp = yt_dlp
        self.opts = yt_options(cookies, deno)
        self.kbps, self.fps, self.full = kbps, fps, full
        self.ydl_lock = threading.Lock()  # yt-dlp は 1 つずつ使う (cookie のファイルを同時に書かないように)
        self.ops = concurrent.futures.ThreadPoolExecutor(1)       # 再生の操作は 1 つずつ
        self.list_ops = concurrent.futures.ThreadPoolExecutor(1)  # 一覧を読むのと、次の動画を先に調べるの
        self.cache = {}  # 一覧の URL -> (読んだ時刻, [(動画の URL, 題名, チャンネル, 長さ)])
        self.cache_lock = threading.Lock()
        self.lock = threading.Lock()
        self.session = None
        # 再生の様子。cond で守る
        self.cond = threading.Condition()
        self.lists = []          # [(名前, URL)]。L で送った順
        self.stream = None       # 鳴らしている YtStream
        self.gen = 0             # 動画が変わるたびに増やす。送る側はこれを見て Z を送る
        self.holding = False     # 一時停止中。音もコマも送らない
        self.cur = (None, -1)    # 鳴らしている (一覧の URL, 動画の番号)
        self.state = "stopped"
        self.shuffle = False
        self.history = []        # 前に鳴らした動画の番号 (前の動画へ戻るとき)
        self.next_pick = -1      # シャッフル中に次に鳴らす動画の番号 (先に調べておくため、先に決める)
        self.prefetched = None   # (一覧の URL, 動画の番号, 調べた時刻, yt-dlp の情報)
        self.sent_ms = 0         # 今の動画の、送った所の時刻 (ms)
        self.resume_ms = 0       # セッションが切れたときの所。R で続きから鳴らす

    # --- yt-dlp ---

    def _extract(self, url, **extra):
        with self.ydl_lock, self.yt_dlp.YoutubeDL(dict(self.opts, **extra)) as ydl:
            return ydl.extract_info(url, download=False)

    def _flat(self, url, limit):
        """一覧の中身 [(URL, 題名, チャンネル, 長さ)]。動画は開かずに一覧だけ読む。SP_TRACKS_TTL の間覚えておく"""
        with self.cache_lock:
            hit = self.cache.get(url)
        if hit and time.monotonic() - hit[0] < SP_TRACKS_TTL:
            return hit[1]
        info = self._extract(url, extract_flat="in_playlist", playlistend=limit)
        items = []
        for e in info.get("entries") or []:
            u = e and (e.get("url") or e.get("id") and "https://www.youtube.com/watch?v=" + e["id"])
            if u:
                items.append((u, e.get("title") or "", e.get("channel") or e.get("uploader") or "", e.get("duration")))
        with self.cache_lock:
            self.cache[url] = (time.monotonic(), items)
        return items

    def _resolve(self, url):
        """動画の情報 (音と映像の URL を含む)。見られなければ None と理由"""
        try:
            return self._extract(url, format=YT_FORMAT), None
        except Exception as e:  # yt-dlp の DownloadError など
            sys.stderr.write("youtube: %s: %s\n" % (url, e))
            return None, yt_reason(e)

    # --- 操作 ---

    def _error(self, s, text):
        try:
            s.send(b"E", text.encode())
        except SessionClosed:
            pass

    def _status(self, s):
        """状態 (S) を知らせる。形は Spotify と同じ"""
        with self.cond:
            url, i = self.cur
            n = next((k for k, (_, u) in enumerate(self.lists) if u == url), -1)
            text = "%s\t%d\t%d\t%d\t%d" % (self.state, self.shuffle, n, s.cmds, i if n >= 0 else -1)
        s.send(b"S", text.encode())

    def _stop_stream(self):
        """鳴らしている動画をやめる。送る側は Z を送って、次の動画を待つ"""
        with self.cond:
            old, self.stream = self.stream, None
            self.gen += 1
            self.cond.notify_all()
        if old:
            old.close()

    def _pick_next(self, n_items, i):
        """i の次に鳴らす番号。シャッフル中はランダム (先に決めておいたもの)。一覧の終わりなら -1"""
        with self.cond:
            if self.shuffle:
                if not 0 <= self.next_pick < n_items or self.next_pick == i:
                    self.next_pick = random.choice([k for k in range(n_items) if k != i] or [i])
                return self.next_pick
        return i + 1 if i + 1 < n_items else -1

    def _start(self, s, url, i, start_ms=0):
        """一覧 url の i 番目の動画を鳴らし始める。見られない動画は飛ばして次へ (YT_FAIL_MAX 本まで)"""
        items = self._flat(url, YT_ITEMS)
        for _ in range(YT_FAIL_MAX):
            self._stop_stream()
            with self.cond:
                self.cur, self.state, self.holding, self.next_pick = (url, i), "playing", False, -1
                pre, self.prefetched = self.prefetched, None
            video, title, channel, duration = items[i]
            s.send(b"T", to_lcd(title, SP_TITLE_MAX) + b"\t" + to_lcd(channel, SP_TITLE_MAX)
                   + b"\t%d" % (duration or -1))
            self._status(s)
            if pre and pre[:2] == (url, i) and time.monotonic() - pre[2] < YT_PREFETCH_TTL:
                info, why = pre[3], None
            else:
                info, why = self._resolve(video)
            if s.closed:
                return
            if info:
                break
            self._error(s, why)
            i = self._pick_next(len(items), i)
            if i < 0:
                break
        else:
            info = None
        if not info:
            with self.cond:
                self.state = "stopped"
            return self._status(s)
        st = YtStream(info, self.kbps, self.fps, self.cond, start_ms, self.full)
        with self.cond:
            self.stream, self.sent_ms = st, start_ms
            self.cond.notify_all()
        sys.stderr.write("youtube: play %s (%s)\n" % (info.get("title"), "+".join(
            f.get("format_id", "?") for f in info.get("requested_formats") or [info])))
        # 題名とチャンネルは、一覧のものより詳しいことがある。長さは一覧に無いことがある (ライブは -1)
        s.send(b"T", to_lcd(info.get("title") or title, SP_TITLE_MAX) + b"\t"
               + to_lcd(info.get("channel") or channel, SP_TITLE_MAX)
               + b"\t%d" % (-1 if info.get("is_live") else int(info.get("duration") or -1)))
        # 次の動画を先に調べておく (切り替えの待ちを短くする)
        nxt = self._pick_next(len(items), i)
        if nxt >= 0:
            def prefetch():
                info, _ = self._resolve(items[nxt][0])
                if info:
                    with self.cond:
                        if self.cur[0] == url:
                            self.prefetched = (url, nxt, time.monotonic(), info)
            self.list_ops.submit(prefetch)

    def _play(self, s, n, i, shuffle):
        """n 番目の一覧を i 番目の動画から鳴らす。shuffle ならシャッフルを入れて、ランダムな動画から"""
        url = self.lists[n][1]
        try:
            items = self._flat(url, YT_ITEMS)
        except Exception as e:
            sys.stderr.write("youtube: list %d: %s\n" % (n, e))
            return self._error(s, yt_reason(e))
        if not items:
            return self._error(s, "no videos")
        with self.cond:
            self.history = []
            if shuffle:
                self.shuffle = True
        self._start(s, url, random.randrange(len(items)) if shuffle else min(i, len(items) - 1))

    def _skip(self, s, step):
        """次 (step 1) か前 (step -1) の動画へ。次はシャッフル中ならランダム、一覧の終わりなら頭へ。前は鳴らした順に戻る"""
        url, i = self.cur
        if url is None:
            return
        items = self._flat(url, YT_ITEMS)
        with self.cond:
            back = self.history.pop() if step < 0 and self.history else None
        if step > 0:
            nxt = self._pick_next(len(items), i)
            nxt = 0 if nxt < 0 else nxt
            with self.cond:
                self.history = (self.history + [i])[-SP_LISTS:]
        else:
            nxt = back if back is not None else (i - 1) % len(items)
        self._start(s, url, nxt)

    def _ended(self, s, gen):
        """動画を最後まで送った。一覧の次へ (シャッフル中はランダム)。一覧の終わりなら止める"""
        with self.cond:
            if gen != self.gen:
                return  # もう別の動画にした
        url, i = self.cur
        nxt = self._pick_next(len(self._flat(url, YT_ITEMS)), i)
        if nxt < 0:
            self._stop_stream()
            with self.cond:
                self.state = "stopped"
            return self._status(s)
        with self.cond:
            self.history = (self.history + [i])[-SP_LISTS:]
        self._start(s, url, nxt)

    def _hold(self, s, on):
        """一時停止 / 再開。送る側は次のフレームから送らない / 送る。状態の知らせは送らない
        (命令を読むスレッドから呼ぶので、送る窓が空くのを待つと、窓を空ける C を読めなくなる)"""
        with self.cond:
            self.holding = on
            if self.stream:
                self.state = "paused" if on else "playing"
            self.cond.notify_all()

    def _resume(self, s):
        """再開。セッションが切れて動画をやめていたら、切れたときの所から鳴らし直す"""
        with self.cond:
            restart = self.stream is None and self.cur[0] is not None and self.state != "stopped"
            url, i = self.cur
        if restart:
            self._start(s, url, i, self.resume_ms)
        else:
            self._hold(s, False)
            self._status(s)

    def _toggle_shuffle(self, s):
        with self.cond:
            self.shuffle = not self.shuffle
            self.next_pick = -1
            self.prefetched = None  # シャッフルでない次の動画を調べていた
        self._status(s)

    def _end_session(self, s):
        """Pico が切れた。動画をやめ (「並列で他のことはしない」)、つなぎ直したら続きから鳴らせるように所を覚えておく。
        次につないだときは一時停止として知らせ、再開 (R) で続きから鳴らす"""
        with self.cond:
            self.resume_ms = max(0, self.sent_ms - YT_RESUME_BACK)
            if self.state != "stopped":
                self.state = "paused"
        self._stop_stream()

    def _command(self, s, data):
        kind, arg = data[:1], data[1:].decode("ascii", "replace")
        if kind == b"C":
            if arg.isdigit():
                s.ack(int(arg))
            return
        sys.stderr.write("youtube: %s\n" % data.decode("ascii", "replace"))
        if kind in (b"K", b"R", b"P", b"U", b"N", b"B"):
            s.cmds += 1
        n, _, i = arg.partition(":")
        n = int(n) if n.isdigit() and int(n) < len(self.lists) else -1
        i = int(i) if i.isdigit() else -1
        if kind == b"K":
            self._hold(s, True)  # 今すぐ止める
            self._submit(self._status, s)
        elif kind == b"R":
            self._submit(self._resume, s)
        elif kind == b"P" and n >= 0:
            self._submit(self._play, s, n, max(i, 0), False)
        elif kind == b"U" and n >= 0:
            self._submit(self._play, s, n, 0, True)
        elif kind == b"Q" and n >= 0 and i >= 0:
            self.list_ops.submit(self._send_items, s, n, i)
        elif kind == b"N":
            self._submit(self._skip, s, 1)
        elif kind == b"B":
            self._submit(self._skip, s, -1)
        elif kind == b"F":
            self._submit(self._toggle_shuffle, s)

    def _submit(self, fn, *args):
        """操作を順に1つずつする。失敗したらログに出す (投げっぱなしだと例外が消える)"""
        def done(f):
            if f.exception() and not isinstance(f.exception(), SessionClosed):
                sys.stderr.write("youtube: %s: %r\n" % (fn.__name__, f.exception()))
        self.ops.submit(fn, *args).add_done_callback(done)

    # --- Pico へ送る ---

    def _send_lists(self, s):
        """一覧の一覧を送る。頭は YT_LISTS (おすすめ・登録チャンネル・後で見る・高評価)、そのあとに自分の再生リスト。
        再生リストを読めなければ、その理由 (Pico に出す) を返す"""
        lists, why = list(YT_LISTS), None
        try:
            lists += [(title, url) for url, title, _, _ in self._flat(YT_PLAYLISTS, SP_LISTS - len(YT_LISTS))
                      if "list=WL" not in url and "list=LL" not in url]
        except Exception as e:
            sys.stderr.write("youtube: playlists: %s\n" % e)
            why = yt_reason(e)
        with self.cond:
            self.lists = lists
        for i, (name, _) in enumerate(lists):
            s.send(b"L", b"%d\t%d\t" % (i, len(lists)) + (to_lcd(name, SP_NAME_MAX) or b"?"))
        return why

    def _send_items(self, s, n, start):
        """n 番目の一覧の、start 番目から SP_PAGE 本の題名とチャンネルを I で送る (Spotify の曲の一覧と同じ形)"""
        try:
            items = self._flat(self.lists[n][1], YT_ITEMS)
            if not items:
                return s.send(b"I", b"%d\t-1\t0\t" % n)
            for i in range(start, min(start + SP_PAGE, len(items))):
                _, title, channel, _ = items[i]
                s.send(b"I", b"%d\t%d\t%d\t" % (n, i, len(items)) + (to_lcd(title, SP_TRACK_MAX) or b"?")
                       + b"\t" + to_lcd(channel, SP_ARTIST_MAX))
        except SessionClosed:
            pass
        except Exception as e:
            sys.stderr.write("youtube: list %d: %s\n" % (n, e))
            self._error(s, yt_reason(e))

    def _send_av(self, s):
        """鳴らしている動画の MP3 と LCD のコマを送る。コマは、その時刻の音より先に送る。
        コマには、その時刻の音が、このセッションで送った MP3 の何バイト目かを付ける (Pico はそこを鳴らすときに出す。
        動画の切れ目や一時停止や、Pico が読み捨てた分があっても、Pico が受け取った MP3 のバイト数で数えるので合う)"""
        gen, sent, base, ended = None, 0, 0, False

        def ready():
            st = self.stream
            return s.closed or self.gen != gen or (st is not None and not self.holding and not ended and st.ready(sent))

        while not s.closed:
            with self.cond:
                self.cond.wait_for(ready, 1)
                if s.closed:
                    break
                new = self.gen != gen
                if new:
                    gen, sent, ended = self.gen, 0, False
                elif not ready() or self.stream is None or self.holding or ended:
                    continue
                else:
                    st = self.stream
                    frames, data = st.take(sent)
            if new:
                s.send(b"Z")  # これより前の音は前の動画 (Pico が曲送りを頼んだのなら、Pico はそれまでを捨てる)
                base = s.audio
                continue
            if not data:
                ended = True
                self._submit(self._ended, s, gen)
                continue
            for ms, codes, glyphs in frames:
                offset = (base + (ms - st.start_ms) * self.kbps // 8) & 0xFFFFFFFF
                s.send(b"V", struct.pack("<IIB", offset, ms, st.cols) + codes + b"".join(bytes(g) for g in glyphs))
            if not s.send(b"A", data, held=lambda: self.holding or self.gen != gen):
                continue  # 一時停止か、別の動画にした。続きは次に送る
            s.audio += len(data)
            sent += len(data)
            with self.cond:
                st.trim(sent)
                self.sent_ms = st.start_ms + sent * 8 // self.kbps

    def _heartbeat(self, s):
        """何も送っていなければ HEARTBEAT 秒ごとに H を送る"""
        while not s.wait_closed(5):
            if time.monotonic() - s.last_send >= HEARTBEAT:
                s.send(b"H")

    def serve(self, handler, win):
        """Pico との WebSocket が開いてから閉じるまで"""
        s = StreamSession(handler.connection, win)
        with self.lock:
            old, self.session = self.session, s
        if old:
            old.close()
        sys.stderr.write("youtube: session start win=%d\n" % win)

        def run(target):
            def body():
                try:
                    target(s)
                except SessionClosed:
                    pass
                except Exception as e:  # 送る側が死んだら全体を閉じる (Pico はつなぎ直してくる)
                    sys.stderr.write("youtube: %s: %r\n" % (target.__name__, e))
                    s.close()
            threading.Thread(target=body, daemon=True).start()

        handler.connection.settimeout(SP_TIMEOUT)
        try:
            why = self._send_lists(s)
            self._status(s)
            if why:
                self._error(s, why)  # 状態 (S) のあとに送る。Pico は S で知らせの文字を消す
            run(self._send_av)
            run(self._heartbeat)
            while not s.closed:
                op, data = ws_read(s.read)
                if op is None or op == 0x8:
                    break
                if op == 0x9:
                    s.send(b"", data, 0xA)  # ping には pong を返す
                elif op == 0x2 and data:
                    self._command(s, data)
        except (OSError, EOFError, SessionClosed) as e:
            if not s.closed:
                sys.stderr.write("youtube: %r\n" % e)
        finally:
            s.close()
            with self.lock:
                last = self.session is s
                if last:
                    self.session = None
            if last:
                self._submit(self._end_session, s)
            sys.stderr.write("youtube: session end (%d KB audio, unread max %d/%d)\n"
                             % (s.audio // 1024, s.unread_max, win))


YOUTUBE = None  # YOUTUBE_COOKIES があれば main() が作る


def youtube_test(url, cookies, deno, kbps, fps, full):
    """Pico を使わずに、url の動画を LCD のコマにして端末に出す (# が点いているドット)。2 値化の具合と、
    実時間より速く作れるかを見る。端末ならその速さで描き直し、パイプに流すなら待たずに全部出す"""
    import yt_dlp
    t0 = time.monotonic()
    with yt_dlp.YoutubeDL(dict(yt_options(cookies, deno), format=YT_FORMAT)) as ydl:
        info = ydl.extract_info(url, download=False)
    print("%s / %s (%s 秒)。調べるのに %.1f 秒。形式 %s" % (
        info.get("title"), info.get("channel"), info.get("duration"), time.monotonic() - t0,
        "+".join(f.get("format_id", "?") for f in info.get("requested_formats") or [info])))
    cond = threading.Condition()
    st = YtStream(info, kbps, fps, cond, full=full)
    tty = sys.stdout.isatty()
    t1, count, glyph_sum, full = time.monotonic(), 0, 0, 0
    try:
        while True:
            with cond:
                cond.wait_for(lambda: st.frames or st.video_done)
                if not st.frames:
                    break
                ms, codes, glyphs = st.frames.popleft()
                st.mp3_base += len(st.mp3)  # MP3 は捨てる
                del st.mp3[:]
                cond.notify_all()
            count += 1
            glyph_sum += len(glyphs)
            full += len(glyphs) == YT_GLYPHS
            if tty:
                time.sleep(max(0, t1 + ms / 1000 - time.monotonic()))
                print("\x1b[H\x1b[J%d:%02d\n%s" % (ms // 60000, ms // 1000 % 60, yt_art(codes, glyphs)), flush=True)
            else:
                print("%d ms (%d glyphs)\n%s" % (ms, len(glyphs), yt_art(codes, glyphs)))
    except KeyboardInterrupt:
        pass
    finally:
        st.close()
    el = time.monotonic() - t1
    print("%d コマ (%.1f 秒分) を %.1f 秒で作った。自作文字は平均 %.1f 個、%d 個使い切ったコマが %d%%" % (
        count, count / fps, el, glyph_sum / max(count, 1), YT_GLYPHS, 100 * full // max(count, 1)))


# ---------------- HTTP ----------------

def stream_after(url):
    """/stream?after=<id> の id。無ければ None"""
    after = urllib.parse.parse_qs(url.query).get("after", [""])[0]
    return int(after) if after.isdigit() else None


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.0 にすると応答のたびに接続が閉じる。Content-Length を付けずに流し続けられる。
    # Pico も HTTP/1.0 で頼んでくるので、Funnel の手前でも chunked にならない
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def _text(self, body, code=200):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _authorized(self):
        if hmac.compare_digest(self.headers.get("X-Key", ""), KEY):
            return True
        self._text("bad key\n", 403)
        return False

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        service = {"/spotify": SPOTIFY, "/youtube": YOUTUBE}.get(url.path)
        if service:
            if self._authorized():
                self._websocket(url, service)
            return
        if url.path != "/stream":
            return self._text("espoke discord relay\n" if url.path == "/" else "not found\n",
                              200 if url.path == "/" else 404)
        if not self._authorized():
            return
        self._stream(stream_after(url))

    def _websocket(self, url, service):
        """WebSocket に切り替えて、Spotify か YouTube (service) の音と操作をやりとりする"""
        key = self.headers.get("Sec-WebSocket-Key", "")
        if self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            return self._text("websocket only\n", 400)
        win = urllib.parse.parse_qs(url.query).get("win", [""])[0]
        win = min(max(int(win), SP_WIN_MIN), SP_WIN_MAX) if win.isdigit() else 3072
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        # HTTP/1.0 の応答では Funnel の手前のプロキシが切り替えてくれないので、ここだけ 1.1 で返す
        self.wfile.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        self.close_connection = True
        service.serve(self, win)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        if url.path not in ("/post", "/stream"):
            return self._text("not found\n", 404)
        if not self._authorized():
            return
        n = int(self.headers.get("Content-Length") or 0)
        text = self.rfile.read(n).decode("utf-8", "replace")
        if url.path == "/post":
            add_message(STORE.next_id(), datetime.datetime.now(JST), "test", text)
            return self._text("ok\n")
        # Pico から送る文。Pico はつなぐたびに TLS の握手 (4〜8 秒) をするので、
        # 送るのと受け取り直すのを1回の接続で済ませる。結果はヘッダで返す
        text = text.strip()[:SEND_MAX]
        sent = SEND(text) if text else "empty"
        sys.stderr.write("send %r: %s\n" % (text, sent))
        self._stream(stream_after(url), sent)

    def _stream(self, after, sent=None):
        sys.stderr.write("stream start after=%s\n" % after)
        self.connection.settimeout(STREAM_TIMEOUT)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=shift_jis")
        self.send_header("Connection", "close")
        if sent is not None:
            self.send_header("X-Sent", sent)
        self.end_headers()

        sent = 0
        try:
            # 溜まっていた分のあとに空行を1つ送る。Pico はここまでを「前からあった分」として扱う
            with STORE.cond:
                items = STORE.since(after)
            self.wfile.write(b"".join(line for _, line in items) + b"\n")
            if items:
                after = items[-1][0]
                sent += len(items)
            while True:
                with STORE.cond:
                    items = STORE.since(after)
                    if not items:
                        STORE.cond.wait(HEARTBEAT)
                        items = STORE.since(after)
                if items:
                    self.wfile.write(b"".join(line for _, line in items))
                    after = items[-1][0]
                    sent += len(items)
                else:
                    self.wfile.write(b"\n")
        except (BrokenPipeError, ConnectionResetError):
            sys.stderr.write("  stream closed by client (%d sent)\n" % sent)
        except (TimeoutError, socket.timeout):
            sys.stderr.write("  client gone, %ds no progress (%d sent)\n" % (STREAM_TIMEOUT, sent))


class TLSHTTPServer(ThreadingHTTPServer):
    """TLS を自分で受ける HTTP サーバー。Funnel の TCP 転送 (SPOTIFY_FUNNEL_PORT) の先に置く。
    証明書のファイルが新しくなったら、次の接続から読み直す"""

    def __init__(self, addr, handler, cert, key):
        super().__init__(addr, handler)
        self.cert, self.key = cert, key
        self._ctx, self._mtime = None, 0

    def _context(self):
        mtime = os.stat(self.cert).st_mtime
        if self._ctx is None or mtime != self._mtime:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(self.cert, self.key)
            self._ctx, self._mtime = ctx, mtime
        return self._ctx

    def finish_request(self, request, client_address):
        # 握手は接続ごとのスレッドでする (Pico は 5〜9 秒かかる)
        request.settimeout(STREAM_TIMEOUT)
        try:
            tls = self._context().wrap_socket(request, server_side=True)
        except (OSError, ValueError) as e:
            sys.stderr.write("tls: %s %r\n" % (client_address[0], e))
            return
        try:
            self.RequestHandlerClass(tls, client_address, self)
        finally:
            tls.close()


def keep_cert(host, cert, key):
    """tailscale cert で Funnel と同じ証明書を取り出し、1日ごとに取り直す (期限が近ければ tailscaled が更新する)"""
    while True:
        try:
            r = subprocess.run(["tailscale", "cert", "--cert-file", cert, "--key-file", key, host],
                               capture_output=True, text=True, timeout=300)
            if r.returncode:
                sys.stderr.write("tailscale cert: %s\n" % (r.stderr.strip() or r.stdout.strip()))
        except (OSError, subprocess.TimeoutExpired) as e:
            sys.stderr.write("tailscale cert: %r\n" % e)
        time.sleep(CERT_CHECK)


# ---------------- Discord ----------------

def run_discord(token, guild_id, channel_id, send_id):
    """channel_id があればそのチャンネルだけ、なければ guild_id のサーバー全体を受け取る。
    Pico からの文は send_id か channel_id のチャンネル、どちらも無ければ最後に受け取ったチャンネルに送る"""
    global SEND
    import discord

    intents = discord.Intents.default()
    intents.message_content = True  # Developer Portal でも Message Content Intent を ON にしておく
    client = discord.Client(intents=intents)

    def wanted(m):
        if m.author == client.user:
            return False
        if channel_id:
            return m.channel.id == channel_id
        return m.guild is not None and m.guild.id == guild_id

    last_channel = None  # 最後に受け取った発言のチャンネル

    def add(m):
        nonlocal last_channel
        last_channel = m.channel
        add_message(m.id, m.created_at, short_name(m.author.display_name), discord_text(m),
                    None if channel_id else m.channel.name)

    async def send(text):
        to = send_id or channel_id
        channel = (client.get_channel(to) or await client.fetch_channel(to)) if to else last_channel
        if channel is None:
            return "no channel"
        # 合言葉を知っていれば誰でも送れるので、@everyone などで人を呼ばせない
        await channel.send(text, allowed_mentions=discord.AllowedMentions.none())
        sys.stderr.write("sent to #%s\n" % channel)
        return "ok"

    def send_from_pico(text):
        """HTTP のスレッドから呼ぶ。Discord のイベントループで送り、終わるまで待つ"""
        if not client.is_ready():
            return "not ready"
        future = asyncio.run_coroutine_threadsafe(send(text), client.loop)
        try:
            return future.result(SEND_TIMEOUT)
        except concurrent.futures.TimeoutError:
            future.cancel()
            return "timeout"
        except discord.HTTPException as e:
            return "discord %d" % e.status  # 403 なら Bot に送信の権限が無い、404 なら ID が違う

    SEND = send_from_pico

    @client.event
    async def on_ready():
        if channel_id:
            channels = [client.get_channel(channel_id) or await client.fetch_channel(channel_id)]
            where = "#%s" % channels[0]
        else:
            guild = client.get_guild(guild_id)
            if guild is None:
                print("discord: サーバー %d に Bot が入っていない (招待したか確かめる)" % guild_id)
                return
            # 閲覧権限の無いチャンネルは読めない (permissions_for がその分も落としてくれる)
            channels = [c for c in guild.text_channels
                        if c.permissions_for(guild.me).read_message_history]
            where = "サーバー %s の %d チャンネル" % (guild.name, len(channels))

        # 中継を立ち上げ直しても Pico に直近の発言を渡せるよう、履歴から埋めておく。
        # サーバー全体なら、各チャンネルの直近を集めて新しいものから BACKLOG 件
        history = []
        for c in channels:
            try:
                history += [m async for m in c.history(limit=BACKLOG)]
            except discord.Forbidden:
                pass
        history.sort(key=lambda m: m.id)
        for m in [m for m in history if wanted(m)][-BACKLOG:]:
            add(m)
        print("discord: %s として %s を見ている" % (client.user, where))

    @client.event
    async def on_message(m):
        if wanted(m):
            add(m)

    client.run(token, log_handler=None)


# ---------------- Discord アプリの通知から受け取る (--notify) ----------------

NOTIFY_NAME = "org.freedesktop.Notifications"
ECHO_WAIT = 60  # webhook で送った文が、通知になって戻ってくるのを待つ時間 (秒)。戻ってきたら捨てる


def parse_title(title):
    """Discord の通知の題名 "名前 (#チャンネル, カテゴリ)" を (名前, チャンネル) に分ける。
    題名にサーバー名は無い。DM などチャンネルの無い通知は (題名, None)"""
    # Discord は名前・チャンネル・カテゴリを、文字の向きの区切り (U+2068 … U+2069) で1つずつ囲んでくる
    title = re.sub("[\u200e\u200f\u2066-\u2069]", "", title)
    name, sep, rest = title.partition(" (#")
    if not sep or not rest.endswith(")"):
        return title, None
    return name, rest[:-1].split(", ")[0]


def run_notify(channel, webhook):
    """Bot を入れられないサーバー向け。同じ PC で動かす公式の Discord アプリが出すデスクトップ通知を受け取る。
    この中継が D-Bus の通知サーバー (org.freedesktop.Notifications) になり、通知の題名と本文を発言として流す。
    通知の題名にサーバー名は無いので、どのサーバーの発言を受け取るかは Discord の通知の設定で決める。
    channel があれば、そのチャンネルの通知だけを使う。DM は捨てる。
    Pico からの文は webhook に送る。webhook が無ければ受け取るだけ"""
    global SEND
    from jeepney import HeaderFields, MessageType, new_error, new_method_return
    from jeepney.bus_messages import message_bus
    from jeepney.io.blocking import open_dbus_connection

    sent = []  # webhook で送った (時刻, 文)。Discord はそれも通知してくるので、戻ってきたら捨てる
    lock = threading.Lock()

    def send(text):
        data = json.dumps({"content": text, "allowed_mentions": {"parse": []}}).encode()
        req = urllib.request.Request(webhook, data, {"Content-Type": "application/json",
                                                     "User-Agent": "espoke-relay"})
        with lock:
            sent.append((time.monotonic(), text))
        try:
            urllib.request.urlopen(req, timeout=SEND_TIMEOUT).close()
        except urllib.error.HTTPError as e:
            return "discord %d" % e.code  # 404 なら webhook が消されている
        except OSError:
            return "network"
        sys.stderr.write("sent by webhook\n")
        return "ok"

    def echoed(body):
        now = time.monotonic()
        with lock:
            sent[:] = [(t, s) for t, s in sent if now - t < ECHO_WAIT]
            for i, (_, s) in enumerate(sent):
                if s.strip() == body.strip():
                    del sent[i]
                    return True
        return False

    def same(a, b):
        """名前を比べる。全角と半角、大文字と小文字の違いは見ない (IT と it、ＩＴ も同じ)"""
        return a is not None and (unicodedata.normalize("NFKC", a).casefold()
                                  == unicodedata.normalize("NFKC", b).casefold())

    def on_notify(app, title, body):
        # 題名の形が Discord の更新で変わったら、ここに出る生の値を見て parse_title を直す
        sys.stderr.write("notify %r %r %r\n" % (app, title, body))
        if "discord" not in app.lower():
            return
        name, ch = parse_title(title)
        if ch is None or (channel and not same(ch, channel)) or echoed(body):
            return
        add_message(STORE.next_id(), datetime.datetime.now(JST), short_name(name), tidy_text(body),
                    None if channel else ch)

    SEND = send if webhook else (lambda text: "no webhook")

    conn = open_dbus_connection(bus="SESSION")
    reply = conn.send_and_get_reply(message_bus.RequestName(NOTIFY_NAME, 4))  # 4: 空くのを待たない
    if reply.body[0] != 1:
        sys.exit("discord: %s を他のプログラム (デスクトップの通知) が持っている" % NOTIFY_NAME)
    print("discord: Discord アプリの通知から %s を見ている。送るのは %s" % (
        "#%s" % channel if channel else "チャンネルの発言すべて",
        "webhook" if webhook else "無し (受け取るだけ)"))

    count = 0
    while True:
        msg = conn.receive()
        if msg.header.message_type != MessageType.method_call:
            continue
        member = msg.header.fields.get(HeaderFields.member)
        if member == "Notify":
            count += 1
            conn.send(new_method_return(msg, "u", (count,)))
            app, _, _, title, body = msg.body[:5]
            on_notify(app, title, body)
        elif member == "GetCapabilities":
            # body-markup を言わなければ、本文は HTML に直されずにそのままの字で来る
            conn.send(new_method_return(msg, "as", (["body"],)))
        elif member == "GetServerInformation":
            conn.send(new_method_return(msg, "ssss", ("espoke-relay", "espoke", "1", "1.2")))
        elif member == "CloseNotification":
            conn.send(new_method_return(msg))
        else:
            conn.send(new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod"))


# ---------------- 起動 ----------------

def load_env(path):
    """KEY=VALUE の行を環境変数に読み込む (既に設定されているものは上書きしない)"""
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip("'\""))


def funnel_host():
    """tailscale funnel で公開される名前。分からなければ None"""
    try:
        out = subprocess.run(["tailscale", "status", "--json"], capture_output=True,
                             text=True, timeout=5).stdout
        return json.loads(out)["Self"]["DNSName"].rstrip(".") or None
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        return None


def main():
    global KEY, SPOTIFY, YOUTUBE

    ap = argparse.ArgumentParser(
        description="espoke 用: Discord の発言を LCD 用の半角カナにして Pico WH へ流す。Spotify と YouTube の音も流す")
    ap.add_argument("--port", type=int, default=8001)
    ap.add_argument("--env", default="~/espoke/relay.env",
                    help="DISCORD_TOKEN などを書いたファイル (無ければ環境変数だけを見る)")
    how = ap.add_mutually_exclusive_group()
    how.add_argument("--no-discord", action="store_true",
                     help="Discord につながず、POST /post で入れた発言だけを流す")
    how.add_argument("--notify", action="store_true",
                     help="Bot を使わず、同じ PC で動かす Discord アプリの通知から受け取る")
    ap.add_argument("--convert", metavar="TEXT", help="TEXT を変換して表示し、終わる")
    ap.add_argument("--youtube-test", metavar="URL",
                    help="URL の YouTube の動画を、Pico の LCD に出すコマにして端末に出し、終わる")
    args = ap.parse_args()

    if args.convert is not None:
        print(to_lcd(args.convert, TEXT_MAX).decode("cp932"))
        return

    env = os.path.expanduser(args.env)
    if os.path.exists(env):
        load_env(env)
    cookies = os.path.expanduser(os.environ.get("YOUTUBE_COOKIES", ""))
    deno = os.path.expanduser(os.environ.get("YOUTUBE_DENO", "~/espoke/bin/deno"))
    yt_kbps = int(os.environ.get("YOUTUBE_KBPS", YT_KBPS))
    yt_fps = int(os.environ.get("YOUTUBE_FPS", YT_FPS))
    yt_full = os.environ.get("YOUTUBE_FULL", "1") != "0"
    if args.youtube_test:
        return youtube_test(args.youtube_test, cookies, deno, yt_kbps, yt_fps, yt_full)

    # ログにリダイレクトしても起動メッセージがすぐ出るようにする
    sys.stdout.reconfigure(line_buffering=True)

    KEY = os.environ.get("RELAY_KEY", "")
    if not KEY:
        sys.exit("RELAY_KEY が設定されていない (%s か環境変数に書く)" % env)
    if args.notify:
        try:
            import jeepney  # noqa: F401
        except ImportError:
            sys.exit("--notify には jeepney が要る (~/espoke/venv/bin/pip install jeepney)")
        notify_channel = os.environ.get("DISCORD_NOTIFY_CHANNEL", "").lstrip("#")
        webhook = os.environ.get("DISCORD_WEBHOOK_URL", "")
    elif not args.no_discord:
        token = os.environ.get("DISCORD_TOKEN", "")
        guild = os.environ.get("DISCORD_GUILD_ID", "")
        channel = os.environ.get("DISCORD_CHANNEL_ID", "")
        send_channel = os.environ.get("DISCORD_SEND_CHANNEL_ID", "")
        if not token or not (guild.isdigit() or channel.isdigit()):
            sys.exit("DISCORD_TOKEN と、DISCORD_GUILD_ID (サーバー全体) か DISCORD_CHANNEL_ID (1チャンネル) "
                     "を設定する (%s か環境変数に書く)" % env)

    _reading("")  # 辞書の読み込みに数秒かかるので、つながれる前に済ませておく

    pipe = os.path.expanduser(os.environ.get("SPOTIFY_PIPE", ""))
    if pipe:
        try:
            SPOTIFY = Spotify(os.environ.get("SPOTIFY_API", "http://127.0.0.1:3678"), pipe,
                              int(os.environ.get("SPOTIFY_KBPS", "128")))
        except ImportError:
            sys.exit("Spotify には lameenc が要る (~/espoke/venv/bin/pip install lameenc)")
        print("spotify: %s から %s kbps の MP3 にして流す" % (pipe, SPOTIFY.kbps))
    if cookies:
        try:
            YOUTUBE = YouTube(cookies, deno, yt_kbps, yt_fps, yt_full)
        except ImportError:
            sys.exit('YouTube には yt-dlp が要る (~/espoke/venv/bin/pip install "yt-dlp[default]")')
        if not os.path.exists(cookies):
            print("youtube: %s が無い (ブラウザの cookie を書き出して置く)" % cookies)
        if not os.path.exists(deno):
            print("youtube: %s が無い (YouTube の JS を解くのに deno が要る)" % deno)
        print("youtube: %s kbps の MP3 と、%d コマ/秒の LCD の動画 (%s) にして流す"
              % (yt_kbps, yt_fps, "LCD 全体" if yt_full else "横長の動画は左の 6x2 文字"))

    # Funnel が 127.0.0.1 へ中継してくるので、外には直接開けない
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    host = funnel_host() or "<マシン名>.<tailnet>.ts.net"
    print("  http://127.0.0.1:%d/" % args.port)
    print()
    print("公開していなければ:  tailscale funnel --bg --https=%d %d" % (FUNNEL_PORT, args.port))
    if SPOTIFY or YOUTUBE:
        # Spotify と YouTube は TLS をここで受ける。証明書は tailscale cert で Funnel と同じものを取り出す
        tls_port = int(os.environ.get("SPOTIFY_TLS_PORT", SPOTIFY_TLS_PORT))
        cert, key = (os.path.join(os.path.dirname(env), name) for name in ("tls.crt", "tls.key"))
        if funnel_host():
            threading.Thread(target=keep_cert, args=(host, cert, key), daemon=True).start()
        tls_server = TLSHTTPServer(("127.0.0.1", tls_port), Handler, cert, key)
        threading.Thread(target=tls_server.serve_forever, daemon=True).start()
        print("Spotify と YouTube も: tailscale funnel --bg --tcp=%d tcp://127.0.0.1:%d" % (SPOTIFY_FUNNEL_PORT, tls_port))
    print()
    print("espoke/arduino_secrets.h に書く:")
    print('  #define RELAY_HOST "%s"' % host)
    print('  #define RELAY_PORT %d' % FUNNEL_PORT)
    # 合言葉は端末で動かしたときだけ出す (systemd で動かすとログに残るので)
    print('  #define RELAY_KEY  "%s"' % (KEY if sys.stdout.isatty() else "<relay.env の RELAY_KEY>"))
    print()
    print("Ctrl-C で停止")

    if args.no_discord:
        server.serve_forever()
    elif args.notify:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        run_notify(notify_channel, webhook)
    else:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        run_discord(token, int(guild) if guild.isdigit() else None,
                    int(channel) if channel.isdigit() else None,
                    int(send_channel) if send_channel.isdigit() else None)


if __name__ == "__main__":
    main()
