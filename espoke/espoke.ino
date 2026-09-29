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
 *   3. シリアルに "music" と打つか BOOTSEL を押すと、MUSIC_URL の音楽サーバから
 *      曲を順番に取ってきて流し続ける。"play" なら AUDIO_URL の通知音を1回だけ
 *   4. LCD1602A に曲名と状態を表示
 *
 * 音声フォーマット:
 *   16bit PCM の WAV。モノラル/ステレオどちらでも可。
 *   サンプリングレートは 44100 の整数分の1 (44100 / 22050 / 11025) のみ対応し、
 *   整数倍のサンプル&ホールドで 44100 ステレオへ引き伸ばして A2DP に流す。
 *   48000 など割り切れないレートは非対応 (リサンプラを積む余裕がないため)。
 *   mp3 や m4a はそのままでは鳴らない。tools/serve_music.py が ffmpeg で
 *   変換しながら流してくれるので、Pico 側は WAV を読むだけで済む。
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <BluetoothAudio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include "arduino_secrets.h"  // WIFI_SSID / WIFI_PASS / AUDIO_URL / MUSIC_URL

// --- 設定 ---
static const char *BT_LOCAL_NAME  = "espoke";  // イヤホン側に見える名前
static const char *BT_TARGET_NAME = "";        // 接続先の名前 (前方一致)。空なら最初の1台
static const char *BT_TARGET_ADDR = "a0:0c:e2:c6:1d:04";  // 空でなければスキャンせず直接繋ぐ
static const int    SCAN_SECONDS  = 8;
static const int    A2DP_RATE     = 44100;     // A2DPSource は 44100 か 48000 のみ
static const size_t A2DP_BUFFER   = 65536;     // 約370ms 分。曲を流し続けるので厚めに取る
static const bool   BOOTSEL_SKIP  = true;      // 再生中の BOOTSEL で次の曲へ送る

static const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)
static const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)
static const uint8_t LCD_COLS  = 16;
static const uint8_t LCD_ROWS  = 2;
static const uint8_t LCD_ADDR_DEFAULT = 0x27;
// ------------

// .ino はビルド時に関数プロトタイプが先頭へ自動生成されるので、
// 引数や戻り値に使う型はここで定義しておく必要がある
struct WavInfo {
  uint32_t sampleRate;
  uint16_t channels;
  uint16_t bits;
  uint32_t dataBytes;  // 0 なら長さ不明 (最後まで読む)
};

enum PlayResult {
  PLAY_NONE,   // 中断要求なし (再生ループの中だけで使う)
  PLAY_DONE,   // 最後まで鳴らした
  PLAY_STOP,   // stop で止めた
  PLAY_SKIP,   // next / prev / rand / BOOTSEL で打ち切った
  PLAY_ERROR,  // Wi-Fi か HTTP で失敗した
};

A2DPSource a2dp;
LiquidCrystal_I2C *lcd = nullptr;

// 1回の read で 2KB 前後まとめて取る。小刻みに読むと lwIP の往復が増えて
// スループットが落ち、Bluetooth と帯域を取り合ったときに underflow しやすい
static const size_t IN_FRAMES = 1024;
static int16_t inBuf[IN_FRAMES * 2];       // 最大ステレオ
static int16_t outBuf[IN_FRAMES * 4 * 2];  // 最大4倍アップサンプル × ステレオ

static unsigned long lastScan = 0;
static const unsigned long RETRY_MS = 15000;           // 未接続時に再試行する間隔
static const unsigned long CONNECT_TIMEOUT_MS = 15000;  // ストリーム開始を待つ上限
static const unsigned long WIFI_TIMEOUT_MS = 30000;    // Wi-Fi 接続を待つ上限
static const unsigned long WIFI_RETRY_MS = 30000;      // Wi-Fi 再接続を試みる間隔
static const unsigned long HTTP_TIMEOUT_MS = 15000;    // サーバの応答を待つ上限
static unsigned long lastWiFiTry = 0;

