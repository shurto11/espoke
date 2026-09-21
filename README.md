# espoke

Raspberry Pi Pico WH で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **Raspberry Pi Pico WH**（RP2040 / Wi-Fi・Bluetooth 付き / ピンヘッダ実装済み）。
開発は **MicroPython + mpremote（CLI）** で行う。Arduino IDE は使わない。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | Raspberry Pi Pico WH + LCD1602A（I2C）の表示サンプル |

## lcd1602_hello — LCD1602A に文字を表示する

Pico WH に I2C 接続した LCD1602A に文字を表示する最初のサンプル。

- 1行目: `Hello, Pico WH!`（シリアル／REPL から送った文字列に置き換わる）
- 2行目: 起動からの経過秒数

```
+----------------+
|Hello, Pico WH! |
|uptime 12s      |
+----------------+
```

| ファイル | 内容 |
|---|---|
| `lcd1602_hello/main.py` | アプリ本体。I2C スキャン → LCD 初期化 → 表示ループ |
| `lcd1602_hello/lcd1602.py` | PCF8574 バックパック経由で HD44780 を叩く最小ドライバ（外部ライブラリ不要） |

---

## 0. ターゲットボードについて

**Raspberry Pi Pico WH** は Pico W にピンヘッダとデバッグ用3ピンコネクタをはんだ付け済みにしたもの。
基板・機能は Pico W と同一なので、**ファームウェアも情報も「Pico W」のものをそのまま使う**。

| 項目 | Pico WH の値 |
|---|---|
| チップ | RP2040（Cortex-M0+ デュアルコア。MicroPython 既定 125MHz / 最大 133MHz） |
| Flash | 2MB（外付け QSPI） |
| SRAM | 264KB |
| 無線 | Infineon CYW43439（Wi-Fi 4 2.4GHz / Bluetooth 5.2） |
| USB | **micro-B**（Type-C ではない） |
| GPIO | 3.3V。**5V 耐性はない** |
| 使える GPIO | GP0〜GP22、GP26〜GP28（GP26〜28 は ADC 兼用） |
| 使えない GPIO | **GP23・GP24・GP25・GP29**（CYW43439 と電源制御が専有） |
| リセットボタン | **なし**（RUN ピンか USB 抜き差し、または `mpremote reset`） |

- 「W」= 無線あり、「H」= ヘッダ実装済み。`WH` は両方。
- **オンボード LED は GP25 ではない。** Pico W/WH では LED が CYW43439 側に繋がっているため、MicroPython では `Pin("LED", Pin.OUT)` で扱う。Pico（無印）向けの `Pin(25)` のコードはそのままでは光らない。
- RP2040 は **I2C0 / I2C1 で使えるピンが決まっている**（ESP32 のようにどのピンにでも割り当てることはできない）。

  | ペリフェラル | SDA に使えるピン | SCL に使えるピン |
  |---|---|---|
  | I2C0 | GP0, GP4, GP8, GP12, GP16, GP20, GP28 | GP1, GP5, GP9, GP13, GP17, GP21 |
  | I2C1 | GP2, GP6, GP10, GP14, GP18, GP22, GP26 | GP3, GP7, GP11, GP15, GP19, GP27 |

  （GP 番号を4で割った余りが 0・1 なら I2C0、2・3 なら I2C1。偶数が SDA、奇数が SCL。上の表は Pico W/WH で使えないピンを除いたもの。）

## 1. 必要なもの

| 部品 | 数 | 備考 |
|---|---|---|
| Raspberry Pi Pico WH | 1 | Pico W + ヘッダでも同じ |
| LCD1602A + I2C バックパック（PCF8574） | 1 | LCD の裏に I2C 変換基板がはんだ付けされたもの。4ピン（GND/VCC/SDA/SCL） |
| I2C 用双方向レベル変換モジュール | 1 | 推奨。BSS138 を使った 4ch 品など（理由は後述） |
| ブレッドボード、ジャンパワイヤ | 適量 | Pico WH は 40ピン。ブレッドボードに跨がせて挿す |
| USB ケーブル | 1 | **micro-B**。**データ通信対応のもの**（充電専用だと認識しない） |

> I2C バックパックが付いていない LCD1602A（16ピンのみ）の場合は、PCF8574 バックパックを別途購入して LCD にはんだ付けすると配線が4本で済む。

## 2. 配線

### 2.1 電圧についての注意

- LCD1602A は **5V 駆動**。3.3V では文字がほぼ見えないことが多い。
- Pico WH の GPIO は **3.3V**。5V 耐性はない。
- PCF8574 バックパックには SDA/SCL を VCC（5V）へ引き上げるプルアップ抵抗が載っていることが多く、そのままつなぐと GPIO に 5V がかかる。

そのため、**レベル変換モジュールを挟む**構成を推奨する。

