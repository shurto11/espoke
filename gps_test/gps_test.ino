/*
 * GPS (M5Stack Unit GPS v1.1 = AT6668 + MAX2659) の配線と受信の確認
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow
 * 配線: docs/board_layout.svg の GPS 用ピン
 *   GP4 (物理6番, UART1 TX) → Grove の白 (GPS の入力。Unit の表示は TX)
 *   GP5 (物理7番, UART1 RX) ← Grove の黄 (GPS の出力。Unit の表示は RX)
 *   5V → 赤、GND → 黒
 *
 * 動作:
 *   1. UART を始める前に、GPIO だけで線を見る
 *      - GP5 (黄) に GPS の信号が来ているか。来ていなければ、HIGH のままか LOW のままかで原因を絞る
 *      - GP4 (白) に GPS の信号が来ていないか (来ていれば黄と白が逆)
 *      - GP4・GP5 が隣の GP3・GP6 や、互いにショートしていないか
 *   2. 115200bps で NMEA を受け、チェックサムが合う文が来るか (GPS → Pico)
 *      来なければほかの速さも試す (速さが変えられているのか、線が来ていないのかを分ける)
 *   3. $PCAS06,0 (版数を聞く) を送り、SW= の返事が来るか (Pico → GPS)
 *   4. 全部通ったら基板上の LED を点け、以後 5 秒ごとに受信状況を表示する
 *      (衛星の数と信号の強さ・測位できたか・UTC 時刻)
 *
 * シリアル (115200bps) に送るコマンド:
 *   r: もう一度最初からテストする
 *   n: GPS から来た NMEA をそのまま流す / 止める
 *   v: 版数と測位モードを聞き直す
 *   d: GP4・GP5 の HIGH が GPS の出力か抵抗のプルアップかを見分ける (データが来ないときの切り分け)
 *
 * LCD (GP0/GP1) にも 1 秒ごとに出すので、モバイルバッテリーで外へ持ち出しても受信を確かめられる:
 *   1 行目: 測位前は「heard 衛星の数 max 一番強い信号 (dB)」、測位したら「FIX used 使っている衛星の数」
 *           NMEA が来ていなければ「no NMEA」
 *   2 行目: GPS から取れた UTC 時刻。まだなら「no time」と起動からの秒数
 * 測位すると、Unit の LED も 1 秒ごとに点滅する (PPS)
 */
#include <hardware/structs/sio.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- 設定 ---
const int      PIN_TX   = 4;  // GP4 (物理6番) UART1 TX → GPS の入力 (白)
const int      PIN_RX   = 5;  // GP5 (物理7番) UART1 RX ← GPS の出力 (黄)
const int      PIN_NB_TX = 3;  // GP3 (物理5番) GP4 の隣
const int      PIN_NB_RX = 6;  // GP6 (物理9番) GP5 の隣の隣 (間の 8番ピンは GND)
const int      PIN_GND_WRONG = 2;  // GP2 (物理4番)。GPS 用ピンの GND (5,16) の真上にあり、3番ピン (GND) と間違えやすい
const uint32_t GPS_BAUD = 115200;  // Unit GPS v1.1 の既定
const uint32_t TRY_BAUDS[] = {115200, 9600, 38400, 57600, 230400, 460800, 19200, 4800};
const int      TRY_BAUD_COUNT = sizeof(TRY_BAUDS) / sizeof(TRY_BAUDS[0]);
const uint32_t WATCH_MS  = 1500;  // GPIO で信号を見る長さ。NMEA は 1 秒ごとにまとめて来る
const uint32_t DATA_EDGES = 100;  // これ以上変化したら、その線に NMEA が来ているとみなす
const uint32_t BAUD_MS   = 2500;  // 1 つの速さで NMEA を待つ長さ
const uint32_t REPLY_MS  = 1500;  // 問い合わせの返事を待つ長さ
const uint32_t STATUS_MS = 5000;
// ------------