// 連続再生の状態
static bool   autoPlay = false;            // 曲を続けて流しているか
static String nextPath = "/next.wav";      // 次に取りに行くエンドポイント
static int    playErrors = 0;              // 連続で失敗した回数
static unsigned long lastBootsel = 0;      // BOOTSEL を最後に見た時刻

static String serialLine = "";             // 受信途中のコマンド

// 曲名はサーバが X-Track ヘッダで返してくる。collectHeaders() で拾う指定をしておく
static const char *HTTP_HEADERS[] = { "X-Track" };

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

// ---------------- シリアル ----------------

// 改行まで溜まったら1行返す。溜まっていなければ空文字列。
// readStringUntil() は改行が来るまで最大1秒ブロックするので、
// 再生中に呼ぶとその間だけ音が途切れる。こちらは待たない
static String readLine() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialLine.length()) {
        String out = serialLine;
        serialLine = "";
        out.trim();
        return out;
      }
    } else if (serialLine.length() < 160) {
      serialLine += c;
    }
  }
  return String();
}

// ---------------- Bluetooth ----------------

static void onConnect(void *, bool connected) {
  if (connected) {
    Serial.printf("A2DP connected: %s\n", bd_addr_to_str(a2dp.getSinkAddress()));
  } else {
    Serial.println("A2DP disconnected");
  }
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
  status("BT connecting", label);
  if (!a2dp.connect(addr)) {
    status("BT rejected", label);
    return false;
  }

  // connect() の戻り値は「要求が受理されたか」だけ。ここから先は非同期で
  // AVDTP シグナリング → SBC ネゴシエーション → ストリーム開始 と進み、
  // そこまで到達して初めて connected() が true になる
  unsigned long start = millis();
  while (!a2dp.connected() && millis() - start < CONNECT_TIMEOUT_MS) {
    delay(50);
  }
  if (!a2dp.connected()) {
    status("BT timeout", label);
    return false;
  }
  status("BT ready", label);
  return true;
}

static bool scanAndConnect();

static bool findAndConnect() {
  // アドレスが分かっているならスキャンを飛ばす。8秒のインクワイアリを挟まない分、
  // 相手が応答できる状態のうちに繋ぎにいける
  if (BT_TARGET_ADDR[0] != '\0') {
    uint8_t addr[6];
    if (parseAddr(BT_TARGET_ADDR, addr)) {
      return connectTo(addr, BT_TARGET_ADDR);
    }
    Serial.printf("BT_TARGET_ADDR \"%s\" を解釈できない\n", BT_TARGET_ADDR);
  }
  return scanAndConnect();
}

static bool scanAndConnect() {
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

  return connectTo(found[pick].address(), found[pick].name());
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

// 再生中にコマンドやボタンで中断されたかを見る。中断がなければ PLAY_NONE
static PlayResult pollAbort() {
  String cmd = readLine();
  if (cmd.length()) {
    if (cmd == "stop") return PLAY_STOP;
    if (cmd == "next") { nextPath = "/next.wav";   return PLAY_SKIP; }
    if (cmd == "prev") { nextPath = "/prev.wav";   return PLAY_SKIP; }
    if (cmd == "rand") { nextPath = "/random.wav"; return PLAY_SKIP; }
    Serial.println("再生中に使えるのは stop / next / prev / rand");
  }

  // BOOTSEL の読み取りは一瞬フラッシュと割り込みを止めるので、頻繁には見に行かない
  if (BOOTSEL_SKIP && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      nextPath = "/next.wav";
      return PLAY_SKIP;
    }
  }
  return PLAY_NONE;
}

