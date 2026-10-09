# espoke

Raspberry Pi Pico WH で作るポケベル（pokebell）。電子工作の配線資料とファームウェアを置くリポジトリ。

現在のターゲットは **Raspberry Pi Pico WH**（RP2040 / Wi-Fi・Bluetooth 付き / ピンヘッダ実装済み）。
開発は **arduino-pico（C++）+ arduino-cli** で行う。Arduino IDE は使わない。

やりたいこと:

- LCD1602A にメッセージを表示する
- **Bluetooth イヤホンに接続して通知音を鳴らす**（A2DP）
- 鳴らす音声を **Wi-Fi 経由で取得する**
- **PC に置いた音楽ファイル（mp3 / m4a）を Wi-Fi 経由で聞く**
- **microSD に入れた MP3 を、ネットのない外でも聞く**
- **Discord のメッセージを受け取って LCD に出す**（漢字は読みに直して半角カナで出す）

## 構成

| ディレクトリ | 内容 |
|---|---|
| `lcd1602_hello/` | LCD1602A（I2C）の表示サンプル |
| `morse_input/` | 4つのスイッチ（GP10〜13）でモールス信号を打ち、アルファベットを LCD に入力するサンプル |
| `bt_earphone/` | Bluetooth イヤホンに接続して通知音を鳴らすサンプル |
| `espoke/` | 本体。microSD の MP3 や、Wi-Fi で取得した WAV を Bluetooth イヤホンで再生し、LCD に状態を出す。SW1〜SW4 で再生の操作とモールス入力ができる。Discord のメッセージも受け取って出す |
| `sd_test/` | microSD スロット（CK-40）の配線・カード・ファイル一覧・読み書きを確かめるテスト |
| `docs/` | 部品の取り付け手順など |
| `tools/` | PC 側で通知音や音楽ライブラリを HTTP 配信する補助スクリプトと、Discord の中継 |

ビルド実測値（`rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble`）:

| スケッチ | Flash | RAM（グローバル） |
|---|---|---|
| `lcd1602_hello` | 319KB / 2093KB (15%) | 69KB / 256KB (26%) |
| `bt_earphone` | 514KB (24%) | 96KB (36%) |
| `espoke` | 779KB (38%) | 146KB (56%) |

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

## 7. espoke — microSD や Wi-Fi の音声をイヤホンで鳴らす

本体。`bt_earphone` に Wi-Fi・LCD・microSD を足したもの。

1. Wi-Fi に接続
2. Bluetooth イヤホンをスキャンして接続し、microSD の MP3 を探して曲の一覧を作る
3. シリアルに `sd` と打つと、**microSD の MP3 を順に流し続ける**（7.6 節）
4. シリアルに `music` と打つと、`MUSIC_URL` の音楽サーバから曲を順に取ってきて流し続ける
5. BOOTSEL を押すと、最後に選んだ方（起動直後は SD に曲があれば SD）で連続再生を始める
6. シリアルに `play` と打つと `AUDIO_URL` の通知音を1回だけ鳴らす
7. LCD に曲名と状態を表示。**GP15 のスイッチで表示ページを切り替える**（7.5 節）
8. **SW1〜SW4 で再生を操作する**（再生 / 一時停止・前の曲・次の曲・ランダム）。入力ページではモールス入力になる（7.5 節）
9. **Discord のメッセージを受け取る**。届いたらメッセージページに切り替えて出す（10 章）

### 7.1 設定ファイル

`espoke/arduino_secrets.h` を作る（`.gitignore` 済み。パスワードをコミットしないため）。

```bash
cp espoke/arduino_secrets.h.example espoke/arduino_secrets.h
$EDITOR espoke/arduino_secrets.h
```

```c
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// 通知音。tools/serve_audio.py が配る
#define AUDIO_URL "http://192.168.1.10:8000/notify.wav"

// 音楽サーバ。tools/serve_music.py が配る。末尾にスラッシュは付けない
#define MUSIC_URL "http://192.168.1.20:8000"

// Discord の中継。tools/discord_relay.py が起動時に表示する行をそのまま書く（10 章）
#define RELAY_HOST "dynabook.tailxxxx.ts.net"
#define RELAY_PORT 8443
#define RELAY_KEY  "your-relay-key"
```

`RELAY_HOST` を書かなければ（前からある `arduino_secrets.h` のままなら）Discord は受け取らない。