// ---------------- NMEA ----------------

const int LINE_MAX = 120;
char      line[LINE_MAX];
int       lineLen = -1;     // -1: '$' を待っている
bool      echoNmea = false;

uint32_t gpsBaud = 0;   // NMEA が読めた速さ。0: 読めていない
uint32_t nmeaGood = 0, nmeaBad = 0, rxBytes = 0;
char     lastReply[LINE_MAX];   // 問い合わせを待っている文字列が入った行
const char *waitFor = nullptr;

struct Sky {          // 衛星系ごとの GSV のまとめ (1 周ぶん)
  const char *talker;
  const char *name;
  int inView, withSignal, maxSnr;       // 確定した値
  int curView, curSignal, curMax;       // 集計中
  uint32_t at;                          // 確定した時刻
};
Sky sky[] = {
  {"GP", "GPS", 0, 0, 0, 0, 0, 0, 0},
  {"BD", "BDS", 0, 0, 0, 0, 0, 0, 0},
  {"GB", "BDS", 0, 0, 0, 0, 0, 0, 0},
  {"GL", "GLONASS", 0, 0, 0, 0, 0, 0, 0},
  {"GA", "Galileo", 0, 0, 0, 0, 0, 0, 0},
  {"GQ", "QZSS", 0, 0, 0, 0, 0, 0, 0},
};
const int SKY_COUNT = sizeof(sky) / sizeof(sky[0]);

int   fixQuality = -1, satsUsed = -1;
float hdop = -1, altitude = 0;
char  fixTime[12] = "", fixDate[8] = "", rmcStatus = '?';
char  lat[16] = "", ns = ' ', lon[16] = "", ew = ' ';
char  antenna[40] = "";
char  version[LINE_MAX] = "", mode[LINE_MAX] = "";

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// line (先頭の '$' を除く) をカンマで切る。'*' 以降は捨てる
int splitFields(char *s, char **f, int maxF) {
  char *star = strchr(s, '*');
  if (star) *star = 0;
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < maxF; p++) {
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

void handleSentence(char *s) {   // s は '$' の次から。チェックサム確認済み
  char copy[LINE_MAX];
  strncpy(copy, s, sizeof(copy));
  copy[sizeof(copy) - 1] = 0;
  if (waitFor && strstr(copy, waitFor)) {
    strncpy(lastReply, copy, sizeof(lastReply));
    waitFor = nullptr;
  }

  char *f[40];
  int   n = splitFields(s, f, 40);
  if (strlen(f[0]) < 5) return;
  const char *type = f[0] + 2;

  if (!strcmp(type, "GGA") && n >= 10) {
    strncpy(fixTime, f[1], sizeof(fixTime) - 1);
    strncpy(lat, f[2], sizeof(lat) - 1);
    ns = f[3][0] ? f[3][0] : ' ';
    strncpy(lon, f[4], sizeof(lon) - 1);
    ew = f[5][0] ? f[5][0] : ' ';
    fixQuality = f[6][0] ? atoi(f[6]) : 0;
    satsUsed   = f[7][0] ? atoi(f[7]) : 0;
    hdop       = f[8][0] ? atof(f[8]) : -1;
    altitude   = atof(f[9]);
  } else if (!strcmp(type, "RMC") && n >= 10) {
    strncpy(fixTime, f[1], sizeof(fixTime) - 1);
    rmcStatus = f[2][0] ? f[2][0] : '?';
    strncpy(fixDate, f[9], sizeof(fixDate) - 1);
  } else if (!strcmp(type, "GSV") && n >= 4) {
    for (int i = 0; i < SKY_COUNT; i++) {
      Sky &k = sky[i];
      if (strncmp(f[0], k.talker, 2)) continue;
      int total = atoi(f[1]), num = atoi(f[2]);
      if (num == 1) k.curView = k.curSignal = k.curMax = 0;
      k.curView = atoi(f[3]);
      for (int g = 4; g + 3 < n; g += 4) {   // PRN, 仰角, 方位, SNR (NMEA 4.1 は最後に信号 ID が付く)
        if (!f[g][0]) continue;
        int snr = f[g + 3][0] ? atoi(f[g + 3]) : 0;
        if (snr > 0) k.curSignal++;
        if (snr > k.curMax) k.curMax = snr;
      }
      if (num == total) {
        k.inView = k.curView;
        k.withSignal = k.curSignal;
        k.maxSnr = k.curMax;
        k.at = millis();
      }
    }
  } else if (!strcmp(type, "TXT") && n >= 5) {
    if (strstr(f[4], "ANTENNA")) strncpy(antenna, f[4], sizeof(antenna) - 1);
  }
}

void feed(char c) {
  rxBytes++;
  if (echoNmea) Serial.write(c);
  if (c == '$') {
    lineLen = 0;
    return;
  }
  if (lineLen < 0) return;
  if (c == '\r' || c == '\n') {
    line[lineLen] = 0;
    lineLen = -1;
    char *star = strrchr(line, '*');
    if (!star || strlen(star) < 3) {
      nmeaBad++;
      return;
    }
    uint8_t sum = 0;
    for (char *p = line; p < star; p++) sum ^= (uint8_t)*p;
    int hi = hexVal(star[1]), lo = hexVal(star[2]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)(hi * 16 + lo)) {
      nmeaBad++;
      return;
    }
    nmeaGood++;
    handleSentence(line);
    return;
  }
  if (c < 0x20 || c > 0x7e || lineLen >= LINE_MAX - 1) {   // 化けた文字・長すぎる行
    nmeaBad++;
    lineLen = -1;
    return;
  }
  line[lineLen++] = c;
}

