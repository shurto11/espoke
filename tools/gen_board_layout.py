#!/usr/bin/env python3
"""ユニバーサル基板 (36x27 穴) の配置・配線図 SVG を生成する。"""
import sys

COLS, ROWS = 36, 27
P = 22            # 穴ピッチ (px)
OX, OY = 60, 90   # 穴 (1,1) の位置

def hx(x): return OX + (x - 1) * P
def hy(y): return OY + (y - 1) * P

out = []
def add(s): out.append(s)

W = OX + (COLS - 1) * P + 60
H = OY + (ROWS - 1) * P + 350

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    'font-family="Noto Sans CJK JP, Noto Sans JP, sans-serif">')
add(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
add(f'<text x="{OX - 20}" y="32" font-size="20" font-weight="bold" fill="#222">'
    'espoke ユニバーサル基板 配置図（36×27穴）</text>')
add(f'<text x="{OX - 20}" y="54" font-size="13" fill="#555">'
    '部品面（表）から見た図。座標は (x=列, y=行)、左上が (1,1)。1マス = 2.54mm</text>')

# 基板
add(f'<rect x="{hx(1) - P * 0.8}" y="{hy(1) - P * 0.8}" width="{(COLS - 1) * P + P * 1.6}" '
    f'height="{(ROWS - 1) * P + P * 1.6}" rx="6" fill="#e9dcc0" stroke="#b39c6b" stroke-width="2"/>')

# 座標ラベル
for x in range(1, COLS + 1):
    bold = x % 5 == 0 or x == 1
    add(f'<text x="{hx(x)}" y="{OY - P * 1.1}" font-size="10" text-anchor="middle" '
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
add(f'<rect x="{hx(21)}" y="{hy(12) - 10}" width="{hx(34) - hx(21)}" height="{2 * P + 20}" rx="4" fill="#e9dcc0"/>')
add(f'<rect x="{hx(19) - 8}" y="{hy(2) - 8}" width="{(36 - 19) * P + 16}" height="{(25 - 2) * P + 16}" '
    'rx="8" fill="none" stroke="#8a7a55" stroke-width="1.5" stroke-dasharray="6 5"/>')
add(f'<text x="{(hx(19) + hx(36)) / 2}" y="{hy(13)}" font-size="15" text-anchor="middle" fill="#6d5f3e">'
    '空きエリア（x=19〜36）</text>')
add(f'<text x="{(hx(19) + hx(36)) / 2}" y="{hy(14)}" font-size="12" text-anchor="middle" fill="#6d5f3e">'
    'レベル変換・LCD 用ピンなどを置ける</text>')

# ---- Raspberry Pi Pico（USB を上、ソケット列 x=9 / x=16、1番ピン = (9,2)）----
PX_L, PX_R, PY0 = 9, 16, 2
left = ["GP0", "GP1", "GND", "GP2", "GP3", "GP4", "GP5", "GND", "GP6", "GP7",
        "GP8", "GP9", "GND", "GP10", "GP11", "GP12", "GP13", "GND", "GP14", "GP15"]
right = {21: "GP16", 22: "GP17", 23: "GND", 24: "GP18", 25: "GP19", 26: "GP20", 27: "GP21",
         28: "GND", 29: "GP22", 30: "RUN", 31: "GP26", 32: "GP27", 33: "GND", 34: "GP28",
         35: "ADC_VREF", 36: "3V3", 37: "3V3_EN", 38: "GND", 39: "VSYS", 40: "VBUS"}
used = {16, 17, 18, 19, 20, 23}

bx0 = hx(PX_L) - 0.63 * P
bx1 = hx(PX_R) + 0.63 * P
by0 = hy(PY0) - 0.54 * P
by1 = hy(PY0 + 19) + 0.54 * P
add(f'<rect x="{bx0}" y="{by0}" width="{bx1 - bx0}" height="{by1 - by0}" rx="5" '
    'fill="#1f7a3a" fill-opacity="0.88" stroke="#0f4d22" stroke-width="2"/>')
ux = (bx0 + bx1) / 2
add(f'<rect x="{ux - 18}" y="{by0 - 10}" width="36" height="24" rx="3" fill="#c0c0c0" stroke="#666"/>')
add(f'<text x="{ux}" y="{by0 + 5}" font-size="10" text-anchor="middle" fill="#222" font-weight="bold">USB</text>')
add(f'<text x="{ux}" y="{hy(11.5) + 4}" font-size="12" text-anchor="middle" fill="#fff" font-weight="bold">Pico WH</text>')

def pin_label(name, n, used_):
    col = "#ffe680" if used_ else "#cfe9d6"
    return col, ("bold" if used_ else "normal")

for i, name in enumerate(left):
    n = i + 1
    y = PY0 + i
    u = n in used
    col, fw = pin_label(name, n, u)
    add(f'<rect x="{hx(PX_L) - 5}" y="{hy(y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
        f'<circle cx="{hx(PX_L)}" cy="{hy(y)}" r="2.5" fill="#d9b44a"/>')
    add(f'<text x="{hx(PX_L) + 9}" y="{hy(y) + 3.5}" font-size="9.5" fill="{col}" font-weight="{fw}">{n} {name}</text>')
for n, name in right.items():
    y = PY0 + (40 - n)
    u = n in used
    col, fw = pin_label(name, n, u)
    add(f'<rect x="{hx(PX_R) - 5}" y="{hy(y) - 5}" width="10" height="10" fill="#222" stroke="#000"/>'
        f'<circle cx="{hx(PX_R)}" cy="{hy(y)}" r="2.5" fill="#d9b44a"/>')
    add(f'<text x="{hx(PX_R) - 9}" y="{hy(y) + 3.5}" font-size="9.5" text-anchor="end" fill="{col}" '
        f'font-weight="{fw}">{name} {n}</text>')

# ---- 配線（先に描いて、スイッチの足の下に潜らせる）----
GND_C = "#222222"
SW = [  # (名前, 上端 y, GPIO 名, ピン番号, 色, 経路)
    ("SW1", 3,  "GP12", 16, "#1f6fd1", [(4, 3), (8, 3), (8, 17), (9, 17)]),
    ("SW2", 9,  "GP13", 17, "#1a9b4b", [(4, 9), (7, 9), (7, 18), (9, 18)]),
    ("SW3", 15, "GP14", 19, "#e07b00", [(4, 15), (6, 15), (6, 20), (9, 20)]),
    ("SW4", 21, "GP15", 20, "#9b3fc4", [(4, 21), (9, 21)]),
]

def poly(pts, color, width=4.2, dash=None):
    d = " ".join(f"{hx(x)},{hy(y)}" for x, y in pts)
    extra = f' stroke-dasharray="{dash}"' if dash else ""
    add(f'<polyline points="{d}" fill="none" stroke="{color}" stroke-width="{width}" '
        f'stroke-linecap="round" stroke-linejoin="round" stroke-opacity="0.9"{extra}/>')

for _, _, _, _, color, path in SW:
    poly(path, color)

gnd_path = [(1, 7), (1, 26), (17, 26), (17, 19), (16, 19)]
poly(gnd_path, GND_C)
for _, top, *_ in SW:
    poly([(1, top + 4), (2, top + 4)], GND_C)

# ---- タクトスイッチ ----
for name, top, gp, pin, color, _ in SW:
    x0, x1, y0, y1 = 2, 4, top, top + 4
    add(f'<rect x="{hx(x0) - 6}" y="{hy(y0) - 6}" width="{hx(x1) - hx(x0) + 12}" '
        f'height="{hy(y1) - hy(y0) + 12}" rx="4" fill="#3a3a3a" fill-opacity="0.82" stroke="#111" stroke-width="1.5"/>')
    add(f'<circle cx="{(hx(x0) + hx(x1)) / 2}" cy="{(hy(y0) + hy(y1)) / 2}" r="13" fill="#777" stroke="#ccc" stroke-width="1.5"/>')
    add(f'<text x="{(hx(x0) + hx(x1)) / 2}" y="{(hy(y0) + hy(y1)) / 2 + 4}" font-size="11" '
        f'text-anchor="middle" fill="#fff" font-weight="bold">{name}</text>')
    legs = {(x0, y0): "#aaa", (x1, y0): color, (x0, y1): GND_C, (x1, y1): "#aaa"}
    for (lx, ly), c in legs.items():
        add(f'<circle cx="{hx(lx)}" cy="{hy(ly)}" r="5.2" fill="{c}" stroke="#fff" stroke-width="1.5"/>')
    add(f'<text x="{hx(x1) + 9}" y="{hy(y0) - 7}" font-size="10.5" fill="{color}" font-weight="bold">'
        f'→ {gp}（{pin}番）</text>')

add(f'<text x="{hx(3)}" y="{hy(3) - 11}" font-size="9.5" text-anchor="middle" fill="#8a1c1c" '
    'font-weight="bold">設置済み</text>')

# ---- 凡例 ----
ly = hy(ROWS) + 45
add(f'<text x="{OX - 20}" y="{ly}" font-size="14" font-weight="bold" fill="#222">配線</text>')
rows_ = [(s[4], f"{s[0]}  足({4},{s[1]}) → Pico {s[3]}番ピン {s[2]}") for s in SW]
rows_.append((GND_C, "GND  各スイッチの足(2, y+4) → x=1 の列 → 行26 → x=17 → Pico 23番ピン GND"))
for i, (c, t) in enumerate(rows_):
    yy = ly + 22 + i * 21
    add(f'<line x1="{OX - 20}" y1="{yy - 4}" x2="{OX + 10}" y2="{yy - 4}" stroke="{c}" stroke-width="4.5" stroke-linecap="round"/>')
    add(f'<text x="{OX + 20}" y="{yy}" font-size="12.5" fill="#222">{t}</text>')

nx = OX - 20
ly2 = ly + 22 + len(rows_) * 21 + 20
add(f'<text x="{nx}" y="{ly2}" font-size="14" font-weight="bold" fill="#222">メモ</text>')
notes = [
    "・スイッチは対角の2本だけを使う（向きを問わず「押したときだけ導通」）",
    "・灰色の足は未使用。固定のためにはんだ付けだけする",
    "・GPIO は INPUT_PULLUP、押すと LOW（外付け抵抗は不要）",
    "・Pico のソケット2列は穴7個分（x=9 と x=16）離す",
    "・USB は基板の上端側。Pico 本体の下（x=10〜15）には配線を通さない",
    "・配線が交差する箇所はないので、被覆線でもスズメッキ線でも組める",
]
for i, t in enumerate(notes):
    add(f'<text x="{nx}" y="{ly2 + 22 + i * 21}" font-size="12.5" fill="#333">{t}</text>')

add('</svg>')
sys.stdout.write("\n".join(out))
