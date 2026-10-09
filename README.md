# espoke

Raspberry Pi Pico WH で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **Raspberry Pi Pico WH**（RP2040 / Wi-Fi・Bluetooth 付き / ピンヘッダ実装済み）。
開発は **arduino-pico（C++）+ arduino-cli** で行う。Arduino IDE は使わない。

やりたいこと:

- LCD1602A にメッセージを表示する
- **Bluetooth イヤホン（A2DP）で音を鳴らす**
- **microSD に入れた MP3 を、ネットのない外でも聞く**
- **Discord のメッセージを受け取って LCD に出す**（漢字は読みに直して半角カナで出す）。モールスで打った文を Discord に送る

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | LCD1602A（I2C）の表示サンプル |
| `morse_input/` | 4つのスイッチ（GP10〜13）でモールス信号を打ち、アルファベットを LCD に入力するサンプル |
| `bt_earphone/` | Bluetooth イヤホンに接続して通知音を鳴らすサンプル |
| `espoke/` | 本体。microSD の MP3 を Bluetooth イヤホンで再生し、LCD に状態を出す。SW1〜SW4 で再生の操作とモールス入力ができる。Discord のメッセージを受け取って出し、モールスで打った文を送る |
| `sd_test/` | microSD スロット（CK-40）の配線・カード・ファイル一覧・読み書きを確かめるテスト |
| `docs/` | 部品の取り付け手順など |
| `tools/` | Discord の中継（PC 側で動かす）と、基板の配置図を作るスクリプト |

ビルド実測値（`rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble`）:

| スケッチ | Flash | RAM（グローバル） |
|---|---|---|
| `lcd1602_hello` | 319KB / 2093KB (15%) | 69KB / 256KB (26%) |
| `bt_earphone` | 514KB (24%) | 96KB (36%) |
| `espoke` | 786KB (37%) | 145KB (55%) |

---

## 0. ターゲットボードについて

**Raspberry Pi Pico WH** は Pico W にピンヘッダとデバッグ用3ピンコネクタをはんだ付け済みにしたもの。
基板・機能は Pico W と同一なので、**ボード定義もファームウェアも「Pico W」のものをそのまま使う**。

| 項目 | Pico WH の値 |
|---|---|
| チップ | RP2040（Cortex-M0+ デュアルコア。arduino-pico の既定は 200MHz、データシート上の定格は 133MHz） |
| Flash | 2MB（外付け QSPI） |
| SRAM | 264KB |
| 無線 | Infineon CYW43439（Wi-Fi 4 2.4GHz / **Bluetooth 5.2 Classic + LE**） |
| USB | **micro-B**（Type-C ではない） |
| GPIO | 3.3V。**5V 耐性はない** |
| 使える GPIO | GP0〜GP22、GP26〜GP28（GP26〜28 は ADC 兼用） |
| 使えない GPIO | **GP23・GP24・GP25・GP29**（CYW43439 と電源制御が専有） |
| リセットボタン | **なし**（RUN ピンか USB 抜き差し） |

- 「W」= 無線あり、「H」= ヘッダ実装済み。`WH` は両方。
- **Wi-Fi と Bluetooth は同じ CYW43439 を共有する。** 同時に使うと帯域を取り合うので、音声ストリーミング中は Wi-Fi のスループットが落ちる。
- **オンボード LED は GP25 ではない。** Pico W/WH では LED が CYW43439 側に繋がっているため、`LED_BUILTIN` を使う。Pico（無印）向けの `digitalWrite(25, ...)` は効かない。
- RP2040 は **I2C0 / I2C1 で使えるピンが決まっている**（ESP32 のようにどのピンにでも割り当てることはできない）。

  | ペリフェラル | SDA に使えるピン | SCL に使えるピン |
  |---|---|---|
  | I2C0 (`Wire`) | GP0, GP4, GP8, GP12, GP16, GP20, GP28 | GP1, GP5, GP9, GP13, GP17, GP21 |
  | I2C1 (`Wire1`) | GP2, GP6, GP10, GP14, GP18, GP22, GP26 | GP3, GP7, GP11, GP15, GP19, GP27 |

  （GP 番号を4で割った余りが 0・1 なら I2C0、2・3 なら I2C1。偶数が SDA、奇数が SCL。上の表は Pico W/WH で使えないピンを除いたもの。）

## 1. 必要なもの

| 部品 | 数 | 備考 |
|---|---|---|
| Raspberry Pi Pico WH | 1 | Pico W + ヘッダでも同じ |
| Bluetooth イヤホン / スピーカー | 1 | **A2DP 対応のもの**（普通のワイヤレスイヤホンなら対応している） |
| LCD1602A + I2C バックパック（PCF8574） | 1 | LCD の裏に I2C 変換基板がはんだ付けされたもの。4ピン（GND/VCC/SDA/SCL） |
| I2C 用双方向レベル変換モジュール | 1 | 推奨（理由は後述）。スイッチサイエンスの FET 搭載 I2C 用双方向レベルシフタ（SSCI-023962）を使う |
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
   Raspberry Pi Pico WH     レベルシフタ（SSCI-023962）  LCD1602A (PCF8574)
                         ┌──────────────────┐
   3V3(OUT) pin36 ───────┤3V3             5V├──┬───────── VCC
   VBUS     pin40 ───────┼──────────────────┼──┘
   GP0(SDA) pin1  ───────┤SDA3V3       SDA5V├──────────── SDA
   GP1(SCL) pin2  ───────┤SCL3V3       SCL5V├──────────── SCL
                         └──────────────────┘
   GND      pin38 ─────────────────────────────────────── GND
```

| Pico WH | 物理ピン番号 | レベルシフタ 3.3V 側 | レベルシフタ 5V 側 | LCD バックパック |
|---|---|---|---|---|
| `3V3(OUT)` | 36 | 3V3 | | |
| `VBUS`（USB の 5V） | 40 | | 5V | VCC |
| `GND` | 38（3, 8, 13… でも可） | （GND ピンなし） | （GND ピンなし） | GND |
| `GP0` | 1 | SDA3V3 | SDA5V | SDA |
| `GP1` | 2 | SCL3V3 | SCL5V | SCL |

- GP0 / GP1 は I2C0（`Wire`）の組み合わせ。スケッチもこの値で書いてある。
- `VBUS`（pin40）は **USB 給電中のみ 5V**。電池駆動に変える場合は 5V を別途用意する。
- GND は必ず全部共通にする。SSCI-023962 には GND ピンがないので、LCD の GND は Pico の GND に直接つなぐ。
- SSCI-023962 は 3.3V 側・5V 側の両方に 4.7kΩ のプルアップ抵抗が載っている。ピンヘッダは付属しないので、1×3 を 2 本はんだ付けする。
- 別のピンに変えたい場合は、0章の I2C ピン対応表から **同じペリフェラルの SDA/SCL の組** を選ぶ。**GP23/24/25/29 は使えない。**

### 2.3 簡易配線（レベル変換なし）

手元にレベル変換がなく、とりあえず動作確認したい場合。

| Pico WH | 物理ピン番号 | LCD バックパック |
|---|---|---|
| `VBUS` | 40 | VCC |
| `GND` | 38 | GND |
| `GP0` | 1 | SDA |
| `GP1` | 2 | SCL |

この構成で動く例は多いが、GPIO に 5V のプルアップがかかるため**定格外**。長時間使う・本番に組み込む場合はレベル変換を入れること。

### 2.4 コントラスト調整

バックパック裏の**青い半固定抵抗（ポテンショメータ）**でコントラストを調整する。初回はほぼ確実に調整が必要。

- 何も見えない → 回していくと文字が現れる
- 上段に黒い四角が16個並ぶ → 電源は来ているが初期化できていない（I2C 配線・アドレスを確認）

## 3. 開発環境

### 3.1 なぜ MicroPython をやめたか

当初は MicroPython + mpremote で開発していたが、**Bluetooth イヤホンを鳴らすために C++ へ移行した**。

MicroPython の `bluetooth` モジュールは実機で確認したところ **BLE 専用**で、イヤホンに音を飛ばす A2DP が存在しない。

```
bluetooth attrs: ['BLE', 'FLAG_INDICATE', 'FLAG_NOTIFY', 'FLAG_READ', 'FLAG_WRITE',
                  'FLAG_WRITE_NO_RESPONSE', 'UUID']