### 2.2 推奨配線（レベル変換あり）

Pico WH を **USB コネクタが上** になるように置くと、左上が1番ピン、そこから左側を下へ数えていく。

```
   Raspberry Pi Pico WH     レベル変換              LCD1602A (PCF8574)
                         ┌─────────────┐
   3V3(OUT) pin36 ───────┤LV         HV├──┬───────── VCC
   VBUS     pin40 ───────┼─────────────┼──┘
   GND      pin38 ───────┤GND       GND├──────────── GND
   GP0(SDA) pin1  ───────┤LV1       HV1├──────────── SDA
   GP1(SCL) pin2  ───────┤LV2       HV2├──────────── SCL
                         └─────────────┘
```

| Pico WH | 物理ピン番号 | レベル変換 LV 側 | レベル変換 HV 側 | LCD バックパック |
|---|---|---|---|---|
| `3V3(OUT)` | 36 | LV | | |
| `VBUS`（USB の 5V） | 40 | | HV | VCC |
| `GND` | 38（3, 8, 13… でも可） | GND | GND | GND |
| `GP0` | 1 | LV1 | HV1 | SDA |
| `GP1` | 2 | LV2 | HV2 | SCL |

- GP0 / GP1 は I2C0 の組み合わせ。`main.py` もこの値で書いてある。
- `VBUS`（pin40）は **USB 給電中のみ 5V**。電池駆動に変える場合は 5V を別途用意する。
- GND は必ず全部共通にする。
- 別のピンに変えたい場合は、0章の I2C ピン対応表から **同じペリフェラル（I2C0 なら I2C0）の SDA/SCL の組** を選ぶ。**GP23/24/25/29 は使えない。**

### 2.3 簡易配線（レベル変換なし）

手元にレベル変換がなく、とりあえず動作確認したい場合。

| Pico WH | 物理ピン番号 | LCD バックパック |
|---|---|---|
| `VBUS` | 40 | VCC |
| `GND` | 38 | GND |
| `GP0` | 1 | SDA |
| `GP1` | 2 | SCL |

この構成で動く例は多いが、GPIO に 5V のプルアップがかかるため**定格外**。長時間使う・本番に組み込む場合はレベル変換を入れること。
（バックパック上のプルアップ抵抗を外し、Pico 側で 3.3V に 4.7kΩ でプルアップする方法もある。）

### 2.4 コントラスト調整

バックパック裏の**青い半固定抵抗（ポテンショメータ）**でコントラストを調整する。
初回はほぼ確実に調整が必要。

- 何も見えない → 回していくと文字が現れる
- 上段に黒い四角が16個並ぶ → 電源は来ているが初期化できていない（I2C 配線・アドレスを確認）

## 3. 開発環境

### 3.1 言語とツールの選定

**Arduino IDE は不要。** GUI は一切使わず、**MicroPython + `mpremote`（CLI）** で開発する。

| 選択肢 | 判定 | 理由 |
|---|---|---|
| **MicroPython + mpremote** | **採用** | REPL で1行ずつ試せる。コンパイル・リンク・UF2 生成が不要で、転送は `.py` のコピーだけ。Wi-Fi/HTTP/JSON が標準ライブラリで書けるので、ポケベル（文字を受信して LCD に出す）用途に直結する。RP2040 は MicroPython の本家ターゲットでドキュメントも公式 |
| C/C++（pico-sdk） | 見送り | 最速・最小だが、CMake と `arm-none-eabi` ツールチェーンの用意が要り、1文字直すたびにビルド → UF2 転送 → 再起動。LCD 表示程度に速度は要らない |
| Arduino core（arduino-pico） | 見送り | 既存の `.ino` を流用できるが、IDE を使わなくても arduino-cli + core の数百MB が必要。Pico では純正の MicroPython のほうが情報が新しい |
| CircuitPython | 見送り | USB ドライブに D&D できて手軽だが、REPL 越しの自動化（CI やスクリプトからの流し込み）は mpremote のほうが素直 |

この方針変更に伴い、旧 ESP32 版の `lcd1602_hello.ino` は削除し、`main.py` + `lcd1602.py` に置き換えた。

### 3.2 MicroPython ファームウェアの書き込み（初回のみ）

Pico WH には **Pico W 用（`RPI_PICO_W`）のビルド**を使う。無印 Pico 用（`RPI_PICO`）を焼くと無線が使えないので注意。

1. ファームウェアを取得する（最新版は <https://micropython.org/download/RPI_PICO_W/> で確認）

   ```bash
   curl -LO https://micropython.org/resources/firmware/RPI_PICO_W-20260824-v1.29.0.uf2
   ```

