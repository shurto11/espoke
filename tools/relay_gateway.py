#!/usr/bin/env python3
"""
espoke の中継 (discord_relay.py) の予備の入口。中継とは別の PC (ubuntubook) の上で動かす。

Tailscale の Funnel の入口が、中継の PC (dynabook) へだけつながなくなることがある
(TLS の握手の途中ですぐ切られる。中継の PC を tailnet から見ると何も届いていない)。
そうなっても、別の PC の Funnel は通る。そこで、この PC でも同じ 2 つのポートを公開し、
届いたものを tailnet の中を通して中継へ渡す。Pico は RELAY_HOST につなげなければ RELAY_HOST2 (この PC) を試す。

    Funnel 8443 (TLS は tailscaled) ─> 127.0.0.1:8001 ─┐
    Funnel 10000 (TCP のまま)       ─> 127.0.0.1:8010 ─┴─ TLS ─> <中継の PC>:10000 (tailnet の中) ─> 中継
                                       この PC の証明書で TLS を受ける

中継の 10000 番 (TLS を中継が受ける口) は、8443 番と同じ処理 (Discord の /stream、/spotify、/youtube) を
受けるので、どちらも 10000 番へ渡す。10000 番へ渡すときに、ここで TLS を受け直すのは、Pico が証明書の名前を確かめるため
(中継の証明書は中継の PC の名前)。Pico へ書くときは CHUNK バイトずつにして、TLS のレコードを Pico の
受信バッファ (4KB) に収める。

準備 (一度だけ):
    sudo tailscale set --operator=$USER      # tailscale cert と funnel を sudo なしで
    mkdir -p ~/espoke && cp tools/relay_gateway.py ~/espoke/
    tailscale funnel --bg --https=8443 8001
    tailscale funnel --bg --tcp=10000 tcp://127.0.0.1:8010
    # systemd のユーザーの unit にして常に動かす (ログアウトしても動かすなら loginctl enable-linger)
    python3 ~/espoke/relay_gateway.py --upstream dynabook.<tailnet>.ts.net:10000

証明書は tailscale cert で ~/espoke/tls.crt と tls.key に取り出す (起動時と1日ごと)。
"""

import argparse
import json
import os
import select
import socket
import ssl
import subprocess
import sys
import threading
import time

CHUNK = 2048            # 一度に読んで渡す上限。Pico へ書く TLS のレコードはこれ以下になる
IDLE = 120              # どちらからも何も来なければ切る (秒)。中継は 30 秒ごとに空行、Pico は 30 秒ごとに C を送る
CONNECT_TIMEOUT = 15    # 中継につなぐ・TLS の握手の上限 (Pico との握手は 5〜9 秒かかる)
WRITE_TIMEOUT = 60      # 書けないまま待つ上限
CERT_CHECK = 24 * 3600  # tailscale cert で証明書を取り直す間隔 (秒)。期限が近ければ tailscaled が更新する


def own_host():
    """tailscale funnel で公開される、この PC の名前。分からなければ None"""
    try:
        out = subprocess.run(["tailscale", "status", "--json"], capture_output=True,
                             text=True, timeout=5).stdout
        return json.loads(out)["Self"]["DNSName"].rstrip(".") or None
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired):
        return None


def get_cert(host, cert, key):
    try:
        r = subprocess.run(["tailscale", "cert", "--cert-file", cert, "--key-file", key, host],
                           capture_output=True, text=True, timeout=300)
        if r.returncode:
            sys.stderr.write("tailscale cert: %s\n" % (r.stderr.strip() or r.stdout.strip()))
    except (OSError, subprocess.TimeoutExpired) as e:
        sys.stderr.write("tailscale cert: %r\n" % e)


def keep_cert(host, cert, key):
    while True:
        time.sleep(CERT_CHECK)
        get_cert(host, cert, key)


