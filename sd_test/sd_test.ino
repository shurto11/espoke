/*
 * microSD スロット (サンハヤト CK-40) の動作確認
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow
 * 配線: docs/sd_music_parts.md の 3 章、docs/board_layout.svg
 *
 * 動作:
 *   1. カードとやりとりする前に、GPIO だけで信号線の配線を確かめる
 *      - CS・MOSI・MISO に 10kΩ のプルアップが付いているか (Pico からの線が切れていないか)
 *      - 信号線が GND とショートしていないか (CLK は 3.3V 側も)
 *      - 信号線どうし、または他の GPIO とショートしていないか
 *   2. カードを初期化し、種類・容量・ファイルシステムを表示する
 *      失敗したら SdFat のエラーコードと、疑うべき箇所を表示する
 *   3. ファイル一覧を表示する。MP3 は最初のフレームを読み、espoke で再生できる形式かを判定する
 *   4. テスト用ファイル (/sdtest.bin、1MB) を書いて読み戻し、中身と速度を確かめる
 *      SPI クロックを 4MHz (SD ライブラリの既定) → 12MHz → 25MHz と上げて読み、
 *      どこまで化けずに読めるかも見る。終わったらテスト用ファイルは消す
 *   5. 全部通ったら基板上の LED を点ける
 *
 * シリアル (115200bps) に送るコマンド:
 *   r: もう一度最初からテストする (カードを挿し直したあとなど)
 *   l: ファイル一覧だけ表示する
 */
#include <SPI.h>
#include <SdFat.h>

// --- 設定 ---
const int PIN_MISO = 16;  // GP16 (物理21番) ⑦ DAT0
const int PIN_SCK  = 18;  // GP18 (物理24番) ⑤ CLK
const int PIN_MOSI = 19;  // GP19 (物理25番) ③ CMD
const int PIN_CS   = 20;  // GP20 (物理26番) ② DAT3/CS

const uint32_t BASE_MHZ   = 4;  // SD ライブラリの既定 (SPI_HALF_SPEED) と同じ
const uint32_t READ_MHZ[] = {4, 12, 25};
const int      READ_MHZ_COUNT = sizeof(READ_MHZ) / sizeof(READ_MHZ[0]);

const char    *TEST_PATH  = "/sdtest.bin";
const uint32_t TEST_BYTES = 1024UL * 1024;
const int      LIST_MAX_DEPTH = 5;

// espoke で MP3 を鳴らすのに必要な読み出し速度 (MP3 の最大 320kbps)
const uint32_t NEED_KBPS = 320 / 8;

struct SdPin {
  const char *name;
  int         pin;
  bool        pulledUp;  // 10kΩ で 3.3V にプルアップしている線か
};

const SdPin SD_PINS[] = {
  {"CS (DAT3)",   PIN_CS,   true},
  {"MOSI (CMD)",  PIN_MOSI, true},
  {"CLK",         PIN_SCK,  false},
  {"MISO (DAT0)", PIN_MISO, true},
};
const int SD_PIN_COUNT = sizeof(SD_PINS) / sizeof(SD_PINS[0]);

// SD 以外の、外に出ている GPIO。GP23〜25・GP29 は Pico W の内部 (無線チップ等) で使うので除く
const int OTHER_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 17, 21, 22, 26, 27, 28};
const int OTHER_COUNT  = sizeof(OTHER_PINS) / sizeof(OTHER_PINS[0]);
// ------------

SdFs     sd;
uint8_t  buf[4096];
uint32_t patternSeed;

SdSpiConfig spiConfig(uint32_t mhz) {
  return SdSpiConfig(PIN_CS, SHARED_SPI, SD_SCK_MHZ(mhz), &SPI);
}

bool sdBegin(uint32_t mhz, bool mountVolume) {
  sd.end();
  SPI.setRX(PIN_MISO);
  SPI.setSCK(PIN_SCK);
  SPI.setTX(PIN_MOSI);
  return mountVolume ? sd.begin(spiConfig(mhz)) : sd.cardBegin(spiConfig(mhz));
}

void printSdError() {
  uint8_t code = sd.sdErrorCode();
  if (code == 0) return;
  Serial.print("  SD error: ");
  printSdErrorSymbol(&Serial, code);
  Serial.printf(" (code=0x%02X, data=0x%02X) ", code, sd.sdErrorData());
  printSdErrorText(&Serial, code);
  Serial.println();
}

uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// ---------------- 1. 配線 ----------------