static PlayResult playUrl(const String &url) {
  if (WiFi.status() != WL_CONNECTED) {
    status("no wifi", "cannot play");
    return PLAY_ERROR;
  }
  if (!a2dp.connected()) {
    status("no earphone", "cannot play");
    return PLAY_ERROR;
  }

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    status("bad url", url.substring(0, LCD_COLS));
    return PLAY_ERROR;
  }
  http.collectHeaders(HTTP_HEADERS, 1);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    status("http error", String(code));
    http.end();
    return PLAY_ERROR;
  }

  // 曲名はサーバが教えてくれる。無ければ URL の末尾で代用する
  String title = http.header("X-Track");
  if (!title.length()) {
    int slash = url.lastIndexOf('/');
    title = (slash >= 0) ? url.substring(slash + 1) : url;
  }

  WiFiClient *stream = http.getStreamPtr();
  WavInfo info = {0, 0, 0, 0};
  if (!parseWav(stream, &info)) {
    status("bad wav", "header");
    http.end();
    return PLAY_ERROR;
  }

  uint32_t rateMul = (info.sampleRate > 0) ? (uint32_t)A2DP_RATE / info.sampleRate : 0;
  if (info.bits != 16 || info.channels < 1 || info.channels > 2 ||
      rateMul == 0 || rateMul > 4 || rateMul * info.sampleRate != (uint32_t)A2DP_RATE) {
    Serial.printf("unsupported: %luHz %uch %ubit\n",
                  (unsigned long)info.sampleRate, info.channels, info.bits);
    status("unsupported", String(info.sampleRate) + "Hz " + String(info.channels) + "ch");
    http.end();
    return PLAY_ERROR;
  }

  Serial.printf("playing %s — %luHz %uch %ubit, %lu bytes (x%lu upsample)\n",
                title.c_str(), (unsigned long)info.sampleRate, info.channels, info.bits,
                (unsigned long)info.dataBytes, (unsigned long)rateMul);
  printLine(0, title);

  const size_t inBytesMax  = IN_FRAMES * info.channels * 2;
  const size_t outBytesMax = IN_FRAMES * rateMul * 2 * 2;
  uint32_t remaining = info.dataBytes;
  uint32_t played    = 0;
  unsigned long lastLcd = 0;
  PlayResult result = PLAY_DONE;

  // A2DP ストリームは接続直後から流れ続けているので、play を打つまでの無音区間でも
  // underflow フラグが立つ。再生直前に一度読み捨てて、以降の取りこぼしだけを見る
  a2dp.getUnderflow();

  while (a2dp.connected()) {
    PlayResult abort = pollAbort();
    if (abort != PLAY_NONE) {
      result = abort;
      break;
    }

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

  // 曲を続けて流すときは無音で締めない。次の曲を取りに行っている間、
  // バッファに残った末尾がそのまま鳴り続けるので繋ぎが詰まる。
  // 打ち切ったときも同じ理由で、残りをそのまま鳴らし切らせる
  if (result == PLAY_DONE && !autoPlay) {
    memset(outBuf, 0, sizeof(outBuf));
    for (int i = 0; i < 4 && a2dp.connected(); i++) {
      while ((size_t)a2dp.availableForWrite() < sizeof(outBuf)) delay(1);
      a2dp.write((const uint8_t *)outBuf, sizeof(outBuf));
    }
  }

  if (a2dp.getUnderflow()) {
    Serial.println("warning: audio underflow (Wi-Fi が追いついていない)");
  }
  printLine(1, String(played / 1024) + "KB " +
               (result == PLAY_DONE ? "done" : result == PLAY_STOP ? "stop" : "skip"));
  return result;
}

// ---------------- コマンド ----------------

// MUSIC_URL 配下のテキストを取ってきてシリアルに出す
static void showText(const String &path) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("no wifi");
    return;
  }
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String(MUSIC_URL) + path)) {
    Serial.println("MUSIC_URL がおかしい");
    return;
  }
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    Serial.println(http.getString());
  } else {
    Serial.printf("http %d (%s が動いているか?)\n", code, MUSIC_URL);
  }
  http.end();
}

