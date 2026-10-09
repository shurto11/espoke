/*
 * espoke — Wi-Fi で受け取った音声を Bluetooth イヤホンで鳴らす
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble
 *       (Arduino IDE なら ツール → IP/Bluetooth Stack → "IPv4 + Bluetooth")
 *
 * 動作:
 *   1. Wi-Fi に接続し、microSD の MP3 を探して曲の一覧を作る
 *   2. Bluetooth イヤホン (A2DP sink) をスキャンして接続
 *   3. シリアルに "sd" と打つと、microSD の MP3 をパス順に流し続ける
 *      "music" なら MUSIC_URL の音楽サーバから曲を取ってきて流し続ける
 *      BOOTSEL を押すと、最後に選んだ方 (起動直後は SD に曲があれば SD) で再生を始める
 *      "play" なら AUDIO_URL の通知音を1回だけ
 *   4. LCD1602A に曲名と状態を表示。GP15 のスイッチで表示ページを切り替える
 *   5. SW1〜SW4 (GP10〜13) でモールス信号を打ってアルファベットを入力する
 *      SW1 ツー / SW2 トン / SW3 backspace / SW4 enter (符号を文字に確定。空なら空白)
 *      SW1 か SW2 を押すと入力ページに切り替わる
 *
 * 音声フォーマット:
 *   microSD: MP3 (Layer III)。Pico 上で libmad (BackgroundAudio ライブラリ同梱) でデコードする。
 *   Wi-Fi:   16bit PCM の WAV。tools/serve_music.py が mp3 や m4a を ffmpeg で
 *            変換しながら流してくれるので、Pico 側は WAV を読むだけで済む。
 *   どちらもモノラル/ステレオ可。サンプリングレートは 44100 の整数分の1 (44100 / 22050 / 11025)
 *   のみ対応し、整数倍のサンプル&ホールドで 44100 ステレオへ引き伸ばして A2DP に流す。
 *   48000 など割り切れないレートは非対応 (リサンプラを積む余裕がないため)。
 *
 * 必要なライブラリ: LiquidCrystal I2C, BackgroundAudio (arduino-cli lib install で入れる)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <BluetoothAudio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <algorithm>
// BackgroundAudio に入っている libmad を使う。arduino-cli がライブラリを見つけられるよう、
// libmad を読み込んでいるこのヘッダ経由で取り込む (再生クラス自体は使わない)
#include <BackgroundAudioMP3.h>

#include "arduino_secrets.h"  // WIFI_SSID / WIFI_PASS / AUDIO_URL / MUSIC_URL

// --- 設定 ---
static const char *BT_LOCAL_NAME  = "espoke";  // イヤホン側に見える名前
static const char *BT_TARGET_NAME = "";        // 接続先の名前 (前方一致)。空なら最初の1台
static const char *BT_TARGET_ADDR = "a0:0c:e2:c6:1d:04";  // 空でなければスキャンせず直接繋ぐ
static const int    SCAN_SECONDS  = 8;
static const int    A2DP_RATE     = 44100;     // A2DPSource は 44100 か 48000 のみ
// A2DPSource はこの値を「バイト数」ではなく「16bit サンプル数」として扱い、2倍のバイトを確保する。
// 32768 で 64KB・約370ms 分。RAM は全部で 256KB しかなく、MP3 デコーダの作業領域 (約29KB) も要るので、
// 65536 (128KB) にすると a2dp.begin() が確保に失敗して Bluetooth が動かなくなる
static const size_t A2DP_BUFFER   = 32768;
static const bool   BOOTSEL_SKIP  = true;      // 再生中の BOOTSEL で次の曲へ送る

static const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)
static const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)
static const uint8_t LCD_COLS  = 16;
static const uint8_t LCD_ROWS  = 2;
static const uint8_t LCD_ADDR_DEFAULT = 0x27;
static const int     PIN_BUTTON = 15;  // GP15 (物理20番ピン)。もう片側は GND へ
static const int     PIN_DASH   = 10;  // SW1 GP10 (物理14番ピン) ツー
static const int     PIN_DOT    = 11;  // SW2 GP11 (物理15番ピン) トン
static const int     PIN_BACK   = 12;  // SW3 GP12 (物理16番ピン) backspace
static const int     PIN_ENTER  = 13;  // SW4 GP13 (物理17番ピン) enter

// microSD (CK-40)。配線は docs/sd_music_parts.md の 3 章
static const int     PIN_SD_MISO = 16;  // GP16 (物理21番ピン) ⑦ DAT0
static const int     PIN_SD_SCK  = 18;  // GP18 (物理24番ピン) ⑤ CLK
static const int     PIN_SD_MOSI = 19;  // GP19 (物理25番ピン) ③ CMD
static const int     PIN_SD_CS   = 20;  // GP20 (物理26番ピン) ② DAT3/CS
static const size_t  SD_MAX_TRACKS = 300;  // 曲のパスは RAM に持つので上限を設ける (1曲 70 バイト前後)
static const int     SD_MAX_DEPTH  = 5;    // フォルダを潜る深さ
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
  PLAY_ERROR,  // Wi-Fi・HTTP・SD のどれかで失敗した
};

enum Source { SRC_SD, SRC_WIFI };           // 連続再生で曲を取ってくる先
enum Step { STEP_NEXT, STEP_PREV, STEP_RAND };  // 次にどの曲へ進むか

// MP3 デコーダの作業領域 (約 29KB)
static const size_t MP3_IN_SIZE = 4096;  // 192kbps なら1フレーム約 630 バイト、320kbps でも約 1KB
struct Mp3Decoder {
  struct mad_stream stream;
  struct mad_frame  frame;
  struct mad_synth  synth;
  uint8_t           in[MP3_IN_SIZE + MAD_BUFFER_GUARD];
  bool              eof;  // ファイルを読み終え、末尾に MAD_BUFFER_GUARD 分の 0 を足した
};

A2DPSource a2dp;
LiquidCrystal_I2C *lcd = nullptr;

// 1回の read で 2KB 前後まとめて取る。小刻みに読むと lwIP の往復が増えて
// スループットが落ち、Bluetooth と帯域を取り合ったときに underflow しやすい
static const size_t IN_FRAMES = 1024;
static int16_t inBuf[IN_FRAMES * 2];       // 最大ステレオ

// A2DPSource::write() (arduino-pico 6.1.0) は、リングバッファの終わりをまたぐ書き込みで
// 後半にデータの先頭部分をもう一度書いてしまう (2回目の memcpy が buffer から読み直している)。
// 書き込みを常に A2DP_CHUNK サンプルずつにして、バッファの大きさをちょうど割り切らせ、
// 終わりをまたがないようにする。MP3 の1フレーム (2304 サンプル) をそのまま書くと、
// 約 0.37 秒ごとに音が壊れる
static const size_t A2DP_CHUNK = 2048;     // 16bit サンプル数 (1024 ステレオフレーム、4KB)
static_assert(A2DP_BUFFER % A2DP_CHUNK == 0, "A2DP_BUFFER must be a multiple of A2DP_CHUNK");
static int16_t pcmChunk[A2DP_CHUNK];
static size_t  pcmChunkLen = 0;            // pcmChunk に溜まっているサンプル数

static unsigned long lastScan = 0;
static const unsigned long RETRY_MS = 15000;           // 未接続時に再試行する間隔
static const unsigned long CONNECT_TIMEOUT_MS = 15000;  // ストリーム開始を待つ上限
static const unsigned long WIFI_TIMEOUT_MS = 30000;    // Wi-Fi 接続を待つ上限
static const unsigned long WIFI_RETRY_MS = 30000;      // Wi-Fi 再接続を試みる間隔
static const unsigned long HTTP_TIMEOUT_MS = 15000;    // サーバの応答を待つ上限
static unsigned long lastWiFiTry = 0;
static bool   wifiEnabled = true;          // "wifi off" で false にすると、つなぎ直しもしない

// 連続再生の状態
static bool   autoPlay = false;            // 曲を続けて流しているか
static Source source = SRC_WIFI;           // 起動時に SD に曲があれば SRC_SD にする
static Step   nextStep = STEP_NEXT;        // 次の曲の選び方
static int    playErrors = 0;              // 連続で失敗した回数
static unsigned long lastBootsel = 0;      // BOOTSEL を最後に見た時刻

// microSD の曲一覧 (パス順)
static bool                sdReady = false;
static std::vector<String> sdTracks;
static int                 sdIndex = -1;   // 最後に選んだ曲。-1 なら次は先頭から
// 再生のたびに確保すると、ヒープが細切れになったときに確保できなくなるので、最初から持っておく
static Mp3Decoder          mp3;

static String serialLine = "";             // 受信途中のコマンド

// LCD の表示ページ。0 は曲名や状態を出す通常画面で、スイッチを押すたびに次へ進む
enum Page { PAGE_MAIN, PAGE_INPUT, PAGE_WIFI, PAGE_BT, PAGE_SYSTEM, PAGE_COUNT };
static int    page = PAGE_MAIN;
static String mainLines[2];                // 通常画面の内容。他のページを見ている間も更新しておく
static unsigned long lastPageDraw = 0;

static const unsigned long DEBOUNCE_MS = 30;
static bool   buttonStable = HIGH;         // チャタリングを除いた状態 (INPUT_PULLUP なので離すと HIGH)
static bool   buttonRaw = HIGH;
static unsigned long buttonChanged = 0;

// モールス入力
struct KeyState {
  int           pin;
  bool          raw;
  bool          stable;
  unsigned long changedAt;
};
enum { KEY_DASH, KEY_DOT, KEY_BACK, KEY_ENTER, KEY_COUNT };
static KeyState keys[KEY_COUNT] = {
  {PIN_DASH,  HIGH, HIGH, 0},
  {PIN_DOT,   HIGH, HIGH, 0},
  {PIN_BACK,  HIGH, HIGH, 0},
  {PIN_ENTER, HIGH, HIGH, 0},
};

struct Morse {
  char        ch;
  const char *code;
};
static const Morse MORSE_TABLE[] = {
  {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
  {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
  {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
  {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
  {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
  {'Z', "--.."},
  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
  {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
};
static const size_t MAX_CODE = 6;          // 1文字の符号は最長5つ (数字)
static const size_t MAX_TEXT = 64;
static String morseCode = "";              // 入力中の符号 ("-" と ".")
static String morseText = "";              // 確定した文章

// 曲名はサーバが X-Track ヘッダで返してくる。collectHeaders() で拾う指定をしておく
static const char *HTTP_HEADERS[] = { "X-Track" };

// ---------------- LCD ----------------

static void drawRow(uint8_t row, const String &text) {
  if (!lcd) return;
  String s = text.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s);
}

// 通常画面の1行を書き換える。他のページを表示中なら覚えておくだけ
static void printLine(uint8_t row, const String &text) {
  mainLines[row] = text;
  if (page == PAGE_MAIN) drawRow(row, text);
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

// 符号を文字にする。表になければ 0
static char decodeMorse(const String &code) {
  for (const Morse &m : MORSE_TABLE) {
    if (code == m.code) return m.ch;
  }
  return 0;
}

// 今のページを描き直す。PAGE_MAIN 以外は中身が刻々と変わるので定期的に呼ぶ
static void drawPage() {
  lastPageDraw = millis();
  switch (page) {
    case PAGE_WIFI:
      if (WiFi.status() == WL_CONNECTED) {
        drawRow(0, "WiFi " + String(WiFi.RSSI()) + "dBm");
        drawRow(1, WiFi.localIP().toString());
      } else {
        drawRow(0, "WiFi --");
        drawRow(1, WIFI_SSID);
      }
      break;
    case PAGE_BT:
      if (a2dp.connected()) {
        // "aa:bb:cc:dd:ee:ff" は17文字で1行に収まらないのでコロンを抜く
        String addr = bd_addr_to_str(a2dp.getSinkAddress());
        addr.replace(":", "");
        drawRow(0, "BT connected");
        drawRow(1, addr);
      } else {
        drawRow(0, "BT --");
        drawRow(1, BT_TARGET_ADDR[0] ? BT_TARGET_ADDR : "scan");
      }
      break;
    case PAGE_INPUT: {
      // 1行目は末尾にカーソル代わりの '_' を付け、収まらなければ末尾側を見せる
      String top = morseText + "_";
      if (top.length() > LCD_COLS) top = top.substring(top.length() - LCD_COLS);
      drawRow(0, top);
      // 2行目は左に符号、右端に確定したときの文字
      String bottom = morseCode;
      if (morseCode.length()) {
        char ch = decodeMorse(morseCode);
        while (bottom.length() < LCD_COLS - 3) bottom += ' ';
        bottom += "= ";
        bottom += ch ? ch : '?';
      }
      drawRow(1, bottom);
      break;
    }
    case PAGE_SYSTEM: {
      unsigned long sec = millis() / 1000;
      char up[17];
      snprintf(up, sizeof(up), "up %luh%02lum%02lus", sec / 3600, sec / 60 % 60, sec % 60);
      drawRow(0, up);
      drawRow(1, "heap " + String(rp2040.getFreeHeap() / 1024) + "KB");
      break;
    }
    default:
      drawRow(0, mainLines[0]);
      drawRow(1, mainLines[1]);
      break;
  }
}

static void onKey(int id) {
  switch (id) {
    case KEY_DASH:
    case KEY_DOT:
      if (morseCode.length() < MAX_CODE) morseCode += (id == KEY_DASH) ? '-' : '.';
      break;
    case KEY_BACK:
      if (morseCode.length()) {
        morseCode.remove(morseCode.length() - 1);
      } else if (morseText.length()) {
        morseText.remove(morseText.length() - 1);
      }
      break;
    case KEY_ENTER:
      if (morseCode.length()) {
        char ch = decodeMorse(morseCode);
        if (ch && morseText.length() < MAX_TEXT) morseText += ch;
        if (!ch) Serial.printf("unknown code: %s\n", morseCode.c_str());
        morseCode = "";
      } else if (morseText.length() && morseText.length() < MAX_TEXT) {
        morseText += ' ';
      }
      break;
  }
  Serial.printf("morse code=\"%s\" text=\"%s\"\n", morseCode.c_str(), morseText.c_str());
  // 符号を打ち始めたら入力ページへ。backspace / enter は入力ページを見ているときだけ表示に効く
  if (id == KEY_DASH || id == KEY_DOT) page = PAGE_INPUT;
  if (page == PAGE_INPUT) drawPage();
}

// スイッチを見て、押されたらページを送る。再生中も呼ばれるので待たずに返す
static void pollButton() {
  for (int i = 0; i < KEY_COUNT; i++) {
    KeyState &k = keys[i];
    bool raw = digitalRead(k.pin);
    if (raw != k.raw) {
      k.raw = raw;
      k.changedAt = millis();
    } else if (raw != k.stable && millis() - k.changedAt >= DEBOUNCE_MS) {
      k.stable = raw;
      if (raw == LOW) onKey(i);
    }
  }

  bool raw = digitalRead(PIN_BUTTON);
  if (raw != buttonRaw) {
    buttonRaw = raw;
    buttonChanged = millis();
  } else if (raw != buttonStable && millis() - buttonChanged >= DEBOUNCE_MS) {
    buttonStable = raw;
    if (buttonStable == LOW) {
      page = (page + 1) % PAGE_COUNT;
      Serial.printf("page %d\n", page);
      drawPage();
    }
  }

  if (page != PAGE_MAIN && page != PAGE_INPUT && millis() - lastPageDraw >= 1000) drawPage();
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
    pollButton();
    delay(10);
  }
  if (!a2dp.connected()) {
    status("BT timeout", label);
    return false;
  }
  feedSilence();  // ストリームはもう始まっているので、初期化されていないバッファが鳴る前に埋める
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

// 溜まった pcmChunk を A2DP に書く。丸ごと入る空きができるまで待つ
static void flushPcmChunk() {
  while (a2dp.connected() && (size_t)a2dp.availableForWrite() < sizeof(pcmChunk)) delay(1);
  if (a2dp.connected()) a2dp.write((const uint8_t *)pcmChunk, sizeof(pcmChunk));
  pcmChunkLen = 0;
}

// 鳴らすものがない間も、A2DP のバッファを無音で満たしておく。
// A2DPSource はバッファが空になると、読み出し位置の 256 サンプル (約 2.9ms) を送り直し続ける。
// そこにあるのは1周前の音か、確保しただけで初期化されていないメモリなので、
// 約 344Hz の繰り返しになり、音量に関係なく大きなピーという音が鳴る。
// 1周分を無音で書けば、その後にバッファが空になっても送り直されるのは無音になる。待たずに返す
static void feedSilence() {
  while (a2dp.connected() && (size_t)a2dp.availableForWrite() >= sizeof(pcmChunk)) {
    memset(pcmChunk + pcmChunkLen, 0, (A2DP_CHUNK - pcmChunkLen) * sizeof(int16_t));
    a2dp.write((const uint8_t *)pcmChunk, sizeof(pcmChunk));
    pcmChunkLen = 0;
  }
}

// 16bit PCM (channels 本のインターリーブ) を rateMul 倍のサンプル&ホールドで
// 44100 ステレオに引き伸ばして A2DP に書く。A2DP_CHUNK に満たない端数は次に回す
static void writePcm(const int16_t *in, size_t frames, int channels, uint32_t rateMul) {
  for (size_t f = 0; f < frames; f++) {
    int16_t l = in[f * channels];
    int16_t r = (channels == 2) ? in[f * channels + 1] : l;
    for (uint32_t k = 0; k < rateMul; k++) {
      pcmChunk[pcmChunkLen++] = l;
      pcmChunk[pcmChunkLen++] = r;
      if (pcmChunkLen == A2DP_CHUNK) flushPcmChunk();
    }
  }
}

// 1曲の再生を締める。summary は LCD の2行目に出す再生量、slowCause は音が途切れたときの原因の説明
static void finishPlayback(PlayResult result, const String &summary, const char *slowCause) {
  // 曲を続けて流すときは無音で締めない。次の曲を取りに行っている間、
  // バッファに残った末尾がそのまま鳴り続けるので繋ぎが詰まる。
  // 打ち切ったときも同じ理由で、残りをそのまま鳴らし切らせる
  if (result == PLAY_DONE && !autoPlay) {
    // 端数を無音で埋めて書き出し、続けてバッファ1周分 (約370ms) の無音を流す
    memset(pcmChunk + pcmChunkLen, 0, (A2DP_CHUNK - pcmChunkLen) * sizeof(int16_t));
    flushPcmChunk();
    for (size_t i = 0; i < A2DP_BUFFER / A2DP_CHUNK && a2dp.connected(); i++) {
      memset(pcmChunk, 0, sizeof(pcmChunk));
      flushPcmChunk();
    }
  }

  if (a2dp.getUnderflow()) {
    Serial.printf("warning: audio underflow (%s)\n", slowCause);
  }
  printLine(1, summary + " " +
               (result == PLAY_DONE ? "done" : result == PLAY_STOP ? "stop" :
                result == PLAY_SKIP ? "skip" : "error"));
}

// "wifi off" で切断し、つなぎ直しもやめる。"wifi on" で loop() からのつなぎ直しを再開する
// (SD から流している間は、止めるまでつながない)。WiFi.disconnect() は Wi-Fi の接続を切るだけで、
// 無線チップは止めないので Bluetooth はそのまま使える
static void setWiFi(const String &cmd) {
  if (cmd == "wifi off") {
    wifiEnabled = false;
    WiFi.disconnect();
    status("WiFi off", "");
  } else if (cmd == "wifi on") {
    wifiEnabled = true;
    lastWiFiTry = millis() - WIFI_RETRY_MS;  // 次の loop() ですぐつなぎにいく
    Serial.println("WiFi: reconnect when not playing from SD");
  } else {
    Serial.printf("wifi=%s connected=%d\n", wifiEnabled ? "on" : "off", WiFi.status() == WL_CONNECTED);
  }
}

// 再生中にコマンドやボタンで中断されたかを見る。中断がなければ PLAY_NONE
static PlayResult pollAbort() {
  pollButton();

  String cmd = readLine();
  if (cmd.length()) {
    if (cmd == "stop") return PLAY_STOP;
    if (cmd == "next") { nextStep = STEP_NEXT; return PLAY_SKIP; }
    if (cmd == "prev") { nextStep = STEP_PREV; return PLAY_SKIP; }
    if (cmd == "rand") { nextStep = STEP_RAND; return PLAY_SKIP; }
    if (cmd.startsWith("wifi")) {
      setWiFi(cmd);
      return PLAY_NONE;
    }
    Serial.println("再生中に使えるのは stop / next / prev / rand / wifi on / wifi off");
  }

  // BOOTSEL の読み取りは一瞬フラッシュと割り込みを止めるので、頻繁には見に行かない
  if (BOOTSEL_SKIP && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      nextStep = STEP_NEXT;
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
  // SD の再生で省電力にしていたら戻す。WAV を取ってくるには常時受信のほうが速い
  WiFi.noLowPowerMode();

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
  // 書く量に、pcmChunk に溜まっている端数を書き出す分 (最大1チャンク) を足す
  const size_t outBytesMax = IN_FRAMES * rateMul * 2 * 2 + sizeof(pcmChunk);
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

    writePcm(inBuf, got / (info.channels * 2), info.channels, rateMul);

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

  finishPlayback(result, String(played / 1024) + "KB", "Wi-Fi が追いついていない");
  return result;
}

// ---------------- microSD ----------------

static bool isMp3(const String &name) {
  // macOS がコピー時に作る "._曲名.mp3" は中身が MP3 ではないので除く
  if (name.startsWith("._")) return false;
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3");
}

static void scanSdDir(const String &dir, int depth) {
  File d = SD.open(dir);
  if (!d) return;
  while (sdTracks.size() < SD_MAX_TRACKS) {
    File e = d.openNextFile();
    if (!e) break;
    String name = e.name();
    String path = dir + (dir.endsWith("/") ? "" : "/") + name;
    if (e.isDirectory()) {
      if (depth + 1 < SD_MAX_DEPTH && name != "System Volume Information") scanSdDir(path, depth + 1);
    } else if (isMp3(name)) {
      sdTracks.push_back(path);
    }
    e.close();
  }
  d.close();
}

// カードを (挿し直されていても) 初期化し直し、MP3 の一覧を作り直す
static bool setupSD() {
  SD.end();
  SPI.setRX(PIN_SD_MISO);
  SPI.setSCK(PIN_SD_SCK);
  SPI.setTX(PIN_SD_MOSI);
  sdReady = SD.begin(PIN_SD_CS);
  sdTracks.clear();
  if (!sdReady) {
    Serial.println("SD: no card");
    return false;
  }
  scanSdDir("/", 0);
  // アルバムのフォルダ順・曲番号順に並ぶよう、大文字小文字を区別せずパスで並べる
  std::sort(sdTracks.begin(), sdTracks.end(), [](const String &a, const String &b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
  if (sdIndex >= (int)sdTracks.size()) sdIndex = -1;
  Serial.printf("SD: %u tracks%s\n", (unsigned)sdTracks.size(),
                sdTracks.size() >= SD_MAX_TRACKS ? " (limit reached)" : "");
  return !sdTracks.empty();
}

// nextStep に従って次の曲を選ぶ
static int pickSdTrack() {
  int n = sdTracks.size();
  switch (nextStep) {
    case STEP_PREV:
      return sdIndex <= 0 ? n - 1 : sdIndex - 1;
    case STEP_RAND: {
      if (sdIndex < 0 || n == 1) return random(n);
      int r = random(n - 1);
      return r >= sdIndex ? r + 1 : r;  // 今の曲以外から選ぶ
    }
    default:
      return (sdIndex + 1) % n;
  }
}

// パスの最後の要素から拡張子を除いて曲名にする
static String trackTitle(const String &path) {
  String name = path.substring(path.lastIndexOf('/') + 1);
  int dot = name.lastIndexOf('.');
  return dot > 0 ? name.substring(0, dot) : name;
}

// ID3v2 タグ (曲名や画像が入っている) を読み飛ばし、最初の MP3 フレームの位置へ進める
static void skipId3(File &f) {
  uint8_t h[10];
  if (f.read(h, 10) == 10 && memcmp(h, "ID3", 3) == 0) {
    // サイズは 7bit ずつの 4 バイト (syncsafe)。フッタ付きならさらに 10 バイト
    uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                    ((uint32_t)(h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    f.seek(10 + size + ((h[5] & 0x10) ? 10 : 0));
  } else {
    f.seek(0);
  }
}

// 未使用の入力を前に詰め、空いたところをファイルから埋める。ファイルを読み終えたら
// MAD_BUFFER_GUARD 分の 0 を足して最後のフレームまで出させる。もう足すものがなければ false
static bool mp3Refill(Mp3Decoder *m, File &f) {
  if (m->eof) return false;
  size_t keep = 0;
  if (m->stream.next_frame) {
    keep = m->stream.bufend - m->stream.next_frame;
    memmove(m->in, m->stream.next_frame, keep);
  }
  int n = f.read(m->in + keep, MP3_IN_SIZE - keep);
  if (n <= 0) {
    memset(m->in + keep, 0, MAD_BUFFER_GUARD);
    n = MAD_BUFFER_GUARD;
    m->eof = true;
  }
  mad_stream_buffer(&m->stream, m->in, keep + n);
  m->stream.error = MAD_ERROR_NONE;
  return true;
}

// 1フレーム分デコードして m->synth.pcm に出す。曲の終わりか、続けられないエラーなら false
static bool mp3Decode(Mp3Decoder *m, File &f) {
  while (true) {
    if (m->stream.buffer == nullptr || m->stream.error == MAD_ERROR_BUFLEN) {
      if (!mp3Refill(m, f)) return false;
    }
    if (mad_frame_decode(&m->frame, &m->stream) == 0) break;
    if (MAD_RECOVERABLE(m->stream.error)) continue;  // 壊れたフレームやタグの残りは飛ばして同期を取り直す
    if (m->stream.error != MAD_ERROR_BUFLEN) {
      Serial.printf("mp3 decode error 0x%04x\n", m->stream.error);
      return false;
    }
  }
  mad_synth_frame(&m->synth, &m->frame);
  return true;
}

static PlayResult playSd(int index) {
  if (!a2dp.connected()) {
    status("no earphone", "cannot play");
    return PLAY_ERROR;
  }
  const String &path = sdTracks[index];
  String title = trackTitle(path);

  File f = SD.open(path, FILE_READ);
  if (!f) {
    status("sd open error", title);
    return PLAY_ERROR;
  }
  // arduino-pico は Wi-Fi をつなぐと省電力を切る (常時受信)。そのままだと同じチップの
  // Bluetooth と無線を取り合い、音がブツブツ途切れたので、SD から流す間は省電力にする
  if (WiFi.status() == WL_CONNECTED) WiFi.defaultLowPowerMode();

  Mp3Decoder *m = &mp3;
  mad_stream_init(&m->stream);
  mad_frame_init(&m->frame);
  mad_synth_init(&m->synth);
  m->eof = false;
  skipId3(f);

  Serial.printf("playing %s (%d/%u) — %lu bytes\n", path.c_str(), index + 1,
                (unsigned)sdTracks.size(), (unsigned long)f.size());
  printLine(0, title);

  PlayResult result = PLAY_DONE;
  uint32_t rateMul = 1;
  uint32_t frames = 0;
  uint64_t decodeUs = 0;
  uint32_t underflows = 0;
  uint32_t maxGapUs = 0;            // loop が1周するのにかかった最長時間。長ければ何かに止められている
  uint32_t lastIter = micros();
  unsigned long lastLcd = 0;
  unsigned long started = millis();
  a2dp.getUnderflow();  // playUrl と同じく、再生前の無音区間の underflow は数えない

  while (a2dp.connected()) {
    uint32_t now = micros();
    if (now - lastIter > maxGapUs) maxGapUs = now - lastIter;
    lastIter = now;

    PlayResult abort = pollAbort();
    if (abort != PLAY_NONE) {
      result = abort;
      break;
    }

    // MP3 の1フレームは最大 1152 サンプル。引き伸ばした後のステレオ分と、
    // pcmChunk に溜まっている端数を書き出す分 (最大1チャンク) が入るまで待つ
    if ((size_t)a2dp.availableForWrite() < 1152 * 4 * rateMul + sizeof(pcmChunk)) {
      delay(1);
      continue;
    }

    uint32_t t0 = micros();
    if (!mp3Decode(m, f)) {
      if (!m->eof) result = PLAY_ERROR;
      break;
    }
    decodeUs += micros() - t0;
    frames++;

    struct mad_pcm &pcm = m->synth.pcm;
    rateMul = pcm.samplerate ? (uint32_t)A2DP_RATE / pcm.samplerate : 0;
    if (rateMul == 0 || rateMul > 4 || rateMul * pcm.samplerate != (uint32_t)A2DP_RATE) {
      Serial.printf("unsupported: %uHz\n", pcm.samplerate);
      status("unsupported", String(pcm.samplerate) + "Hz");
      result = PLAY_ERROR;
      break;
    }
    // libmad はモノラルのとき左だけ埋めるので、右にも写してステレオとして扱う
    if (pcm.channels == 1) {
      for (size_t i = 0; i < pcm.length; i++) pcm.samplesX[i][1] = pcm.samplesX[i][0];
    }
    writePcm(&pcm.samplesX[0][0], pcm.length, 2, rateMul);

    if (millis() - lastLcd >= 500) {
      lastLcd = millis();
      printLine(1, "sd " + String((uint32_t)((uint64_t)f.position() * 100 / f.size())) + "%");
      // 途切れたらその場で出す。耳で聞いた途切れとログを突き合わせられるように。
      // 最初の1秒は、止まっていて空だったバッファが溜まるまでの分なので数えない
      if (a2dp.getUnderflow() && millis() - started >= 1000) {
        underflows++;
        Serial.printf("underflow at %lus (wifi %s)\n", (millis() - started) / 1000,
                      WiFi.status() == WL_CONNECTED ? "connected" : "off");
      }
    }
  }

  uint32_t pos = f.position();
  f.close();
  mad_synth_finish(&m->synth);
  mad_frame_finish(&m->frame);
  mad_stream_finish(&m->stream);

  // 1フレームは 1152 サンプル = 約 26ms。デコードにその何割を使ったかで CPU の余裕が分かる
  if (frames) {
    uint32_t avgUs = decodeUs / frames;
    Serial.printf("mp3: %lu frames, decode avg %lu us/frame (%lu%% of real time), "
                  "longest loop %lu ms, underflow %lu times\n",
                  (unsigned long)frames, (unsigned long)avgUs,
                  (unsigned long)(avgUs * 100 / (1152UL * 1000000 / A2DP_RATE)),
                  (unsigned long)(maxGapUs / 1000), (unsigned long)underflows);
  }
  finishPlayback(result, String(pos / 1024) + "KB", "SD の読み込みかデコードが追いついていない");
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

static void startAutoPlay(Step step) {
  nextStep = step;
  autoPlay = true;
  playErrors = 0;
}

static void printSdTracks() {
  if (sdTracks.empty()) {
    Serial.println(sdReady ? "SD: no mp3" : "SD: no card");
    return;
  }
  for (size_t i = 0; i < sdTracks.size(); i++) {
    Serial.printf("%c%3u %s\n", (int)i == sdIndex ? '>' : ' ', (unsigned)(i + 1), sdTracks[i].c_str());
  }
}

static const char *HELP =
    "commands: sd / music / play [url] / next / prev / rand / stop /"
    " list / now / shuffle / wifi on|off / scan / status";

static void handleCommand(String cmd) {
  if (!cmd.length()) return;

  if (cmd.startsWith("play")) {
    String url = cmd.substring(4);
    url.trim();
    autoPlay = false;
    playUrl(url.length() ? url : String(AUDIO_URL));
  } else if (cmd == "sd") {
    // カードを挿し直したときのために、毎回読み直す
    autoPlay = false;
    if (setupSD()) {
      source = SRC_SD;
      startAutoPlay(STEP_NEXT);
    } else {
      status("sd", sdReady ? "no mp3" : "no card");
    }
  } else if (cmd == "music") {
    source = SRC_WIFI;
    startAutoPlay(STEP_NEXT);
  } else if (cmd == "next") {
    startAutoPlay(STEP_NEXT);
  } else if (cmd == "prev") {
    startAutoPlay(STEP_PREV);
  } else if (cmd == "rand") {
    startAutoPlay(STEP_RAND);
  } else if (cmd == "stop") {
    autoPlay = false;
    status("espoke", "stopped");
  } else if (cmd == "list") {
    if (source == SRC_SD) printSdTracks();
    else showText("/list");
  } else if (cmd == "now") {
    if (source != SRC_SD) showText("/now");
    else if (sdIndex >= 0) Serial.printf("%d/%u %s\n", sdIndex + 1, (unsigned)sdTracks.size(), sdTracks[sdIndex].c_str());
    else Serial.println("SD: not started");
  } else if (cmd == "shuffle") {
    if (source == SRC_SD) Serial.println("SD では rand で曲を飛ばす");
    else showText("/shuffle");
  } else if (cmd.startsWith("wifi")) {
    setWiFi(cmd);
  } else if (cmd == "scan") {
    autoPlay = false;
    a2dp.disconnect();
    a2dp.clearPairing();
    findAndConnect();
    lastScan = millis();
  } else if (cmd == "status") {
    Serial.printf("wifi=%d ip=%s bt=%d src=%s auto=%d sd=%d tracks=%u heap=%luKB page=%d button=%s text=\"%s\"\n",
                  WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str(),
                  a2dp.connected(), source == SRC_SD ? "sd" : "wifi", autoPlay, sdReady,
                  (unsigned)sdTracks.size(), (unsigned long)(rp2040.getFreeHeap() / 1024), page,
                  digitalRead(PIN_BUTTON) == LOW ? "pressed" : "released",
                  morseText.c_str());
  } else {
    Serial.println(HELP);
  }
}

// ---------------- setup / loop ----------------

static bool connectWiFi() {
  status("WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    pollButton();
    delay(10);
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

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  for (KeyState &k : keys) pinMode(k.pin, INPUT_PULLUP);
  setupLCD();
  status("espoke", "booting...");

  connectWiFi();
  lastWiFiTry = millis();

  a2dp.setName(BT_LOCAL_NAME);
  a2dp.setFrequency(A2DP_RATE);
  a2dp.setBufferSize(A2DP_BUFFER);
  a2dp.onConnect(onConnect);
  // A2DP のバッファは大きいので、ヒープが細切れになる前 (SD の曲一覧を作る前) に確保させる
  if (!a2dp.begin()) {
    status("BT init failed", "out of memory?");
  }

  // BTstack は User Confirmation Request をアプリに投げるだけで、既定では自動応答
  // しない (hci.c の ssp_auto_accept は 0)。イヤホンも Pico も入力装置を持たない
  // NoInputNoOutput なので Just Works ペアリングになり、ここを自動承認しないと
  // SSP が確認待ちのまま止まる
  {
    BluetoothLock b;
    gap_ssp_set_auto_accept(true);
  }

  // SD に曲があれば、BOOTSEL や next で SD から流す
  if (setupSD()) source = SRC_SD;

  findAndConnect();
  lastScan = millis();

  Serial.println(HELP);
}

void loop() {
  // Wi-Fi は起動時に落ちることがあるので、切れていれば定期的に張り直す。
  // ただし張り直しは最大 WIFI_TIMEOUT_MS 待つので、SD から流している間はやらない
  // (外で聞いているときに、曲の合間が無音で止まってしまう)
  bool sdPlaying = autoPlay && source == SRC_SD;
  if (wifiEnabled && !sdPlaying && WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiTry >= WIFI_RETRY_MS) {
    lastWiFiTry = millis();
    connectWiFi();
  }

  if (!a2dp.connected() && millis() - lastScan >= RETRY_MS) {
    lastScan = millis();
    findAndConnect();
  }

  pollButton();
  handleCommand(readLine());

  // 止まっているときの BOOTSEL は連続再生の開始。再生中は pollAbort() が
  // 次の曲へ送るので、ここでは autoPlay を見て二重に反応しないようにする
  if (!autoPlay && millis() - lastBootsel >= 250) {
    lastBootsel = millis();
    if (BOOTSEL) {
      while (BOOTSEL) delay(1);
      startAutoPlay(STEP_NEXT);
    }
  }

  // 止まっている間は無音を流しておく。連続再生中は曲の切れ目に無音を挟まないよう、ここでは書かない
  if (!autoPlay) {
    feedSilence();
    return;
  }

  // イヤホン (Wi-Fi から流すときは Wi-Fi も) が切れている間は再接続を待つ (張り直しは上の処理に任せる)
  if (!a2dp.connected() || (source == SRC_WIFI && WiFi.status() != WL_CONNECTED)) {
    feedSilence();
    delay(100);
    return;
  }

  PlayResult r;
  if (source == SRC_SD) {
    if (sdTracks.empty()) {
      autoPlay = false;
      status("sd", sdReady ? "no mp3" : "no card");
      return;
    }
    sdIndex = pickSdTrack();
    r = playSd(sdIndex);
  } else {
    static const char *const STEP_PATHS[] = {"/next.wav", "/prev.wav", "/random.wav"};  // Step の順
    r = playUrl(String(MUSIC_URL) + STEP_PATHS[nextStep]);
  }

  if (r == PLAY_STOP) {
    autoPlay = false;
    nextStep = STEP_NEXT;
  } else if (r == PLAY_ERROR) {
    // 一時的なものかもしれないので数回は粘り、それでも駄目なら止める。
    // SD では同じ向きに次の曲へ進むので、読めない曲が1曲あっても飛ばして続く
    if (++playErrors >= 3) {
      autoPlay = false;
      playErrors = 0;
      status("music stopped", source == SRC_SD ? "sd card?" : "server down?");
    } else if (source == SRC_WIFI) {
      delay(2000);
    }
  } else {
    playErrors = 0;
    if (r == PLAY_DONE) nextStep = STEP_NEXT;
    // PLAY_SKIP のときは pollAbort() が nextStep を設定済み
  }
}