BLE attrs: ['active', 'config', 'gap_advertise', 'gap_connect', 'gap_disconnect',
            'gap_scan', 'gattc_*', 'gatts_*', 'irq']
```

CYW43439 は**ハードとしては Bluetooth Classic に対応している**が、MicroPython 側が ACL/SCO を実装していないため、A2DP を載せる土台がない。

| 選択肢 | 判定 | 理由 |
|---|---|---|
| **arduino-pico（earlephilhower）** | **採用** | 同梱の `BluetoothAudio` ライブラリに `A2DPSource` があり、スキャン・接続・PCM 書き込みが数行で書ける。`WiFi` / `HTTPClient` も同梱。ビルドは arduino-cli だけで完結し、IDE は要らない |
| pico-sdk + BTstack を直接 | 見送り | 同じことはできるが、CMake と BTstack のイベントハンドラを自前で書く必要があり記述量が多い |
| MicroPython | **不可** | BLE のみ。A2DP なし（上記の実機確認） |
| CircuitPython | **不可** | 同じく Bluetooth Classic 非対応 |

### 3.2 インストール

```bash
# arduino-cli 本体（未導入の場合）
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

URL=https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index --additional-urls "$URL"
arduino-cli core install rp2040:rp2040 --additional-urls "$URL"
arduino-cli lib install "LiquidCrystal I2C"
arduino-cli lib install BackgroundAudio   # espoke の MP3 デコード (libmad)。1.4.4 で確認
```

core は約 1.5GB ある（ツールチェーンと pico-sdk を含むため）。`~/.arduino15` の空き容量に注意。

### 3.3 FQBN — Bluetooth を有効にする

**ここが最重要。** 既定では Bluetooth スタックが入らないので、`ipbtstack` を指定する。

```
rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble
```

| 指定 | 意味 |
|---|---|
| `rpipicow` | Raspberry Pi Pico W（WH も同じ） |
| `ipbtstack=ipv4btcble` | **IPv4 + Bluetooth**。これがないと `BluetoothAudio.h` がコンパイルエラーになる |

Arduino IDE を使う場合は `ツール` → `IP/Bluetooth Stack` → `IPv4 + Bluetooth` が同じ意味。
選べる値の一覧は次で確認できる。

```bash
arduino-cli board details --fqbn rp2040:rp2040:rpipicow
```

### 3.4 Linux でのポート権限

`/dev/ttyACM0` は `root:dialout` 所有のため、`dialout` グループに入っていないと書き込みもシリアルも使えない。

```bash
sudo usermod -aG dialout $USER   # 恒久対応。反映には再ログイン（または再起動）が必要
sudo chmod a+rw /dev/ttyACM0     # 今すぐ使いたいとき（挿し直すと戻る）
```

`/dev/ttyACM0` は USB を挿し直すたび・リセットのたびに作り直されるので、`chmod` はそのつど消える。
`usermod` 済みで**まだ再ログインしていない**なら、`sg` でそのシェルだけグループを切り替えると待たずに使える（パスワード不要）。

```bash
sg dialout -c "arduino-cli upload --fqbn ... -p /dev/ttyACM0 espoke"
```

## 4. ビルドと書き込み

```bash
cd ~/ssd/electronic/espoke
FQBN="rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble"

