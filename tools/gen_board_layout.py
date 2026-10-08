#!/usr/bin/env python3
"""ユニバーサル基板 (36x27 穴) の配置・配線図 SVG を生成する。

座標は (x=列, y=行) で、部品面（表）から見て左下の穴が (1,1)。
使い方:
    python3 tools/gen_board_layout.py > docs/board_layout.svg              # 表（部品面）
    python3 tools/gen_board_layout.py --back > docs/board_layout_back.svg  # 裏（はんだ面）
"""
import sys

BACK = "--back" in sys.argv[1:]   # 裏から見ると左右が反対になる
SIDE = -1 if BACK else 1

COLS, ROWS = 36, 27
P = 22            # 穴ピッチ (px)
OX, OY = 60, 90   # 左上の穴 (1,ROWS) の位置

def hx(x): return OX + ((COLS - x) if BACK else (x - 1)) * P
def hy(y): return OY + (ROWS - y) * P

out = []
def add(s): out.append(s)

def span(x0, x1):
    """x0〜x1 の穴を囲む矩形の左端と幅 (px)。"""
    a, b = sorted((hx(x0), hx(x1)))
    return a, b - a

W = OX + (COLS - 1) * P + 60
H = OY + (ROWS - 1) * P + 360

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    'font-family="Noto Sans CJK JP, Noto Sans JP, sans-serif">')
add(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
add(f'<text x="{OX - 20}" y="32" font-size="20" font-weight="bold" fill="#222">'
    'espoke ユニバーサル基板 配置図（36×27穴）</text>')
if BACK:
    add(f'<text x="{OX - 20}" y="54" font-size="13" fill="#b03030" font-weight="bold">'
        'はんだ面（裏）から見た図。表の図と左右が反対で、x=1 は右端。座標は表の図と同じ番号</text>')
else:
    add(f'<text x="{OX - 20}" y="54" font-size="13" fill="#555">'
        '部品面（表）から見た図。座標は (x=列, y=行)、左下が (1,1)。1マス = 2.54mm</text>')

# 基板
add(f'<rect x="{OX - P * 0.8}" y="{hy(ROWS) - P * 0.8}" width="{(COLS - 1) * P + P * 1.6}" '
    f'height="{(ROWS - 1) * P + P * 1.6}" rx="6" fill="#e9dcc0" stroke="#b39c6b" stroke-width="2"/>')

# 座標ラベル
for x in range(1, COLS + 1):
    bold = x % 5 == 0 or x == 1
    add(f'<text x="{hx(x)}" y="{hy(1) + P * 1.5}" font-size="10" text-anchor="middle" '
        f'fill="{"#333" if bold else "#999"}" font-weight="{"bold" if bold else "normal"}">{x}</text>')
for y in range(1, ROWS + 1):
    bold = y % 5 == 0 or y == 1
    add(f'<text x="{OX - P * 1.4}" y="{hy(y) + 3.5}" font-size="10" text-anchor="end" '
        f'fill="{"#333" if bold else "#999"}" font-weight="{"bold" if bold else "normal"}">{y}</text>')

# 穴
for x in range(1, COLS + 1):
    for y in range(1, ROWS + 1):
        add(f'<circle cx="{hx(x)}" cy="{hy(y)}" r="3.2" fill="#c9a24a" stroke="#9c7a2c" stroke-width="0.8"/>'
            f'<circle cx="{hx(x)}" cy="{hy(y)}" r="1.3" fill="#5a4a2a"/>')

# ---- 配線（先に描いて、部品の足の下に潜らせる）----
GND_C = "#222222"
V33_C = "#e8731a"   # 3.3V
V5_C = "#d42a2a"    # 5V
SDA_C = "#159a8c"
SCL_C = "#c9a400"
SD_C = "#5b5bd6"
SW_X0 = [2, 9, 16, 23]   # 各スイッチの左の足の x（4個とも配線まで設置済み）
SW_Y0, SW_Y1 = 3, 8
SW = [  # (名前, GPIO 名, ピン番号, 色, 経路)
    ("SW1", "GP10", 14, "#1f6fd1", [(4, 8), (4, 14), (15, 14), (15, 19)]),
    ("SW2", "GP11", 15, "#1a9b4b", [(11, 8), (11, 13), (16, 13), (16, 19)]),
    ("SW3", "GP12", 16, "#e07b00", [(18, 8), (18, 12), (17, 12), (17, 19)]),
    ("SW4", "GP13", 17, "#9b3fc4", [(25, 8), (25, 15), (18, 15), (18, 19)]),
]

def poly(pts, color, width=4.2, dash=None):
    d = " ".join(f"{hx(x)},{hy(y)}" for x, y in pts)
    da = f' stroke-dasharray="{dash}"' if dash else ""
    add(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
        f'stroke-linecap="round" stroke-linejoin="round" stroke-opacity="0.9"{da}/>')

def insulated(pts, color):
    """裏で設置済みの線の上を越える被覆線。白い縁取りで、ほかの線より上に描く。"""
    d = " ".join(f"{hx(x)},{hy(y)}" for x, y in pts)
    add(f'<polyline class="ins-case" points="{d}" fill="none" stroke="#fff" stroke-width="8.5" '
        'stroke-linecap="round" stroke-linejoin="round"/>')
    add(f'<polyline class="ins" points="{d}" fill="none" stroke="{color}" stroke-width="4" '
        'stroke-linecap="round" stroke-linejoin="round"/>')

for *_, color, path in SW:
    poly(path, color)

# GND: スイッチの足 → 行1 → x=27 → 行17 → Pico 18番ピン（設置済み）
poly([(2, 1), (27, 1), (27, 17), (19, 17), (19, 19)], GND_C)
for x0 in SW_X0:
    poly([(x0, SW_Y0), (x0, 1)], GND_C)

# ---- microSD（CK-40）: ピン列 y=20、x=28〜35。カードの差し込み口は基板の上端 ----
CK_X, CK_Y = 28, 20
CK_PINS = ["DAT2", "CS", "CMD", "VDD", "CLK", "VSS", "DAT0", "DAT1"]   # 1〜8番
def ckx(name): return CK_X + CK_PINS.index(name)

SD = [  # (信号, GPIO 名, ピン番号, 経路)
    ("CS",   "GP20", 26, [(16, 26), (16, 22), (ckx("CS"), 22), (ckx("CS"), CK_Y)]),
    ("CMD",  "GP19", 25, [(17, 26), (17, 23), (ckx("CMD"), 23), (ckx("CMD"), CK_Y)]),
    ("CLK",  "GP18", 24, [(18, 26), (18, 24), (ckx("CLK"), 24), (ckx("CLK"), CK_Y)]),
    ("DAT0", "GP16", 21, [(21, 26), (ckx("DAT0"), 26), (ckx("DAT0"), CK_Y)]),
]
# VSS: Pico 23番ピン GND → 行25 → x=33 → パスコンの下端 (33,16) まで
SD_GND = [(19, 26), (19, 25), (ckx("VSS"), 25), (ckx("VSS"), 16)]
for *_, path in SD:
    poly(path, SD_C, 3.4)
poly(SD_GND, GND_C, 3.4)

# プルアップ 10kΩ（足を y=18 と y=14 に）と 3.3V の横線（y=14）
PULL = ["DAT2", "CS", "CMD", "DAT0", "DAT1"]
R_TOP, R_BOT = 18, 14
for n in PULL:
    poly([(ckx(n), CK_Y), (ckx(n), R_TOP)], SD_C, 3.4)
poly([(CK_X, R_BOT), (ckx("DAT1"), R_BOT)], V33_C)
poly([(ckx("VDD"), CK_Y), (ckx("VDD"), R_BOT)], V33_C)

# ---- I2C 用レベルシフタ（SSCI-023962）: 左へ 90° 回して、上の行が 3.3V 側、下の行が 5V 側 ----
# Pico の 1番（SDA）・2番（SCL）ピンの真下に置き、LCD 用ピンは基板の左端に立てる
LS_X = [1, 2, 3]
LS_T, LS_B = 17, 13                     # 2 行は穴 4 個分離れる
LS_TOP = ["3V3", "SDA3V3", "SCL3V3"]    # 左から
LS_BOT = ["5V", "SDA5V", "SCL5V"]
HDR_X = 1
HDR = {12: "VCC", 11: "SDA", 10: "SCL", 9: "GND"}   # LCD 用ピン（x=1）
EDGE = 0.45   # 基板の左端（x=1 の穴の外側）

# 裏の配線: SDA・SCL は Pico のピンから真下へ。3.3V は Pico 36番ピン → 行21（Pico の下）→ x=1
poly([(2, 19), (2, LS_T)], SDA_C)
poly([(3, 19), (3, LS_T)], SCL_C)
poly([(6, 26), (6, 21), (1, 21), (1, LS_T)], V33_C)
# 5V 側 → LCD 用ピン、LCD の GND → x=1 → 行1 の GND の端 (2,1)
poly([(1, LS_B), (1, 12)], V5_C)
poly([(2, LS_B), (2, 11), (1, 11)], SDA_C)
poly([(3, LS_B), (3, 10), (1, 10)], SCL_C)
poly([(1, 9), (1, 1), (2, 1)], GND_C)
# 残りの 2 本は裏の被覆線にする。両端はピンや部品の足のランドに直接はんだ付けする。
# 5V: x=1 は 3.3V が使うので、基板の左端（穴の外側）を通す
# 3.3V（microSD 側）: 設置済みの GND の線の上を越える
INSULATED = [  # (名前, 色, 経路)
    ("5V", V5_C, [(2, 26), (EDGE, 26), (EDGE, LS_B), (1, LS_B)]),
    ("3V3", V33_C, [(6, 21), (26, 21), (26, R_BOT), (CK_X, R_BOT)]),
]
for _, color, pts in INSULATED:
    insulated(pts, color)

# ---- Raspberry Pi Pico（USB を左、ピン行 y=19 / y=26）----
# 下をくぐる配線より後に描いて、表の図では本体で隠す
# USB を上にしたときの左列 (1〜20) が下の行、右列 (21〜40) が上の行になる。
PY_B, PY_T = 19, 26
left = ["GP0", "GP1", "GND", "GP2", "GP3", "GP4", "GP5", "GND", "GP6", "GP7",
        "GP8", "GP9", "GND", "GP10", "GP11", "GP12", "GP13", "GND", "GP14", "GP15"]
right = {21: "GP16", 22: "GP17", 23: "GND", 24: "GP18", 25: "GP19", 26: "GP20", 27: "GP21",
         28: "GND", 29: "GP22", 30: "RUN", 31: "GP26", 32: "GP27", 33: "GND", 34: "GP28",
         35: "VREF", 36: "3V3", 37: "EN", 38: "GND", 39: "VSYS", 40: "VBUS"}  # 35=ADC_VREF, 37=3V3_EN
used = {1, 2, 14, 15, 16, 17, 18, 21, 23, 24, 25, 26, 36, 40}

def pin_x(n):
    return n + 1 if n <= 20 else 42 - n   # 1番 = (2,19)、40番 = (2,26)

bx, bw = span(pin_x(1), pin_x(20))
bx0, bx1 = bx - 0.63 * P, bx + bw + 0.63 * P
by0 = hy(PY_T) - 0.54 * P
by1 = hy(PY_B) + 0.54 * P
uy = (by0 + by1) / 2
ux = bx1 if BACK else bx0   # USB 側の端
if BACK:
    # 本体は反対側にあるので輪郭だけ描く
    add(f'<rect x="{bx0}" y="{by0}" width="{bx1 - bx0}" height="{by1 - by0}" rx="5" '
        'fill="#d5e3cf" fill-opacity="0.3" stroke="#1f7a3a" stroke-width="2" stroke-dasharray="7 5"/>')
    add(f'<rect x="{ux - 14}" y="{uy - 18}" width="26" height="36" rx="3" fill="none" stroke="#666" stroke-dasharray="4 3"/>')
    add(f'<text x="{ux - 1}" y="{uy + 4}" font-size="9" text-anchor="middle" fill="#444" font-weight="bold">USB</text>')
    add(f'<text x="{(bx0 + bx1) / 2}" y="{uy + 5}" font-size="14" text-anchor="middle" fill="#1f5a30" '
        'font-weight="bold">Pico WH（本体は反対側）</text>')
else:
    add(f'<rect x="{bx0}" y="{by0}" width="{bx1 - bx0}" height="{by1 - by0}" rx="5" '
        'fill="#1f7a3a" fill-opacity="0.88" stroke="#0f4d22" stroke-width="2"/>')
    add(f'<rect x="{ux - 12}" y="{uy - 18}" width="26" height="36" rx="3" fill="#c0c0c0" stroke="#666"/>')
    add(f'<text x="{ux + 1}" y="{uy + 4}" font-size="9" text-anchor="middle" fill="#222" font-weight="bold">USB</text>')
    add(f'<text x="{(bx0 + bx1) / 2}" y="{uy + 5}" font-size="14" text-anchor="middle" fill="#fff" '
        'font-weight="bold">Raspberry Pi Pico WH</text>')

def pin(n, name, y, below):
    x = hx(pin_x(n))
    u = n in used
    if BACK:
        col = "#8a1c1c" if u else "#3d6b4a"
    else:
        col = "#ffe680" if u else "#cfe9d6"
    fw = "bold" if u else "normal"
    add(f'<rect x="{x - 5}" y="{hy(y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
        f'<circle cx="{x}" cy="{hy(y)}" r="2.5" fill="#d9b44a"/>')
    # 下の行はピンの上に、上の行はピンの下に、番号と名前の2段でラベルを置く
    t1, t2 = (hy(y) - 22, hy(y) - 11) if below else (hy(y) + 17, hy(y) + 28)
    for ty, t in ((t1, n), (t2, name)) if below else ((t1, name), (t2, n)):
        add(f'<text x="{x}" y="{ty}" font-size="8" text-anchor="middle" fill="{col}" font-weight="{fw}">{t}</text>')

for i, name in enumerate(left):
    pin(i + 1, name, PY_B, True)
for n, name in right.items():
    pin(n, name, PY_T, False)


def leg(x, y, color, r=5.2):
    add(f'<circle cx="{hx(x)}" cy="{hy(y)}" r="{r}" fill="{color}" stroke="#fff" stroke-width="1.5"/>')

def label(x, y, t, color, size=9, anchor="middle", weight="bold", halo=False):
    if halo:   # 線の上に重なるときは、基板色の下地を敷く
        w = sum(size if ord(c) > 0x2000 else size * 0.62 for c in str(t)) + 4
        add(f'<rect x="{x - w / 2}" y="{y - size}" width="{w}" height="{size + 3}" rx="2" fill="#e9dcc0"/>')
    add(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" fill="{color}" font-weight="{weight}">{t}</text>')

# 抵抗・コンデンサー
for n in PULL:
    x = hx(ckx(n))
    add(f'<rect x="{x - 4.5}" y="{hy(R_TOP) + 14}" width="9" height="{hy(R_BOT) - hy(R_TOP) - 28}" rx="4" '
        'fill="#d8c7a0" stroke="#6b5a35" stroke-width="1.2"/>')
    for i, c in enumerate(("#8b4513", "#000", "#ff8c00")):   # 茶黒橙 = 10kΩ
        add(f'<rect x="{x - 4.5}" y="{hy(R_TOP) + 22 + i * 7}" width="9" height="3" fill="{c}"/>')
    leg(ckx(n), R_TOP, SD_C, 3.6)
    leg(ckx(n), R_BOT, V33_C, 3.6)
for y, name in ((18, "10μF"), (16, "0.1μF")):
    x0, x1 = sorted((hx(ckx("VDD")), hx(ckx("VSS"))))
    add(f'<rect x="{x0 + 6}" y="{hy(y) - 6}" width="{x1 - x0 - 12}" height="12" rx="6" '
        f'fill="{"#2b4fa0" if y == 18 else "#c98b2b"}" stroke="#222" stroke-width="1"/>')
    leg(ckx("VDD"), y, V33_C, 3.6)
    leg(ckx("VSS"), y, GND_C, 3.6)
    label((x0 + x1) / 2, hy(y) + 3.5, name, "#fff", 8)

# CK-40 本体（22.86×24.13mm。ピン列の 1.0 穴下〜8.2 穴上、1番ピンの 0.67 穴左〜8番ピンの 1.14 穴右）
cx0, cw = span(CK_X, CK_X + 7)
cl, cr = (cx0 - 1.14 * P, cx0 + cw + 0.67 * P) if BACK else (cx0 - 0.67 * P, cx0 + cw + 1.14 * P)
ct, cb = hy(CK_Y) - 8.2 * P, hy(CK_Y) + 1.0 * P
if BACK:
    add(f'<rect x="{cl}" y="{ct}" width="{cr - cl}" height="{cb - ct}" rx="3" fill="none" stroke="#1f6a3a" '
        'stroke-width="2" stroke-dasharray="7 5"/>')
else:
    add(f'<rect x="{cl}" y="{ct}" width="{cr - cl}" height="{cb - ct}" rx="3" fill="#2f8a4a" fill-opacity="0.35" '
        'stroke="#1f6a3a" stroke-width="2"/>')
    sx0, sx1 = hx(CK_X + 1) - 4, hx(CK_X + 6) + 4   # スロット（金属ケース）
    add(f'<rect x="{sx0}" y="{ct - 4}" width="{sx1 - sx0}" height="{hy(CK_Y + 2) - ct + 4}" rx="2" '
        'fill="#b8b8b8" fill-opacity="0.75" stroke="#777" stroke-width="1.2"/>')
label((cl + cr) / 2, hy(CK_Y + 6) + 4, "microSD", "#123d22", 13, halo=BACK)
label((cl + cr) / 2, hy(CK_Y + 5) + 2, "CK-40", "#123d22", 11, halo=BACK)
label((cl + cr) / 2, hy(CK_Y + 3) + 10, "（本体は反対側）" if BACK else "↑ カードを差す", "#333", 9.5, weight="normal", halo=BACK)
for i, n in enumerate(CK_PINS):
    x = hx(CK_X + i)
    add(f'<rect x="{x - 5}" y="{hy(CK_Y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
        f'<circle cx="{x}" cy="{hy(CK_Y)}" r="2.5" fill="#d9b44a"/>')
    label(x, hy(CK_Y) - 22, i + 1, "#123d22", 8)
    label(x, hy(CK_Y) - 11, n, "#123d22", 8)
for y in range(CK_Y + 3, CK_Y + 7):   # FG・カード検出の穴（ピンは付けない）
    for x in (CK_X, CK_X + 7):
        add(f'<circle cx="{hx(x)}" cy="{hy(y)}" r="4.2" fill="none" stroke="#555" stroke-width="1.2" stroke-dasharray="2 2"/>')

# レベルシフタ本体（12.5×7.5mm）。90° 回すので縦長になる
lx0, lw = span(LS_X[0], LS_X[-1])
lt, lb = hy(LS_T) - 0.46 * P, hy(LS_B) + 0.46 * P
add(f'<rect x="{lx0 - 0.48 * P}" y="{lt}" width="{lw + 0.96 * P}" height="{lb - lt}" rx="3" '
    + ('fill="none" stroke="#1d3f8a" stroke-width="2" stroke-dasharray="7 5"/>' if BACK else
       'fill="#2456c4" fill-opacity="0.35" stroke="#1d3f8a" stroke-width="2"/>'))
for y, names, down in ((LS_T, LS_TOP, True), (LS_B, LS_BOT, False)):
    for lx, n in zip(LS_X, names):
        x = hx(lx)
        add(f'<rect x="{x - 5}" y="{hy(y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
            f'<circle cx="{x}" cy="{hy(y)}" r="2.5" fill="#d9b44a"/>')
        # ピン名は本体の内側に縦書き（上の行は下向き、下の行は上向き）
        tx, ty, rot = (x - 2.5, hy(y) + 9, 90) if down else (x + 2.5, hy(y) - 9, -90)
        add(f'<text transform="translate({tx},{ty}) rotate({rot})" font-size="7" fill="{"#1d3f8a" if BACK else "#fff"}" '
            f'font-weight="bold">{n}</text>')
label(hx(LS_X[-1]) + SIDE * 17, hy(15.5) + 4, "← レベルシフタ" if SIDE > 0 else "レベルシフタ →",
      "#1d3f8a", 9.5, "start" if SIDE > 0 else "end")

# LCD 用ピン（1×4）。ピン名は基板の外側に書く
for y, n in HDR.items():
    x = hx(HDR_X)
    add(f'<rect x="{x - 5}" y="{hy(y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
        f'<circle cx="{x}" cy="{hy(y)}" r="2.5" fill="#d9b44a"/>')
    label(x - 9 * SIDE, hy(y) + 3, n, "#222", 7.5, "end" if SIDE > 0 else "start")
label(hx(4) + SIDE * 10, hy(10.5) + 4, "← LCD 用ピン" if SIDE > 0 else "LCD 用ピン →",
      "#222", 9.5, "start" if SIDE > 0 else "end")

# ---- タクトスイッチ（12mm 角、足は 2×5 穴）----
BODY = 12 / 2.54 * P   # 本体 12mm
for (name, gp, n, color, _), x0 in zip(SW, SW_X0):
    x1 = x0 + 2
    cx = (hx(x0) + hx(x1)) / 2
    cy = (hy(SW_Y0) + hy(SW_Y1)) / 2
    if BACK:
        add(f'<rect x="{cx - BODY / 2}" y="{cy - BODY / 2}" width="{BODY}" height="{BODY}" rx="4" '
            'fill="#ddd3bd" fill-opacity="0.45" stroke="#555" stroke-width="1.5" stroke-dasharray="6 4"/>')
        add(f'<text x="{cx}" y="{cy + 4}" font-size="12" text-anchor="middle" fill="#444" font-weight="bold">{name}</text>')
    else:
        add(f'<rect x="{cx - BODY / 2}" y="{cy - BODY / 2}" width="{BODY}" height="{BODY}" rx="4" '
            'fill="#3a3a3a" fill-opacity="0.82" stroke="#111" stroke-width="1.5"/>')
        add(f'<circle cx="{cx}" cy="{cy}" r="{BODY * 0.33}" fill="#777" stroke="#ccc" stroke-width="1.5"/>')
        add(f'<text x="{cx}" y="{cy + 4}" font-size="12" text-anchor="middle" fill="#fff" font-weight="bold">{name}</text>')
    legs = {(x0, SW_Y0): GND_C, (x1, SW_Y0): "#aaa", (x0, SW_Y1): "#aaa", (x1, SW_Y1): color}
    for (lx, ly), c in legs.items():
        leg(lx, ly, c)
    add(f'<text x="{hx(x1) + 8 * SIDE}" y="{hy(SW_Y1) - 8}" font-size="10.5" fill="{color}" font-weight="bold" '
        f'text-anchor="{"end" if BACK else "start"}">{gp}（{n}番）</text>')

for x0 in SW_X0:
    add(f'<text x="{(hx(x0) + hx(x0 + 2)) / 2}" y="{hy(SW_Y1) + 12}" font-size="9.5" text-anchor="middle" '
        f'fill="{"#8a1c1c" if BACK else "#ffd0d0"}" font-weight="bold">設置済み</text>')

# ---- 凡例 ----
ly = hy(1) + 70
add(f'<text x="{OX - 20}" y="{ly}" font-size="14" font-weight="bold" fill="#222">配線</text>')
rows_ = [(c, f"{name}  足({path[0][0]},{path[0][1]}) → Pico {n}番ピン {gp}") for name, gp, n, c, path in SW]
rows_ += [
    (GND_C, "GND  各スイッチの足 → 行1 → x=27 → 行17 → Pico 18番ピン GND（ここまで設置済み）"),
    (SCL_C, "SCL  Pico 2番ピン GP1 → 真下のレベルシフタ SCL3V3 ／ SCL5V → x=3 → 行10 → LCD 用ピン SCL"),
    (SDA_C, "SDA  Pico 1番ピン GP0 → 真下のレベルシフタ SDA3V3 ／ SDA5V → x=2 → 行11 → LCD 用ピン SDA"),
    (V33_C, "3.3V  Pico 36番ピン 3V3 → 行21（Pico の下）→ x=1 → レベルシフタ 3V3"),
    (V33_C, "3.3V  (6,21) → 行21 → x=26 → 行14（被覆線）→ microSD の VDD・プルアップ"),
    (V5_C, "5V  Pico 40番ピン VBUS → x=1 の外側（基板の端、被覆線）→ レベルシフタ 5V → LCD 用ピン VCC"),
    (GND_C, "LCD の GND  LCD 用ピン GND → x=1 → 設置済みの GND 線の端 (2,1)"),
    (SD_C, "microSD  Pico 26番 GP20→CS、25番 GP19→CMD、24番 GP18→CLK、21番 GP16→DAT0（Pico の下の行22〜24・26）"),
    (GND_C, "microSD VSS  Pico 23番ピン GND → 行25（Pico の下）→ x=33 → VSS。パスコンの GND 側もこの線"),
    (SD_C, "microSD  DAT2・CS・CMD・DAT0・DAT1 は 10kΩ で行14（3.3V）へ。VDD–VSS 間に 10μF と 0.1μF"),
]
for i, (c, t) in enumerate(rows_):
    yy = ly + 22 + i * 21
    add(f'<line x1="{OX - 20}" y1="{yy - 4}" x2="{OX + 10}" y2="{yy - 4}" stroke="{c}" stroke-width="4.5" stroke-linecap="round"/>')
    add(f'<text x="{OX + 20}" y="{yy}" font-size="12.5" fill="#222">{t}</text>')

ly2 = ly + 22 + len(rows_) * 21 + 20
add(f'<text x="{OX - 20}" y="{ly2}" font-size="14" font-weight="bold" fill="#222">メモ</text>')
notes = [
    "・裏返すと左右が反対になる。基板の端に x=1 / x=36 と書いておくと迷わない",
    "・USB 側（Pico の 1番・40番ピン）は、この面では右端にくる",
    "・点線の四角は反対側（表）にある部品の輪郭。はんだ付けするのは丸い足とピン",
    "・白い縁取りの 2 本は被覆線。3.3V は設置済みの GND の線の上を越え、5V は基板の端（x=1 の外側）を通る",
    "・ほかの線どうしの交差はないので、スズメッキ線でよい",
] if BACK else [
    "・スイッチ4個・Pico・その配線（GND を含む）は設置済み。この図ではそこを変えない",
    "・スイッチは対角の2本（左下と右上）だけを使う。GPIO は INPUT_PULLUP、押すと LOW",
    "・実線は裏の配線。Pico の下（y=20〜25）の配線も裏に通す",
    "・白い縁取りの 2 本は裏の被覆線（3.3V は設置済みの GND の線を越え、5V は x=1 の外側を通る）。ほかはスズメッキ線でよい",
    "・被覆線は最後にはんだ付けする。そのあと近くのランドを触るときは、こてを被覆に当てない（被覆が溶ける）",
    "・レベルシフタ（SSCI-023962）は部品面を上にして左へ 90° 回す。3.3V 側が上（行17）、5V 側が下（行13）",
    "・向きの確認: レベルシフタの 3V3 と 5V のピンが、どちらも左端（x=1）にくる",
    "・レベルシフタには GND ピンがない。LCD の GND は x=1 を下ろして、行1 の設置済み GND 線の端 (2,1) につなぐ",
    "・LCD 用ピンは上から VCC・SDA・SCL・GND。バックパックの並び（GND・VCC・SDA・SCL）と違うので、名前どうしをつなぐ",
    "・CK-40 は 1〜8番ピン（1×8）だけピンヘッダを付ける。FG・CD の穴の下は配線が通るので、ピンを付けない",
    "・CK-40 の差し込み口は基板の上端から少しはみ出す（カードを押し込みやすい）",
]
for i, t in enumerate(notes):
    add(f'<text x="{OX - 20}" y="{ly2 + 22 + i * 21}" font-size="12.5" fill="#333">{t}</text>')

H = ly2 + 22 + len(notes) * 21 + 10
out[0] = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
          'font-family="Noto Sans CJK JP, Noto Sans JP, sans-serif">')
out[1] = f'<rect width="{W}" height="{H}" fill="#ffffff"/>'
add('</svg>')
sys.stdout.write("\n".join(out))