class Gateway:
    def __init__(self, upstream, cert, key):
        host, _, port = upstream.rpartition(":")
        self.up_host, self.up_port = host, int(port)
        self.up_ctx = ssl.create_default_context()  # 中継の証明書 (Let's Encrypt) を名前まで確かめる
        self.cert, self.key = cert, key
        self._ctx, self._mtime = None, 0
        self.lock = threading.Lock()

    def server_context(self):
        """Pico との TLS。証明書のファイルが新しくなったら、次の接続から読み直す"""
        mtime = os.stat(self.cert).st_mtime
        with self.lock:
            if self._ctx is None or mtime != self._mtime:
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ctx.load_cert_chain(self.cert, self.key)
                self._ctx, self._mtime = ctx, mtime
            return self._ctx

    def serve(self, port, tls):
        ls = socket.create_server(("127.0.0.1", port))
        while True:
            conn, _ = ls.accept()
            threading.Thread(target=self.handle, args=(conn, tls), daemon=True).start()

    def handle(self, down, tls):
        name = "tls" if tls else "http"
        t0 = time.monotonic()
        up = None
        counts = [0, 0]  # Pico へ渡したバイト数、中継へ渡したバイト数
        why = "closed"
        try:
            down.settimeout(CONNECT_TIMEOUT)
            if tls:
                down = self.server_context().wrap_socket(down, server_side=True)
            raw = socket.create_connection((self.up_host, self.up_port), timeout=CONNECT_TIMEOUT)
            up = self.up_ctx.wrap_socket(raw, server_hostname=self.up_host)
            why = pump(down, up, counts)
        except (OSError, ValueError) as e:
            why = repr(e)
        finally:
            for s in (down, up):
                if s:
                    try:
                        s.close()
                    except OSError:
                        pass
        sys.stderr.write("%s: %d s, to pico %d KB, to relay %d KB (%s)\n"
                         % (name, time.monotonic() - t0, counts[0] // 1024, counts[1] // 1024, why))


def pump(down, up, counts):
    """両方向に渡す。TLS (ssl.SSLSocket) は別のスレッドから同時に読み書きすると壊れるので、1 つのスレッドで
    届いている方から読んではもう一方へ書く。読むのは待たずに (レコードの途中までなら、また select で待つ)"""
    socks = (down, up)
    for s in socks:
        s.settimeout(WRITE_TIMEOUT)
    while True:
        ready = [s for s in socks if isinstance(s, ssl.SSLSocket) and s.pending()]
        if not ready:
            ready = select.select(socks, [], [], IDLE)[0]
            if not ready:
                return "idle"
        for s in ready:
            s.settimeout(0)
            try:
                data = s.recv(CHUNK)
            except (ssl.SSLWantReadError, BlockingIOError):
                continue
            finally:
                s.settimeout(WRITE_TIMEOUT)
            if not data:
                return "pico closed" if s is down else "relay closed"
            other = up if s is down else down
            other.sendall(data)
            counts[0 if s is up else 1] += len(data)


def main():
    ap = argparse.ArgumentParser(description="espoke の中継の予備の入口 (Funnel から tailnet の中の中継へ渡す)")
    ap.add_argument("--upstream", required=True, help="中継の TLS の口 (例 dynabook.<tailnet>.ts.net:10000)")
    ap.add_argument("--http-port", type=int, default=8001, help="Funnel の 8443 番の転送先 (既定 8001)")
    ap.add_argument("--tls-port", type=int, default=8010, help="Funnel の 10000 番の転送先 (既定 8010)")
    ap.add_argument("--dir", default=os.path.expanduser("~/espoke"), help="証明書を置く場所 (既定 ~/espoke)")
    args = ap.parse_args()

    host = own_host()
    if not host:
        sys.exit("tailscale status から自分の名前が分からない")
    cert, key = os.path.join(args.dir, "tls.crt"), os.path.join(args.dir, "tls.key")
    get_cert(host, cert, key)
    threading.Thread(target=keep_cert, args=(host, cert, key), daemon=True).start()

    gw = Gateway(args.upstream, cert, key)
    threading.Thread(target=gw.serve, args=(args.http_port, False), daemon=True).start()
    print("%s の予備の入口: 127.0.0.1:%d (Funnel 8443) と 127.0.0.1:%d (Funnel 10000, TLS) を %s へ"
          % (host, args.http_port, args.tls_port, args.upstream), flush=True)
    print("公開していなければ:  tailscale funnel --bg --https=8443 %d" % args.http_port)
    print("                     tailscale funnel --bg --tcp=10000 tcp://127.0.0.1:%d" % args.tls_port, flush=True)
    gw.serve(args.tls_port, True)


if __name__ == "__main__":
    main()
