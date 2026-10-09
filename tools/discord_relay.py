#!/usr/bin/env python3
"""
espoke 用の Discord 中継。常時動かしておく PC (学校の dynabook) の上で動かす。

Discord の決めたチャンネルに書き込まれた発言を受け取り、LCD1602A に出せる文字だけの
1行にして Pico WH へ流す。LCD の文字 ROM (A00) にあるのは英数字と半角カタカナだけで、
その並びは JIS X 0201 (cp932 の1バイト文字) と同じ。漢字は MeCab (fugashi + unidic-lite)
で読みに直してから半角カナにする。辞書は数十 MB あって Pico には載らないので、ここでやる。

Pico は Tailscale に入れないので、tailscale funnel でこのサーバを
https://<マシン名>.<tailnet>.ts.net:8443 として公開し、Pico から HTTPS でつながせる。
(dynabook の 443 番は tailnet 向けの tailscale serve で使っているので、Funnel は 8443 番にする)
    tailscale funnel --bg --https=8443 8001

必要なもの:
    python3 -m venv ~/espoke/venv
    ~/espoke/venv/bin/pip install discord.py 'fugashi[unidic-lite]'

設定 (環境変数。--env で読むファイルにも書ける):
    DISCORD_TOKEN       Bot のトークン
    DISCORD_CHANNEL_ID  受け取るチャンネルの ID
    RELAY_KEY           Pico と決めておく合言葉。Funnel で誰でもつなげるので、これで弾く

使い方:
    ~/espoke/venv/bin/python discord_relay.py                 # ~/espoke/relay.env を読んで動かす
    ~/espoke/venv/bin/python discord_relay.py --no-discord    # Discord につながず /post だけで試す
    ~/espoke/venv/bin/python discord_relay.py --convert "今日は雨"   # 変換結果だけ見る

    GET  /stream?after=<id>  after より新しい発言を1行ずつ流し続ける (X-Key ヘッダが要る)
    POST /post               本文 (UTF-8) を差出人 test の発言として流す。試験用 (X-Key ヘッダが要る)

1行は  <メッセージ ID> TAB <HH:MM> TAB <名前> TAB <本文> LF  で、cp932 のバイト列。
ID は Discord のメッセージ ID (snowflake) で、時間とともに増えるので、Pico は最後に
受け取った ID を after に付けてつなぎ直せば取りこぼさない。

空行は2つの意味で送る。つないだ直後は、溜まっていた分のあとに1つ送り、「つなぐ前から
あった分はここまで」と知らせる (Pico は起動直後の古い発言で着信を知らせないようにする)。
その後は何もない間 HEARTBEAT 秒ごとに送り、Pico が「つながっているが新着がない」と分かるようにする。
"""

import argparse
import bisect
import datetime
import hmac
import json
import os
import re
import socket
import subprocess
import sys
import threading
import unicodedata
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NAME_MAX = 10   # 名前のバイト数。LCD の1行目に、右端の HH:MM と並べて出す
TEXT_MAX = 200  # 本文のバイト数。Pico は2行目に流して出す
KEEP = 20       # 覚えておく発言の数。つなぎ直した Pico に取りこぼした分を送るため
BACKLOG = 8     # after なしでつないできたときに送る数 (Pico が持っておける数)
HEARTBEAT = 30  # 新着がないときに空行を送る間隔 (秒)
FUNNEL_PORT = 8443  # tailscale funnel で公開するポート (443 / 8443 / 10000 から選べる)

# 送信が詰まったまま何秒待つか。Pico は電源が落ちたり電波が切れたりすると
# FIN を返さずに消えるので、これを入れないとスレッドが居座る (serve_music.py と同じ)
STREAM_TIMEOUT = 30

JST = datetime.timezone(datetime.timedelta(hours=9))

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


def to_lcd(text, limit):
    """LCD1602A (A00 ROM) にそのまま書ける cp932 のバイト列にする"""
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
            out.append("?")  # 読めなかった漢字や、ハングルなど ROM に無い文字
        # 絵文字や記号は捨てる
    text = re.sub(r" +", " ", "".join(out)).strip()
    return text.encode("cp932")[:limit]


def discord_text(message):
    """Discord の発言から、表示する文を取り出す"""
    text = message.clean_content  # メンションは @名前 になっている
    text = re.sub(r"<a?:(\w+):\d+>", r":\1:", text)  # カスタム絵文字
    text = re.sub(r"https?://\S+", "URL", text)
    text = re.sub(r"\*\*|__|~~|\|\||`", "", text)    # 太字・下線・打ち消し・伏せ字・コード
    if message.attachments:
        text += " [添付]"
    if message.stickers:
        text += " [スタンプ]"
    return text


