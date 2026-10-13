#!/usr/bin/env python3
"""
espoke の Spotify 中継 (discord_relay.py の /spotify) を、Pico の代わりに試す。

Pico と同じく WebSocket でつなぎ、届いたプレイリスト・曲名・状態を表示して、音 (MP3) を mpv に流す。
読んだ量を C で返すので、win を Pico の受信バッファより小さくしても止まらずに流れるかを確かめられる。
標準ライブラリだけで動く。

    python3 tools/spotify_probe.py http://127.0.0.1:8001               # dynabook の上で
    python3 tools/spotify_probe.py https://dynabook.<tailnet>.ts.net:8443 # Funnel 越し (Pico と同じ道)
    python3 tools/spotify_probe.py --out song.mp3 --throttle 128 URL   # mpv が無ければファイルに書く

Pico と同じく Funnel の 10000 番 (中継が TLS を受ける) につなぐなら https://<マシン名>.<tailnet>.ts.net:10000。
tailnet の中の PC では名前が tailnet の中のアドレスになり、Funnel を通らない。Pico と同じ道を通すには
--connect に公開のアドレス (dig +short @1.1.1.1 <マシン名>.<tailnet>.ts.net) を書く。

RELAY_KEY は --key か環境変数か --env のファイルから読む。つないだら標準入力で操作する:
    p <番号> [<曲>]  プレイリストを (その曲から) 再生 (番号は 0 から)   u <番号>  シャッフルで再生
    t <番号> <曲>  その曲から 16 曲の名前を出す   n 次の曲   b 前の曲
    k 一時停止   r 再開   f シャッフル   q 終わる
"""

import argparse
import base64
import hashlib
import os
import socket
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
CREDIT_STEP = 1024   # Pico と同じく、これだけ読むたびに読んだ量を知らせる
KEEPALIVE = 30       # 何も知らせていなくても、この間隔で知らせる (中継は 90 秒で切る)


def load_key(args):
    if args.key:
        return args.key
    if os.environ.get("RELAY_KEY"):
        return os.environ["RELAY_KEY"]
    path = os.path.expanduser(args.env)
    if os.path.exists(path):
        for line in open(path):
            k, _, v = line.strip().partition("=")
            if k.strip() == "RELAY_KEY":
                return v.strip().strip("'\"")
    sys.exit("RELAY_KEY が分からない (--key か環境変数か %s)" % args.env)


