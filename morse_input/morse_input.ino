/*
 * モールス信号でアルファベットを入力し、LCD1602A に表示する
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow
 * ライブラリ: LiquidCrystal I2C (Frank de Brabander)
 * 配線: docs/board_layout.svg (SW1〜SW4)、LCD は GP0 (SDA) / GP1 (SCL)
 *
 * 操作:
 *   SW1 (GP10) backspace。符号の入力中なら最後の1符号を、そうでなければ最後の1文字を消す
 *   SW2 (GP11) トン (.)
 *   SW3 (GP12) ツー (-)
 *   SW4 (GP13) enter。入力中の符号を文字に確定する。符号が空なら空白を入れる
 *
 * 表示:
 *   1行目 入力した文章 (長くなったら末尾の15文字を表示)
 *   2行目 入力中の符号と、その時点で確定したときの文字
 *         例: "-.-.        = C"   該当する文字がなければ "= ?"
 *
 * シリアル (115200bps) からも操作できる: '-' '.' がツー/トン、'b' が backspace、
 * 'e' が enter。確定した文章はシリアルにも出す
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- 設定 ---
const int     PIN_SDA   = 0;   // GP0 (物理1番ピン)
const int     PIN_SCL   = 1;   // GP1 (物理2番ピン)
const uint8_t LCD_COLS  = 16;
const uint8_t LCD_ROWS  = 2;
const uint8_t LCD_ADDR_DEFAULT = 0x27;

const int PIN_BACK  = 10;  // SW1 GP10 (物理14番ピン)
const int PIN_DOT   = 11;  // SW2 GP11 (物理15番ピン)
const int PIN_DASH  = 12;  // SW3 GP12 (物理16番ピン)
const int PIN_ENTER = 13;  // SW4 GP13 (物理17番ピン)

const unsigned long DEBOUNCE_MS = 30;
const size_t MAX_CODE = 6;   // 1文字の符号は最長でも5つ (数字)。余裕を見て6
const size_t MAX_TEXT = 64;
// ------------

struct Button {
  int           pin;
  bool          raw;
  bool          stable;   // INPUT_PULLUP なので離すと HIGH
  unsigned long changedAt;
};

enum { BTN_BACK, BTN_DOT, BTN_DASH, BTN_ENTER, BTN_COUNT };
Button buttons[BTN_COUNT] = {
  {PIN_BACK,  HIGH, HIGH, 0},
  {PIN_DOT,   HIGH, HIGH, 0},
  {PIN_DASH,  HIGH, HIGH, 0},
  {PIN_ENTER, HIGH, HIGH, 0},
};

struct Morse {
  char        ch;
  const char *code;
};

const Morse MORSE_TABLE[] = {
  {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},   {'E', "."},
  {'F', "..-."},  {'G', "--."},   {'H', "...."},  {'I', ".."},    {'J', ".---"},
  {'K', "-.-"},   {'L', ".-.."},  {'M', "--"},    {'N', "-."},    {'O', "---"},
  {'P', ".--."},  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
  {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},  {'Y', "-.--"},
  {'Z', "--.."},
  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"}, {'4', "....-"},
  {'5', "....."}, {'6', "-...."}, {'7', "--..."}, {'8', "---.."}, {'9', "----."},
};
const int MORSE_COUNT = sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]);

LiquidCrystal_I2C *lcd = nullptr;
String code = "";  // 入力中の符号 ("-" と ".")
String text = "";  // 確定した文章

// 符号を文字にする。表になければ 0
char decode(const String &c) {
  for (int i = 0; i < MORSE_COUNT; i++) {
    if (c == MORSE_TABLE[i].code) return MORSE_TABLE[i].ch;
  }
  return 0;
}

// ---------------- LCD ----------------

void drawRow(uint8_t row, const String &s) {
  String t = s.substring(0, LCD_COLS);
  while (t.length() < LCD_COLS) t += ' ';
  lcd->setCursor(0, row);
  lcd->print(t);
}

void draw() {
  // 1行目: 末尾にカーソル代わりの '_' を付け、収まらなければ末尾側を見せる
  String top = text + "_";
  if (top.length() > LCD_COLS) top = top.substring(top.length() - LCD_COLS);
  drawRow(0, top);

  // 2行目: 左に符号、右端に確定したときの文字
  String bottom = code;
  if (code.length()) {
    char ch = decode(code);
    while (bottom.length() < LCD_COLS - 3) bottom += ' ';
    bottom += "= ";
    bottom += ch ? ch : '?';
  }
  drawRow(1, bottom);
}

void setupLCD() {
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

// ---------------- 入力 ----------------

void onPress(int id) {
  switch (id) {
    case BTN_DASH:
    case BTN_DOT:
      if (code.length() < MAX_CODE) code += (id == BTN_DASH) ? '-' : '.';
      break;
    case BTN_BACK:
      if (code.length()) {
        code.remove(code.length() - 1);
      } else if (text.length()) {
        text.remove(text.length() - 1);
      }
      break;
    case BTN_ENTER:
      if (code.length()) {
        char ch = decode(code);
        if (ch) {
          if (text.length() < MAX_TEXT) text += ch;
        } else {
          Serial.printf("unknown code: %s\n", code.c_str());
        }
        code = "";
      } else if (text.length() && text.length() < MAX_TEXT) {
        text += ' ';
      }
      break;
  }
  Serial.printf("code=\"%s\" text=\"%s\"\n", code.c_str(), text.c_str());
  draw();
}

void pollButtons() {
  for (int i = 0; i < BTN_COUNT; i++) {
    Button &b = buttons[i];
    bool raw = digitalRead(b.pin);
    if (raw != b.raw) {
      b.raw = raw;
      b.changedAt = millis();
    } else if (raw != b.stable && millis() - b.changedAt >= DEBOUNCE_MS) {
      b.stable = raw;
      if (raw == LOW) onPress(i);
    }
  }
}

void pollSerial() {
  while (Serial.available()) {
    switch ((char)Serial.read()) {
      case '-': onPress(BTN_DASH);  break;
      case '.': onPress(BTN_DOT);   break;
      case 'b': onPress(BTN_BACK);  break;
      case 'e': onPress(BTN_ENTER); break;
    }
  }
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 2000) delay(10);

  for (int i = 0; i < BTN_COUNT; i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }
  delay(10);  // プルアップが効くまで少し待つ
  for (int i = 0; i < BTN_COUNT; i++) {
    buttons[i].raw = buttons[i].stable = digitalRead(buttons[i].pin);
  }

  setupLCD();
  draw();
  Serial.println("morse input: SW1=backspace SW2=. SW3=- SW4=enter");
}

void loop() {
  pollButtons();
  pollSerial();
}