arduino-cli compile --fqbn "$FQBN" bt_earphone
arduino-cli upload  --fqbn "$FQBN" -p /dev/ttyACM0 bt_earphone
```

書き込みは **UF2 方式**（`.uf2` を `RPI-RP2` ドライブにコピーする）。ボードを BOOTSEL 状態にする必要がある。

- **BOOTSEL ボタンを押しながら USB を挿す**のが確実
- 既に arduino-pico のスケッチが載っていれば、`arduino-cli upload` が自動でリセットしてくれる
- この自動リセットは `/dev/ttyACM0` を 1200bps で開いて行う。**ポートを開けないと無言で失敗し、
  `No drive to deploy.` になる**（3.4 参照）。`id` に `dialout` が出ていなければ `sg dialout -c` 経由で叩く
- ドライブが自動マウントされない場合は手動で行う

  ```bash
  lsblk -o NAME,LABEL                     # RPI-RP2 を探す
  udisksctl mount -b /dev/sdc1
  arduino-cli compile --fqbn "$FQBN" --output-dir /tmp/build bt_earphone
  cp /tmp/build/*.uf2 /media/$USER/RPI-RP2/ && sync
  ```

書き込み後は `2e8a:f00a Raspberry Pi Pico W` として `/dev/ttyACM0` に出てくる。

```bash
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

USB CDC なのでボーレートの値自体に意味はない。

## 5. lcd1602_hello — LCD に文字を表示する

- 1行目: `Hello, Pico WH!`（シリアルから送った文字列に置き換わる）
- 2行目: 起動からの経過秒数

```
+----------------+
|Hello, Pico WH! |
|uptime 12s      |
+----------------+
```

起動時に I2C をスキャンするので、アドレスの設定は不要（PCF8574 は `0x27`、PCF8574A は `0x3F` が多い）。

```
I2C scan...
  found: 0x27
LCD address: 0x27
Type text and press Enter to show it on the LCD.
```

`no device found` が出た場合、スケッチは既定の `0x27` にフォールバックして動き続ける。
シリアルは正常に見えるのに LCD に何も出ないときは配線を疑うこと。

| 定数 | 既定値 | 内容 |
|---|---|---|
| `PIN_SDA` / `PIN_SCL` | 0 / 1 | I2C0 のピン（GP 番号） |
| `LCD_COLS` / `LCD_ROWS` | 16 / 2 | LCD の桁数・行数（2004 なら 20 / 4） |
| `LCD_ADDR_DEFAULT` | 0x27 | スキャンで見つからなかったときに使うアドレス |

## 6. bt_earphone — イヤホンに繋いで通知音を鳴らす

Bluetooth まわりだけを切り出したサンプル。**Wi-Fi も LCD も使わない。**

1. 周囲の A2DP 機器をスキャンして一覧表示
2. `TARGET_NAME` に前方一致する機器（空なら最初の1台）へ接続
3. 接続できたら「ピピッ」というポケベル風の通知音を鳴らし続ける
4. 未接続なら15秒ごとに自動で再スキャン。BOOTSEL でペアリング破棄 + 即再スキャン

```
scanning for 8 seconds...
  [0] WF-1000XM5              38:18:4c:xx:xx:xx  rssi=-52
connecting to [0] WF-1000XM5 ...
  ok
A2DP connected: 38:18:4c:xx:xx:xx
volume: 62%
```

| 定数 | 既定値 | 内容 |
|---|---|---|
| `LOCAL_NAME` | `espoke` | イヤホン側に表示される名前 |
| `TARGET_NAME` | `""` | 接続先の名前（前方一致）。空なら最初に見つかった機器 |
| `TARGET_ADDR` | MACアドレス | **空でなければスキャンせず直接接続する**（推奨） |
| `SCAN_SECONDS` | 8 | スキャン時間 |
| `TONE_HZ` | 880 | 通知音の高さ |
| `TONE_LEVEL` | 6000 | 振幅（16bit なので最大 32767） |

`TARGET_ADDR` を設定すると 8 秒のインクワイアリを飛ばせる。相手がペアリングモードにいる
時間は数分しかないので、アドレスが分かっているなら直接繋いだほうが確実に間に合う。
アドレスは一度スキャンすれば分かる。

```
  [0] OpenRun by Shokz         a0:0c:e2:c6:1d:04  rssi=-52
```

## 6.1 ペアリングの仕組みとハマりどころ

**ここが一番つまずく。** 実機で4つの問題を踏んだので、順に記録しておく。

### `gap_ssp_set_auto_accept(true)` が必須

**これを呼ばないと、入力装置を持たない機器同士では永久に接続できない。**

イヤホンも Pico も画面もキーパッドも持たないので、SSP（Secure Simple Pairing）は
**Just Works** になる。このとき BTstack は `HCI_EVENT_USER_CONFIRMATION_REQUEST` を
アプリに投げるが、**自分では応答しない**（`hci.c` の `ssp_auto_accept` の既定値が 0）。
arduino-pico の `BluetoothAudio` ライブラリもこのイベントを処理していないため、
誰も確認応答を返さないまま止まる。

```cpp
a2dp.begin();
{
  BluetoothLock b;               // BTstack API を叩く前にロックを取る
  gap_ssp_set_auto_accept(true);
}
```

症状は「接続要求は通るが、そのまま無反応」。デバッグ出力を有効にすると、
User Confirmation Request（HCI イベント `0x33`）の直後で止まっているのが見える。

```
EVT <= 2B 04 ... 03 00 04     IO Capability Request Reply (NoInputNoOutput)
EVT <= 32 09 ... 03 00 04     相手も NoInputNoOutput → Just Works
EVT <= 33 0A ... 30 85 06 00  User Confirmation Request ← 誰も応答しない
```

### `connect()` の戻り値は接続成立を意味しない

`A2DPSource::connect()` が返すのは「接続要求が受理されたか」だけ。実際の接続は
**AVDTP シグナリング → SBC コーデック交渉 → ストリーム開始**と非同期に進み、
そこまで到達して初めて `connected()` が `true` になる。戻り値だけで判定すると、
成功しているのに失敗と誤判定する。

```cpp
if (!a2dp.connect(addr)) { /* 要求そのものが弾かれた */ }
unsigned long start = millis();
while (!a2dp.connected() && millis() - start < 15000) delay(50);
```

### `clearPairing()` を毎回呼んではいけない

`clearPairing()` は `gap_delete_all_link_keys()` を呼ぶ。毎回の接続前に呼ぶと
保存済みのリンクキーが消え、**ペアリング済みの機器にも再接続できなくなる**。
明示的にペアリングをやり直したいときだけ呼ぶこと。

### ファームウェアを書き込むとリンクキーは失われる

リンクキーはフラッシュ上の TLV バンクに保存されるが（`btstack_link_key_db_tlv`）、
実機で確認したところ**スケッチを書き込み直すと消える**。書き込みのたびに
イヤホンをペアリングモードに入れ直す必要がある。

## 6.2 接続できないときの調べ方

Bluetooth のデバッグ出力を有効にしてビルドすると、HCI レベルのやり取りが全部出る。

```bash
FQBN="rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble,dbgport=Serial,dbglvl=Bluetooth"
```

`A2DP Source: Connection failed, status 0xNN` のコードで原因が切り分けられる。

| status | 意味 | 原因 |
|---|---|---|
| `0x04` | Page Timeout | 相手が応答しない。**電源オフ・スリープ・他機器に接続中**。範囲外 |
| `0x18` | Pairing Not Allowed | **ペアリングモードに入っていない**。新規の鍵交換を拒否された |
| `0x66` | L2CAP 接続失敗 | 上記の結果として AVDTP のチャンネルが開けなかった（二次的な症状） |

成功時はこう出る。

```
pairing complete, status 00
A2DP Source: Received SBC codec configuration, sampling frequency 44100
A2DP Source: Stream established a2dp_cid 0x08
A2DP Source: Stream started, a2dp_cid 0x08
A2DP connected: A0:0C:E2:C6:1D:04
volume: 80%
```

> **イヤホンは必ずペアリングモードにしてから**接続させること。
> 既にスマホなどと接続済みだと `0x04` か `0x18` で撥ねられる。
> スマホ側の Bluetooth を切ってから試すのが確実。

## 7. espoke — microSD の MP3 をイヤホンで鳴らす

本体。`bt_earphone` に Wi-Fi・LCD・microSD を足したもの。

1. Wi-Fi に接続
2. microSD の MP3 を探して曲の一覧を作る。Bluetooth イヤホンには、**System の画面で SW4 を押したときだけ**つなぐ（7.5 節）
3. **microSD の MP3 を順に流し続ける**。Music の画面の SW1 長押し、BOOTSEL、シリアルの `sd` などで始める（7.6 節）
4. LCD の画面は**ホームから選ぶ**。SW2 / SW3 で Music・Discord・System を選び、SW4 で開く。SW1 で前の画面に戻る（7.5 節）
5. **Music の画面では SW1〜SW4 で再生を操作する**（SW1 長押しで再生 / 一時停止・前の曲・次の曲・シャッフルの入り切り）。曲名と状態も出る（7.5 節）
6. **Discord のメッセージを受け取る**。届いたら Receive の画面に切り替えて出す（8 章）
7. **Send の画面でモールスで打った文を Discord に送る**。空白のあとで enter（空の enter を2回続ける）を押すと送る（7.5 節）
8. System の画面で Wi-Fi と Bluetooth の接続の様子を見る（7.5 節）

### 7.1 設定ファイル

`espoke/arduino_secrets.h` を作る（`.gitignore` 済み。パスワードをコミットしないため）。

```bash
cp espoke/arduino_secrets.h.example espoke/arduino_secrets.h
$EDITOR espoke/arduino_secrets.h
```

```c
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// Discord の中継。tools/discord_relay.py が起動時に表示する行をそのまま書く（8 章）
#define RELAY_HOST "dynabook.tailxxxx.ts.net"
#define RELAY_PORT 8443
#define RELAY_KEY  "your-relay-key"
```

`RELAY_HOST` を書かなければ Discord は使わない。前からある `arduino_secrets.h` に `AUDIO_URL` や `MUSIC_URL` が残っていても、使わないだけでビルドは通る。

### 7.2 音声フォーマットの制約

`A2DPSource` は **44100Hz か 48000Hz の 16bit ステレオ**しか受け取らない。
このスケッチは 44100Hz で動かし、MP3 をデコードした音を整数倍のサンプル&ホールドで引き伸ばして流す。

| 項目 | 対応 |
|---|---|
| 形式 | **MP3（Layer III）のみ**（7.6 節） |
| チャンネル | モノラル / ステレオ（モノラルは左右へ複製） |
| サンプリングレート | **44100 / 22050 / 11025**（44100 の整数分の1のみ） |
| 非対応 | 48000 など割り切れないレート（リサンプラを積む余裕がない）、m4a（AAC） |

48000Hz を通したい場合は `A2DP_RATE` を 48000 にして、素材側も 48000・24000・12000 に揃える。

### 7.3 シリアルコマンド

| コマンド | 動作 |
|---|---|
| `sd` | microSD を読み直し、MP3 を順に**流し続ける**（前回最後に選んだ曲の次から。起動後の初回は先頭から） |
| `pause` | 一時停止 / 再開を切り替える（Music の画面の SW1 長押しと同じ）。止まっているときは `next` と同じく再生を始める |
| `next` | 次の曲へ（再生中でも効く。シャッフル中はランダムな曲へ）。止まっているときは再生を始める |
| `prev` | 前の曲へ |
| `rand` | ランダムな曲へ |
| `stop` | 連続再生をやめる。次に始めると止めた曲の次から（途中から聞き直したいなら `pause`） |
| `list` | 曲一覧をシリアルに出す。番号とパス（`>` が今の曲） |
| `now` | 今かかっている曲を表示 |
| `shuffle` | シャッフルを入り切りする（Music の画面の SW4 と同じ。再生中でも効く） |
| `vol` | 今の音量を表示 |
| `vol <0-100>` | 音量の目盛りを指定（再生中でも効く）。100 が等倍（0dB）で、1 目盛り 0.6dB ずつ下がる。0 は無音。起動時は 35（-39dB） |
| `vol +` / `vol -` | 音量を 5 目盛り（3dB）ずつ上げる / 下げる |
| `wifi off` | Wi-Fi を切り、つなぎ直しもやめる（外で使うとき）。Bluetooth はそのまま |
| `wifi on` | Wi-Fi のつなぎ直しを再開する（連続再生している間は、止めてからつなぐ） |
| `bt` | イヤホンに1回つなぎにいく（System の Bluetooth で SW4 を押したのと同じ。7.5 節） |
| `scan` | ペアリングを破棄して Bluetooth を再スキャン |
| `relay` | Discord の中継につなぎ直す。失敗が続いて延びていた間隔も戻す（8.6 節） |
| `status` | Wi-Fi / IP / Bluetooth / 連続再生 / 一時停止 / シャッフル / 音量 / SD の曲数 / 空きヒープの状態を表示 |

**再生中に受け付けるのは `pause` / `stop` / `next` / `prev` / `rand` / `shuffle` / `vol` / `wifi on` / `wifi off` だけ。**
他のコマンドは曲が終わるまで処理されない。

BOOTSEL ボタンは状況で意味が変わる。

| 状況 | BOOTSEL |
|---|---|
| 止まっているとき | 連続再生を始める |
| 再生中 | 次の曲へ送る（`next`） |

> BOOTSEL の読み取りは一瞬だけフラッシュと割り込みを止めるので、再生中は
> 250ms に1回しか見に行かない。それでも音が乱れるようなら `BOOTSEL_SKIP` を
> `false` にすると、再生中は一切触らなくなる。

### 7.4 主な定数

| 定数 | 既定値 | 内容 |
|---|---|---|
| `BT_LOCAL_NAME` | `espoke` | イヤホン側に表示される名前 |
| `BT_TARGET_NAME` | `""` | 接続先の名前（前方一致）。空なら最初の1台 |
| `BT_TARGET_ADDR` | MACアドレス | 空でなければスキャンせず直接繋ぐ（6章参照） |
| `WIFI_RETRY_MS` | 30000 | Wi-Fi につながらないとき、つなぎ直す間隔。つなぐのは待たずに始めるので、その間も操作できる。連続再生している間はつなぎ直さない（7.6 節） |
| `HTTP_TIMEOUT_MS` | 15000 | 中継の応答を待つ上限。Discord に送るときは、中継が送り終えるまで返事が来ない |
| `A2DP_RATE` | 44100 | A2DP の送出レート。44100 か 48000 |
| `A2DP_BUFFER` | 32768 | 約370ms 分（64KB）。デコードや中継とのやりとりで止まる間を吸収する。**単位はバイトではなく 16bit サンプル数**で、2倍のバイトが確保される。RAM の都合でこれ以上は増やせない（7.6 節） |
| `BOOTSEL_SKIP` | `true` | 再生中の BOOTSEL で次の曲へ送るか |
| `PIN_BUTTON` | 15 | 戻るスイッチをつなぐ GPIO（無くてもよい。SW1 で戻れる） |
| `LONG_PRESS_MS` | 600 | SW1 をこれだけ押し続けると長押し（Music で再生 / 一時停止、Send で戻る） |
| `PIN_SD_MISO` など | 16 / 18 / 19 / 20 | microSD の MISO・CLK・MOSI・CS（[docs/sd_music_parts.md](docs/sd_music_parts.md) の 3 章） |
| `SD_MAX_TRACKS` | 300 | SD から拾う曲数の上限。パスを RAM に持つため（1曲 70 バイト前後） |
| `SD_MAX_DEPTH` | 5 | SD のフォルダを潜る深さ |
| `MSG_MAX` | 8 | 覚えておく Discord のメッセージの数 |
| `MSG_SCROLL_MS` | 300 | 16 文字を超える本文を1文字ずつ流す間隔 |
| `RELAY_RETRY_MS` | 30000 | 中継へのつなぎ直しを試みる間隔。失敗が続くと倍ずつ延ばす（最大 8 分） |
| `RELAY_IDLE_MS` | 90000 | これだけ何も届かなければ中継との接続が切れたとみなす |

### 7.5 画面とスイッチ（ホームと SW1〜SW4）

LCD の画面は**ホーム**から選んで開く。ホームにあるのは Music・Discord・System の3つ。

```
ホーム ─┬─ Music      曲名と再生の様子
        ├─ Discord ─┬─ Receive   受け取ったメッセージを見る
        │           └─ Send      モールスで打って送る
        └─ System     Wi-Fi / Bluetooth（イヤホンにつなぐ）/ 稼働時間と空きヒープ