### 7.2 音声フォーマットの制約

`A2DPSource` は **44100Hz か 48000Hz の 16bit ステレオ**しか受け取らない。
このスケッチは 44100Hz で動かし、取得した WAV を整数倍のサンプル&ホールドで引き伸ばして流す。

| 項目 | 対応 |
|---|---|
| 形式 | Wi-Fi: **16bit PCM の WAV のみ**（`fmt` タグ 1）。microSD: **MP3 のみ**（7.6 節） |
| チャンネル | モノラル / ステレオ（モノラルは左右へ複製） |
| サンプリングレート | **44100 / 22050 / 11025**（44100 の整数分の1のみ） |
| 非対応 | 48000、8bit、24bit、可変長リサンプルが要るもの |

48000Hz を通したい場合は `A2DP_RATE` を 48000 にして、素材側も 48000・24000・12000 に揃える。

Wi-Fi と Bluetooth は CYW43439 を共有しているので、HTTP の帯域は素直に効く。

| 形式 | 帯域 | 用途 |
|---|---|---|
| 22050Hz モノラル | 44 KB/s | 通知音。余裕がある |
| **44100Hz モノラル** | **88 KB/s** | **音楽の既定。**高音を削らずに帯域を半分にできる |
| 44100Hz ステレオ | 176 KB/s | 電波が良ければ。`serve_music.py --stereo` |

音楽で帯域を削るなら、22050Hz に落として高音を捨てるよりモノラルにするほうが音の劣化が小さい。
ステレオが欲しくて `warning: audio underflow` が出るなら、ルータとの距離を詰める
（`A2DP_BUFFER` は RAM の都合でこれ以上増やせない。7.6 節）。

実機での動作ログ。

```
wifi=1 ip=192.168.40.104 bt=1
playing 22050Hz 1ch 16bit, 35280 bytes (x2 upsample)
[done] 34KB played
```

### 7.3 シリアルコマンド

| コマンド | 動作 |
|---|---|
| `sd` | microSD を読み直し、MP3 を順に**流し続ける**（前回最後に選んだ曲の次から。起動後の初回は先頭から） |
| `music` | `MUSIC_URL` から曲を順に取ってきて**流し続ける** |
| `pause` | 一時停止 / 再開を切り替える（SW1 と同じ）。止まっているときは `next` と同じく再生を始める |
| `next` | 次の曲へ（再生中でも効く）。止まっているときは、最後に選んだ方（SD か Wi-Fi）で再生を始める |
| `prev` | 前の曲へ |
| `rand` | ランダムな曲へ |
| `stop` | 連続再生をやめる。次に始めると止めた曲の次から（途中から聞き直したいなら `pause`） |
| `list` | 曲一覧をシリアルに出す。SD なら番号とパス（`>` が今の曲）、Wi-Fi なら番号・LCD 用の名前・パス |
| `now` | 今かかっている曲を表示 |
| `shuffle` | サーバ側のシャッフルを ON/OFF（Wi-Fi のときだけ。SD では `rand` を使う） |
| `play` | `AUDIO_URL` の通知音を1回鳴らす |
| `play <url>` | 指定した URL を1回鳴らす |
| `vol` | 今の音量を表示 |
| `vol <0-100>` | 音量の目盛りを指定（再生中でも効く）。100 が等倍（0dB）で、1 目盛り 0.6dB ずつ下がる。0 は無音。起動時は 35（-39dB） |
| `vol +` / `vol -` | 音量を 5 目盛り（3dB）ずつ上げる / 下げる |
| `wifi off` | Wi-Fi を切り、つなぎ直しもやめる（外で使うとき）。Bluetooth はそのまま |
| `wifi on` | Wi-Fi のつなぎ直しを再開する（SD から流している間は、止めてからつなぐ） |
| `scan` | ペアリングを破棄して Bluetooth を再スキャン |
| `relay` | Discord の中継につなぎ直す。失敗が続いて延びていた間隔も戻す（10.6 節） |
| `status` | Wi-Fi / IP / Bluetooth / 再生元（`src`）/ 連続再生 / 一時停止 / 音量 / SD の曲数 / 空きヒープの状態を表示 |

**再生中に受け付けるのは `pause` / `stop` / `next` / `prev` / `rand` / `vol` / `wifi on` / `wifi off` だけ。**
他のコマンドは曲が終わるまで処理されない。