def make_line(msg_id, when, author, text):
    body = to_lcd(text, TEXT_MAX) or to_lcd("[表示できない]", TEXT_MAX)
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


def add_message(msg_id, when, author, text):
    line = make_line(msg_id, when, author, text)
    STORE.add(msg_id, line)
    sys.stderr.write("msg %s\n" % line.rstrip(b"\n").decode("cp932"))


# ---------------- HTTP ----------------

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
        if url.path != "/stream":
            return self._text("espoke discord relay\n" if url.path == "/" else "not found\n",
                              200 if url.path == "/" else 404)
        if not self._authorized():
            return
        after = urllib.parse.parse_qs(url.query).get("after", [""])[0]
        self._stream(int(after) if after.isdigit() else None)

    def do_POST(self):
        if urllib.parse.urlparse(self.path).path != "/post":
            return self._text("not found\n", 404)
        if not self._authorized():
            return
        n = int(self.headers.get("Content-Length") or 0)
        text = self.rfile.read(n).decode("utf-8", "replace")
        add_message(STORE.next_id(), datetime.datetime.now(JST), "test", text)
        self._text("ok\n")

    def _stream(self, after):
        sys.stderr.write("stream start after=%s\n" % after)
        self.connection.settimeout(STREAM_TIMEOUT)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=shift_jis")
        self.send_header("Connection", "close")
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


# ---------------- Discord ----------------

def run_discord(token, channel_id):
    import discord

    intents = discord.Intents.default()
    intents.message_content = True  # Developer Portal でも Message Content Intent を ON にしておく
    client = discord.Client(intents=intents)

    def add(m):
        add_message(m.id, m.created_at, m.author.display_name, discord_text(m))

    @client.event
    async def on_ready():
        # 中継を立ち上げ直しても Pico に直近の発言を渡せるよう、履歴から埋めておく
        channel = client.get_channel(channel_id) or await client.fetch_channel(channel_id)
        history = [m async for m in channel.history(limit=BACKLOG)]
        for m in reversed(history):
            add(m)
        print("discord: %s として #%s を見ている" % (client.user, channel))

    @client.event
    async def on_message(m):
        if m.channel.id == channel_id and m.author != client.user:
            add(m)

    client.run(token, log_handler=None)


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
    global KEY

    ap = argparse.ArgumentParser(
        description="espoke 用: Discord の発言を LCD 用の半角カナにして Pico WH へ流す")
    ap.add_argument("--port", type=int, default=8001)
    ap.add_argument("--env", default="~/espoke/relay.env",
                    help="DISCORD_TOKEN などを書いたファイル (無ければ環境変数だけを見る)")
    ap.add_argument("--no-discord", action="store_true",
                    help="Discord につながず、POST /post で入れた発言だけを流す")
    ap.add_argument("--convert", metavar="TEXT", help="TEXT を変換して表示し、終わる")
    args = ap.parse_args()

    if args.convert is not None:
        print(to_lcd(args.convert, TEXT_MAX).decode("cp932"))
        return

    # ログにリダイレクトしても起動メッセージがすぐ出るようにする
    sys.stdout.reconfigure(line_buffering=True)

    env = os.path.expanduser(args.env)
    if os.path.exists(env):
        load_env(env)
    KEY = os.environ.get("RELAY_KEY", "")
    if not KEY:
        sys.exit("RELAY_KEY が設定されていない (%s か環境変数に書く)" % env)
    if not args.no_discord:
        token = os.environ.get("DISCORD_TOKEN", "")
        channel = os.environ.get("DISCORD_CHANNEL_ID", "")
        if not token or not channel.isdigit():
            sys.exit("DISCORD_TOKEN と DISCORD_CHANNEL_ID を設定する (%s か環境変数に書く)" % env)

    _reading("")  # 辞書の読み込みに数秒かかるので、つながれる前に済ませておく

    # Funnel が 127.0.0.1 へ中継してくるので、外には直接開けない
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    host = funnel_host() or "<マシン名>.<tailnet>.ts.net"
    print("  http://127.0.0.1:%d/" % args.port)
    print()
    print("公開していなければ:  tailscale funnel --bg --https=%d %d" % (FUNNEL_PORT, args.port))
    print()
    print("espoke/arduino_secrets.h に書く:")
    print('  #define RELAY_HOST "%s"' % host)
    print('  #define RELAY_PORT %d' % FUNNEL_PORT)
    print('  #define RELAY_KEY  "%s"' % KEY)
    print()
    print("Ctrl-C で停止")

    if args.no_discord:
        server.serve_forever()
    else:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        run_discord(token, int(channel))


if __name__ == "__main__":
    main()