// 黄と白が逆につながっているとき用。ハードの UART1 は TX=GP4・RX=GP5 に決まっているので、
// PIO のソフト UART で TX=GP5・RX=GP4 にして、GPS 自体が動いているかを確かめる
SerialPIO       swappedPort(PIN_RX, PIN_TX, 2048);
bool            swapped = false;        // 今 swappedPort で話しているか
bool            wiringSwapped = false;  // 配線の確認で、GPS の出力が GP4 に来ていたか
HardwareSerial *gps = &Serial2;

void pump() {
  while (gps->available()) feed((char)gps->read());
}

void stopUart() {
  Serial2.end();
  swappedPort.end();
}

void startUart(uint32_t baud) {
  stopUart();
  if (swapped) {
    swappedPort.begin(baud);
    gps = &swappedPort;
  } else {
    Serial2.setTX(PIN_TX);
    Serial2.setRX(PIN_RX);
    Serial2.setFIFOSize(2048);
    Serial2.begin(baud);
    gps = &Serial2;
  }
}

// "PCAS06,0" → "$PCAS06,0*1B\r\n"
void sendCmd(const char *body) {
  uint8_t sum = 0;
  for (const char *p = body; *p; p++) sum ^= (uint8_t)*p;
  char buf[64];
  snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, sum);
  gps->print(buf);
  Serial.printf("  sent %s", buf);
}

// 問い合わせを送り、key を含む行が来るまで待つ
bool ask(const char *body, const char *key, char *out, size_t outSize) {
  for (int attempt = 0; attempt < 3; attempt++) {
    lastReply[0] = 0;
    waitFor = key;
    sendCmd(body);
    uint32_t t0 = millis();
    while (millis() - t0 < REPLY_MS) {
      pump();
      if (!waitFor) {
        strncpy(out, lastReply, outSize - 1);
        out[outSize - 1] = 0;
        return true;
      }
    }
  }
  waitFor = nullptr;
  return false;
}

// ---------------- 1. 配線 (GPIO だけで見る) ----------------

struct Watch {
  uint32_t edges;
  bool     first, last;
  uint32_t highSamples, samples;
};