BOOTSEL ボタンは状況で意味が変わる。

| 状況 | BOOTSEL |
|---|---|
| 止まっているとき | 連続再生を始める（最後に選んだ方。起動直後は SD に曲があれば SD） |
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
| `WIFI_TIMEOUT_MS` | 30000 | Wi-Fi 接続を待つ上限 |
| `WIFI_RETRY_MS` | 30000 | Wi-Fi が切れているとき再接続を試みる間隔。SD から連続再生している間は試みない（7.6 節） |
| `HTTP_TIMEOUT_MS` | 15000 | サーバの応答を待つ上限 |
| `A2DP_RATE` | 44100 | A2DP の送出レート。44100 か 48000 |
| `A2DP_BUFFER` | 32768 | 約370ms 分（64KB）。通信のゆらぎを吸収する。**単位はバイトではなく 16bit サンプル数**で、2倍のバイトが確保される。RAM の都合でこれ以上は増やせない（7.6 節） |
| `IN_FRAMES` | 1024 | 1回の読み取りで扱うフレーム数。小刻みに読むとスループットが落ちる |
| `BOOTSEL_SKIP` | `true` | 再生中の BOOTSEL で次の曲へ送るか |
| `PIN_BUTTON` | 15 | 表示切り替えスイッチをつなぐ GPIO |
| `PIN_SD_MISO` など | 16 / 18 / 19 / 20 | microSD の MISO・CLK・MOSI・CS（[docs/sd_music_parts.md](docs/sd_music_parts.md) の 3 章） |
| `SD_MAX_TRACKS` | 300 | SD から拾う曲数の上限。パスを RAM に持つため（1曲 70 バイト前後） |
| `SD_MAX_DEPTH` | 5 | SD のフォルダを潜る深さ |
| `MSG_MAX` | 8 | 覚えておく Discord のメッセージの数 |
| `MSG_SCROLL_MS` | 300 | 16 文字を超える本文を1文字ずつ流す間隔 |
| `RELAY_RETRY_MS` | 30000 | 中継へのつなぎ直しを試みる間隔。失敗が続くと倍ずつ延ばす（最大 8 分） |
| `RELAY_IDLE_MS` | 90000 | これだけ何も届かなければ中継との接続が切れたとみなす |

### 7.5 スイッチ（表示切り替えと SW1〜SW4）

タクトスイッチを1個足すと、押すたびに LCD の表示ページが切り替わる。

| スイッチの片側 | スイッチのもう片側 |
|---|---|
| `GP15`（物理20番ピン） | `GND`（物理18番ピンなど） |

- 抵抗は要らない。Pico 内蔵のプルアップ（`INPUT_PULLUP`）を使うので、離すと HIGH・押すと LOW になる
- 4本足のタクトスイッチは、**対角の2本**を使えば向きを間違えない
- チャタリングはソフトで 30ms 待って吸収している
- ユニバーサル基板にはんだ付けして組み立てる手順は [docs/universal_board.md](docs/universal_board.md)

| ページ | 1行目 | 2行目 |
|---|---|---|
| 1. 通常 | 曲名 / 状態 | 再生中は帯域ごとの音の大きさの棒（下の説明）。一時停止中や止まっているときは進み具合や状態（一時停止中は後ろに `pause`） |
| 2. メッセージ | Discord の発言者の名前と時刻（`HH:MM`） | 本文。16 文字を超えると流れる（10.6 節）。まだ無ければ中継とのつながり具合 |
| 3. 入力 | モールスで入力した文章 | 入力中の符号と、確定したときの文字 |
| 4. Wi-Fi | 電波強度（dBm） | IP アドレス |
| 5. Bluetooth | 接続状態 | イヤホンの MAC アドレス（コロン抜き） |
| 6. システム | 起動からの時間 | 空きヒープ |

通常画面の2行目の棒は、左の 86Hz から右の 16kHz までを対数で16本に分け、それぞれの帯域の強さを8段で出す。
音量（`vol`）を掛ける前の音で見るので、`vol` を変えても棒の高さは変わらない。
A2DP のバッファにはおよそ 370ms 先の音まで入っているので、書いたときではなく、今イヤホンへ送り出している所の棒を出す。
棒が下がるときは 80ms ごとに1段ずつ下げる。

