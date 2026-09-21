/*
 * Bluetooth イヤホンに接続してテスト音を鳴らす
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble
 *       (Arduino IDE なら ツール → IP/Bluetooth Stack → "IPv4 + Bluetooth")
 *
 * 動作:
 *   1. 周囲の Bluetooth オーディオ機器 (A2DP sink) をスキャンして一覧表示
 *   2. TARGET_NAME に前方一致する機器 (空なら最初に見つかった機器) へ接続
 *   3. 接続できたら "ピピッ" というポケベル風の通知音を鳴らし続ける
 *   4. BOOTSEL ボタンでペアリングを破棄して再スキャン
 *
 * イヤホン側は必ず「ペアリングモード」にしてから電源を入れること。
 * 既に他の機器と接続済みだと、スキャンに出てこない。
 */

#include <BluetoothAudio.h>

// --- 設定 ---
static const char *LOCAL_NAME   = "espoke";  // イヤホン側に見える名前
static const char *TARGET_NAME  = "";        // 接続先の名前 (前方一致)。空なら最初の1台
static const char *TARGET_ADDR  = "a0:0c:e2:c6:1d:04";  // 空でなければスキャンせず直接繋ぐ
static const int   SCAN_SECONDS = 8;
static const int   SAMPLE_RATE  = 44100;     // A2DPSource は 44100 か 48000 のみ
static const int   TONE_HZ      = 880;       // 通知音の高さ
static const int   TONE_LEVEL   = 6000;      // 振幅 (16bit なので最大 32767)
// ------------

A2DPSource a2dp;

static int16_t  sineTable[256];
static uint32_t phase     = 0;  // 位相 (上位8bit をテーブル添字に使う)
static uint32_t phaseInc  = 0;
static uint32_t sampleNo  = 0;  // 通算サンプル数。鳴動パターンの時計に使う

static const size_t FRAMES = 64;
static int16_t pcm[FRAMES * 2];  // インタリーブされた L,R

static const unsigned long RETRY_MS = 8000;            // 未接続時に再試行する間隔
static const int REJECT_LIMIT = 2;                    // 連続 rejected で再起動する回数
static const unsigned long CONNECT_TIMEOUT_MS = 15000; // ストリーム開始を待つ上限
static unsigned long lastScan = 0;
static int rejectCount = 0;  // 連続 rejected 回数

static bool scanAndConnect();

// 鳴動パターン: 1.6秒を1周期として「ピッ(120ms) 休(80ms) ピッ(120ms) 休(1280ms)」
static bool beepOn(uint32_t n) {
  const uint32_t period = SAMPLE_RATE * 16 / 10;
  uint32_t ms = (n % period) * 1000 / SAMPLE_RATE;
  return (ms < 120) || (ms >= 200 && ms < 320);
}

static void fillPCM() {
  for (size_t i = 0; i < FRAMES; i++) {
    int16_t s = beepOn(sampleNo) ? sineTable[phase >> 24] : 0;
    pcm[i * 2]     = s;
    pcm[i * 2 + 1] = s;
    phase += phaseInc;
    sampleNo++;
  }
}

static void onConnect(void *, bool connected) {
  if (connected) {
    Serial.printf("A2DP connected: %s\n", bd_addr_to_str(a2dp.getSinkAddress()));
  } else {
    Serial.println("A2DP disconnected");
  }
}

static void onVolume(void *, int pct) {
  Serial.printf("volume: %d%%\n", pct);
}

// "aa:bb:cc:dd:ee:ff" を 6バイトに変換する
static bool parseAddr(const char *str, uint8_t *out) {
  unsigned v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
    return false;
  }
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

