# espoke

Raspberry Pi Pico WH で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **Raspberry Pi Pico WH**（RP2040 / Wi-Fi・Bluetooth 付き / ピンヘッダ実装済み）。
開発は **arduino-pico（C++）+ arduino-cli** で行う。Arduino IDE は使わない。

やりたいこと:

- LCD1602A にメッセージを表示する
- **Bluetooth イヤホンに接続して通知音を鳴らす**（A2DP）
- 鳴らす音声を **Wi-Fi 経由で取得する**

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | LCD1602A（I2C）の表示サンプル |
| `bt_earphone/` | Bluetooth イヤホンに接続して通知音を鳴らすサンプル |
| `espoke/` | 本体。Wi-Fi で WAV を取得して Bluetooth イヤホンで再生し、LCD に状態を出す |
| `tools/` | PC 側で音声を用意して HTTP 配信する補助スクリプト |

ビルド実測値（`rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble`）:

| スケッチ | Flash | RAM（グローバル） |
|---|---|---|
| `lcd1602_hello` | 319KB / 2093KB (15%) | 69KB / 256KB (26%) |
| `bt_earphone` | 514KB (24%) | 96KB (36%) |
| `espoke` | 578KB (27%) | 103KB (39%) |

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

- GP0 / GP1 は I2C0（`Wire`）の組み合わせ。スケッチもこの値で書いてある。
- `VBUS`（pin40）は **USB 給電中のみ 5V**。電池駆動に変える場合は 5V を別途用意する。
- GND は必ず全部共通にする。
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

## 7. espoke — Wi-Fi で取得した音声をイヤホンで鳴らす

本体。`bt_earphone` に Wi-Fi と LCD を足したもの。

1. Wi-Fi に接続
2. Bluetooth イヤホンをスキャンして接続
3. シリアルに `play` と打つか BOOTSEL を押すと、`AUDIO_URL` から WAV を取得して再生
4. LCD に状態を表示

### 7.1 設定ファイル

`espoke/arduino_secrets.h` を作る（`.gitignore` 済み。パスワードをコミットしないため）。

```bash
cp espoke/arduino_secrets.h.example espoke/arduino_secrets.h
$EDITOR espoke/arduino_secrets.h
```

```c
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"
#define AUDIO_URL "http://192.168.1.10:8000/notify.wav"
```

### 7.2 音声フォーマットの制約

`A2DPSource` は **44100Hz か 48000Hz の 16bit ステレオ**しか受け取らない。
このスケッチは 44100Hz で動かし、取得した WAV を整数倍のサンプル&ホールドで引き伸ばして流す。

| 項目 | 対応 |
|---|---|
| 形式 | **16bit PCM の WAV のみ**（`fmt` タグ 1）。MP3 などは不可 |
| チャンネル | モノラル / ステレオ（モノラルは左右へ複製） |
| サンプリングレート | **44100 / 22050 / 11025**（44100 の整数分の1のみ） |
| 非対応 | 48000、8bit、24bit、可変長リサンプルが要るもの |

48000Hz を通したい場合は `A2DP_RATE` を 48000 にして、素材側も 48000・24000・12000 に揃える。

22050Hz モノラルなら 44.1KB/s なので、Wi-Fi と Bluetooth を同時に使っても余裕がある。
44100Hz ステレオ（176KB/s）は帯域が厳しく、`warning: audio underflow` が出やすい。

実機での動作ログ。

```
wifi=1 ip=192.168.40.104 bt=1
playing 22050Hz 1ch 16bit, 35280 bytes (x2 upsample)
[done] 34KB played
```

### 7.3 シリアルコマンド

| コマンド | 動作 |
|---|---|
| `play` | `AUDIO_URL` を取得して再生 |
| `play <url>` | 指定した URL を再生 |
| `scan` | ペアリングを破棄して Bluetooth を再スキャン |
| `status` | Wi-Fi / IP / Bluetooth の接続状態を表示 |

BOOTSEL ボタンでも `play` と同じことができる。

### 7.4 主な定数