int checkWiring() {
  int problems = 0;
  stopUart();
  const int pins[] = {PIN_TX, PIN_RX, PIN_NB_TX, PIN_NB_RX, PIN_GND_WRONG};
  const int COUNT = 5;
  Watch w[COUNT] = {};
  uint32_t diffTxRx = 0;   // GP4 と GP5 が違う値だった回数 (0 ならショートを疑う)

  for (int i = 0; i < COUNT; i++) pinMode(pins[i], INPUT_PULLDOWN);
  delay(5);
  uint32_t prev = sio_hw->gpio_in;
  for (int i = 0; i < COUNT; i++) w[i].first = w[i].last = (prev >> pins[i]) & 1;
  uint32_t t0 = millis();
  while (millis() - t0 < WATCH_MS) {
    uint32_t v = sio_hw->gpio_in;
    for (int i = 0; i < COUNT; i++) {
      bool b = (v >> pins[i]) & 1;
      if (b != (bool)((prev >> pins[i]) & 1)) w[i].edges++;
      w[i].highSamples += b;
      w[i].samples++;
      w[i].last = b;
    }
    if (((v >> PIN_TX) & 1) != ((v >> PIN_RX) & 1)) diffTxRx++;
    prev = v;
  }
  const Watch &tx = w[0], &rx = w[1], &nbTx = w[2], &nbRx = w[3], &gp2 = w[4];

  // GPS の GND が 3番ピンでなく 4番ピン (GP2) に来ていると、Unit の GND が浮いて GP2 が持ち上がる
  if (gp2.last) {
    Serial.println("  GP2     NG: HIGH with pull-down -> the GPS GND wire is on pin 4 (GP2), not pin 3 (GND)?");
    problems++;
  }

  // NMEA が 1 回来れば変化は数千回。GPS の入力側の線は、つながっている相手 (GPS の入力) が弱いので、
  // 隣の線から数回〜数十回の雑音を拾うことがある。それは数えない
  bool dataOnRx = rx.edges >= DATA_EDGES, dataOnTx = tx.edges >= DATA_EDGES;
  wiringSwapped = dataOnTx && !dataOnRx;

  // GP5 (黄 = GPS の出力)
  const char *verdict;
  if (dataOnRx) {
    verdict = "OK: GPS is sending";
  } else if (dataOnTx) {
    verdict = "NG: the GPS data is on GP4 instead -> yellow and white are swapped";
  } else if (rx.last) {
    verdict = "NG: stays HIGH, no data (GPS GND not connected? then GP4 is HIGH too. Else the GPS is silent)";
  } else {
    verdict = "NG: stays LOW (no 5V/GND to the GPS, yellow not reaching GP5, or GP5 shorted to GND pin 8)";
  }
  if (!dataOnRx) problems++;
  Serial.printf("  GP5 RX  (yellow, GPS out) edges=%-6lu now=%s  %s\n",
                (unsigned long)rx.edges, rx.last ? "HIGH" : "LOW ", verdict);

  // GP4 (白 = GPS の入力)。GPS の信号が来てはいけない
  if (dataOnTx && dataOnRx && diffTxRx == 0) {
    verdict = "NG: same signal as GP5 -> GP4 and GP5 shorted";
    problems++;
  } else if (wiringSwapped) {
    verdict = "NG: GPS data arrives on GP4 -> yellow and white are swapped";
    problems++;
  } else if (dataOnTx) {
    verdict = "NG: toggles (picking up a signal from somewhere)";
    problems++;
  } else {
    verdict = tx.edges ? "OK: quiet (a little noise)" : "OK: quiet";
  }
  Serial.printf("  GP4 TX  (white, GPS in)   edges=%-6lu now=%s  %s\n",
                (unsigned long)tx.edges, tx.last ? "HIGH" : "LOW ", verdict);

  // 隣のピン
  if (nbRx.edges > 0) {
    Serial.printf("  GP%d     NG: GPS data shows up here -> GP%d shorted to the GPS line\n", PIN_NB_RX, PIN_NB_RX);
    problems++;
  }
  if (nbTx.edges > 0) {
    Serial.printf("  GP%d     NG: GPS data shows up here -> GP%d shorted to the GPS line\n", PIN_NB_TX, PIN_NB_TX);
    problems++;
  }

  // GP4 を LOW / HIGH にして、隣の GP3 がついてくるか (GP4 の先は GPS の入力なので、引いてよい)
  pinMode(PIN_NB_TX, INPUT_PULLUP);
  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, LOW);
  delay(2);
  bool followLow = !digitalRead(PIN_NB_TX);
  pinMode(PIN_NB_TX, INPUT_PULLDOWN);
  digitalWrite(PIN_TX, HIGH);
  delay(2);
  bool followHigh = digitalRead(PIN_NB_TX);
  pinMode(PIN_TX, INPUT);
  pinMode(PIN_NB_TX, INPUT);
  pinMode(PIN_NB_RX, INPUT);
  pinMode(PIN_GND_WRONG, INPUT);
  if (followLow && followHigh) {
    Serial.printf("  GP%d     NG: follows GP4 -> GP3 and GP4 shorted (pins 5 and 6)\n", PIN_NB_TX);
    problems++;
  } else {
    Serial.printf("  GP%d/GP%d neighbours: OK\n", PIN_NB_TX, PIN_NB_RX);
  }
  return problems;
}

