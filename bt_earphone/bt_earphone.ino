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

static const unsigned long RETRY_MS = 15000;  // 未接続時に再スキャンする間隔
static unsigned long lastScan = 0;

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

// スキャンして接続する。成功したら true
static bool findAndConnect() {
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

  Serial.printf("connecting to [%d] %s ...\n", pick, found[pick].name());
  if (!a2dp.connect(found[pick].address())) {
    Serial.println("  failed");
    return false;
  }
  Serial.println("  ok");
  return true;
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

  findAndConnect();
  lastScan = millis();
  Serial.println("未接続なら15秒ごとに自動で再スキャンする (BOOTSEL でペアリング破棄 + 即再スキャン)");
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
