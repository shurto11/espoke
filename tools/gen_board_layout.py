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
    add(f'<text x="{OX - P * 1.1}" y="{hy(y) + 3.5}" font-size="10" text-anchor="end" '
        f'fill="{"#333" if bold else "#999"}" font-weight="{"bold" if bold else "normal"}">{y}</text>')

# 穴
for x in range(1, COLS + 1):
    for y in range(1, ROWS + 1):
        add(f'<circle cx="{hx(x)}" cy="{hy(y)}" r="3.2" fill="#c9a24a" stroke="#9c7a2c" stroke-width="0.8"/>'
            f'<circle cx="{hx(x)}" cy="{hy(y)}" r="1.3" fill="#5a4a2a"/>')

# 空きエリア
FX0, FX1, FY0, FY1 = 29, 36, 2, 26
fcx = (hx(FX0) + hx(FX1)) / 2
fx, fw = span(FX0, FX1)
add(f'<rect x="{fx - 8}" y="{hy(FY1) - 8}" width="{fw + 16}" height="{hy(FY0) - hy(FY1) + 16}" '
    'rx="8" fill="none" stroke="#8a7a55" stroke-width="1.5" stroke-dasharray="6 5"/>')
add(f'<rect x="{fx - 2}" y="{hy(15) - 18}" width="{fw + 4}" height="{2 * P + 14}" rx="4" fill="#e9dcc0"/>')
add(f'<text x="{fcx}" y="{hy(15)}" font-size="14" text-anchor="middle" fill="#6d5f3e">空きエリア</text>')
add(f'<text x="{fcx}" y="{hy(15) + 18}" font-size="11" text-anchor="middle" fill="#6d5f3e">（x=29〜36）</text>')
add(f'<text x="{fcx}" y="{hy(15) + 34}" font-size="10.5" text-anchor="middle" fill="#6d5f3e">レベル変換・LCD 用</text>')

# ---- Raspberry Pi Pico（USB を左、ソケット行 y=19 / y=26）----
# USB を上にしたときの左列 (1〜20) が下の行、右列 (21〜40) が上の行になる。
PY_B, PY_T = 19, 26
left = ["GP0", "GP1", "GND", "GP2", "GP3", "GP4", "GP5", "GND", "GP6", "GP7",
        "GP8", "GP9", "GND", "GP10", "GP11", "GP12", "GP13", "GND", "GP14", "GP15"]
right = {21: "GP16", 22: "GP17", 23: "GND", 24: "GP18", 25: "GP19", 26: "GP20", 27: "GP21",
         28: "GND", 29: "GP22", 30: "RUN", 31: "GP26", 32: "GP27", 33: "GND", 34: "GP28",
         35: "VREF", 36: "3V3", 37: "EN", 38: "GND", 39: "VSYS", 40: "VBUS"}  # 35=ADC_VREF, 37=3V3_EN
used = {14, 15, 16, 17, 18}

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
        'fill="#d5e3cf" stroke="#1f7a3a" stroke-width="2" stroke-dasharray="7 5"/>')
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

# ---- 配線（先に描いて、スイッチの足の下に潜らせる）----
GND_C = "#222222"
SW_X0 = [2, 9, 16, 23]   # 各スイッチの左の足の x（SW1 は (2,3)-(4,8) に設置済み）
SW_Y0, SW_Y1 = 3, 8
SW = [  # (名前, GPIO 名, ピン番号, 色, 経路)
    ("SW1", "GP10", 14, "#1f6fd1", [(4, 8), (4, 14), (15, 14), (15, 19)]),
    ("SW2", "GP11", 15, "#1a9b4b", [(11, 8), (11, 13), (16, 13), (16, 19)]),
    ("SW3", "GP12", 16, "#e07b00", [(18, 8), (18, 12), (17, 12), (17, 19)]),
    ("SW4", "GP13", 17, "#9b3fc4", [(25, 8), (25, 15), (18, 15), (18, 19)]),
]

def poly(pts, color, width=4.2):
    d = " ".join(f"{hx(x)},{hy(y)}" for x, y in pts)
    add(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
        f'stroke-linecap="round" stroke-linejoin="round" stroke-opacity="0.9"/>')

for *_, color, path in SW:
    poly(path, color)

poly([(2, 1), (27, 1), (27, 17), (19, 17), (19, 19)], GND_C)
for x0 in SW_X0:
    poly([(x0, SW_Y0), (x0, 1)], GND_C)

