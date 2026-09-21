# espoke

ESP32 で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **ESP32-S3-WROOM-1 N8R8**（8MB Flash / 8MB OPI PSRAM）。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | ESP32-S3-WROOM-1 N8R8 + LCD1602A（I2C）の表示サンプル |

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

## 0. ターゲットモジュールについて

ESP32-S3-WROOM-1 の型番は `N<フラッシュ容量>R<PSRAM容量>` で、**N8R8** は次の構成。

| 項目 | N8R8 の値 |
|---|---|
| チップ | ESP32-S3（Xtensa LX7 デュアルコア 240MHz） |
| Flash | 8MB / Quad SPI（3.3V） |
| PSRAM | 8MB / **Octal SPI（OPI）** |
| アンテナ | PCB アンテナ（`-1U` は外部アンテナ） |
| 使えない GPIO | GPIO26〜GPIO32（Flash）、**GPIO33〜GPIO37（OPI PSRAM）** |

ここが後の Arduino IDE 設定（`Flash Size` = 8MB、`PSRAM` = OPI PSRAM）に直結する。
**N8R2 や N16R8 と設定を間違えるとブートループする**ので、モジュール表面のシルク印刷で型番を確認しておくこと。

このモジュールを載せた開発ボードとしては **ESP32-S3-DevKitC-1（N8R8 版）** が代表的で、本書はそれを前提に書いている。
素の WROOM-1 モジュール単体で使う場合は、別途 USB-シリアル変換・自動リセット回路・3.3V 電源が必要。

## 1. 必要なもの

| 部品 | 数 | 備考 |
|---|---|---|
| ESP32-S3-WROOM-1 N8R8 搭載ボード | 1 | ESP32-S3-DevKitC-1（N8R8）など |
| LCD1602A + I2C バックパック（PCF8574） | 1 | LCD の裏に I2C 変換基板がはんだ付けされたもの。4ピン（GND/VCC/SDA/SCL） |
| I2C 用双方向レベル変換モジュール | 1 | 推奨。BSS138 を使った 4ch 品など（理由は後述） |
| ブレッドボード、ジャンパワイヤ | 適量 | |
| USB ケーブル | 1 | Type-C。**データ通信対応のもの** |

> I2C バックパックが付いていない LCD1602A（16ピンのみ）の場合は、PCF8574 バックパックを別途購入して LCD にはんだ付けすると配線が4本で済む。

## 2. 配線

### 2.1 電圧についての注意

- LCD1602A は **5V 駆動**。3.3V では文字がほぼ見えないことが多い。
- ESP32-S3-WROOM-1 の GPIO は **3.3V**。5V 耐性はない。
- PCF8574 バックパックには SDA/SCL を VCC（5V）へ引き上げるプルアップ抵抗が載っていることが多く、そのままつなぐと GPIO に 5V がかかる。

そのため、**レベル変換モジュールを挟む**構成を推奨する。

### 2.2 推奨配線（レベル変換あり）

```
 ESP32-S3-WROOM-1       レベル変換              LCD1602A (PCF8574)
      (N8R8)         ┌─────────────┐
   3V3 ──────────────┤LV         HV├──┬───────── VCC
   5V ───────────────┼─────────────┼──┘
   GND ──────────────┤GND       GND├──────────── GND
   GPIO8 (SDA) ──────┤LV1       HV1├──────────── SDA
   GPIO9 (SCL) ──────┤LV2       HV2├──────────── SCL
                     └─────────────┘
```

| ESP32-S3-WROOM-1 | レベル変換 LV 側 | レベル変換 HV 側 | LCD バックパック |
|---|---|---|---|
| 3V3 | LV | | |
| 5V | | HV | VCC |
| GND | GND | GND | GND |
| GPIO8（DevKitC-1 のシルクは `IO8`） | LV1 | HV1 | SDA |
| GPIO9（同 `IO9`） | LV2 | HV2 | SCL |