// 内蔵プルアップ/プルダウンを付けて読み、外付けのプルアップと GND・3.3V へのショートを見る
int checkPulls() {
  int problems = 0;
  for (int i = 0; i < SD_PIN_COUNT; i++) {
    const SdPin &p = SD_PINS[i];
    pinMode(p.pin, INPUT_PULLUP);
    delay(2);
    bool withPullUp = digitalRead(p.pin);
    pinMode(p.pin, INPUT_PULLDOWN);
    delay(2);
    bool withPullDown = digitalRead(p.pin);
    pinMode(p.pin, INPUT);

    const char *verdict = "OK";
    if (!withPullUp) {
      verdict = "NG: stays LOW (shorted to GND?)";
    } else if (p.pulledUp && !withPullDown) {
      // 内蔵プルダウン (約50kΩ) に外付け 10kΩ が勝てば HIGH になるはず
      verdict = "NG: no 10k pull-up seen (pull-up missing, or the wire from the Pico is open)";
    } else if (!p.pulledUp && withPullDown) {
      verdict = "NG: stays HIGH (shorted to 3.3V or to CS/MOSI/MISO?)";
    }
    if (strcmp(verdict, "OK") != 0) problems++;
    Serial.printf("  %-12s GP%-2d  %s\n", p.name, p.pin, verdict);
  }
  return problems;
}

// 信号線を 1 本ずつ LOW に引き、ほかの GPIO が一緒に LOW になったらショートとみなす
int checkShorts() {
  int problems = 0;
  bool baseSd[SD_PIN_COUNT];
  bool baseOther[OTHER_COUNT];

  for (int i = 0; i < SD_PIN_COUNT; i++) pinMode(SD_PINS[i].pin, INPUT_PULLUP);
  for (int i = 0; i < OTHER_COUNT; i++) pinMode(OTHER_PINS[i], INPUT_PULLUP);
  delay(5);
  for (int i = 0; i < SD_PIN_COUNT; i++) baseSd[i] = digitalRead(SD_PINS[i].pin);
  for (int i = 0; i < OTHER_COUNT; i++) baseOther[i] = digitalRead(OTHER_PINS[i]);

  for (int i = 0; i < SD_PIN_COUNT; i++) {
    if (!baseSd[i]) continue;  // GND とのショートは checkPulls で出している
    pinMode(SD_PINS[i].pin, OUTPUT);
    digitalWrite(SD_PINS[i].pin, LOW);
    delayMicroseconds(200);

    for (int j = i + 1; j < SD_PIN_COUNT; j++) {
      if (baseSd[j] && !digitalRead(SD_PINS[j].pin)) {
        Serial.printf("  NG: %s (GP%d) and %s (GP%d) are shorted\n", SD_PINS[i].name,
                      SD_PINS[i].pin, SD_PINS[j].name, SD_PINS[j].pin);
        problems++;
      }
    }
    for (int j = 0; j < OTHER_COUNT; j++) {
      if (baseOther[j] && !digitalRead(OTHER_PINS[j])) {
        Serial.printf("  NG: %s (GP%d) is shorted to GP%d\n", SD_PINS[i].name, SD_PINS[i].pin,
                      OTHER_PINS[j]);
        problems++;
      }
    }
    pinMode(SD_PINS[i].pin, INPUT_PULLUP);
    delayMicroseconds(200);
  }

  for (int i = 0; i < SD_PIN_COUNT; i++) pinMode(SD_PINS[i].pin, INPUT);
  for (int i = 0; i < OTHER_COUNT; i++) pinMode(OTHER_PINS[i], INPUT);
  if (problems == 0) Serial.println("  no shorts between the SD lines and other GPIOs");
  return problems;
}

// ---------------- 2. カード ----------------