```

- ホームでは、1行目に選んでいる項目の名前が `←` `→` に挟まれて出る。**SW2 で左、SW3 で右**に動かし（端まで行くと反対の端に回る）、**SW4 で開く**
- 2行目には、選んでいる項目の今の様子が出る（Music なら曲名か状態、Discord なら最新の発言者と時刻、System なら `WiFi ok  BT --` のような接続の有無）
- Discord を開くと、同じ形で Receive（受信）と Send（送信）を選ぶ画面になる
- **SW1 で前（1つ上）の画面に戻る**（Receive・Send → Discord、Music・Discord・System → ホーム）。
  Send の画面だけは SW1 が backspace なので、**長押し（0.6 秒）で戻る**
- **電源を入れると、1 秒ほどでホームが出る**（LCD のライブラリの初期化が 1 秒待つ）。Bluetooth・SD・Wi-Fi の準備はそのあとで、様子は Music の画面に出る。
  Wi-Fi はつながるのを待たずにつなぎ始め、つながると `WiFi ok` と出る。シリアルがつながるのも待たないので、PC でログを見るときは、つながる前の分は出ない

長押しがあるのは SW1 だけ:

- 長押しは離すのを待たずに、0.6 秒たったところで動く。このときは短く押したときの役目（戻る / backspace）はしない
- SW1 を短く押したときの役目は、長押しと区別するため**離したときに**動く
- 長押しで何かするのは Music（再生 / 一時停止）と Send（戻る）。ほかの画面では長押しも短く押したのと同じく戻る
- SW2〜SW4 には長押しが無く、押したときにすぐ動く（トン・ツーはすぐ反応しないと打ちにくい）

GP15 にタクトスイッチを足すと、押すたびに前の画面に戻る（SW1 と同じ。Send でも戻る）。つないでいなければ何もしない。

| スイッチの片側 | スイッチのもう片側 |
|---|---|
| `GP15`（物理20番ピン） | `GND`（物理18番ピンなど） |

- 抵抗は要らない。Pico 内蔵のプルアップ（`INPUT_PULLUP`）を使うので、離すと HIGH・押すと LOW になる
- 4本足のタクトスイッチは、**対角の2本**を使えば向きを間違えない
- チャタリングはソフトで 30ms 待って吸収している
- ユニバーサル基板にはんだ付けして組み立てる手順は [docs/universal_board.md](docs/universal_board.md)

| 画面 | 1行目 | 2行目 |
|---|---|---|
| ホーム / Discord | `←` 選んでいる項目 `→` | 選んでいる項目の今の様子 |
| Music | 曲名 / 状態。シャッフル中は右端に `⇄` の印 | 再生中は帯域ごとの音の大きさの棒（下の説明）。一時停止中や止まっているときは進み具合や状態（一時停止中は後ろに `pause`） |
| Receive | Discord の発言者の名前と時刻（`HH:MM`） | 本文。16 文字を超えると流れる（8.6 節）。まだ無ければ中継とのつながり具合 |
| Send | モールスで入力した文章 | 入力中の符号と、確定したときの文字。送ったあとは送った結果 |
| System（Wi-Fi） | 電波強度（dBm） | IP アドレス |
| System（Bluetooth） | 接続状態 | イヤホンの MAC アドレス（コロン抜き）。つないでいないときは `SW4: connect`、つなぎにいっている間は `connecting...`、失敗したら理由（`BT timeout` など） |
| System（その他） | 起動からの時間 | 空きヒープ |

Music の画面の2行目の棒は、左の 86Hz から右の 16kHz までを対数で16本に分け、それぞれの帯域の強さを8段で出す。
音量（`vol`）を掛ける前の音で見るので、`vol` を変えても棒の高さは変わらない。
A2DP のバッファにはおよそ 370ms 先の音まで入っているので、書いたときではなく、今イヤホンへ送り出している所の棒を出す。
棒が下がるときは 80ms ごとに1段ずつ下げる。

Music 以外の画面を見ている間も曲名や状態は裏で更新しているので、開いたときには最新の内容が出る。
ホーム・Discord・Receive（メッセージが無いとき）・System は1秒ごと、それ以外の画面も5秒ごとに描き直す。
描き直すたびに LCD の 4bit の区切りを合わせ直すので、I2C の通信が化けて画面全体がでたらめになっても、長くて5秒で（スイッチで画面を変えればすぐに）戻る。
Discord のメッセージが届くと、どの画面を見ていても Receive の画面に切り替わる。
このときだけ、SW1（か GP15）で、Discord ではなく元の画面に戻る。
スイッチは再生中も効き、音は途切れない。

SW1〜SW4（GP10〜13）の役目は、開いている画面で変わる。

| 画面 | SW1 | SW1 長押し | SW2 | SW3 | SW4 |
|---|---|---|---|---|---|
| ホーム | （何もしない） | （何もしない） | ← | → | 開く |
| Discord | 戻る | 戻る | ← | → | 開く |
| Music | 戻る | 再生 / 一時停止 | 前の曲 | 次の曲（シャッフル中はランダム） | シャッフルの入り切り |
| Receive | 戻る | 戻る | 古いメッセージ | 新しいメッセージ | （何もしない） |
| Send | backspace | 戻る | トン | ツー | enter |
| System | 戻る | 戻る | ← | → | イヤホンにつなぐ（Bluetooth を見ているとき） |

- 再生の操作はシリアルの `pause` / `prev` / `next` / `shuffle` と同じ。止まっているときに 前の曲 / 次の曲 / 再生 を押すと再生が始まる
- 一時停止中に曲を送ると、一時停止は解けて次の曲が鳴る
- **シャッフル中は、「次の曲」がすべてランダムな曲になる。** SW3 だけでなく、曲が終わって次へ進むとき、BOOTSEL、再生を始めるときも同じ。
  前の曲（SW2）は、曲順で1つ前に戻る。
  起動したときはシャッフルしていない
- System の画面は SW2 / SW3 で Wi-Fi → Bluetooth → その他（稼働時間と空きヒープ）を切り替える。端まで行くと反対の端に回る
- **イヤホンには自分からはつながない。** 起動したときも、切れたときもつなぎ直さない。
  System で Bluetooth を見ながら SW4 を押すと、1回だけつなぎにいく（最大 15 秒。その間は `connecting...`）。
  失敗したら理由が出るので、イヤホンの電源やペアリングモードを確かめてもう一度押す。
  見つからない相手を探し続けると、Bluetooth と Wi-Fi が無線を取り合って Wi-Fi がほとんど通らなくなる（NTP や中継の TLS が失敗する）ため
- イヤホンがつながっていないまま再生を始めると、Music の画面に `no earphone` `System > BT` と出て、つながるまで待つ
- Receive を開くと、最新のメッセージから出る
- Send の enter は、符号があれば文字に確定し、無ければ空白を入れる。**空白のあとでもう一度押すと、文を Discord に送る**。
  つまり最後の文字を確定してから enter を2回押すと送れる（`HI` なら `....` enter `..` enter enter enter）。
  64 文字いっぱいで空白が入らないときは、1回で送る
- 送っている間（4〜8 秒）は2行目に `sending...` と出て、ほかの操作は止まる。再生中なら音もその間だけ止まり、送り終えると続きから鳴る。
  送れたら文が消えて `sent`、送れなければ文は残って `send: <理由>` と出る（8.6 節）。残った文はそのまま enter を押せば送り直せる。
  打ちかけの文は、画面を離れても残る（Discord の画面の Send の2行目に出る）

### 7.6 microSD から聞く

microSD スロット（CK-40）の部品と配線は [docs/sd_music_parts.md](docs/sd_music_parts.md)。
配線を確かめるには、先に `sd_test` を書き込んでシリアルを見る（全部通ると `ALL OK` と出て LED が点く）。

カードの準備:

- FAT32 でフォーマットし、MP3 を入れる。フォルダに分けてよい（5 階層まで潜る）
- 曲は**パスの順**（大文字小文字は区別しない）に並ぶ。アルバムごとのフォルダに、`01-曲名.mp3` のように曲番号を付けておくと、アルバム順・曲順に流れる
- 対応するのは MP3（Layer III）で、サンプリングレートは 44100 の整数分の1（44100 / 22050 / 11025）。普通の音楽の MP3 は 44100 なのでそのまま鳴る
- **m4a（AAC）は鳴らない**。`ffmpeg -i in.m4a -c:a libmp3lame -b:a 192k out.mp3` で MP3 にする
- macOS がコピーのときに作る `._曲名.mp3` は無視する
- カードを挿し直したら `sd` を打つと読み直す

仕組み:

- MP3 は `BackgroundAudio` ライブラリに入っている libmad で、`loop()` の中で1フレーム（1152 サンプル、約26ms）ずつデコードし、A2DP に書く。ライブラリの再生クラス（割り込みでデコードする）は使っていない。再生中もスイッチやコマンドが効く
- 実機（44.1kHz ステレオ 192kbps、CPU 200MHz、`-Os`）では、1フレームのデコードに平均 11ms（実時間の約42%）
- 曲の終わりに `mp3: ... decode avg ... us/frame (..% of real time), longest loop .. ms, underflow .. times` と出る。`longest loop` は `loop()` が1周にかかった最長時間で、`A2DP_BUFFER` の 370ms に近づくと音が切れる。再生中に音が切れると、その場で `underflow at 12s (wifi connected)` のように出る（再生開始から1秒間は、空のバッファが溜まるまでの分なので数えない）
- SD の読み出しは 4MHz（SD ライブラリの既定）で約 375KB/s。320kbps の MP3 でも 40KB/s なので十分
- 連続再生している間は Wi-Fi のつなぎ直しをしない（外で聞いているときに、電波を探す間に音が途切れないように）。外では `wifi off` で切っておくとよい
- **SD から流している間は、Wi-Fi を省電力モード（`WiFi.defaultLowPowerMode()`）にする。** arduino-pico は Wi-Fi につなぐと省電力を切る（`noLowPowerMode()`、常時受信）。そのままだと、同じ CYW43439 の Bluetooth と無線を取り合い、Wi-Fi につながっているときだけ SD の曲がずっと途切れた（`underflow` も出た）。省電力にすると、Wi-Fi につないだままでも途切れなくなった。一度 SD から流したあとは省電力のままにしている（中継の受信が少し遅れるだけで困らない）

RAM（256KB）の使い方には余裕がない。

| 使い道 | 大きさ |
|---|---|
| グローバル変数（BTstack・Wi-Fi・MP3 デコーダの作業領域 約29KB など） | 約146KB |
| `A2DP_BUFFER`（32768 サンプル） | 64KB |
| 残り（Wi-Fi・Bluetooth の動的確保、SD の曲一覧、Discord の中継との TLS など） | 約46KB |

- `A2DP_BUFFER` を以前の 65536 にすると 128KB を確保しようとして `a2dp.begin()` が失敗し、Bluetooth が一切つながらなくなる（`BT rejected` が続く）
- 大きな確保が細切れのヒープで失敗しないよう、`a2dp.begin()` は SD の曲一覧を作る前に呼び、MP3 デコーダの作業領域は最初から静的に持っている

Discord の中継とは TLS でつなぐ（8 章）ので、BearSSL がヒープを使う
（受信バッファ `RELAY_TLS_RX`、BearSSL 用のスタック `RELAY_TLS_STACK`、コンテキストなど）。
Wi-Fi につないだ後の空きは 30KB ほどしかなく、既定の 16KB の受信バッファでは足りない。
そこで受信バッファを **4KB** にしている。
- 本来、16KB より小さくするには、サーバが MFLN（レコードを小さくする取り決め、RFC 6066）に応じる必要がある。
  Funnel の向こうで TLS を受ける Go（tailscaled）は MFLN に応じない
- ただし Go は、データを1レコード 1.2KB ほどに分けて送る（送った量が 128KB を超えるまで）。
  `openssl s_client -msg` で実測すると、一番大きいレコードは証明書の 3430 バイトだった
- Let's Encrypt のチェーンが長くなってつなげなくなったら、`RELAY_TLS_RX` を増やす

BearSSL は、arduino-pico が用意する専用のスタック（`StackThunk`、6400 バイト固定）の上で動く。
P-384 の証明書を確かめると、ここを **6224 バイト**まで使った（余裕 176 バイト）。あふれると、隣にあるヒープを黙って壊す。
実際、最初に試したとき、つないだ直後から SD が応答しなくなり、USB を抜き差しするまで戻らなかった
（あふれたせいかは確かめきれていない）。
そこで、つなぐ前に 8KB（`RELAY_TLS_STACK`）のものに取り替えている（`growTlsStack()`）。
コアには大きさを変える設定がないが、スタックの場所は BearSSL を呼ぶたびに `stack_thunk_top` から読まれるので、BearSSL の外でなら取り替えられる。

つないだときにシリアルに出る行で、ヒープとスタックの余裕が分かる。実機では次のとおりだった。

```
relay: tls 5722 ms, heap 28KB -> 20KB, bearssl stack 6216/8192 bytes
```

A2DP への書き込みは、必ず `A2DP_CHUNK`（2048 サンプル）ずつまとめている。
arduino-pico 6.1.0 の `A2DPSource::write()` には、リングバッファの終わりをまたぐ書き込みで、
後半にデータの先頭部分をもう一度書いてしまう不具合がある（2回目の `memcpy` が `buffer` の先頭から読んでいる）。
MP3 の1フレーム（2304 サンプル）をそのまま書くとバッファを割り切れず、約0.37秒ごとに一部のデータが壊れる。
バッファが空になるわけではないので `warning: audio underflow` は出ない。
（コードを読んで見つけた不具合。実機で最初に途切れたときは上の Wi-Fi の問題も重なっていたので、これだけでどう聞こえるかは確かめていない。）
`A2DP_BUFFER` を変えるときは `A2DP_CHUNK` の倍数にすること（`static_assert` で確かめている）。

## 8. tools/discord_relay.py — Discord とやりとりする

Discord の決めたサーバー（か1つのチャンネル）の発言を、espoke の LCD に出す。
逆に、espoke の Send の画面で打った文を Bot として Discord に送る。

### 8.1 経路

```
   Discord ──Bot── discord_relay.py (127.0.0.1:8001)        dynabook（学校）
                          │ tailscale funnel
                          ▼
            https://dynabook.<tailnet>.ts.net:8443
                          │ インターネット
                   家の Wi-Fi ── Pico WH（BearSSL で HTTPS）
