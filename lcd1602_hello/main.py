"""
LCD1602A (I2Cバックパック PCF8574 付き) に文字を表示する

対応ボード: Raspberry Pi Pico WH / Pico W (RP2040)
必要なファイル: lcd1602.py (同じ階層に置く)

動作:
  1. 起動時に I2C バスをスキャンし、見つかったアドレスで LCD を初期化
  2. 1行目に固定メッセージ、2行目に起動からの経過秒数を表示
  3. USB シリアル (REPL) から1行送ると、その文字列を1行目に表示
"""

import select
import sys
from machine import I2C, Pin
from time import ticks_diff, ticks_ms

from lcd1602 import LCD1602

# --- 設定 ---
I2C_ID = 0     # I2C0。GP0/GP1 は I2C0 のピン
PIN_SDA = 0    # GP0 (物理1番ピン)
PIN_SCL = 1    # GP1 (物理2番ピン)
LCD_COLS = 16
LCD_ROWS = 2
LCD_ADDR_DEFAULT = 0x27  # スキャンで見つからなかった場合に使う
# ------------


def scan_i2c(i2c):
    """I2C バスをスキャンし、最初に応答したアドレスを返す (見つからなければ 0)"""
    print("I2C scan...")
    found = i2c.scan()
    for addr in found:
        print("  found: 0x%02X" % addr)
    if not found:
        print("  no device found")
    return found[0] if found else 0


def print_line(lcd, row, text):
    """指定行を16文字ぶん上書きする (前の文字が残らないよう空白で埋める)"""
    s = text[:LCD_COLS]
    lcd.move_to(0, row)
    lcd.putstr(s + " " * (LCD_COLS - len(s)))


def main():
    i2c = I2C(I2C_ID, sda=Pin(PIN_SDA), scl=Pin(PIN_SCL), freq=100000)

    addr = scan_i2c(i2c) or LCD_ADDR_DEFAULT
    print("LCD address: 0x%02X" % addr)

    lcd = LCD1602(i2c, addr, LCD_COLS, LCD_ROWS)
    lcd.backlight(True)

    line1 = "Hello, Pico WH!"
    print_line(lcd, 0, line1)
    print_line(lcd, 1, "")
    print("Type text and press Enter to show it on the LCD. (Ctrl-C to stop)")

    # USB シリアルからの入力をブロックせずに読むための poll
    poller = select.poll()
    poller.register(sys.stdin, select.POLLIN)
    buf = ""
    started = ticks_ms()
    last_update = started

    while True:
        # シリアルから受け取った文字列を1行目に表示
        while poller.poll(0):
            ch = sys.stdin.read(1)
            if ch in ("\n", "\r"):
                text = buf.strip()
                buf = ""
                if text:
                    line1 = text
                    print_line(lcd, 0, line1)
                    print('LCD <- "%s"' % line1)
            else:
                buf += ch

        # 2行目に経過秒数を1秒ごとに表示
        now = ticks_ms()
        if ticks_diff(now, last_update) >= 1000:
            last_update = now
            print_line(lcd, 1, "uptime %ds" % (ticks_diff(now, started) // 1000))


try:
    main()
except KeyboardInterrupt:
    print("stopped")