def connect(url, key, win, address=None, insecure=False):
    u = urllib.parse.urlparse(url)
    tls = u.scheme == "https"
    port = u.port or (443 if tls else 80)
    sock = socket.create_connection((address or u.hostname, port), timeout=15)
    if tls:
        ctx = ssl.create_default_context()
        if insecure:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(sock, server_hostname=u.hostname)
    nonce = base64.b64encode(os.urandom(16)).decode()
    sock.sendall(("GET /spotify?win=%d HTTP/1.1\r\nHost: %s\r\nX-Key: %s\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                  % (win, u.hostname, key, nonce)).encode())
    head = b""
    while b"\r\n\r\n" not in head:
        b = sock.recv(1)
        if not b:
            sys.exit("切れた: %r" % head)
        head += b
    lines = head.decode("latin-1").split("\r\n")
    print(lines[0])
    if " 101 " not in lines[0] + " ":
        sys.exit("WebSocket にならなかった")
    want = base64.b64encode(hashlib.sha1((nonce + WS_GUID).encode()).digest()).decode()
    if not any(l.lower() == "sec-websocket-accept: " + want.lower() for l in lines):
        print("warning: Sec-WebSocket-Accept が合わない")
    sock.settimeout(None)
    return sock


class Probe:
    def __init__(self, sock, out, throttle):
        self.sock = sock
        self.out = out
        self.rate = throttle * 125 if throttle else 0  # 読む速さの上限 (バイト/秒)
        self.wlock = threading.Lock()
        self.read = 0
        self.reported = 0
        self.last_report = time.monotonic()
        self.waiting_z = False
        self.audio = 0
        self.dropped = 0
        self.t0 = time.monotonic()

    def send(self, payload):
        mask = os.urandom(4)
        n = len(payload)
        head = bytes([0x82, 0x80 | n]) if n < 126 else bytes([0x82, 0xFE]) + n.to_bytes(2, "big")
        with self.wlock:
            self.sock.sendall(head + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def recv(self, n):
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise EOFError
            data += chunk
        self.read += n
        if self.read - self.reported >= CREDIT_STEP or time.monotonic() - self.last_report >= KEEPALIVE:
            self.credit()
        return data

    def credit(self):
        self.reported = self.read
        self.last_report = time.monotonic()
        self.send(b"C%d" % self.read)

    def command(self, line):
        cmd = line.split()
        if not cmd:
            return
        c = cmd[0]
        if c == "p" and len(cmd) in (2, 3) and all(x.isdigit() for x in cmd[1:]):
            self.waiting_z = True
            self.send(b"P" + ":".join(cmd[1:]).encode())
        elif c == "u" and len(cmd) == 2 and cmd[1].isdigit():
            self.waiting_z = True
            self.send(b"U" + cmd[1].encode())
        elif c == "t" and len(cmd) == 3 and all(x.isdigit() for x in cmd[1:]):
            self.send(b"Q" + ":".join(cmd[1:]).encode())
        elif c in ("n", "b"):
            self.waiting_z = True
            self.send(b"N" if c == "n" else b"B")
        elif c in ("k", "f", "r"):
            self.send({"k": b"K", "f": b"F", "r": b"R"}[c])
        else:
            print("p <番号> [<曲>] / u <番号> / t <番号> <曲> / n / b / k / f / r / q")

    def run(self):
        while True:
            head = self.recv(2)
            opcode, n = head[0] & 0x0F, head[1] & 0x7F
            if n == 126:
                n = int.from_bytes(self.recv(2), "big")
            elif n == 127:
                n = int.from_bytes(self.recv(8), "big")
            data = self.recv(n)
            if opcode == 0x8:
                print("close")
                return
            if opcode != 0x2 or not data:
                continue
            kind, body = data[:1], data[1:]
            if kind == b"A":
                if self.waiting_z:
                    self.dropped += len(body)
                    continue
                self.audio += len(body)
                if self.out:
                    self.out.write(body)
                    self.out.flush()
                if self.rate:
                    ahead = self.audio / self.rate - (time.monotonic() - self.t0)
                    if ahead > 0:
                        time.sleep(ahead)
            elif kind == b"Z":
                print("Z (捨てた %d バイト)" % self.dropped)
                self.waiting_z = False
                self.dropped = 0
            else:
                text = body.decode("cp932", "replace").replace("\t", " | ")
                print("%s %s" % (kind.decode(), text))


def main():
    ap = argparse.ArgumentParser(description="espoke の Spotify 中継を Pico の代わりに試す")
    ap.add_argument("url", help="http://127.0.0.1:8001 か https://<マシン名>.<tailnet>.ts.net:8443")
    ap.add_argument("--key", help="RELAY_KEY")
    ap.add_argument("--env", default="~/espoke/relay.env", help="RELAY_KEY を書いたファイル")
    ap.add_argument("--win", type=int, default=49152, help="まだ読んでいなくてよい量 (Pico の SP_WIN と同じ)")
    ap.add_argument("--connect", metavar="ADDR", help="URL の名前ではなく、このアドレスにつなぐ (証明書は URL の名前で確かめる)")
    ap.add_argument("--insecure", action="store_true", help="証明書を確かめない (手元の自己署名の証明書で試すとき)")
    ap.add_argument("--out", default="mpv", help="mpv (既定) か、MP3 を書くファイル名。none なら捨てる")
    ap.add_argument("--throttle", type=int, default=0, metavar="KBPS",
                    help="音をこの速さでしか読まない (ファイルに書くとき、Pico の消費を真似る)")
    args = ap.parse_args()

    sock = connect(args.url, load_key(args), args.win, args.connect, args.insecure)
    player = None
    if args.out == "mpv":
        player = subprocess.Popen(["mpv", "--no-video", "--really-quiet", "-"], stdin=subprocess.PIPE)
        out = player.stdin
    elif args.out == "none":
        out = None
    else:
        out = open(args.out, "wb")

    probe = Probe(sock, out, args.throttle)

    def read_stdin():
        for line in sys.stdin:
            if line.strip() == "q":
                break
            probe.command(line)
        sock.close()

    threading.Thread(target=read_stdin, daemon=True).start()
    try:
        probe.run()
    except (EOFError, OSError) as e:
        print("終わり (%r)" % e)
    finally:
        print("audio %d KB in %.0f s" % (probe.audio // 1024, time.monotonic() - probe.t0))
        if player:
            player.stdin.close()
            player.wait()


if __name__ == "__main__":
    main()
