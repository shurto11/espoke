#!/usr/bin/env python3
"""
espoke 用の音声を HTTP で配る簡易サーバ。

Pico WH 側は WAV を素のまま読むので、ここで用意するのは
16bit PCM / 44100・22050・11025 Hz / モノラルかステレオ の WAV に限る。

使い方:
    python3 tools/serve_audio.py                # notify.wav を作って :8000 で配る
    python3 tools/serve_audio.py --port 9000
    python3 tools/serve_audio.py --dir ~/sounds # 既存の WAV があるディレクトリを配る
"""

import argparse
import math
import os
import socket
import struct
import wave
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

RATE = 22050  # Pico 側で 2倍に引き伸ばして 44100 にする


def make_notify_wav(path):
    """ポケベルらしい "ピピッ" を 16bit モノラルで書き出す"""
    beeps = [(0.0, 0.12), (0.20, 0.32)]  # (開始秒, 終了秒)
    total = 0.8
    frames = bytearray()
    for i in range(int(RATE * total)):
        t = i / RATE
        env = 0.0
        for a, b in beeps:
            if a <= t < b:
                # 端を 10ms かけて出し入れし、"プツッ" というクリック音を防ぐ
                env = max(env, min(1.0, (t - a) / 0.01, (b - t) / 0.01))
        frames += struct.pack("<h", int(math.sin(2 * math.pi * 880 * t) * 9000 * env))

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def lan_ip():
    """デフォルトルート側の IP を調べる (実際には送信しない)"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--dir", default=None, help="配信するディレクトリ (既定: tools/audio)")
    args = ap.parse_args()

    root = args.dir or os.path.join(os.path.dirname(os.path.abspath(__file__)), "audio")
    os.makedirs(root, exist_ok=True)

    notify = os.path.join(root, "notify.wav")
    if not os.path.exists(notify):
        print(f"generating {notify} ...")
        make_notify_wav(notify)

    ip = lan_ip()
    print(f"serving {root} on http://{ip}:{args.port}/")
    for name in sorted(os.listdir(root)):
        if name.lower().endswith(".wav"):
            print(f"  http://{ip}:{args.port}/{name}")
    print('この URL を espoke/arduino_secrets.h の AUDIO_URL に書く。Ctrl-C で停止。')

    handler = partial(SimpleHTTPRequestHandler, directory=root)
    ThreadingHTTPServer(("0.0.0.0", args.port), handler).serve_forever()


if __name__ == "__main__":
    main()
