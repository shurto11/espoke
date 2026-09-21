/*
 * LCD1602A (I2Cバックパック PCF8574 付き) に文字を表示する
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow
 * ライブラリ: LiquidCrystal I2C (Frank de Brabander)
 *
 * 動作:
 *   1. 起動時に I2C バスをスキャンし、見つかったアドレスで LCD を初期化
 *   2. 1行目に固定メッセージ、2行目に起動からの経過秒数を表示
 *   3. シリアルモニタ (115200bps) から1行送ると、その文字列を1行目に表示
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- 設定 ---
const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)。I2C0 の SDA
const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)。I2C0 の SCL
const uint8_t LCD_COLS  = 16;
const uint8_t LCD_ROWS  = 2;
const uint8_t LCD_ADDR_DEFAULT = 0x27;  // スキャンで見つからなかった場合に使う
// ------------

LiquidCrystal_I2C *lcd = nullptr;
String line1 = "Hello, Pico WH!";
unsigned long lastUpdate = 0;

// I2C バスをスキャンし、最初に応答したアドレスを返す (見つからなければ 0)
uint8_t scanI2C() {
  uint8_t found = 0;
  Serial.println("I2C scan...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found: 0x%02X\n", addr);
      if (found == 0) found = addr;
    }
  }
  if (found == 0) Serial.println("  no device found");
  return found;
}

// 指定行を16文字ぶん上書きする (前の文字が残らないよう空白で埋める)
void printLine(uint8_t row, const String &text) {
  String s = text.substring(0, LCD_COLS);
  while (s.length() < LCD_COLS) s += ' ';
  lcd->setCursor(0, row);
  lcd->print(s);
}

void setup() {
  Serial.begin(115200);
  // USB CDC なので、PC 側がポートを開くまで最大2秒待つ
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) delay(10);

  // RP2040 は I2C0/I2C1 で使えるピンが決まっている。Wire = I2C0, Wire1 = I2C1
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();

  uint8_t addr = scanI2C();
  if (addr == 0) addr = LCD_ADDR_DEFAULT;
  Serial.printf("LCD address: 0x%02X\n", addr);

  lcd = new LiquidCrystal_I2C(addr, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();

  printLine(0, line1);
  printLine(1, "");
  Serial.println("Type text and press Enter to show it on the LCD.");
}

void loop() {
  // シリアルから受け取った文字列を1行目に表示
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      line1 = input;
      printLine(0, line1);
      Serial.printf("LCD <- \"%s\"\n", line1.c_str());
    }
  }

  // 2行目に経過秒数を1秒ごとに表示
  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();
    printLine(1, "uptime " + String(millis() / 1000) + "s");
  }
}
