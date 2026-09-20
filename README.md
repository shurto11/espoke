# espoke

ESP32 で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **ESP32-S3**。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | ESP32-S3 + LCD1602A（I2C）の表示サンプル |

## lcd1602_hello — LCD1602A に文字を表示する

ESP32-S3 に I2C 接続した LCD1602A に文字を表示する最初のサンプル。

- 1行目: `Hello, ESP32-S3!`（シリアルモニタから送った文字列に置き換わる）
- 2行目: 起動からの経過秒数

```
+----------------+
|Hello, ESP32-S3!|
|uptime 12s      |
+----------------+
```

---

## 1. 必要なもの

| 部品 | 数 | 備考 |
|---|---|---|
| ESP32-S3 開発ボード | 1 | ESP32-S3-DevKitC-1、ESP32-S3 SuperMini、XIAO ESP32S3 など |
| LCD1602A + I2C バックパック（PCF8574） | 1 | LCD の裏に I2C 変換基板がはんだ付けされたもの。4ピン（GND/VCC/SDA/SCL） |
| I2C 用双方向レベル変換モジュール | 1 | 推奨。BSS138 を使った 4ch 品など（理由は後述） |
| ブレッドボード、ジャンパワイヤ | 適量 | |
| USB ケーブル | 1 | ボードに合ったもの（Type-C が多い）。**データ通信対応のもの** |

> I2C バックパックが付いていない LCD1602A（16ピンのみ）の場合は、PCF8574 バックパックを別途購入して LCD にはんだ付けすると配線が4本で済む。

## 2. 配線

### 2.1 電圧についての注意

- LCD1602A は **5V 駆動**。3.3V では文字がほぼ見えないことが多い。
- ESP32-S3 の GPIO は **3.3V**。5V 耐性はない。
- PCF8574 バックパックには SDA/SCL を VCC（5V）へ引き上げるプルアップ抵抗が載っていることが多く、そのまま ESP32-S3 につなぐと GPIO に 5V がかかる。

そのため、**レベル変換モジュールを挟む**構成を推奨する。

### 2.2 推奨配線（レベル変換あり）

```
   ESP32-S3           レベル変換              LCD1602A (PCF8574)
                   ┌─────────────┐
   3V3 ────────────┤LV         HV├──┬───────── VCC
   5V ─────────────┼─────────────┼──┘
   GND ────────────┤GND       GND├──────────── GND
   GPIO8 (SDA) ────┤LV1       HV1├──────────── SDA
   GPIO9 (SCL) ────┤LV2       HV2├──────────── SCL
                   └─────────────┘
```

| ESP32-S3 | レベル変換 LV 側 | レベル変換 HV 側 | LCD バックパック |
|---|---|---|---|
| 3V3 | LV | | |
| 5V | | HV | VCC |
| GND | GND | GND | GND |
| GPIO8 | LV1 | HV1 | SDA |
| GPIO9 | LV2 | HV2 | SCL |

ボード別のピン表記:

| ボード | SDA | SCL | 5V |
|---|---|---|---|
| ESP32-S3-DevKitC-1 | `IO8` | `IO9` | `5V` |
| ESP32-S3 SuperMini | `8` | `9` | `5V` |
| XIAO ESP32S3 | `D4`（GPIO5） | `D5`（GPIO6） | `5V` |

- GPIO8 / GPIO9 は Arduino core が ESP32-S3 の既定 I2C ピンとして定義している組み合わせ。
- **XIAO ESP32S3 だけ既定が異なる**（SDA=GPIO5 / SCL=GPIO6）。スケッチ冒頭の `PIN_SDA` / `PIN_SCL` を `5` / `6` に変更すること。XIAO の `D8`〜`D10` は GPIO7/8/9 とずれているので、GPIO 番号で合わせるのが安全。
- 避けるピン: GPIO0・GPIO3・GPIO45・GPIO46（ストラッピングピン）、GPIO19・GPIO20（USB D-/D+）、GPIO26〜GPIO32（内蔵フラッシュ用）。PSRAM 付きモジュール（N16R8 など）では GPIO33〜GPIO37 も使えない。
- `5V` ピンは USB 給電時に 5V が出ている。
- GND は必ず全部共通にする。

### 2.3 簡易配線（レベル変換なし）

手元にレベル変換がなく、とりあえず動作確認したい場合。

| ESP32-S3 | LCD バックパック |
|---|---|
| 5V | VCC |
| GND | GND |
| GPIO8 | SDA |
| GPIO9 | SCL |

この構成で動く例は多いが、ESP32-S3 の GPIO に 5V のプルアップがかかるため**定格外**。長時間使う・本番に組み込む場合はレベル変換を入れること。
（バックパック上のプルアップ抵抗を外し、ESP32-S3 側で 3.3V に 4.7kΩ でプルアップする方法もある。）

### 2.4 コントラスト調整

バックパック裏の**青い半固定抵抗（ポテンショメータ）**でコントラストを調整する。
初回はほぼ確実に調整が必要。

- 何も見えない → 回していくと文字が現れる
- 上段に黒い四角が16個並ぶ → 電源は来ているが初期化できていない（I2C 配線・アドレスを確認）

## 3. ソフトウェアの準備（Arduino IDE）