void printInitHints() {
  uint8_t code = sd.sdErrorCode();
  if (code == SD_CARD_ERROR_CMD0) {
    if (sd.sdErrorData() == 0xFF) {
      Serial.println("  The card never answered (MISO stayed HIGH). Check:");
      Serial.println("   - a card is inserted all the way (push until it clicks)");
      Serial.println("   - CK-40 (4) VDD gets 3.3V and (6) VSS goes to GND");
      Serial.println("   - CS -> (2), MOSI -> (3), CLK -> (5), MISO -> (7) on the CK-40 (pin order 1-8)");
    } else if (sd.sdErrorData() == 0x00) {
      Serial.println("  MISO stayed LOW. Check MISO (GP16 - CK-40 (7)) for a short to GND,");
      Serial.println("  and that VDD/VSS are not swapped.");
    } else {
      Serial.println("  The card answered with garbage. Check CLK/MOSI/MISO wiring and solder joints.");
    }
  } else if (code == SD_CARD_ERROR_CMD8 || code == SD_CARD_ERROR_ACMD41 ||
             code == SD_CARD_ERROR_CMD58) {
    Serial.println("  The card answered but did not finish starting up. Check:");
    Serial.println("   - VDD is really 3.3V at the CK-40, and the 0.1uF/10uF caps are near VDD-VSS");
    Serial.println("   - try another card");
  } else {
    Serial.println("  Communication broke during start-up. Check CLK/MOSI/MISO solder joints.");
  }
}

bool checkCard() {
  if (!sdBegin(BASE_MHZ, false)) {
    Serial.println("  card init failed");
    printSdError();
    printInitHints();
    return false;
  }

  cid_t    cid;
  csd_t    csd;
  uint32_t ocr;
  if (!sd.card()->readCID(&cid) || !sd.card()->readCSD(&csd) || !sd.card()->readOCR(&ocr)) {
    Serial.println("  card answered, but reading its registers failed (MISO/CLK signal?)");
    printSdError();
    return false;
  }

  const char *type = "unknown";
  switch (sd.card()->type()) {
    case SD_CARD_TYPE_SD1:  type = "SD1"; break;
    case SD_CARD_TYPE_SD2:  type = "SD2"; break;
    case SD_CARD_TYPE_SDHC: type = csd.capacity() < 70000000 ? "SDHC" : "SDXC"; break;
  }
  Serial.printf("  card:    %s, %.2f GB (%lu sectors)\n", type, csd.capacity() * 512.0 / 1e9,
                (unsigned long)csd.capacity());
  Serial.printf("  maker:   0x%02X, OEM %c%c, product %.5s, made %d/%02d\n", cid.mid, cid.oid[0],
                cid.oid[1], cid.pnm, cid.mdtYear(), cid.mdtMonth());
  Serial.printf("  OCR:     0x%08lX\n", (unsigned long)ocr);

  if (!sd.volumeBegin()) {
    Serial.println("  NG: no FAT16/FAT32/exFAT volume found. Format the card as FAT32 on the PC.");
    printSdError();
    return false;
  }
  const char *fs = sd.fatType() == FAT_TYPE_EXFAT ? "exFAT" : sd.fatType() == FAT_TYPE_FAT32 ? "FAT32" : "FAT16";
  uint64_t total = (uint64_t)sd.clusterCount() * sd.bytesPerCluster();
  Serial.printf("  volume:  %s, %.2f GB, cluster %lu KB\n", fs, total / 1e9,
                (unsigned long)(sd.bytesPerCluster() / 1024));

  Serial.println("  counting free space (may take ~10 s)...");
  int32_t freeClusters = sd.freeClusterCount();
  if (freeClusters < 0) {
    Serial.println("  NG: reading the FAT failed");
    printSdError();
    return false;
  }
  Serial.printf("  free:    %.2f GB\n", (double)freeClusters * sd.bytesPerCluster() / 1e9);
  return true;
}

// ---------------- 3. ファイル一覧 ----------------

int fileCount, mp3Count, playableCount;

// ID3v2 タグ (曲名や画像が入っている) を読み飛ばし、最初の MP3 フレームの先頭まで進める
bool skipId3(FsFile &f) {
  uint8_t h[10];
  f.seekSet(0);
  if (f.read(h, 10) != 10) return false;
  if (memcmp(h, "ID3", 3) != 0) {
    f.seekSet(0);
    return true;
  }
  // サイズは 7bit ずつの 4 バイト (syncsafe)。フッタ付きならさらに 10 バイト
  uint32_t size = ((uint32_t)(h[6] & 0x7f) << 21) | ((uint32_t)(h[7] & 0x7f) << 14) |
                  ((uint32_t)(h[8] & 0x7f) << 7) | (h[9] & 0x7f);
  return f.seekSet(10 + size + ((h[5] & 0x10) ? 10 : 0));
}