6ページ目の次は通常画面に戻る。通常画面以外を見ている間も曲名や状態は裏で更新しているので、
戻ったときには最新の内容が出る。4〜6ページ目は1秒ごとに描き直す。
Discord のメッセージが届くと、どのページを見ていてもメッセージページに切り替わる。
このときだけ、GP15 を押すと次のページではなく元のページに戻る。
スイッチは再生中も効き、音は途切れない。

SW1〜SW4（GP10〜13）の役目は、表示中のページで変わる。

| ページ | SW1 | SW2 | SW3 | SW4 |
|---|---|---|---|---|
| 2. メッセージ | 再生 / 一時停止 | 古いメッセージ | 新しいメッセージ | ランダムな曲 |
| 3. 入力 | backspace | トン | ツー | enter |
| それ以外 | 再生 / 一時停止 | 前の曲 | 次の曲 | ランダムな曲 |

- 再生の操作はシリアルの `pause` / `prev` / `next` / `rand` と同じ。止まっているときはどれを押しても再生が始まる
- 一時停止中に曲を送ると、一時停止は解けて次の曲が鳴る
- モールスを打つときは、GP15 で入力ページ（通常画面の2つ先）に切り替えてから打つ
- Wi-Fi の曲は、30秒より長く一時停止すると、再開したときに少しだけ鳴って次の曲へ進む。
  音楽サーバが、送れないまま30秒たつと接続を切るため（9.7 節）。SD の曲はいくら止めても続きから鳴る

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

- MP3 は `BackgroundAudio` ライブラリに入っている libmad で、`loop()` の中で1フレーム（1152 サンプル、約26ms）ずつデコードし、A2DP に書く。ライブラリの再生クラス（割り込みでデコードする）は使っていない。Wi-Fi の WAV と同じく、再生中もスイッチやコマンドが効く
- 実機（44.1kHz ステレオ 192kbps、CPU 200MHz、`-Os`）では、1フレームのデコードに平均 11ms（実時間の約42%）
- 曲の終わりに `mp3: ... decode avg ... us/frame (..% of real time), longest loop .. ms, underflow .. times` と出る。`longest loop` は `loop()` が1周にかかった最長時間で、`A2DP_BUFFER` の 370ms に近づくと音が切れる。再生中に音が切れると、その場で `underflow at 12s (wifi connected)` のように出る（再生開始から1秒間は、空のバッファが溜まるまでの分なので数えない）
- SD の読み出しは 4MHz（SD ライブラリの既定）で約 375KB/s。320kbps の MP3 でも 40KB/s なので十分
- Wi-Fi の再接続は最大30秒待つので、外で聞いているときに曲の合間が止まらないよう、SD から連続再生している間は再接続しない。外では `wifi off` で切っておくとよい
- **SD から流している間は、Wi-Fi を省電力モード（`WiFi.defaultLowPowerMode()`）にする。** arduino-pico は Wi-Fi につなぐと省電力を切る（`noLowPowerMode()`、常時受信）。そのままだと、同じ CYW43439 の Bluetooth と無線を取り合い、Wi-Fi につながっているときだけ SD の曲がずっと途切れた（`underflow` も出た）。省電力にすると、Wi-Fi につないだままでも途切れなくなった。Wi-Fi の WAV を流すとき（`playUrl`）は、速さが要るので常時受信に戻す

RAM（256KB）の使い方には余裕がない。

| 使い道 | 大きさ |
|---|---|
| グローバル変数（BTstack・Wi-Fi・MP3 デコーダの作業領域 約29KB など） | 約146KB |
| `A2DP_BUFFER`（32768 サンプル） | 64KB |
| 残り（Wi-Fi・Bluetooth の動的確保、SD の曲一覧、Discord の中継との TLS など） | 約46KB |

- `A2DP_BUFFER` を以前の 65536 にすると 128KB を確保しようとして `a2dp.begin()` が失敗し、Bluetooth が一切つながらなくなる（`BT rejected` が続く）
- 大きな確保が細切れのヒープで失敗しないよう、`a2dp.begin()` は SD の曲一覧を作る前に呼び、MP3 デコーダの作業領域は最初から静的に持っている

Discord の中継とは TLS でつなぐ（10 章）ので、BearSSL がヒープを使う
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
（コードを読んで見つけた不具合。実機で最初に途切れたときは上の Wi-Fi の問題も重なっていたので、これだけでどう聞こえるかは確かめていない。
Wi-Fi の WAV 再生は 2048 サンプルずつ書いていたので、たまたま踏んでいなかった。）
`A2DP_BUFFER` を変えるときは `A2DP_CHUNK` の倍数にすること（`static_assert` で確かめている）。