static void handleCommand(String cmd) {
  if (!cmd.length()) return;

  if (cmd.startsWith("play")) {
    String url = cmd.substring(4);
    url.trim();
    autoPlay = false;
    playUrl(url.length() ? url : String(AUDIO_URL));
  } else if (cmd == "music" || cmd == "next") {
    nextPath = "/next.wav";
    autoPlay = true;
    playErrors = 0;
  } else if (cmd == "prev") {
    nextPath = "/prev.wav";
    autoPlay = true;
    playErrors = 0;
  } else if (cmd == "rand") {
    nextPath = "/random.wav";
    autoPlay = true;
    playErrors = 0;
  } else if (cmd == "stop") {
    autoPlay = false;
    status("espoke", "stopped");
  } else if (cmd == "list") {
    showText("/list");
  } else if (cmd == "now") {
    showText("/now");
  } else if (cmd == "shuffle") {
    showText("/shuffle");
  } else if (cmd == "scan") {
    autoPlay = false;
    a2dp.disconnect();
    a2dp.clearPairing();
    findAndConnect();
    lastScan = millis();
  } else if (cmd == "status") {
    Serial.printf("wifi=%d ip=%s bt=%d auto=%d next=%s\n",
                  WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str(),
                  a2dp.connected(), autoPlay, nextPath.c_str());
  } else {
    Serial.println("commands: play [url] / music / next / prev / rand / stop /"
                   " list / now / shuffle / scan / status");
  }
}

// ---------------- setup / loop ----------------

static bool connectWiFi() {
  status("WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    status("WiFi ok", WiFi.localIP().toString());
    return true;
  }
  status("WiFi failed", WIFI_SSID);
  return false;
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) delay(10);

  setupLCD();
  status("espoke", "booting...");

  connectWiFi();
  lastWiFiTry = millis();

  a2dp.setName(BT_LOCAL_NAME);
  a2dp.setFrequency(A2DP_RATE);
  a2dp.setBufferSize(A2DP_BUFFER);
  a2dp.onConnect(onConnect);
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

  Serial.println("commands: play [url] / music / next / prev / rand / stop /"
                 " list / now / shuffle / scan / status");
}

void loop() {
  // Wi-Fi は起動時に落ちることがあるので、切れていれば定期的に張り直す
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiTry >= WIFI_RETRY_MS) {
    lastWiFiTry = millis();
    connectWiFi();
  }

  if (!a2dp.connected() && millis() - lastScan >= RETRY_MS) {
    lastScan = millis();
    findAndConnect();
  }

  handleCommand(readLine());

  // 止まっているときの BOOTSEL は連続再生の開始。再生中は pollAbort() が
  // 次の曲へ送るので、ここでは autoPlay を見て二重に反応しないようにする
  if (!autoPlay && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      nextPath = "/next.wav";
      autoPlay = true;
      playErrors = 0;
    }
  }

  if (!autoPlay) return;

  // Wi-Fi かイヤホンが切れている間は再接続を待つ (張り直しは上の処理に任せる)
  if (WiFi.status() != WL_CONNECTED || !a2dp.connected()) {
    delay(100);
    return;
  }

  PlayResult r = playUrl(String(MUSIC_URL) + nextPath);
  if (r == PLAY_STOP) {
    autoPlay = false;
    nextPath = "/next.wav";
  } else if (r == PLAY_ERROR) {
    // 一時的なものかもしれないので数回は粘り、それでも駄目なら止める
    if (++playErrors >= 3) {
      autoPlay = false;
      playErrors = 0;
      status("music stopped", "server down?");
    } else {
      delay(2000);
    }
  } else {
    playErrors = 0;
    if (r == PLAY_DONE) nextPath = "/next.wav";
    // PLAY_SKIP のときは pollAbort() が nextPath を設定済み
  }
}