// ---------------- 診断 (d コマンド) ----------------
// 線が HIGH のとき、それが GPS の出力が出している HIGH (強い) か、抵抗で引かれているだけ (弱い) かを見分ける。
// 数 µs だけ LOW に引いて離し、HIGH に戻るまでの時間を測る。出力なら一瞬で戻り、抵抗なら配線の容量の分だけ遅れる。

float nsPerSpin = 0;

void calibrateSpin() {
  volatile uint32_t sink = 0;
  uint32_t t0 = micros();
  for (uint32_t i = 0; i < 200000; i++) sink += sio_hw->gpio_in;
  nsPerSpin = (micros() - t0) * 1000.0f / 200000;
}

// 戻るまでのループ回数。-1: 戻らない
int32_t riseSpins(int pin, bool *lowWhileHeld) {
  uint32_t mask = 1u << pin;
  sio_hw->gpio_clr = mask;
  noInterrupts();
  sio_hw->gpio_oe_set = mask;   // LOW に引く
  busy_wait_us_32(5);
  *lowWhileHeld = !(sio_hw->gpio_in & mask);
  sio_hw->gpio_oe_clr = mask;   // 離す
  int32_t n = 0;
  while (!(sio_hw->gpio_in & mask)) {
    if (++n > 400000) {
      n = -1;
      break;
    }
  }
  interrupts();
  return n;
}