# ---- タクトスイッチ（12mm 角、足は 2×5 穴）----
BODY = 12 / 2.54 * P   # 本体 12mm
for (name, gp, n, color, _), x0 in zip(SW, SW_X0):
    x1 = x0 + 2
    cx = (hx(x0) + hx(x1)) / 2
    cy = (hy(SW_Y0) + hy(SW_Y1)) / 2
    if BACK:
        add(f'<rect x="{cx - BODY / 2}" y="{cy - BODY / 2}" width="{BODY}" height="{BODY}" rx="4" '
            'fill="#ddd3bd" stroke="#555" stroke-width="1.5" stroke-dasharray="6 4"/>')
        add(f'<text x="{cx}" y="{cy + 4}" font-size="12" text-anchor="middle" fill="#444" font-weight="bold">{name}</text>')
    else:
        add(f'<rect x="{cx - BODY / 2}" y="{cy - BODY / 2}" width="{BODY}" height="{BODY}" rx="4" '
            'fill="#3a3a3a" fill-opacity="0.82" stroke="#111" stroke-width="1.5"/>')
        add(f'<circle cx="{cx}" cy="{cy}" r="{BODY * 0.33}" fill="#777" stroke="#ccc" stroke-width="1.5"/>')
        add(f'<text x="{cx}" y="{cy + 4}" font-size="12" text-anchor="middle" fill="#fff" font-weight="bold">{name}</text>')
    legs = {(x0, SW_Y0): GND_C, (x1, SW_Y0): "#aaa", (x0, SW_Y1): "#aaa", (x1, SW_Y1): color}
    for (lx, ly), c in legs.items():
        add(f'<circle cx="{hx(lx)}" cy="{hy(ly)}" r="5.2" fill="{c}" stroke="#fff" stroke-width="1.5"/>')
    add(f'<text x="{hx(x1) + 8 * SIDE}" y="{hy(SW_Y1) - 8}" font-size="10.5" fill="{color}" font-weight="bold" '
        f'text-anchor="{"end" if BACK else "start"}">{gp}（{n}番）</text>')

add(f'<text x="{hx(3)}" y="{hy(SW_Y1) + 12}" font-size="9.5" text-anchor="middle" fill="{"#8a1c1c" if BACK else "#ffd0d0"}" '
    'font-weight="bold">設置済み</text>')

# ---- 凡例 ----
ly = hy(1) + 70
add(f'<text x="{OX - 20}" y="{ly}" font-size="14" font-weight="bold" fill="#222">配線</text>')
rows_ = [(c, f"{name}  足({path[0][0]},{path[0][1]}) → Pico {n}番ピン {gp}") for name, gp, n, c, path in SW]
rows_.append((GND_C, f"GND  各スイッチの足({'右' if BACK else '左'}下) → 行1 → x=27 → 行17 → x=19 → Pico 18番ピン GND"))
for i, (c, t) in enumerate(rows_):
    yy = ly + 22 + i * 21
    add(f'<line x1="{OX - 20}" y1="{yy - 4}" x2="{OX + 10}" y2="{yy - 4}" stroke="{c}" stroke-width="4.5" stroke-linecap="round"/>')
    add(f'<text x="{OX + 20}" y="{yy}" font-size="12.5" fill="#222">{t}</text>')

ly2 = ly + 22 + len(rows_) * 21 + 20
add(f'<text x="{OX - 20}" y="{ly2}" font-size="14" font-weight="bold" fill="#222">メモ</text>')
notes = [
    "・裏返すと左右が反対になる。基板の端に x=1 / x=36 と書いておくと迷わない",
    "・USB 側（Pico の 1番・40番ピン）は、この面では右端にくる",
    "・点線は反対側（表）にある部品の輪郭。はんだ付けするのは丸い足とソケットのピン",
] if BACK else [
    "・スイッチは対角の2本（左下と右上）だけを使う。向きを問わず「押したときだけ導通」になる",
    "・灰色の足は未使用。固定のためにはんだ付けだけする",
    "・GPIO は INPUT_PULLUP、押すと LOW（外付け抵抗は不要）",
    "・Pico のソケット2行は穴7個分（y=19 と y=26）離す。USB は基板の左端側",
    "・Pico 本体の下（y=20〜25）には配線を通さない",
    "・配線が交差する箇所はないので、被覆線でもスズメッキ線でも組める",
]
for i, t in enumerate(notes):
    add(f'<text x="{OX - 20}" y="{ly2 + 22 + i * 21}" font-size="12.5" fill="#333">{t}</text>')

add('</svg>')
sys.stdout.write("\n".join(out))