// 接続要求を出し、ストリーム開始まで待つ
static bool connectTo(const uint8_t *addr, const char *label) {
  Serial.printf("connecting to %s ...\n", label);
  if (!a2dp.connect(addr)) {
    // 前の接続が中途半端に残っていて a2dp_cid が埋まっている状態
    Serial.println("  rejected (接続要求そのものが受理されなかった)");
    rejectCount++;
  } else {
    // connect() の戻り値は「要求が受理されたか」だけ。ここから先は非同期で
    // AVDTP シグナリング → SBC ネゴシエーション → ストリーム開始 と進み、
    // そこまで到達して初めて connected() が true になる
    unsigned long start = millis();
    while (!a2dp.connected() && millis() - start < CONNECT_TIMEOUT_MS) {
      delay(50);
    }
    if (a2dp.connected()) {
      Serial.println("  ok");
      rejectCount = 0;
      return true;
    }
    // 相手が応答しない (電源オフ・他機器に接続中など) 場合はここに来る。
    // dbglvl=Bluetooth でビルドすると HCI のステータスが出る (0x04 = Page Timeout)
    Serial.println("  timeout (相手が応答しない。電源とペアリング状態を確認)");
    rejectCount = 0;
  }

  // rejected が続くのは、中途半端に張られたシグナリング接続が残って a2dp_cid が
  // 埋まっているとき。ライブラリ側からは解放できないので再起動で状態を捨てる。
  // 単なる応答なし (timeout) では再起動しない — 何度やっても同じなので意味がない
  if (rejectCount >= REJECT_LIMIT) {
    Serial.println("  stuck, rebooting to reset the Bluetooth state");
    delay(200);
    rp2040.reboot();
  }
  return false;
}

// スキャンして接続する。成功したら true
static bool findAndConnect() {
  // アドレスが分かっているならスキャンを飛ばす。8秒のインクワイアリを挟まない分
  // 相手がペアリングモードにいるうちに繋ぎにいける
  if (TARGET_ADDR[0] != '\0') {
    uint8_t addr[6];
    if (parseAddr(TARGET_ADDR, addr)) {
      return connectTo(addr, TARGET_ADDR);
    }
    Serial.printf("TARGET_ADDR \"%s\" を解釈できない\n", TARGET_ADDR);
  }
  return scanAndConnect();
}

static bool scanAndConnect() {
  Serial.printf("scanning for %d seconds...\n", SCAN_SECONDS);
  auto found = a2dp.scan(BluetoothHCI::speaker_cod, SCAN_SECONDS);

  if (found.empty()) {
    Serial.println("  no audio device found (イヤホンをペアリングモードにして再試行)");
    return false;
  }

  int pick = -1;
  for (size_t i = 0; i < found.size(); i++) {
    Serial.printf("  [%u] %-24s %s  rssi=%d\n",
                  (unsigned)i, found[i].name(), found[i].addressString(), found[i].rssi());
    if (pick < 0 && (TARGET_NAME[0] == '\0' || strncmp(found[i].name(), TARGET_NAME, strlen(TARGET_NAME)) == 0)) {
      pick = (int)i;
    }
  }

  if (pick < 0) {
    Serial.printf("  \"%s\" に一致する機器がない\n", TARGET_NAME);
    return false;
  }

  return connectTo(found[pick].address(), found[pick].name());
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {
    delay(10);
  }

  for (int i = 0; i < 256; i++) {
    sineTable[i] = (int16_t)(sinf(i * 2.0f * PI / 256.0f) * TONE_LEVEL);
  }
  phaseInc = (uint32_t)(((uint64_t)TONE_HZ << 32) / SAMPLE_RATE);

  a2dp.setName(LOCAL_NAME);
  a2dp.setFrequency(SAMPLE_RATE);
  a2dp.onConnect(onConnect);
  a2dp.onVolume(onVolume);
  a2dp.begin();

  // BTstack は User Confirmation Request をアプリに投げるだけで、既定では自動応答
  // しない (hci.c の ssp_auto_accept は 0)。イヤホンも Pico も入力装置を持たない
  // NoInputNoOutput なので Just Works ペアリングになり、ここを自動承認しないと
  // SSP が確認待ちのまま止まる
  {
    BluetoothLock b;
    gap_ssp_set_auto_accept(true);
  }

  findAndConnect();
  lastScan = millis();
  Serial.println("未接続なら8秒ごとに自動で再試行する (BOOTSEL でペアリング破棄 + 即再試行)");
}

void loop() {
  // 接続中はバッファが空かないよう PCM を流し込み続ける
  if (a2dp.connected()) {
    while ((size_t)a2dp.availableForWrite() > sizeof(pcm)) {
      fillPCM();
      a2dp.write((const uint8_t *)pcm, sizeof(pcm));
    }
  } else if (millis() - lastScan >= RETRY_MS) {
    // 繋がっていなければ一定間隔で自動的に再スキャンする
    lastScan = millis();
    findAndConnect();
  }

  if (BOOTSEL) {
    while (BOOTSEL) {
      delay(1);
    }
    a2dp.disconnect();
    a2dp.clearPairing();
    findAndConnect();
    lastScan = millis();
  }
}