1. [Arduino IDE 2.x](https://www.arduino.cc/en/software) をインストール
2. ESP32 ボードを追加
   - `ファイル` → `基本設定` → `追加のボードマネージャのURL` に以下を追加
     ```
     https://espressif.github.io/arduino-esp32/package_esp32_index.json
     ```
   - `ツール` → `ボード` → `ボードマネージャ` で **esp32 by Espressif Systems**（3.x）をインストール
3. ライブラリを追加
   - `ツール` → `ライブラリを管理` で **LiquidCrystal I2C**（作者: Frank de Brabander）をインストール
   - 「AVR 用」と警告が出ることがあるが ESP32-S3 でも動作する
4. Linux の場合、シリアルポートの権限を付与（初回のみ、実行後に再ログイン）
   ```bash
   sudo usermod -aG dialout $USER
   ```

## 4. 書き込み

1. `lcd1602_hello/lcd1602_hello.ino` を Arduino IDE で開く
2. `ツール` → `ボード` → `esp32` → ボードを選択
   - DevKitC-1 / SuperMini: **ESP32S3 Dev Module**
   - XIAO: **XIAO_ESP32S3**
3. `ツール` → **USB CDC On Boot** → **Enabled**
   - SuperMini や XIAO、DevKitC-1 の `USB` 側ポートは USB が ESP32-S3 に直結されているため、これを有効にしないとシリアルモニタに何も出ない
   - DevKitC-1 の `UART` 側ポート（USB-シリアル変換チップ経由）を使う場合は Disabled のままでよい
4. `ESP32S3 Dev Module` を選んだ場合、モジュールに合わせて以下も設定する
   - `Flash Size`: 実装容量（N16R8 なら 16MB）
   - `PSRAM`: PSRAM なしなら **Disabled**、N8R2/N16R8 などは **OPI PSRAM**
   - 設定が合っていないと起動時にブートループすることがある
5. `ツール` → `ポート` で選択（USB 直結は `/dev/ttyACM0`、変換チップ経由は `/dev/ttyUSB0`）
6. `→`（書き込み）ボタンを押す
   - ポートが出てこない・書き込みが始まらない場合は、**BOOT ボタンを押したまま RESET（または USB を挿し直し）→ BOOT を離す** でダウンロードモードに入れる
   - 書き込み後、USB 直結ボードは自動でリセットされないことがある。RESET ボタンを押す

### arduino-cli を使う場合

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "LiquidCrystal I2C"

cd ~/ssd/electronic/espoke
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"   # XIAO は esp32:esp32:XIAO_ESP32S3
arduino-cli compile --fqbn "$FQBN" lcd1602_hello
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 lcd1602_hello
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

PSRAM 付きモジュールを `esp32s3` FQBN で使う場合はオプションを足す:

```bash
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M"
```

## 5. 動かし方

1. 書き込み後、シリアルモニタを **115200 bps**、改行コード「LF」または「CR+LF」で開く
2. 起動時に I2C スキャン結果が表示される
   ```
   I2C scan...
     found: 0x27
   LCD address: 0x27
   Type text and press Enter to show it on the LCD.
   ```
3. LCD の1行目に `Hello, ESP32-S3!`、2行目に経過秒数が表示される
4. シリアルモニタに文字を入力して Enter → LCD の1行目がその文字列に変わる（16文字まで、英数字・記号のみ）

## 6. 設定の変更

`lcd1602_hello.ino` 冒頭の定数で変更する。

| 定数 | 既定値 | 内容 |
|---|---|---|
| `PIN_SDA` | 8 | I2C SDA ピン（XIAO ESP32S3 は 5） |
| `PIN_SCL` | 9 | I2C SCL ピン（XIAO ESP32S3 は 6） |
| `LCD_COLS` / `LCD_ROWS` | 16 / 2 | LCD の桁数・行数（2004 なら 20 / 4） |
| `LCD_ADDR_DEFAULT` | 0x27 | スキャンで見つからなかったときに使うアドレス |

アドレスは起動時に自動検出するので、通常は変更不要（PCF8574 は `0x27`、PCF8574A は `0x3F` が多い）。

## 7. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| バックライトも点かない | VCC/GND の配線、5V が来ているか確認 |
| バックライトは点くが何も表示されない | コントラスト調整（青い半固定抵抗を回す） |
| 上段に黒い四角が並ぶだけ | I2C 通信できていない。SDA/SCL の入れ違い、GND 共通化を確認 |
| シリアルに `no device found` | SDA/SCL 配線、レベル変換の LV/HV の電源を確認 |
| 文字化けする | 配線の接触不良。ジャンパワイヤを短くする・挿し直す |
| ポートが出てこない | 充電専用の USB ケーブルになっていないか確認。DevKitC-1 なら挿すポート（UART / USB）が合っているか確認。BOOT ボタンを押しながら挿し直す |
| シリアルモニタに何も出ない | `USB CDC On Boot` を Enabled にして書き込み直す |
| 起動を繰り返す（ブートループ） | `PSRAM` / `Flash Size` の設定がモジュールと合っていない |
| 日本語が表示できない | LCD1602A は英数字とカタカナ（独自コード）のみ。漢字・ひらがなは不可 |