2. **BOOTSEL ボタンを押しながら** USB を挿す。`RPI-RP2` という USB マスストレージとして見える

   ```bash
   lsusb | grep 2e8a          # 2e8a:0003 Raspberry Pi RP2 Boot なら BOOTSEL 状態
   lsblk -o NAME,LABEL        # RPI-RP2 のパーティションを探す
   ```

   自動マウントされない場合は手動でマウントする。

   ```bash
   udisksctl mount -b /dev/sdc1      # デバイス名は lsblk で確認したもの
   ```

3. UF2 をコピーする。コピーが終わると**ボードが自動で再起動**し、ドライブは消える

   ```bash
   cp RPI_PICO_W-*.uf2 /media/$USER/RPI-RP2/ && sync
   ```

4. MicroPython として認識されたことを確認する

   ```bash
   lsusb | grep 2e8a          # 2e8a:0005 MicroPython Board in FS mode
   ls /dev/ttyACM*            # /dev/ttyACM0
   ```

> 2回目以降に BOOTSEL へ入り直したいときは、ボタンを押さずに `mpremote bootloader` でも入れる。

### 3.3 mpremote のインストール

```bash
pipx install mpremote        # または: pip3 install --user mpremote
mpremote version
```

### 3.4 Linux でのポート権限

`/dev/ttyACM0` は `root:dialout` 所有のため、`dialout` グループに入っていないと `mpremote` が `Permission denied` になる。

```bash
sudo usermod -aG dialout $USER   # 恒久対応。反映には再ログイン（または再起動）が必要
sudo chmod a+rw /dev/ttyACM0     # 今すぐ使いたいとき（挿し直すと戻る）
```

`/dev/ttyACM0` は USB を挿し直すたび・`mpremote reset` のたびに作り直されるので、`chmod` はそのつど消える。
`usermod` 済みで**まだ再ログインしていない**なら、`sg` でそのシェルだけグループを切り替えると待たずに使える（パスワード不要）。

```bash
sg dialout -c "mpremote a0 ls"
```

## 4. 転送と実行

Pico のファイルシステムに `.py` を置くだけで動く。コンパイルは不要。

```bash
cd ~/ssd/electronic/espoke

mpremote devs                                   # 接続されているボードの確認
mpremote cp lcd1602_hello/lcd1602.py :          # ドライバをボードへ転送
mpremote run lcd1602_hello/main.py              # 実行（ボードには保存しない）
```

`mpremote run` は**実行中の出力がそのままターミナルに出る**。停止は `Ctrl-C`。
ただし `run` はボードからの出力を流すだけで、**こちらのキー入力はボードに届かない**。
LCD に文字を送って試すときは `mpremote repl` を使う。

また `exec` / `eval` / `cp` / `ls` などのコマンドは実行時に raw REPL へ入るため、**ボード上で動いているスクリプトを中断する**。
動かしたまま覗きたいときは `mpremote repl` だけを使うこと。

電源を入れたら自動で動くようにするには、`main.py` という名前でボードに保存する。

```bash
mpremote cp lcd1602_hello/main.py :main.py      # 起動時に自動実行される
mpremote reset                                  # ハードリセット
mpremote repl                                   # 起動ログを見る（抜けるのは Ctrl-]）
```

> 自動実行を止めたいときは、REPL に入って `Ctrl-C` で中断し、`mpremote rm :main.py` で消す。

### 開発中のループを速くする

ローカルのディレクトリをボードのファイルシステムとしてマウントすると、コピーせずにその場の編集を実行できる。

```bash
mpremote mount lcd1602_hello exec "import main"
```

## 5. 動かし方

1. シリアルを開く（**ボーレート設定は不要**。USB CDC なので速度指定は意味を持たない）
   - 出力を眺めるだけなら `mpremote run lcd1602_hello/main.py`
   - **文字を送って試すなら `mpremote repl`**（`run` ではキー入力が届かない）。ボードに `main.py` を保存済みなら `mpremote reset` の直後に `mpremote repl` で繋ぐと、起動ログから見られる
2. 起動ログで I2C スキャン結果を確認する

   ```
   I2C scan...
     found: 0x27
   LCD address: 0x27
   Type text and press Enter to show it on the LCD. (Ctrl-C to stop)
   ```

   - `found: 0x27` … LCD が見えている。配線OK
   - `no device found` … **LCD が繋がっていない**。スクリプトは既定の `0x27` にフォールバックして動き続けるので、シリアルは正常に見えるが LCD には何も出ない。配線を確認すること
3. LCD の1行目に `Hello, Pico WH!`、2行目に経過秒数が表示される
4. ターミナルに文字を入力して Enter → LCD の1行目がその文字列に変わる（16文字まで、英数字・記号のみ）

   ```
   LCD <- "ESPOKE TEST"
   ```

   この応答が返れば、LCD が未接続でもファームウェア自体は正常に動作している