```

- 中継は学校に置いた dynabook で常時動かし、Pico は家の Wi-Fi で受ける。
- Pico は Tailscale に入れない（WireGuard を動かす余裕がなく、tailnet の `100.x.x.x` も `*.ts.net` の名前も使えない）ので、**Funnel で中継をインターネットに公開し、Pico から HTTPS でつなぐ**。
  誰でもつなげてしまうので、合言葉（`RELAY_KEY`）を `X-Key` ヘッダで送らせ、違えば 403 を返す。中継自体は `127.0.0.1` でしか待ち受けない。
- **Funnel は 8443 番を使う。** dynabook の 443 番は、tailnet の中だけに見せる `tailscale serve`（`127.0.0.1:8787`）に使っている。
  443 で funnel すると、その設定を書き換えて 8787 まで公開してしまう。Funnel が使えるのは 443 / 8443 / 10000 番だけ。
- 家に常時動かしておくマシンは要らない。LTE-M（[docs/lte_m_parts.md](docs/lte_m_parts.md)）にしても、BG96 は TLS を内蔵しているので同じ URL が使える。

### 8.2 なぜ中継で変換するのか

LCD1602A の文字 ROM（A00）にあるのは**英数字と半角カタカナ**だけ。その並びは JIS X 0201 で、cp932（Shift_JIS）の1バイト文字と同じ番号になっている。
漢字を読みに直すには形態素解析の辞書が要る（unidic-lite は展開すると 249MB）。Flash が 2MB の Pico には載らない。
そこで中継が「漢字 → 読み → 半角カナ」まで済ませ、LCD にそのまま書けるバイト列にして渡す。

変換の手順:

1. メンションは `@名前`、カスタム絵文字は `:名前:`、URL は `URL`、添付は `[添付]` にする。太字・打ち消し・伏せ字の記号は外す
2. NFKC で正規化する（全角英数は ASCII に、半角カナは全角カナに揃える）
3. MeCab（fugashi + unidic-lite）で語に分け、**漢字を含む語だけ**読みに置き換える
4. ひらがなをカタカナにし、濁点と半濁点を分けてから半角カナにする（`ガ` → `ｶﾞ`）。半角に無い `ヶ` などは近い字で代用する
5. ROM に無い文字のうち、文字（ハングルや、読みが分からなかった漢字）は `?` にし、絵文字や記号は捨てる。
   `~` は ROM では `→` になるので `-` にする（`\` は `¥` で出る）

| 元の発言 | LCD |
|---|---|
| 今日は雨。ガンダム、ＡＢＣ😀 | `ｷｮｳﾊｱﾒ｡ｶﾞﾝﾀﾞﾑ､ABC` |
| 明日10時に駅前で待ち合わせ！ | `ｱｽ10ｼﾞﾆｴｷﾏｴﾃﾞﾏﾁｱﾜｾ!` |
| discordのbotを作った | `discordﾉbotｦﾂｸｯﾀ` |
| café 한국어 ✨ | `cafe ???` |

- 助詞の「は」「を」は、発音ではなく字のとおり `ﾊ` `ｦ` で出る
- 読みは辞書の判断なので外れることがある（`明日` は `ｱｽ`）。人名も外れやすい
- 変換だけを試すには `--convert` を使う（8.5 節）

### 8.3 Bot を作る

1. [Discord Developer Portal](https://discord.com/developers/applications) で New Application を作り、Bot のページを開く
2. Reset Token でトークンを出す（一度しか表示されないので控える）
3. 同じページの **Message Content Intent を ON** にする。OFF のままだと本文が空で届く
4. OAuth2 → URL Generator で scope に `bot`、権限に View Channels と Read Message History と Send Messages を選び、出てきた URL で自分のサーバーに招待する
   （受け取るだけで招待済みなら、Discord のサーバー設定 → ロール で Bot のロールに「メッセージを送信」を足す）
5. Discord の 設定 → 詳細設定 で開発者モードを ON にし、受け取るサーバーのアイコンを右クリック → サーバー ID をコピー
   （1つのチャンネルだけにするなら、チャンネルを右クリック → チャンネル ID をコピー）

サーバー全体を受け取ると、Bot が見られるチャンネルの発言が全部届く。届かせたくないチャンネルは、Discord 側でそのチャンネルの Bot の閲覧権限を外す。
発言が多いサーバーだと、Pico が覚えている 8 件がすぐ入れ替わり、Receive の画面にも頻繁に切り替わる。

espoke から送った文は、Bot の発言としてチャンネルに出る。送り先は次の順に決まる。

1. `DISCORD_SEND_CHANNEL_ID`（8.4 節）のチャンネル
2. `DISCORD_CHANNEL_ID` のチャンネル（1チャンネルだけ受け取るとき）
3. 最後に発言を受け取ったチャンネル（サーバー全体を受け取るとき。直前の会話に返事をする形になる）

`@everyone` などのメンションは通知しない（合言葉を知っていれば誰でも送れるため）。Bot 自身の発言は受け取らないので、送った文は espoke には戻ってこない。

### 8.4 dynabook 側の準備

```bash
ssh dynabook mkdir -p '~/espoke'
scp tools/discord_relay.py dynabook:~/espoke/
ssh dynabook
python3 -m venv ~/espoke/venv
~/espoke/venv/bin/pip install discord.py 'fugashi[unidic-lite]'
```

設定は `~/espoke/relay.env` に書く（トークンが入るのでリポジトリには入れない）。

```bash
cat > ~/espoke/relay.env <<'END'
DISCORD_TOKEN=Bot のトークン
DISCORD_GUILD_ID=123456789012345678     # サーバー全体
# DISCORD_CHANNEL_ID=123456789012345678 # 1チャンネルだけならこちら (両方あればこちらが効く)
# DISCORD_SEND_CHANNEL_ID=123456789012345678  # espoke から送る先 (無ければ 8.3 節の順で決まる)
RELAY_KEY=合言葉
END
chmod 600 ~/espoke/relay.env
```

`RELAY_KEY` は `openssl rand -hex 16` などで作る。

```bash
~/espoke/venv/bin/python ~/espoke/discord_relay.py
```

起動すると、`arduino_secrets.h` にそのまま書ける行を表示する（7.1 節）。

```
  http://127.0.0.1:8001/