## 8. tools/serve_audio.py — 通知音を配る

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

## 9. tools/serve_music.py — 音楽ライブラリを配る

PC に置いてある mp3 や m4a を、Pico が読める 16bit PCM の WAV に**変換しながら**配る。
ここでは音楽ファイルが dynabook の `~/media/music/` にある前提で書く。

### 9.1 なぜ PC 側で変換するのか

Pico WH には mp3 を解く余裕がない。RP2040 は FPU を持たず、A2DP の SBC
エンコードと Wi-Fi の受信で既に手一杯になる。
一方 PC 側なら ffmpeg に通すだけで済むので、**デコードもリサンプルも PC で終わらせて、
Pico には生の PCM だけを渡す**。Pico 側は WAV ヘッダを読んで A2DP に流すだけになる。

変換し終えるのを待たずに、変換した先から少しずつ流す。曲の頭が鳴り始めるまで待たされない。

### 9.2 Tailscale と Pico の関係

**Pico W/WH に Tailscale は載らない。** WireGuard クライアントを動かす余裕がないので、
Pico は tailnet に参加できない。したがって `100.x.x.x` のアドレスも
`dynabook.tail....ts.net` という名前も Pico からは使えない。

Pico が使うのは **サーバの LAN IP** だけ。Tailscale は、サーバを操作する人間が
`ssh dynabook` で入るための経路であって、音声は通らない。

```
   Pico WH ──── Wi-Fi/LAN ────> 192.168.40.195:8000 ── dynabook
                                  serve_music.py          ~/media/music/
                                  ffmpeg で mp3 → PCM
                                       ↑
                                  Tailscale
                                  (ssh dynabook で起動・停止するのはこちら)
```

つまり **dynabook と Pico が同じ LAN にいる必要がある**。dynabook を外に持ち出すと
Pico からは届かなくなる。

サーバの IP は `MUSIC_URL` に直接書くので、**DHCP で変わると繋がらなくなる**。
ルータで IP 固定（DHCP 予約）しておくと安定する。

### 9.3 dynabook 側の準備

必要なのは **ffmpeg だけ**。サーバ本体は python3 の標準ライブラリしか使わない。

```bash
ssh dynabook
sudo apt install --no-install-recommends ffmpeg
```

> 推奨パッケージ込みの `apt install ffmpeg` は 136パッケージ・展開後 376MB になる。
> `--no-install-recommends` なら 102パッケージ・194MB で、mp3・m4a・flac・ogg は
> すべて読める。

スクリプトを置いて起動する。

```bash
ssh dynabook mkdir -p '~/espoke'
scp tools/serve_music.py dynabook:~/espoke/
ssh dynabook 'python3 ~/espoke/serve_music.py'
```

起動すると、そのまま `MUSIC_URL` に書ける行を表示する。

```
/home/you/media/music  39 tracks
44100Hz mono 16bit PCM  (88 KB/s)

  http://192.168.40.195:8000/

この URL を espoke/arduino_secrets.h の MUSIC_URL に書く:
  #define MUSIC_URL "http://192.168.40.195:8000"
```

### 9.4 エンドポイント

**次の曲がどれかを覚えているのはサーバ側**。Pico は `/next.wav` を叩くだけでよく、
曲順やシャッフルの管理を載せずに済む。

| URL | 動作 |
|---|---|
| `/next.wav` | 次の曲へ進めて、その曲を WAV で返す |
| `/prev.wav` | 前の曲へ戻す |
| `/random.wav` | ランダムに選ぶ |
| `/current.wav` | 今の曲をもう一度 |
| `/track/5.wav` | 5番の曲 |
| `/list` | 曲一覧（番号 `TAB` LCD 用の名前 `TAB` パス） |
| `/now` | 今の曲 |
| `/shuffle` | シャッフルの ON/OFF を切り替え |
| `/rescan` | ディレクトリを読み直す |
| `/` | ブラウザ用の一覧 |

WAV を返すときは、LCD に出すための曲名をヘッダに入れる。

| ヘッダ | 内容 |
|---|---|
| `X-Track` | LCD 用。ASCII 16文字。先頭のトラック番号を落とし、アクセント記号も潰してある |
| `X-Track-Full` | パーセントエンコードしたフルパス |
| `X-Index` | 曲番号 |