void diagnose() {
  stopUart();
  if (nsPerSpin == 0) calibrateSpin();
  Serial.printf("\n[diag] (1 spin = %.0f ns)\n", nsPerSpin);
  {
    // 測り方の確認: 何もつながっていない GP6 を内蔵プルアップで HIGH にして同じことをする。LOW に引けて、抵抗と出るはず
    pinMode(PIN_NB_RX, INPUT_PULLUP);
    delay(3);
    bool held;
    int32_t n = riseSpins(PIN_NB_RX, &held);
    Serial.printf("  control GP6 (internal pull-up, nothing connected): held LOW %s, back HIGH after %.2f us\n",
                  held ? "yes" : "NO", n < 0 ? -1.0f : n * nsPerSpin / 1000);
    pinMode(PIN_NB_RX, INPUT);
  }
  const int pins[] = {PIN_TX, PIN_RX};
  const char *names[] = {"GP4 (white)", "GP5 (yellow)"};
  for (int i = 0; i < 2; i++) {
    int p = pins[i];
    for (int pull = 0; pull < 2; pull++) {
      pinMode(p, pull ? INPUT_PULLDOWN : INPUT);
      delay(3);
      bool idle = digitalRead(p);
      Serial.printf("  %-13s %-9s idle=%s", names[i], pull ? "pulldown" : "no pull", idle ? "HIGH" : "LOW ");
      if (!idle) {
        Serial.println("  (not HIGH, skip)");
        continue;
      }
      bool held;
      int32_t best = 0x7fffffff, worst = 0;
      for (int k = 0; k < 5; k++) {
        delay(2);
        int32_t n = riseSpins(p, &held);
        if (n < 0) { best = worst = -1; break; }
        if (n < best) best = n;
        if (n > worst) worst = n;
      }
      Serial.printf("  held LOW: %s  back HIGH after ", held ? "yes" : "NO (fights back)");
      if (best < 0) {
        Serial.print("never");
      } else {
        Serial.printf("%.2f-%.2f us", best * nsPerSpin / 1000, worst * nsPerSpin / 1000);
        Serial.print(best * nsPerSpin < 100 ? "  -> driven (an output)" : "  -> pulled up (resistor)");
      }
      if (!held) {   // GPS の入力 (GP4) まで LOW に引けないのは、Unit の GND が浮いて電流が信号線から来ているとき
        Serial.print("  (GPS GND not connected?)");
      }
      Serial.println();
    }
    pinMode(p, INPUT);
  }

  // GP4 と GP5 がつながっていないか: 片方を 20µs だけ LOW にして、もう片方を見る
  for (int i = 0; i < 2; i++) {
    int a = pins[i], b = pins[1 - i];
    pinMode(a, INPUT);
    pinMode(b, INPUT_PULLDOWN);
    delay(3);
    bool before = digitalRead(b);
    uint32_t mask = 1u << a;
    sio_hw->gpio_clr = mask;
    noInterrupts();
    sio_hw->gpio_oe_set = mask;
    busy_wait_us_32(20);
    bool during = sio_hw->gpio_in & (1u << b);
    sio_hw->gpio_oe_clr = mask;
    interrupts();
    Serial.printf("  pull %s LOW -> %s %s -> %s%s\n", names[i], names[1 - i], before ? "HIGH" : "LOW",
                  during ? "HIGH" : "LOW", (before && !during) ? "  (follows: GP4 and GP5 connected!)" : "");
    pinMode(a, INPUT);
    pinMode(b, INPUT);
  }
  startUart(gpsBaud ? gpsBaud : GPS_BAUD);
}

// ---------------- 2. GPS → Pico ----------------

uint32_t checkReceive() {
  for (int i = 0; i < TRY_BAUD_COUNT; i++) {
    uint32_t baud = TRY_BAUDS[i];
    startUart(baud);
    nmeaGood = nmeaBad = rxBytes = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < BAUD_MS) pump();
    Serial.printf("  %6lu bps: %4lu bytes, NMEA ok=%lu bad=%lu\n", (unsigned long)baud,
                  (unsigned long)rxBytes, (unsigned long)nmeaGood, (unsigned long)nmeaBad);
    if (nmeaGood >= 3) {
      if (baud != GPS_BAUD) {
        Serial.printf("  -> the GPS talks at %lu bps, not the default %lu\n", (unsigned long)baud,
                      (unsigned long)GPS_BAUD);
      }
      return baud;
    }
    if (rxBytes == 0 && i == 0) {
      Serial.println("  -> nothing at all on GP5; trying other speeds anyway");
    }
  }
  return 0;
}

// ---------------- 3. Pico → GPS ----------------

bool checkSend() {
  version[0] = mode[0] = 0;
  bool ok = ask("PCAS06,0", "SW=", version, sizeof(version));
  if (ok) {
    Serial.printf("  reply: $%s\n", version);
    if (ask("PCAS06,2", "MO=", mode, sizeof(mode))) Serial.printf("  reply: $%s\n", mode);
  } else {
    Serial.println("  no reply to $PCAS06,0 (white wire GP4 -> GPS not connected?)");
  }
  return ok;
}