- GPIO8 / GPIO9 は Arduino core が ESP32-S3 の既定 I2C ピンとして定義している組み合わせ。スケッチもこの値で書いてある。
- 別のピンに変えたい場合、N8R8 で**避けるピン**は以下。
  - GPIO26〜GPIO32 … 内蔵 Flash 用
  - **GPIO33〜GPIO37 … 内蔵 OPI PSRAM 用（N8R8 では使用不可）**
  - GPIO0・GPIO3・GPIO45・GPIO46 … ストラッピングピン
  - GPIO19・GPIO20 … USB D-/D+
- `5V` ピンは USB 給電時に 5V が出ている。
- GND は必ず全部共通にする。

### 2.3 簡易配線（レベル変換なし）

手元にレベル変換がなく、とりあえず動作確認したい場合。

| ESP32-S3-WROOM-1 | LCD バックパック |
|---|---|
| 5V | VCC |
| GND | GND |
| GPIO8 | SDA |
| GPIO9 | SCL |

この構成で動く例は多いが、GPIO に 5V のプルアップがかかるため**定格外**。長時間使う・本番に組み込む場合はレベル変換を入れること。
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
2. `ツール` → `ボード` → `esp32` → **ESP32S3 Dev Module** を選択
   （`ESP32-S3-DevKitC-1` という項目がある版ならそれでもよい）
3. `ツール` の設定を **N8R8 に合わせる**

   | 項目 | N8R8 での設定 |
   |---|---|
   | `USB CDC On Boot` | **Enabled** |
   | `Flash Size` | **8MB (64Mb)** |
   | `Flash Mode` | `QIO 80MHz`（Flash は Quad） |
   | `PSRAM` | **OPI PSRAM** |
   | `Partition Scheme` | `8M with spiffs (3MB APP/1.5MB SPIFFS)` |
   | `Upload Speed` | 921600（不安定なら 115200） |

   - `USB CDC On Boot` は、DevKitC-1 の `USB` 側ポート（ESP32-S3 直結）を使う場合に必須。これが Disabled だとシリアルモニタに何も出ない。`UART` 側ポート（USB-シリアル変換チップ経由）なら Disabled のままでよい。
   - `Flash Size` と `PSRAM` が実物と合っていないと**起動時にブートループする**。N8R8 で `PSRAM` を `QSPI PSRAM` にするのも誤り（N8R8 は Octal）。
4. `ツール` → `ポート` で選択（USB 直結は `/dev/ttyACM0`、変換チップ経由は `/dev/ttyUSB0`）
5. `→`（書き込み）ボタンを押す
   - ポートが出てこない・書き込みが始まらない場合は、**BOOT ボタンを押したまま RESET（または USB を挿し直し）→ BOOT を離す** でダウンロードモードに入れる
   - 書き込み後、USB 直結ポートでは自動リセットされないことがある。RESET ボタンを押す

### arduino-cli を使う場合

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "LiquidCrystal I2C"

cd ~/ssd/electronic/espoke
# ESP32-S3-WROOM-1 N8R8 向けの FQBN
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,FlashMode=qio,PSRAM=opi,PartitionScheme=default_8MB"
arduino-cli compile --fqbn "$FQBN" lcd1602_hello
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 lcd1602_hello
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

指定できるオプション名は次で確認できる。

```bash
arduino-cli board details --fqbn esp32:esp32:esp32s3
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
| `PIN_SDA` | 8 | I2C SDA ピン |
| `PIN_SCL` | 9 | I2C SCL ピン |
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
| 起動を繰り返す（ブートループ） | **N8R8 の設定ミスが最有力**。`Flash Size` = 8MB、`PSRAM` = OPI PSRAM、`Flash Mode` = QIO を確認 |
| `Sketch too big` | `Partition Scheme` が 4MB 用のままになっている。`8M with spiffs` に変更 |
| PSRAM が認識されない（`ESP.getPsramSize()` が 0） | `PSRAM` が `Disabled` か `QSPI PSRAM` になっている。**OPI PSRAM** にする |
| GPIO33〜37 につないだ部品が動かない | N8R8 では OPI PSRAM が占有していて使えない。別のピンを使う |
| 日本語が表示できない | LCD1602A は英数字とカタカナ（独自コード）のみ。漢字・ひらがなは不可 |
