"""
HD44780 互換 LCD (PCF8574 I2C バックパック経由) の最小ドライバ

PCF8574 のビット割り当ては、LCD1602A の裏に載っている一般的なバックパックの配線。
  P0 = RS / P1 = RW / P2 = E / P3 = バックライト / P4-P7 = D4-D7 (4bit モード)
"""

from time import sleep_ms, sleep_us

_RS = 0x01  # 0 = コマンド, 1 = データ
_EN = 0x04  # イネーブル
_BL = 0x08  # バックライト

_ROW_ADDR = (0x00, 0x40, 0x14, 0x54)  # 各行の DDRAM 先頭アドレス


class LCD1602:
    def __init__(self, i2c, addr=0x27, cols=16, rows=2):
        self.i2c = i2c
        self.addr = addr
        self.cols = cols
        self.rows = rows
        self._bl = _BL

        sleep_ms(50)  # 電源投入後の待ち
        # 8bit モードで起動している可能性があるので、規定の手順で 4bit モードへ落とす
        for _ in range(3):
            self._pulse(0x30)
            sleep_ms(5)
        self._pulse(0x20)

        self.command(0x28)  # Function set: 4bit / 2行 / 5x8 ドット
        self.command(0x08)  # Display off
        self.clear()
        self.command(0x06)  # Entry mode: カーソルは右へ進む
        self.command(0x0C)  # Display on / カーソル・ブリンクなし

    # --- 低レベル ---

    def _pulse(self, byte):
        """上位4ビットを D4-D7 に載せ、E を立ち下げて LCD に取り込ませる"""
        b = byte | self._bl
        self.i2c.writeto(self.addr, bytes([b | _EN]))
        sleep_us(1)
        self.i2c.writeto(self.addr, bytes([b]))
        sleep_us(50)

    def _send(self, value, mode):
        self._pulse((value & 0xF0) | mode)
        self._pulse(((value << 4) & 0xF0) | mode)

    def command(self, cmd):
        self._send(cmd, 0)

    # --- 表示 ---

    def clear(self):
        self.command(0x01)
        sleep_ms(2)  # clear は実行に時間がかかる

    def move_to(self, col, row):
        self.command(0x80 | (_ROW_ADDR[row] + col))

    def putstr(self, text):
        for ch in text:
            self._send(ord(ch), _RS)

    def backlight(self, on=True):
        self._bl = _BL if on else 0x00
        self.i2c.writeto(self.addr, bytes([self._bl]))