// ---------------- 4. 受信状況 ----------------

void printStatus() {
  Serial.printf("[%5lus] NMEA ok=%lu bad=%lu | ", (unsigned long)(millis() / 1000),
                (unsigned long)nmeaGood, (unsigned long)nmeaBad);
  if (fixQuality < 0) {
    Serial.print("no GGA yet");
  } else if (fixQuality == 0) {
    Serial.printf("no fix (RMC %c)", rmcStatus);
  } else {
    Serial.printf("FIX q=%d used=%d HDOP=%.1f alt=%.1fm", fixQuality, satsUsed, hdop, altitude);
  }
  if (fixTime[0]) {
    Serial.printf(" | UTC %c%c:%c%c:%c%c", fixTime[0], fixTime[1], fixTime[2], fixTime[3], fixTime[4], fixTime[5]);
    if (strlen(fixDate) == 6) Serial.printf(" 20%c%c-%c%c-%c%c", fixDate[4], fixDate[5], fixDate[2], fixDate[3], fixDate[0], fixDate[1]);
  }
  Serial.println();

  Serial.print("        in view:");
  bool any = false;
  for (int i = 0; i < SKY_COUNT; i++) {
    const Sky &k = sky[i];
    if (!k.at || millis() - k.at > 10000) continue;
    Serial.printf(" %s %d (%d heard, max %ddB)", k.name, k.inView, k.withSignal, k.maxSnr);
    any = true;
  }
  if (!any) Serial.print(" (no GSV yet)");
  if (antenna[0]) Serial.printf(" | %s", antenna);
  Serial.println();
  if (fixQuality > 0) Serial.printf("        pos: %s %c, %s %c (ddmm.mmmm)\n", lat, ns, lon, ew);
}

// ---------------- 全体 ----------------

uint32_t lastStatus = 0;

// ---------------- LCD ----------------

const int      PIN_SDA = 0;   // GP0 (物理1番)
const int      PIN_SCL = 1;   // GP1 (物理2番)
LiquidCrystal_I2C *lcd = nullptr;
uint32_t lastLcd = 0, lcdHoldUntil = 0;

void setupLcd() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  uint8_t addr = 0;
  for (uint8_t a = 1; a < 127 && addr == 0; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) addr = a;
  }
  if (addr == 0) {
    Serial.println("LCD not found (status on serial only)");
    return;
  }
  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();
}

void lcdLines(const char *l1, const char *l2) {
  if (!lcd) return;
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", l1);
  lcd->setCursor(0, 0);
  lcd->print(buf);
  snprintf(buf, sizeof(buf), "%-16s", l2);
  lcd->setCursor(0, 1);
  lcd->print(buf);
}

void lcdStatus() {
  char l1[24], l2[24];
  int heard = 0, maxSnr = 0;
  for (int i = 0; i < SKY_COUNT; i++) {
    const Sky &k = sky[i];
    if (!k.at || millis() - k.at > 10000) continue;
    heard += k.withSignal;
    if (k.maxSnr > maxSnr) maxSnr = k.maxSnr;
  }
  if (nmeaGood == 0) {
    snprintf(l1, sizeof(l1), "no NMEA");
  } else if (fixQuality > 0) {
    snprintf(l1, sizeof(l1), "FIX used %d", satsUsed);
  } else {
    snprintf(l1, sizeof(l1), "heard %d max %ddB", heard, maxSnr);
  }
  if (strlen(fixTime) >= 6) {
    snprintf(l2, sizeof(l2), "UTC %.2s:%.2s:%.2s", fixTime, fixTime + 2, fixTime + 4);
  } else {
    snprintf(l2, sizeof(l2), "no time %lus", (unsigned long)(millis() / 1000));
  }
  lcdLines(l1, l2);
}

