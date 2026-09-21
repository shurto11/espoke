/*
 * espoke — Wi-Fi で受け取った音声を Bluetooth イヤホンで鳴らす
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble
 *       (Arduino IDE なら ツール → IP/Bluetooth Stack → "IPv4 + Bluetooth")
 *
 * 動作:
 *   1. Wi-Fi に接続
 *   2. Bluetooth イヤホン (A2DP sink) をスキャンして接続
 *   3. シリアルに "play" と打つか BOOTSEL を押すと、AUDIO_URL から WAV を取得して再生
 *   4. LCD1602A に状態を表示
 *
 * 音声フォーマット:
 *   16bit PCM の WAV。モノラル/ステレオどちらでも可。
 *   サンプリングレートは 44100 の整数分の1 (44100 / 22050 / 11025) のみ対応し、
 *   整数倍のサンプル&ホールドで 44100 ステレオへ引き伸ばして A2DP に流す。
 *   48000 など割り切れないレートは非対応 (リサンプラを積む余裕がないため)。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <BluetoothAudio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include "arduino_secrets.h"  // WIFI_SSID / WIFI_PASS / AUDIO_URL

// --- 設定 ---
static const char *BT_LOCAL_NAME  = "espoke";  // イヤホン側に見える名前
static const char *BT_TARGET_NAME = "";        // 接続先の名前 (前方一致)。空なら最初の1台
static const int    SCAN_SECONDS  = 8;
static const int    A2DP_RATE     = 44100;     // A2DPSource は 44100 か 48000 のみ
static const size_t A2DP_BUFFER   = 32768;     // 約185ms 分。通信のゆらぎを吸収する

static const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)
static const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)
static const uint8_t LCD_COLS  = 16;
static const uint8_t LCD_ROWS  = 2;
static const uint8_t LCD_ADDR_DEFAULT = 0x27;
// ------------

// .ino はビルド時に関数プロトタイプが先頭へ自動生成されるので、
// 引数に使う型はここで定義しておく必要がある
struct WavInfo {
  uint32_t sampleRate;
  uint16_t channels;
  uint16_t bits;
  uint32_t dataBytes;  // 0 なら長さ不明 (最後まで読む)
};

A2DPSource a2dp;
LiquidCrystal_I2C *lcd = nullptr;

static const size_t IN_FRAMES = 256;
static int16_t inBuf[IN_FRAMES * 2];       // 最大ステレオ
static int16_t outBuf[IN_FRAMES * 4 * 2];  // 最大4倍アップサンプル × ステレオ

static unsigned long lastScan = 0;
static const unsigned long RETRY_MS = 15000;

// ---------------- LCD ----------------

static void printLine(uint8_t row, const String &text) {
  if (!lcd) return;
  String s = text.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s);
}

static void status(const String &a, const String &b) {
  printLine(0, a);
  printLine(1, b);
  Serial.printf("[%s] %s\n", a.c_str(), b.c_str());
}

static void setupLCD() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();

  uint8_t addr = 0;
  for (uint8_t a = 1; a < 127 && addr == 0; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) addr = a;
  }
  if (addr == 0) {
    Serial.println("LCD not found, using default 0x27");
    addr = LCD_ADDR_DEFAULT;
  } else {
    Serial.printf("LCD address: 0x%02X\n", addr);
  }

  lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
}

// ---------------- Bluetooth ----------------

static void onConnect(void *, bool connected) {
  if (connected) {
    Serial.printf("A2DP connected: %s\n", bd_addr_to_str(a2dp.getSinkAddress()));
  } else {
    Serial.println("A2DP disconnected");
  }
}

static bool findAndConnect() {
  status("BT scanning", String(SCAN_SECONDS) + "s ...");
  auto found = a2dp.scan(BluetoothHCI::speaker_cod, SCAN_SECONDS);

  if (found.empty()) {
    status("BT not found", "pairing mode?");
    return false;
  }

  int pick = -1;
  for (size_t i = 0; i < found.size(); i++) {
    Serial.printf("  [%u] %-24s %s  rssi=%d\n",
                  (unsigned)i, found[i].name(), found[i].addressString(), found[i].rssi());
    if (pick < 0 && (BT_TARGET_NAME[0] == '\0' ||
                     strncmp(found[i].name(), BT_TARGET_NAME, strlen(BT_TARGET_NAME)) == 0)) {
      pick = (int)i;
    }
  }
  if (pick < 0) {
    status("BT no match", BT_TARGET_NAME);
    return false;
  }

  status("BT connecting", found[pick].name());
  if (!a2dp.connect(found[pick].address())) {
    status("BT failed", found[pick].name());
    return false;
  }
  status("BT ready", found[pick].name());
  return true;
}

// ---------------- WAV ----------------

// stream から len バイト読み切る。読めた分を返す
static size_t readExact(WiFiClient *s, uint8_t *buf, size_t len, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < len && millis() - start < timeoutMs) {
    int n = s->read(buf + got, len - got);
    if (n > 0) {
      got += n;
      start = millis();
    } else if (!s->connected() && s->available() == 0) {
      break;
    } else {
      delay(1);
    }
  }
  return got;
}

static bool skipBytes(WiFiClient *s, uint32_t len) {
  uint8_t junk[64];
  while (len > 0) {
    size_t chunk = len > sizeof(junk) ? sizeof(junk) : len;
    if (readExact(s, junk, chunk, 3000) != chunk) return false;
    len -= chunk;
  }
  return true;
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// RIFF ヘッダを読み進め、data チャンクの直前まで進める
static bool parseWav(WiFiClient *s, WavInfo *info) {
  uint8_t hdr[12];
  if (readExact(s, hdr, 12, 5000) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    Serial.println("not a RIFF/WAVE file");
    return false;
  }

  bool haveFmt = false;
  for (int guard = 0; guard < 16; guard++) {
    uint8_t ck[8];
    if (readExact(s, ck, 8, 5000) != 8) return false;
    uint32_t size = le32(ck + 4);

    if (memcmp(ck, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || readExact(s, fmt, 16, 5000) != 16) return false;
      uint16_t format   = le16(fmt);
      info->channels    = le16(fmt + 2);
      info->sampleRate  = le32(fmt + 4);
      info->bits        = le16(fmt + 14);
      if (format != 1) {
        Serial.printf("unsupported WAV format tag %u (PCM only)\n", format);
        return false;
      }
      if (size > 16 && !skipBytes(s, size - 16)) return false;
      haveFmt = true;
    } else if (memcmp(ck, "data", 4) == 0) {
      if (!haveFmt) return false;
      info->dataBytes = size;
      return true;
    } else {
      if (!skipBytes(s, size + (size & 1))) return false;
    }
  }
  return false;
}

// ---------------- 再生 ----------------

static void playUrl(const String &url) {
  if (WiFi.status() != WL_CONNECTED) {
    status("no wifi", "cannot play");
    return;
  }
  if (!a2dp.connected()) {
    status("no earphone", "cannot play");
    return;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, url)) {
    status("bad url", url.substring(0, LCD_COLS));
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    status("http error", String(code));
    http.end();
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  WavInfo info = {0, 0, 0, 0};
  if (!parseWav(stream, &info)) {
    status("bad wav", "header");
    http.end();
    return;
  }

  uint32_t rateMul = (info.sampleRate > 0) ? (uint32_t)A2DP_RATE / info.sampleRate : 0;
  if (info.bits != 16 || info.channels < 1 || info.channels > 2 ||
      rateMul == 0 || rateMul > 4 || rateMul * info.sampleRate != (uint32_t)A2DP_RATE) {
    Serial.printf("unsupported: %luHz %uch %ubit\n",
                  (unsigned long)info.sampleRate, info.channels, info.bits);
    status("unsupported", String(info.sampleRate) + "Hz " + String(info.channels) + "ch");
    http.end();
    return;
  }

  Serial.printf("playing %luHz %uch %ubit, %lu bytes (x%lu upsample)\n",
                (unsigned long)info.sampleRate, info.channels, info.bits,
                (unsigned long)info.dataBytes, (unsigned long)rateMul);

  const size_t inBytesMax  = IN_FRAMES * info.channels * 2;
  const size_t outBytesMax = IN_FRAMES * rateMul * 2 * 2;
  uint32_t remaining = info.dataBytes;
  uint32_t played    = 0;
  unsigned long lastLcd = 0;

  while (a2dp.connected()) {
    if ((size_t)a2dp.availableForWrite() < outBytesMax) {
      delay(1);
      continue;
    }

    size_t want = inBytesMax;
    if (info.dataBytes && want > remaining) want = remaining;
    if (want == 0) break;

    size_t got = readExact(stream, (uint8_t *)inBuf, want, 4000);
    if (got < 2) break;
    got &= ~(size_t)(info.channels * 2 - 1);  // フレーム境界に切り詰める

    size_t frames = got / (info.channels * 2);
    size_t o = 0;
    for (size_t f = 0; f < frames; f++) {
      int16_t l = inBuf[f * info.channels];
      int16_t r = (info.channels == 2) ? inBuf[f * info.channels + 1] : l;
      for (uint32_t k = 0; k < rateMul; k++) {
        outBuf[o++] = l;
        outBuf[o++] = r;
      }
    }
    a2dp.write((const uint8_t *)outBuf, o * 2);

    played += got;
    if (info.dataBytes) remaining -= got;

    if (millis() - lastLcd >= 500) {
      lastLcd = millis();
      if (info.dataBytes) {
        printLine(1, "play " + String(played * 100 / info.dataBytes) + "%");
      } else {
        printLine(1, "play " + String(played / 1024) + "KB");
      }
    }
  }
  http.end();

  // 末尾が切れないよう、無音を少し流してからバッファが空くのを待つ
  memset(outBuf, 0, sizeof(outBuf));
  for (int i = 0; i < 8 && a2dp.connected(); i++) {
    while ((size_t)a2dp.availableForWrite() < sizeof(outBuf)) delay(1);
    a2dp.write((const uint8_t *)outBuf, sizeof(outBuf));
  }

  if (a2dp.getUnderflow()) {
    Serial.println("warning: audio underflow (Wi-Fi が追いついていない)");
  }
  status("done", String(played / 1024) + "KB played");
}

// ---------------- setup / loop ----------------

static void connectWiFi() {
  status("WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    status("WiFi ok", WiFi.localIP().toString());
  } else {
    status("WiFi failed", WIFI_SSID);
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  setupLCD();
  status("espoke", "booting...");

  connectWiFi();

  a2dp.setName(BT_LOCAL_NAME);
  a2dp.setFrequency(A2DP_RATE);
  a2dp.setBufferSize(A2DP_BUFFER);
  a2dp.onConnect(onConnect);
  a2dp.begin();

  findAndConnect();
  lastScan = millis();

  Serial.println("commands: play [url] / scan / status");
}

void loop() {
  if (!a2dp.connected() && millis() - lastScan >= RETRY_MS) {
    lastScan = millis();
    findAndConnect();
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.startsWith("play")) {
      String url = cmd.substring(4);
      url.trim();
      playUrl(url.length() ? url : String(AUDIO_URL));
    } else if (cmd == "scan") {
      a2dp.disconnect();
      a2dp.clearPairing();
      findAndConnect();
      lastScan = millis();
    } else if (cmd == "status") {
      Serial.printf("wifi=%d ip=%s bt=%d\n",
                    WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str(),
                    a2dp.connected());
    }
  }

  if (BOOTSEL) {
    while (BOOTSEL) delay(1);
    playUrl(AUDIO_URL);
  }
}