公開していなければ:  tailscale funnel --bg --https=8443 8001

espoke/arduino_secrets.h に書く:
  #define RELAY_HOST "dynabook.tailxxxx.ts.net"
  #define RELAY_PORT 8443
  #define RELAY_KEY  "..."
discord: espoke#1234 として サーバー 自分のサーバー の 5 チャンネル を見ている
```

Funnel で公開する。

```bash
tailscale funnel --bg --https=8443 8001   # 権限で弾かれたら sudo を付ける
tailscale funnel status
```

- 初めてのときは、Funnel を許可するためのリンクが出る。tailnet の管理画面で Funnel を有効にする
- `--bg` を付けると設定が残り、dynabook を再起動しても公開が続く
- 443 番の `tailscale serve` はそのまま残る。`tailscale funnel status` に両方出る

常時動かすには systemd の user unit にする。

```ini
# ~/.config/systemd/user/espoke-relay.service
[Unit]
Description=espoke Discord relay
After=network-online.target

[Service]
ExecStart=%h/espoke/venv/bin/python %h/espoke/discord_relay.py
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
```

```bash
systemctl --user daemon-reload
systemctl --user enable --now espoke-relay
sudo loginctl enable-linger $USER         # ログインしていなくても動かす
journalctl --user -u espoke-relay -f      # 届いた発言は "msg ..." と出る
```

### 8.5 エンドポイント

| URL | 動作 |
|---|---|
| `GET /stream?after=<ID>` | `after` より新しい発言を送り、接続を閉じずに新着を流し続ける。`after` が無ければ直近 8 件から |
| `POST /stream?after=<ID>` | 本文（UTF-8）を Discord に送ってから、`GET /stream` と同じく流す。送った結果は `X-Sent` ヘッダ（下の表） |
| `POST /post` | 本文（UTF-8）を差出人 `test` の発言として流す。Discord を使わずに試すとき |
| `GET /` | 動いているかの確認 |

`/stream` と `/post` には `X-Key: <RELAY_KEY>` ヘッダが要る（無いか違えば 403）。

`POST /stream` は espoke が文を送るときに使う。Pico はつなぐたびに TLS の握手（4〜8 秒）をするので、
送る用の接続を別に張らず、**送るのと受け取り直すのを1回の接続で済ませる**。
`--no-discord` のときは Discord の代わりに、差出人 `espoke` の発言として Pico へ流し返す（Pico の送信を試すとき）。

| `X-Sent` | 意味 |
|---|---|
| `ok` | 送れた |
| `empty` | 本文が空（空白だけ） |
| `no channel` | 送り先が決まらない。サーバー全体を受け取っていて、`DISCORD_SEND_CHANNEL_ID` が無く、中継を起動してから発言を1件も受け取っていない |
| `not ready` | 中継が Discord につながる前 |
| `discord 403` | Bot にそのチャンネルへ送る権限が無い（8.3 節） |
| `discord 404` | `DISCORD_SEND_CHANNEL_ID` か `DISCORD_CHANNEL_ID` が違う |
| `timeout` | Discord が 10 秒以内に応えなかった |

1件は1行で、cp932 のバイト列。

```
<メッセージ ID> TAB <HH:MM> TAB <名前> TAB <本文> LF
```

- ID は Discord のメッセージ ID（snowflake）。時間とともに増えるので、Pico は最後に受け取った ID を `after` に付けてつなぎ直せば、切れていた間の分も受け取れる。
  中継を立ち上げ直しても、起動時に各チャンネルの履歴を読み、新しいものから 8 件を持ち直す
- 名前は 10 バイト、本文は 200 バイトで切る。時刻は日本時間
- サーバー全体を受け取るときは、本文の頭に `#チャンネル名 ` を付ける（例: `#ｻﾞﾂﾀﾞﾝ ｷｮｳﾊｱﾒ`）。LCD の1行目は名前と時刻でいっぱいなので、流れる2行目に入れる。
  チャンネル名は 8 バイトで切り、頭の絵文字や区切り（`💬｜雑談` の `💬｜`）は落とす