void runAll() {
  digitalWrite(LED_BUILTIN, LOW);
  Serial.println("\n==== GPS test (Unit GPS v1.1) ====");
  Serial.printf("pins: TX=GP%d (white) RX=GP%d (yellow)\n", PIN_TX, PIN_RX);

  Serial.println("\n[1/3] wiring (GPIO only)");
  int wiringProblems = checkWiring();

  // 黄と白が逆なら、ソフト UART で入れ替えて GPS 自体を確かめる (挿し替えるまでの仮)
  swapped = wiringSwapped;
  const char *rxName = swapped ? "GP4, swapped" : "GP5";
  const char *txName = swapped ? "GP5, swapped" : "GP4";
  Serial.printf("\n[2/3] GPS -> Pico (NMEA on %s)\n", rxName);
  gpsBaud = checkReceive();

  bool sendOk = false;
  if (gpsBaud) {
    Serial.printf("\n[3/3] Pico -> GPS (query on %s)\n", txName);
    sendOk = checkSend();
    // espoke は出す文を GGA と RMC だけにする ($PCAS03)。電源を切らずに書き込み替えると、その設定が残っていて
    // 衛星ごとの信号の強さ (GSV) が来ない。受信の様子 (heard / max dB) を出すため、GSV も出させる
    sendCmd("PCAS03,1,0,0,1,1,0,0,0,0,0,,,0,0,,,,0");
  } else {
    startUart(GPS_BAUD);
  }

  Serial.println("\n==== result ====");
  Serial.printf("  wiring:      %s\n", wiringProblems == 0 ? "OK" : "NG");
  Serial.printf("  GPS -> Pico: %s%s\n", gpsBaud ? "OK" : "NG", swapped ? " (with GP4/GP5 swapped)" : "");
  Serial.printf("  Pico -> GPS: %s%s\n", gpsBaud ? (sendOk ? "OK" : "NG") : "not tested",
                swapped && gpsBaud ? " (with GP4/GP5 swapped)" : "");
  if (swapped && gpsBaud && sendOk) {
    Serial.println("  -> the GPS itself works. Swap the two signal plugs on the header (pins over GP4 and GP5)");
  }
  bool allOk = wiringProblems == 0 && gpsBaud && sendOk;
  digitalWrite(LED_BUILTIN, allOk ? HIGH : LOW);
  lcdLines(allOk ? "GPS test ALL OK" : "GPS test NG", allOk ? "" : "see serial");
  lcdHoldUntil = millis() + 3000;
  Serial.println(allOk ? "ALL OK" : "SOME CHECKS FAILED");
  Serial.println("Status every 5 s. Send \"r\" to run again, \"n\" to toggle raw NMEA, \"v\" to ask the version.\n");
  lastStatus = millis();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  setupLcd();
  lcdLines("GPS test", "waiting for USB");
  // テスト結果を見落とさないよう、PC 側がポートを開くまで待つ。
  // モバイルバッテリーで動かすときは PC が無いので、8 秒で諦めて進む (結果は LCD に出る)
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 8000) delay(10);
  delay(500);
  runAll();
}

void loop() {
  pump();
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') {
      runAll();
    } else if (c == 'n' || c == 'N') {
      echoNmea = !echoNmea;
      Serial.printf("\nraw NMEA %s\n", echoNmea ? "on" : "off");
    } else if (c == 'v' || c == 'V') {
      Serial.println();
      checkSend();
    } else if (c == 'd' || c == 'D') {
      diagnose();
    }
  }
  if (millis() - lastLcd >= 1000 && (int32_t)(millis() - lcdHoldUntil) >= 0) {
    lastLcd = millis();
    lcdStatus();
  }
  if (!echoNmea && millis() - lastStatus >= STATUS_MS) {
    lastStatus = millis();
    printStatus();
  }
}
