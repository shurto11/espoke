/*
 * ユニバーサル基板のタクトスイッチ (SW1〜SW4) の動作確認
 *
 * 対応ボード: Raspberry Pi Pico WH
 * FQBN: rp2040:rp2040:rpipicow
 * 配線: docs/board_layout.svg
 *
 * 動作:
 *   1. 起動時に各スイッチの状態を表示し、押していないのに LOW のピンを警告する
 *      (足の選び方の間違いや、隣の線とのショートが疑われる)
 *   2. 押す/離すたびにシリアル (115200bps) へ表示し、押した回数を数える
 *   3. どれか1つでも押している間は基板上の LED を点ける
 *   4. シリアルに "s" と送ると、全スイッチの今の状態と回数を表示する
 *   5. スイッチ以外の GPIO も見張り、変化したら "(not a switch pin)" と表示する
 *      (配線が別のピンに付いてしまったときに、どこへ付いたかが分かる)
 *
 * まだ付けていないスイッチのピンは、プルアップで HIGH (離した状態) のまま見える
 */

// --- 設定 ---
struct Button {
  const char *name;
  int         pin;
};

const Button BUTTONS[] = {
  {"SW1", 10},  // GP10 (物理14番ピン)
  {"SW2", 11},  // GP11 (物理15番ピン)
  {"SW3", 12},  // GP12 (物理16番ピン)
  {"SW4", 13},  // GP13 (物理17番ピン)
};
const int           BUTTON_COUNT = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
const unsigned long DEBOUNCE_MS  = 30;

// 外に出ている GPIO。GP23〜25・GP29 は Pico W の内部 (無線チップ等) で使うので除く
const int OTHER_PINS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 14, 15, 16, 17, 18, 19, 20, 21, 22, 26, 27, 28};
const int OTHER_COUNT  = sizeof(OTHER_PINS) / sizeof(OTHER_PINS[0]);
// ------------

bool otherState[OTHER_COUNT];

bool          rawState[BUTTON_COUNT];
bool          stableState[BUTTON_COUNT];
unsigned long changedAt[BUTTON_COUNT];
unsigned long pressCount[BUTTON_COUNT];

const char *stateName(bool level) {
  return level == LOW ? "pressed" : "released";
}

void printStatus() {
  for (int i = 0; i < BUTTON_COUNT; i++) {
    Serial.printf("  %s (GP%d): %-8s  count=%lu\n", BUTTONS[i].name, BUTTONS[i].pin,
                  stateName(stableState[i]), pressCount[i]);
  }
}

void setup() {
  Serial.begin(115200);
  // USB CDC なので、PC 側がポートを開くまで最大5秒待つ
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) delay(10);

  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < BUTTON_COUNT; i++) {
    pinMode(BUTTONS[i].pin, INPUT_PULLUP);
  }
  for (int i = 0; i < OTHER_COUNT; i++) {
    pinMode(OTHER_PINS[i], INPUT_PULLUP);
  }
  delay(10);  // プルアップが効くまで少し待つ

  Serial.println("button test");
  for (int i = 0; i < BUTTON_COUNT; i++) {
    rawState[i] = stableState[i] = digitalRead(BUTTONS[i].pin);
    changedAt[i] = millis();
    pressCount[i] = 0;
  }
  for (int i = 0; i < OTHER_COUNT; i++) {
    otherState[i] = digitalRead(OTHER_PINS[i]);
    if (otherState[i] == LOW) Serial.printf("  GP%d is LOW (not a switch pin)\n", OTHER_PINS[i]);
  }
  printStatus();
  for (int i = 0; i < BUTTON_COUNT; i++) {
    if (stableState[i] == LOW) {
      Serial.printf("  WARNING: %s (GP%d) is LOW without pressing. "
                    "Check the leg pair (use diagonal legs) or a short to GND.\n",
                    BUTTONS[i].name, BUTTONS[i].pin);
    }
  }
  Serial.println("Press a switch. Send \"s\" to show the status.");
}

void loop() {
  bool anyPressed = false;

  for (int i = 0; i < BUTTON_COUNT; i++) {
    bool raw = digitalRead(BUTTONS[i].pin);
    if (raw != rawState[i]) {
      rawState[i] = raw;
      changedAt[i] = millis();
    } else if (raw != stableState[i] && millis() - changedAt[i] >= DEBOUNCE_MS) {
      stableState[i] = raw;
      if (raw == LOW) pressCount[i]++;
      Serial.printf("%s (GP%d) %s  count=%lu\n", BUTTONS[i].name, BUTTONS[i].pin,
                    stateName(raw), pressCount[i]);
    }
    if (stableState[i] == LOW) anyPressed = true;
  }

  // ここはデバウンスせず、変化をそのまま出す (どのピンかが分かれば十分なので)
  for (int i = 0; i < OTHER_COUNT; i++) {
    bool level = digitalRead(OTHER_PINS[i]);
    if (level != otherState[i]) {
      otherState[i] = level;
      Serial.printf("GP%d %s (not a switch pin)\n", OTHER_PINS[i], level == LOW ? "LOW" : "HIGH");
    }
    if (level == LOW) anyPressed = true;
  }

  digitalWrite(LED_BUILTIN, anyPressed ? HIGH : LOW);

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 's' || c == 'S') printStatus();
  }
}