- つないだ直後は、溜まっていた分のあとに**空行を1つ**送る。Pico はここまでを「つなぐ前からあった分」とみなし、起動直後に古い発言で着信表示しない
- その後は、何もない間 30 秒ごとに空行を送る。Pico は 90 秒何も届かなければ切れたとみなす
- HTTP/1.0 で返す。Pico も HTTP/1.0 で頼むので、Funnel の手前の Go のプロキシは chunked にせず、届いた分をすぐ流す

PC から試す。`iconv` は入力が終わるまで溜め込むので、流れを見るには Python で1行ずつ変換する。

```bash
KEY=合言葉
URL=https://dynabook.tailxxxx.ts.net:8443
curl -s --http1.0 -N -H "X-Key: $KEY" $URL/stream \
  | python3 -c 'import sys
for l in sys.stdin.buffer: print(l.decode("cp932"), end="", flush=True)'
curl -H "X-Key: $KEY" --data-binary 'テストです' $URL/post   # 別の端末から
curl -si --http1.0 -N -H "X-Key: $KEY" --data-binary 'HELLO' $URL/stream | head -8   # Discord に送る (X-Sent を見る)

~/espoke/venv/bin/python ~/espoke/discord_relay.py --convert '今日は雨'  # 変換だけ
~/espoke/venv/bin/python ~/espoke/discord_relay.py --no-discord        # Discord なしで /post だけ
```

### 8.6 Pico 側の動き

- 起動後の最初の `loop()` で中継につなぐ。切れたら 30 秒ごとにつなぎ直す。つなげないことが続くと、間隔を倍ずつ延ばす（最大 8 分）。中継が落ちていると握手のタイムアウト（15 秒）まで待つので、曲の合間が何度も止まらないようにするため。中継を直したあとすぐにつなぎ直させるには、シリアルで `relay` を打つ
- **TLS の握手に 4〜8 秒かかる**（実測。Let's Encrypt の P-384 の証明書を確かめるのが重い）。A2DP のバッファ（370ms）では持たないので、つなぐのは止まっている間か曲の合間だけにしている。
  つなぐ前に A2DP のバッファを無音で埋めるので、つなぎ直すときだけ曲の合間が 0.4 秒と握手の分（数秒）だけ空く
- 受け取るときは待たずに、届いた分だけ読む。再生中も届く（SD の再生中は Wi-Fi が省電力なので少し遅れる）
- 証明書は `espoke/relay_ca.h` のルート CA（Let's Encrypt の Root YE / ISRG Root X2 / ISRG Root X1）で確かめる。
  期限を確かめるのに時計が要るので、最初につなぐ前に NTP（`ntp.nict.jp`）で合わせる。
  Wi-Fi につないだ直後は合わせられないことがあるので、失敗したら 10 秒後にやり直す（中継の失敗とは数えず、間隔も延ばさない）
- 直近 8 件を覚えておく。届いたら**Receive の画面に切り替える**（7.5 節）。SW1（か GP15）で元の画面に戻る
- 1行目は名前と時刻、2行目は本文。16 文字を超える本文は 0.3 秒ごとに1文字ずつ流し、頭と末尾で 1.5 秒止まる
- SW2 で古いメッセージ、SW3 で新しいメッセージを見る
- シリアルには `relay: <ID> <時刻> <名前>` と出る（名前は半角カナのバイト列なので化ける）。
  `status` には `relay=1 (no message) msgs=3` のように、つながり具合と覚えている件数が出る
- `wifi off` にすると中継も切る
- Send の画面で送ると決めたら（7.5 節）、つながっていても**中継につなぎ直し、`POST /stream` で文を送る**。返事の流れをそのまま受け取り続けるので、送ったあとに受け取り直す握手は要らない。
  曲の途中でも待たずに送る（その間は無音）。シリアルには `send: ok` のように結果が出る

送った結果は、Send の画面の2行目に出る。