ブラウザや `curl` でもそのまま鳴らせるので、Pico を繋ぐ前に PC だけで確認できる。

```bash
curl -s http://192.168.40.195:8000/list
curl -s http://192.168.40.195:8000/next.wav | aplay
```

### 9.5 オプション

| オプション | 既定値 | 内容 |
|---|---|---|
| `--dir` | `~/media/music` | 配信するディレクトリ（再帰的に探す） |
| `--port` | 8000 | 待ち受けポート |
| `--rate` | 44100 | 44100 / 22050 / 11025 |
| `--stereo` | （モノラル） | ステレオで配る。帯域が倍になる |
| `--volume` | 1.0 | 音量の倍率 |
| `--shuffle` | （曲順） | 最初からシャッフルする |
| `--ffmpeg` | `ffmpeg` | ffmpeg のパス |

拡張子で音楽ファイルを拾う（`.mp3` `.m4a` `.flac` `.ogg` `.opus` `.wav` `.aac` など）。

### 9.6 WAV の長さを 0 で返している理由

変換しながら流すので、送り終わるまで長さが分からない。
そのため `data` チャンクのサイズは **0** にして、`Content-Length` も付けず、
HTTP/1.0 の「接続が閉じたら終わり」で長さを伝えている。

Pico 側は `data` サイズ 0 を「最後まで読む」と解釈する（`WavInfo.dataBytes == 0`）。
このとき LCD の進捗は `%` ではなく `KB` 表示になる。

### 9.7 相手が黙って消えたとき

Pico は電源が落ちても電波が切れても **FIN を返さずに消える**。そのままだと
カーネルが再送を諦めるまで（15分ほど）送信が詰まり、`ffmpeg` とスレッドが居座る。

そのため接続に **30秒の送信タイムアウト**（`STREAM_TIMEOUT`）を張ってある。
30秒まったく送信が進まなければ打ち切り、`ffmpeg` を kill してログに残す。

```
play [2] NMIXX/Blue_Valentine/01-Blue_Valentine.mp3
  client gone, 30s no progress (1808 KB)
```

`next` や `stop` で Pico から切った場合は即座に検知されるので、こちらは
`stopped by client` になる。

Pico で一時停止している間も、Pico は受け取らないので送信が進まない。そのため30秒より長く
止めると、同じく `client gone` で切られる（7.5 節）。

## 10. tools/discord_relay.py — Discord を受け取る

Discord の決めたチャンネルの発言を、espoke の LCD に出す（送信はまだない）。

### 10.1 経路

```
   Discord ──Bot── discord_relay.py (127.0.0.1:8001)        dynabook（学校）
                          │ tailscale funnel
                          ▼
            https://dynabook.<tailnet>.ts.net:8443
                          │ インターネット
                   家の Wi-Fi ── Pico WH（BearSSL で HTTPS）
```

- 中継は学校に置いた dynabook で常時動かし、Pico は家の Wi-Fi で受ける。
- Pico は Tailscale に入れない（9.2 節）ので、**Funnel で中継をインターネットに公開し、Pico から HTTPS でつなぐ**。
  誰でもつなげてしまうので、合言葉（`RELAY_KEY`）を `X-Key` ヘッダで送らせ、違えば 403 を返す。中継自体は `127.0.0.1` でしか待ち受けない。
- **Funnel は 8443 番を使う。** dynabook の 443 番は、tailnet の中だけに見せる `tailscale serve`（`127.0.0.1:8787`）に使っている。
  443 で funnel すると、その設定を書き換えて 8787 まで公開してしまう。Funnel が使えるのは 443 / 8443 / 10000 番だけ。
- 家に常時動かしておくマシンは要らない。LTE-M（[docs/lte_m_parts.md](docs/lte_m_parts.md)）にしても、BG96 は TLS を内蔵しているので同じ URL が使える。
- dynabook を学校に移すと、家からは `music`（`MUSIC_URL` は LAN の IP）が届かなくなる。

### 10.2 なぜ中継で変換するのか