| 定数 | 既定値 | 内容 |
|---|---|---|
| `BT_LOCAL_NAME` | `espoke` | イヤホン側に表示される名前 |
| `BT_TARGET_NAME` | `""` | 接続先の名前（前方一致）。空なら最初の1台 |
| `BT_TARGET_ADDR` | MACアドレス | 空でなければスキャンせず直接繋ぐ（6章参照） |
| `WIFI_TIMEOUT_MS` | 30000 | Wi-Fi 接続を待つ上限 |
| `WIFI_RETRY_MS` | 30000 | Wi-Fi が切れているとき再接続を試みる間隔 |
| `A2DP_RATE` | 44100 | A2DP の送出レート。44100 か 48000 |
| `A2DP_BUFFER` | 32768 | 約185ms 分。通信のゆらぎを吸収する |

## 8. tools/serve_audio.py — PC 側で音声を配る

Pico に渡す WAV を用意して HTTP で配信する。

```bash
python3 tools/serve_audio.py
```

- `tools/audio/notify.wav` がなければ、ポケベル風の「ピピッ」（22050Hz モノラル 16bit / 0.8秒）を生成する
- そのディレクトリを `0.0.0.0:8000` で配信し、**LAN 側の IP を含む URL を表示する**
- 表示された URL をそのまま `AUDIO_URL` に書く

```
serving /home/you/ssd/electronic/espoke/tools/audio on http://192.168.40.115:8000/
  http://192.168.40.115:8000/notify.wav
```

手持ちの音声を使う場合は、対応フォーマットに変換して同じディレクトリに置く。

```bash
ffmpeg -i source.mp3 -ac 1 -ar 22050 -sample_fmt s16 tools/audio/voice.wav
```

`--dir` で別のディレクトリを配信することもできる。生成物は `.gitignore` 済み。

## 9. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| `BluetoothAudio.h: No such file` / `_needsbt.h` のエラー | FQBN に `ipbtstack=ipv4btcble` が入っていない |
| `'WavInfo' has not been declared` | `.ino` はビルド時に関数プロトタイプが先頭へ自動生成される。引数に使う構造体は**ファイル冒頭**で定義する |
| スキャンに何も出ない | イヤホンがペアリングモードになっていない。既に他機器と接続済みだと出てこない。スマホ側の接続を切る |
| **接続要求は通るが無反応のまま固まる** | **`gap_ssp_set_auto_accept(true)` を呼んでいない。**6.1 参照。これが最頻出 |
| `Connection failed, status 0x04` | Page Timeout。イヤホンの電源が入っていない、スリープ、他機器に接続中 |
| `Connection failed, status 0x18` | Pairing Not Allowed。ペアリングモードに入っていない |
| 書き込み直したら繋がらなくなった | リンクキーは書き込みで消える。ペアリングモードに入れ直す |
| `connect()` が false を返すが実は繋がっている | 戻り値は要求の受理可否のみ。`connected()` を待つ（6.1 参照） |
| 一度失敗すると以降ずっと `rejected` | 中途半端なシグナリング接続が残り `a2dp_cid` が埋まっている。再起動で解消 |
| 接続はするが音が出ない | イヤホン側の音量。`volume:` のログが出ていれば A2DP は繋がっている |
| `warning: audio underflow` | Wi-Fi が追いついていない。素材を 22050Hz モノラルに落とすか、`A2DP_BUFFER` を増やす |
| Wi-Fi に繋がらないことがある | 起動時の1回だけでは不安定。`WIFI_RETRY_MS` ごとに `loop()` から張り直している |
| 音がブツブツ切れる | 同上。Wi-Fi と Bluetooth が CYW43439 を共有しているため。ルータとの距離も効く |
| `http error 404` など | `AUDIO_URL` の IP が PC の LAN IP になっているか確認。`tools/serve_audio.py` が表示する URL を使う |
| `unsupported: 48000Hz 2ch 16bit` | 7.2 のフォーマット制約。`ffmpeg -ar 22050 -ac 1` で変換する |
| `bad wav header` | WAV ではない（MP3 を .wav にリネームしただけ等）。`file` コマンドで確認 |
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