| 表示 | 意味 |
|---|---|
| `sending...` | 送っている |
| `sent` | 送れた。打った文は消える |
| `send: discord 403` など | 中継から返った理由（8.5 節の `X-Sent`）。打った文は残る |
| `send: relay error` など | 中継につなげなかった（下の表と同じ） |
| `send: no wifi` | Wi-Fi につながっていない |
| `no RELAY_HOST` | `arduino_secrets.h` に `RELAY_HOST` が無い |

メッセージがまだ1件も無いとき、Receive の画面の2行目（とホームで Discord を選んだときの2行目）にはつながり具合が出る。

| 表示 | 意味 |
|---|---|
| `connecting` | 起動直後。まだつなぎにいっていない |
| `no message` | つながっている。まだ発言が無い |
| `relay error` | TLS でつなげなかった。シリアルに `relay: connect failed (ssl <番号> ...)` が出る |
| `relay 403` など | 中継に断られた。`RELAY_KEY` が違う |
| `relay no reply` | 応答のヘッダが来なかった |
| `relay lost` / `relay silent` | 切れた / 90 秒何も届かなかった。30 秒以内につなぎ直す |
| `ntp failed` | 時計を合わせられなかった。10 秒後にやり直す |
| `wifi off` | `wifi off` で切った |
| `no RELAY_HOST` | `arduino_secrets.h` に `RELAY_HOST` が無い |

## 9. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| `BluetoothAudio.h: No such file` / `_needsbt.h` のエラー | FQBN に `ipbtstack=ipv4btcble` が入っていない |
| `No drive to deploy.` で書き込めない | 自動リセットが効かず BOOTSEL に入っていない。`lsusb` が `2e8a:f00a`（スケッチ実行中）のままなら未リセット。`usermod -aG dialout` 後に再ログインしていないのが原因のことが多く、`id` で確認して `sg dialout -c "arduino-cli upload ..."` で叩く |
| `'Msg' has not been declared` など、自分で定義した構造体が見つからない | `.ino` はビルド時に関数プロトタイプが先頭へ自動生成される。引数に使う構造体は**ファイル冒頭**で定義する |
| スキャンに何も出ない | イヤホンがペアリングモードになっていない。既に他機器と接続済みだと出てこない。スマホ側の接続を切る |
| **接続要求は通るが無反応のまま固まる** | **`gap_ssp_set_auto_accept(true)` を呼んでいない。**6.1 参照。これが最頻出 |
| `Connection failed, status 0x04` | Page Timeout。イヤホンの電源が入っていない、スリープ、他機器に接続中 |
| `Connection failed, status 0x18` | Pairing Not Allowed。ペアリングモードに入っていない |
| 書き込み直したら繋がらなくなった | リンクキーは書き込みで消える。ペアリングモードに入れ直す |
| `connect()` が false を返すが実は繋がっている | 戻り値は要求の受理可否のみ。`connected()` を待つ（6.1 参照） |
| 一度失敗すると以降ずっと `rejected` | 中途半端なシグナリング接続が残り `a2dp_cid` が埋まっている。再起動で解消 |
| 起動直後から `rejected` が続き、`status` の `heap` が 100KB を超えている | `a2dp.begin()` がメモリ不足で失敗している（起動時に `BT init failed` と出る）。`A2DP_BUFFER` を大きくしすぎていないか（7.6 節） |
| SD の曲が Wi-Fi につながっているときだけ途切れる（`underflow at ...` が出る） | Wi-Fi が常時受信になっていて Bluetooth と無線を取り合っている。SD から流す間は省電力にしているか（7.6 節）。`wifi off` で切ると確実 |
| SD の曲が途切れる（underflow は出ない） | A2DP への書き込みが `A2DP_CHUNK` 単位になっていない。7.6 節の `A2DPSource::write()` の不具合 |
| `sd` で `no card` | カードが奥まで入っていない、または配線。`sd_test` で確かめる |
| `sd` で `no mp3` | MP3 がない。m4a は対象外（7.6 節）。`SD_MAX_DEPTH` より深いフォルダも探さない |
| 接続はするが音が出ない | イヤホン側の音量。`volume:` のログが出ていれば A2DP は繋がっている |
| `warning: audio underflow` | デコードか SD の読み出しが追いついていない。曲の終わりに出る `mp3: ...` の行の `longest loop` を見る。`A2DP_BUFFER` は RAM の都合で増やせない（7.6 節） |
| イヤホンにつなぎにいっている間、Wi-Fi・NTP・中継が失敗する | Bluetooth が相手を探している間（最大 15 秒）は、Wi-Fi と無線を取り合う。つながるか諦めれば戻る。イヤホンが無いときは System で SW4 を押さない（7.5 節） |
| Wi-Fi に繋がらないことがある | 起動時の1回目はつながらないことが多い。`WIFI_RETRY_MS`（30 秒）ごとに `loop()` からつなぎ直している |
| LCD の画面全体がでたらめな記号やカナになる | I2C の通信が1回化けて、LCD が 4bit の上下を取り違えている。描き直すときに合わせ直すので、長くて5秒で戻る（7.5 節）。何度も起きるなら、SDA・SCL の線を短くする、レベル変換の配線を確かめる |
| LCD の曲名が化ける / 出ない | 曲名はファイル名をそのまま出すので、日本語など LCD1602A の文字 ROM に無い文字は化ける。英数字のファイル名にする。英数字でも化けるならコントラスト調整を疑う |
| `unsupported: 48000Hz` | 7.2 のフォーマット制約。`ffmpeg -i in.mp3 -ar 44100 -c:a libmp3lame -b:a 192k out.mp3` で 44100Hz にする |
| Wi-Fi に繋がらない | Pico W は **2.4GHz のみ**。5GHz 専用の SSID には繋がらない |
| バックライトも点かない | VCC/GND の配線、`VBUS`（pin40）から 5V が来ているか確認 |
| バックライトは点くが何も表示されない | コントラスト調整（青い半固定抵抗を回す） |
| 上段に黒い四角が並ぶだけ | I2C 通信できていない。SDA/SCL の入れ違い、GND 共通化を確認 |
| `ValueError` / I2C が動かない | I2C0/I2C1 で使えないピンを指定している。0章の対応表を参照 |
| `/dev/ttyACM0` が出てこない | 充電専用の micro-B ケーブルになっていないか確認。USBハブ経由をやめて PC 本体に直挿し |
| `Permission denied` | `dialout` グループに入っていない。`sg dialout -c "..."` なら再ログインせずに通る |
| 書き込みが始まらない | BOOTSEL ボタンを押しながら USB を挿し直す。`RPI-RP2` ドライブが出たら手動で `.uf2` をコピー |
| オンボード LED が光らない | Pico W/WH の LED は GP25 ではない。`LED_BUILTIN` を使う |
| 日本語が表示できない | LCD1602A は英数字とカタカナ（独自コード）のみ。漢字・ひらがなは不可 |
| Receive の画面のカナが記号やキリル文字になる | LCD の文字 ROM が A02（欧文）。半角カナが出るのは A00（日本語）の LCD1602A だけ |
| `relay: connect failed (ssl -1000 ...)` が出る、または `heap` が 15KB を切っている | TLS に使う RAM が足りない（7.6 節）。つなぐには 10KB ほど要る。SD の曲数（`SD_MAX_TRACKS`）を減らすなどして空ける |
| `relay: connect failed` が出るが、`heap` は足りている | 時計が合っていない（`ntp failed`）、Funnel が止まっている、Let's Encrypt がルート CA を替えた（`relay_ca.h` を更新する）のどれか。PC から 8.5 節の curl を試す |
| `relay 403` | `RELAY_KEY` が dynabook の `relay.env` と違う |
| 発言しても本文が空で届く | Developer Portal で Message Content Intent が OFF（8.3 節） |
| espoke から送ると `send: discord 403` | Bot に「メッセージを送信」の権限が無い（8.3 節） |
| espoke から送ると `send: relay 404` | dynabook の `discord_relay.py` が古い（`POST /stream` が無い）。8.4 節の `scp` で入れ直して再起動する |
| PC の curl で、発言してもすぐに出ない | `--http1.0` か `-N` を付けていない。`iconv` を通していると、終わるまで溜め込まれる（8.5 節） |