// 最初のフレームヘッダを探して形式を表示する。espoke で再生できるか (44100 の整数分の 1 か) も判定する
void printMp3Info(FsFile &f) {
  static const uint16_t RATES[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
  static const uint16_t KBPS_V1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t KBPS_V2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  mp3Count++;
  if (!skipId3(f)) {
    Serial.print("  broken ID3 tag");
    return;
  }

  // タグの後ろにゴミがあることもあるので、先頭 4KB の中で同期ワードを探す
  int n = f.read(buf, 4096);
  for (int i = 0; i + 3 < n; i++) {
    uint8_t b1 = buf[i + 1], b2 = buf[i + 2], b3 = buf[i + 3];
    if (buf[i] != 0xFF || (b1 & 0xE0) != 0xE0) continue;
    int ver = (b1 >> 3) & 3;  // 3: MPEG1, 2: MPEG2, 0: MPEG2.5
    int layer = (b1 >> 1) & 3;  // 1: Layer III
    int br = b2 >> 4, sr = (b2 >> 2) & 3;
    if (ver == 1 || layer != 1 || br == 0 || br == 15 || sr == 3) continue;

    int row = ver == 3 ? 0 : ver == 2 ? 1 : 2;
    uint32_t rate = RATES[row][sr];
    uint16_t kbps = (ver == 3 ? KBPS_V1 : KBPS_V2)[br];
    bool mono = (b3 >> 6) == 3;
    Serial.printf("  %luHz %s %ukbps", (unsigned long)rate, mono ? "mono" : "stereo", kbps);
    if (44100 % rate == 0 && 44100 / rate <= 4) {
      playableCount++;
    } else {
      Serial.print("  (espoke cannot play this rate)");
    }
    return;
  }
  Serial.print("  no MP3 frame found");
}

bool hasExt(const char *name, const char *ext) {
  size_t n = strlen(name), e = strlen(ext);
  return n >= e && strcasecmp(name + n - e, ext) == 0;
}

void listDir(FsFile &dir, int depth) {
  FsFile f;
  char   name[128];
  while (f.openNext(&dir, O_RDONLY)) {
    f.getName(name, sizeof(name));
    Serial.printf("  %*s", depth * 2, "");
    if (f.isDir()) {
      Serial.printf("%s/\n", name);
      if (depth + 1 < LIST_MAX_DEPTH) listDir(f, depth + 1);
    } else {
      fileCount++;
      Serial.printf("%-40s %10llu", name, (unsigned long long)f.fileSize());
      if (hasExt(name, ".mp3")) printMp3Info(f);
      Serial.println();
    }
    f.close();
  }
}

bool listFiles() {
  fileCount = mp3Count = playableCount = 0;
  FsFile root;
  if (!root.open("/")) {
    Serial.println("  NG: cannot open the root directory");
    printSdError();
    return false;
  }
  listDir(root, 0);
  root.close();
  Serial.printf("  %d file(s), %d MP3, %d playable by espoke\n", fileCount, mp3Count, playableCount);
  return true;
}

// ---------------- 4. 書き込み・読み出し ----------------

// テスト用ファイルの中身。位置と実行ごとの種から決まる値にして、読み戻したときに比べられるようにする
uint32_t patternAt(uint32_t offset) {
  return (offset + patternSeed) * 2654435761u;
}

bool writeTest(uint32_t *kbps) {
  FsFile f;
  if (!f.open(TEST_PATH, O_RDWR | O_CREAT | O_TRUNC)) {
    Serial.printf("  NG: cannot create %s\n", TEST_PATH);
    printSdError();
    return false;
  }
  patternSeed = micros();
  uint32_t t0 = millis();
  for (uint32_t off = 0; off < TEST_BYTES; off += sizeof(buf)) {
    for (size_t i = 0; i < sizeof(buf); i += 4) {
      uint32_t v = patternAt(off + i);
      memcpy(buf + i, &v, 4);
    }
    if (f.write(buf, sizeof(buf)) != sizeof(buf)) {
      Serial.printf("  NG: write failed at %lu bytes\n", (unsigned long)off);
      printSdError();
      f.close();
      return false;
    }
  }
  if (!f.close()) {
    Serial.println("  NG: closing the test file failed");
    printSdError();
    return false;
  }
  uint32_t ms = millis() - t0;
  *kbps = ms ? TEST_BYTES / ms * 1000 / 1024 : 0;
  return true;
}

// 読み戻して中身を比べる。化けていたバイト数を返す (読めなければ -1)
long readTest(uint32_t *kbps) {
  FsFile f;
  if (!f.open(TEST_PATH, O_RDONLY)) {
    printSdError();
    return -1;
  }
  long     bad = 0;
  uint32_t t0 = millis();
  for (uint32_t off = 0; off < TEST_BYTES; off += sizeof(buf)) {
    if (f.read(buf, sizeof(buf)) != (int)sizeof(buf)) {
      printSdError();
      f.close();
      return -1;
    }
    for (size_t i = 0; i < sizeof(buf); i += 4) {
      uint32_t v = patternAt(off + i);
      if (memcmp(buf + i, &v, 4) != 0) bad += 4;
    }
  }
  uint32_t ms = millis() - t0;
  f.close();
  *kbps = ms ? TEST_BYTES / ms * 1000 / 1024 : 0;
  return bad;
}

// 書き込み・読み出しを試し、化けずに読めた一番速い SPI クロック (MHz) を返す。だめなら 0
uint32_t checkReadWrite() {
  uint32_t kbps;
  if (!writeTest(&kbps)) return 0;
  Serial.printf("  write %4lu KB @ %2lu MHz: %4lu KB/s\n", (unsigned long)(TEST_BYTES / 1024),
                (unsigned long)BASE_MHZ, (unsigned long)kbps);

  uint32_t best = 0;
  for (int i = 0; i < READ_MHZ_COUNT; i++) {
    uint32_t mhz = READ_MHZ[i];
    Serial.printf("  read  %4lu KB @ %2lu MHz: ", (unsigned long)(TEST_BYTES / 1024), (unsigned long)mhz);
    if (!sdBegin(mhz, true)) {
      Serial.println("NG (init failed)");
      printSdError();
      continue;
    }
    long bad = readTest(&kbps);
    if (bad < 0) {
      Serial.println("NG (read failed)");
    } else if (bad > 0) {
      Serial.printf("NG (%ld bytes corrupted)\n", bad);
    } else {
      const char *enough = kbps >= NEED_KBPS ? "enough for MP3" : "too slow for MP3";
      Serial.printf("%4lu KB/s, data OK (%s)\n", (unsigned long)kbps, enough);
      best = mhz;
    }
  }

  if (!sdBegin(BASE_MHZ, true) || !sd.remove(TEST_PATH)) {
    Serial.printf("  could not delete %s (delete it on the PC)\n", TEST_PATH);
    printSdError();
  }
  return best;
}

// ---------------- 全体 ----------------

void runAll() {
  digitalWrite(LED_BUILTIN, LOW);
  sd.end();

  Serial.println("\n==== microSD test ====");
  Serial.printf("pins: CS=GP%d MOSI=GP%d CLK=GP%d MISO=GP%d\n", PIN_CS, PIN_MOSI, PIN_SCK, PIN_MISO);

  Serial.println("\n[1/4] wiring (GPIO only, keep switches released)");
  int wiringProblems = checkPulls() + checkShorts();

  Serial.println("\n[2/4] card");
  bool cardOk = checkCard();

  bool     listOk = false;
  uint32_t bestMhz = 0;
  if (cardOk) {
    Serial.println("\n[3/4] files");
    listOk = listFiles();
    Serial.println("\n[4/4] write / read");
    bestMhz = checkReadWrite();
  }

  Serial.println("\n==== result ====");
  Serial.printf("  wiring:     %s\n", wiringProblems == 0 ? "OK" : "NG");
  Serial.printf("  card:       %s\n", cardOk ? "OK" : "NG");
  if (cardOk) {
    Serial.printf("  files:      %s\n", listOk ? "OK" : "NG");
    if (bestMhz > 0) {
      Serial.printf("  write/read: OK (reliable up to %lu MHz)\n", (unsigned long)bestMhz);
    } else {
      Serial.println("  write/read: NG");
    }
  }
  bool allOk = wiringProblems == 0 && cardOk && listOk && bestMhz > 0;
  digitalWrite(LED_BUILTIN, allOk ? HIGH : LOW);
  Serial.println(allOk ? "ALL OK" : "SOME CHECKS FAILED");
  Serial.println("Send \"r\" to run again, \"l\" to list files.");
}

void setup() {
  Serial.begin(115200);
  // テスト結果を見落とさないよう、PC 側がポートを開くまで待つ
  while (!Serial) delay(10);
  delay(500);

  pinMode(LED_BUILTIN, OUTPUT);
  runAll();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') {
      runAll();
    } else if (c == 'l' || c == 'L') {
      Serial.println();
      if (sdBegin(BASE_MHZ, true)) {
        listFiles();
      } else {
        Serial.println("  card init failed");
        printSdError();
      }
    }
  }
}