LCD1602A の文字 ROM（A00）にあるのは**英数字と半角カタカナ**だけ。その並びは JIS X 0201 で、cp932（Shift_JIS）の1バイト文字と同じ番号になっている。
漢字を読みに直すには形態素解析の辞書が要る（unidic-lite は展開すると 249MB）。Flash が 2MB の Pico には載らない。
そこで中継が「漢字 → 読み → 半角カナ」まで済ませ、LCD にそのまま書けるバイト列にして渡す。
serve_music.py が曲名を ASCII にしてから渡しているのと同じ分担。

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
- 変換だけを試すには `--convert` を使う（10.5 節）

### 10.3 Bot を作る

1. [Discord Developer Portal](https://discord.com/developers/applications) で New Application を作り、Bot のページを開く
2. Reset Token でトークンを出す（一度しか表示されないので控える）
3. 同じページの **Message Content Intent を ON** にする。OFF のままだと本文が空で届く
4. OAuth2 → URL Generator で scope に `bot`、権限に View Channels と Read Message History を選び、出てきた URL で自分のサーバーに招待する
5. Discord の 設定 → 詳細設定 で開発者モードを ON にし、受け取るチャンネルを右クリック → チャンネル ID をコピー

### 10.4 dynabook 側の準備

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
DISCORD_CHANNEL_ID=123456789012345678
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
discord: espoke#1234 として #general を見ている
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

### 10.5 エンドポイント

| URL | 動作 |
|---|---|
| `GET /stream?after=<ID>` | `after` より新しい発言を送り、接続を閉じずに新着を流し続ける。`after` が無ければ直近 8 件から |
| `POST /post` | 本文（UTF-8）を差出人 `test` の発言として流す。Discord を使わずに試すとき |
| `GET /` | 動いているかの確認 |

`/stream` と `/post` には `X-Key: <RELAY_KEY>` ヘッダが要る（無いか違えば 403）。

1件は1行で、cp932 のバイト列。

```
<メッセージ ID> TAB <HH:MM> TAB <名前> TAB <本文> LF
```

- ID は Discord のメッセージ ID（snowflake）。時間とともに増えるので、Pico は最後に受け取った ID を `after` に付けてつなぎ直せば、切れていた間の分も受け取れる。
  中継を立ち上げ直しても、起動時にチャンネルの履歴を 8 件読み直す
- 名前は 10 バイト、本文は 200 バイトで切る。時刻は日本時間
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

~/espoke/venv/bin/python ~/espoke/discord_relay.py --convert '今日は雨'  # 変換だけ
~/espoke/venv/bin/python ~/espoke/discord_relay.py --no-discord        # Discord なしで /post だけ
```

### 10.6 Pico 側の動き

- 起動後の最初の `loop()` で中継につなぐ。切れたら 30 秒ごとにつなぎ直す。つなげないことが続くと、間隔を倍ずつ延ばす（最大 8 分）。中継が落ちていると握手のタイムアウト（15 秒）まで待つので、曲の合間が何度も止まらないようにするため。中継を直したあとすぐにつなぎ直させるには、シリアルで `relay` を打つ
- **TLS の握手に 4〜8 秒かかる**（実測。Let's Encrypt の P-384 の証明書を確かめるのが重い）。A2DP のバッファ（370ms）では持たないので、つなぐのは止まっている間か曲の合間だけにしている。
  つなぐ前に A2DP のバッファを無音で埋めるので、つなぎ直すときだけ曲の合間が 0.4 秒と握手の分（数秒）だけ空く
- 受け取るときは待たずに、届いた分だけ読む。再生中も届く（SD の再生中は Wi-Fi が省電力なので少し遅れる）
- 証明書は `espoke/relay_ca.h` のルート CA（Let's Encrypt の Root YE / ISRG Root X2 / ISRG Root X1）で確かめる。
  期限を確かめるのに時計が要るので、最初につなぐ前に NTP（`ntp.nict.jp`）で合わせる
- 直近 8 件を覚えておく。届いたら**メッセージページに切り替える**（7.5 節）。GP15 を押すと元のページに戻る
- 1行目は名前と時刻、2行目は本文。16 文字を超える本文は 0.3 秒ごとに1文字ずつ流し、頭と末尾で 1.5 秒止まる
- SW2 で古いメッセージ、SW3 で新しいメッセージを見る
- シリアルには `relay: <ID> <時刻> <名前>` と出る（名前は半角カナのバイト列なので化ける）。
  `status` には `relay=1 (no message) msgs=3` のように、つながり具合と覚えている件数が出る
- `wifi off` にすると中継も切る

メッセージがまだ1件も無いとき、メッセージページの2行目にはつながり具合が出る。

| 表示 | 意味 |
|---|---|
| `connecting` | 起動直後。まだつなぎにいっていない |
| `no message` | つながっている。チャンネルにまだ発言が無い |
| `relay error` | TLS でつなげなかった。シリアルに `relay: connect failed (ssl <番号> ...)` が出る |
| `relay 403` など | 中継に断られた。`RELAY_KEY` が違う |
| `relay no reply` | 応答のヘッダが来なかった |
| `relay lost` / `relay silent` | 切れた / 90 秒何も届かなかった。30 秒以内につなぎ直す |
| `ntp failed` | 時計を合わせられなかった |
| `wifi off` | `wifi off` で切った |
| `no RELAY_HOST` | `arduino_secrets.h` に `RELAY_HOST` が無い |

## 11. トラブルシューティング

| 症状 | 原因と対処 |
|---|---|
| `BluetoothAudio.h: No such file` / `_needsbt.h` のエラー | FQBN に `ipbtstack=ipv4btcble` が入っていない |
| `No drive to deploy.` で書き込めない | 自動リセットが効かず BOOTSEL に入っていない。`lsusb` が `2e8a:f00a`（スケッチ実行中）のままなら未リセット。`usermod -aG dialout` 後に再ログインしていないのが原因のことが多く、`id` で確認して `sg dialout -c "arduino-cli upload ..."` で叩く |
| `'WavInfo' has not been declared` | `.ino` はビルド時に関数プロトタイプが先頭へ自動生成される。引数に使う構造体は**ファイル冒頭**で定義する |
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
| `warning: audio underflow` | Wi-Fi が追いついていない。素材を 22050Hz モノラルに落とす。`A2DP_BUFFER` は RAM の都合で増やせない（7.6 節） |
| Wi-Fi に繋がらないことがある | 起動時の1回だけでは不安定。`WIFI_RETRY_MS` ごとに `loop()` から張り直している |
| 音がブツブツ切れる | 同上。Wi-Fi と Bluetooth が CYW43439 を共有しているため。ルータとの距離も効く |
| `http error 404` など | `AUDIO_URL` の IP が PC の LAN IP になっているか確認。`tools/serve_audio.py` が表示する URL を使う |
| `http error -1` / `music stopped server down?` | `MUSIC_URL` の IP かポートが違う、または `serve_music.py` が止まっている。dynabook で `curl -s localhost:8000/now` を試す |
| dynabook に繋がらない | Pico は Tailscale を使えない。dynabook が Pico と**同じ LAN** にいるか、`MUSIC_URL` が `100.x.x.x` ではなく LAN IP になっているか確認（9.2 参照） |
| 昨日まで鳴っていたのに繋がらない | DHCP で dynabook の IP が変わった。ルータで IP 固定するか `MUSIC_URL` を書き直す |
| `ffmpeg が見つからない` | dynabook 側に `sudo apt install --no-install-recommends ffmpeg` |
| 曲と曲の間が長い | 次の曲の HTTP と ffmpeg の起動に数百 ms かかる。`A2DP_BUFFER` の 370ms を超えた分が無音になる |
| LCD の曲名が化ける / 出ない | LCD1602A の文字 ROM に無い文字。サーバが `X-Track` を ASCII に潰して返しているので、それでも化けるならコントラスト調整を疑う |
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
| メッセージページのカナが記号やキリル文字になる | LCD の文字 ROM が A02（欧文）。半角カナが出るのは A00（日本語）の LCD1602A だけ |
| `relay: connect failed (ssl -1000 ...)` が出る、または `heap` が 15KB を切っている | TLS に使う RAM が足りない（7.6 節）。つなぐには 10KB ほど要る。SD の曲数（`SD_MAX_TRACKS`）を減らすなどして空ける |
| `relay: connect failed` が出るが、`heap` は足りている | 時計が合っていない（`ntp failed`）、Funnel が止まっている、Let's Encrypt がルート CA を替えた（`relay_ca.h` を更新する）のどれか。PC から 10.5 節の curl を試す |
| `relay 403` | `RELAY_KEY` が dynabook の `relay.env` と違う |
| 発言しても本文が空で届く | Developer Portal で Message Content Intent が OFF（10.3 節） |
| PC の curl で、発言してもすぐに出ない | `--http1.0` か `-N` を付けていない。`iconv` を通していると、終わるまで溜め込まれる（10.5 節） |