> I2C スキャンは起動時にしか走らない。やり直すには `Ctrl-C` → 再実行するか、`mpremote reset` でリセットする。

## 6. 設定の変更

`main.py` 冒頭の定数で変更する。

| 定数 | 既定値 | 内容 |
|---|---|---|
| `I2C_ID` | 0 | 使う I2C ペリフェラル（0 or 1）。ピンと組み合わせが対応している必要がある |
| `PIN_SDA` | 0 | I2C SDA の GP 番号 |
| `PIN_SCL` | 1 | I2C SCL の GP 番号 |
| `LCD_COLS` / `LCD_ROWS` | 16 / 2 | LCD の桁数・行数（2004 なら 20 / 4） |
| `LCD_ADDR_DEFAULT` | 0x27 | スキャンで見つからなかったときに使うアドレス |

アドレスは起動時に自動検出するので、通常は変更不要（PCF8574 は `0x27`、PCF8574A は `0x3F` が多い）。

## 7. mpremote チートシート

| やりたいこと | コマンド |
|---|---|
| ボード一覧 | `mpremote devs` |
| REPL に入る（抜けるのは `Ctrl-]`） | `mpremote repl` |
| ポートを明示して接続 | `mpremote connect /dev/ttyACM0 repl`（短縮形 `mpremote a0 repl`） |
| ファイル一覧 | `mpremote ls` |
| 転送 / 取得 | `mpremote cp local.py :` / `mpremote cp :main.py .` |
| 削除 | `mpremote rm :main.py` |
| ローカルのスクリプトを実行 | `mpremote run script.py` |
| 一行だけ実行 | `mpremote exec "from machine import Pin; Pin('LED', Pin.OUT).on()"` |
| 値を表示 | `mpremote eval "1+1"` |
| 空き容量 | `mpremote df` |
| ハード／ソフトリセット | `mpremote reset` / `mpremote soft-reset` |
| BOOTSEL に入る（ボタンを押さずに） | `mpremote bootloader` |
| ライブラリ導入（micropython-lib） | `mpremote mip install <パッケージ名>` |

## 8. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| バックライトも点かない | VCC/GND の配線、`VBUS`（pin40）から 5V が来ているか確認 |
| バックライトは点くが何も表示されない | コントラスト調整（青い半固定抵抗を回す） |
| 上段に黒い四角が並ぶだけ | I2C 通信できていない。SDA/SCL の入れ違い、GND 共通化を確認 |
| `no device found` | SDA/SCL 配線、レベル変換の LV/HV の電源を確認。GP0/GP1 以外に挿していないか確認 |
| 文字化けする | 配線の接触不良。ジャンパワイヤを短くする・挿し直す |
| `ValueError: bad SCL pin` | I2C0/I2C1 で使えないピンを指定している。0章の対応表を参照 |
| `/dev/ttyACM0` が出てこない | 充電専用の micro-B ケーブルになっていないか確認。USBハブ経由をやめて PC 本体に直挿し。`lsusb` が `2e8a:0003`（RP2 Boot）なら **MicroPython が入っていない**ので 3.2 を実施 |
| `Permission denied` | `dialout` グループに入っていない。`sudo usermod -aG dialout $USER` して再ログイン（応急処置は `sudo chmod a+rw /dev/ttyACM0`） |
| `mpremote: no device found` | 他のプロセスがポートを掴んでいる（別ターミナルの `mpremote repl`、Thonny、`screen` など）。閉じてから再実行 |
| 挿し直したら再び `Permission denied` | `/dev/ttyACM0` は再列挙のたびに作り直されるので `chmod` は消える。`sg dialout -c "mpremote ..."` なら再ログインせずに通る |
| `mpremote repl` で文字を打っても LCD が変わらない | `mpremote run` で起動していないか確認。`run` はキー入力を転送しない。また、途中で `mpremote exec` などを叩くとスクリプト自体が止まっている |
| REPL に入ると `main.py` の出力が流れ続ける | 自動起動している。`Ctrl-C` で止める。消すなら `mpremote rm :main.py` |
| `Ctrl-C` が効かない | `mpremote repl` で入り直し、`Ctrl-C` → `Ctrl-D`（ソフトリセット）。それでも駄目なら USB 抜き差し |
| リセットボタンがない | Pico には無い。`mpremote reset` か、`RUN`（pin30）と `GND`（pin28）を一瞬ショートする（タクトスイッチを付けると楽） |
| オンボード LED が光らない | Pico W/WH の LED は GP25 ではない。`Pin("LED", Pin.OUT)` を使う |
| Wi-Fi が使えない（`network` が無い等） | 無印 Pico 用の `RPI_PICO` ファームウェアを焼いている。`RPI_PICO_W` を焼き直す |
| 日本語が表示できない | LCD1602A は英数字とカタカナ（独自コード）のみ。漢字・ひらがなは不可 |
