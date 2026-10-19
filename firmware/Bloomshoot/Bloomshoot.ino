// Bloomshoot ESP32-S3 light-box controller
// Based on the tested HAPPY JARZ ESP32-S3 touch/OLED/APA106 hardware path.
//
// Hardware assumptions:
//   Six APA106 addressable LEDs in one DIN -> DOUT chain on GPIO7.
//   SSD1306 128x64 I2C OLED at 0x3C: SDA GPIO8, SCL GPIO6.
//   Capacitive touch pads: UP=4, DOWN=5, LEFT=9, RIGHT=10, A=1, B=2.
//   LED data uses the RGB byte order and 10 MHz RMT pulse timing used by
//   the Happy Jarz firmware. Confirm the first physical LED's color order.
//
// Controls:
//   UP/DOWN    select lamp 1-6
//   LEFT/RIGHT lower/raise the selected lamp brightness by 5%
//   A          cycle the selected lamp through the color palette
//   B          toggle the selected lamp on/off
//
// Start with a verified 5 V LED supply and low brightness. Never power the
// LED chain from an ESP32 GPIO or the 3.3 V rail.

#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "esp32-hal-rmt.h"

static constexpr uint8_t LED_DATA_PIN = 7;
static constexpr uint8_t LED_COUNT = 6;
static constexpr uint8_t INPUT_UP_PIN = 4;
static constexpr uint8_t INPUT_DOWN_PIN = 5;
static constexpr uint8_t INPUT_LEFT_PIN = 9;
static constexpr uint8_t INPUT_RIGHT_PIN = 10;
static constexpr uint8_t INPUT_A_PIN = 1;
static constexpr uint8_t INPUT_B_PIN = 2;
static constexpr uint8_t OLED_SDA_PIN = 8;
static constexpr uint8_t OLED_SCL_PIN = 6;
static constexpr uint8_t INPUT_COUNT = 6;
static constexpr uint8_t BRIGHTNESS_STEP = 5;
static constexpr uint32_t TOUCH_DEBOUNCE_MS = 55;

struct Rgb { uint8_t r, g, b; };
struct PaletteColor { const char *name; Rgb rgb; };

static const PaletteColor palette[] = {
  {"WHITE",       {255, 255, 255}},
  {"WARM WHITE",  {255, 180,  90}},
  {"COOL WHITE",  {170, 215, 255}},
  {"RED",         {255,   0,   0}},
  {"ORANGE",      {255,  55,   0}},
  {"YELLOW",      {255, 150,   0}},
  {"GREEN",       {  0, 255,  25}},
  {"CYAN",        {  0, 200, 255}},
  {"BLUE",        {  0,  35, 255}},
  {"VIOLET",      {100,   0, 255}},
  {"MAGENTA",     {255,   0, 150}},
  {"PINK",        {255,  35,  90}}
};
static constexpr uint8_t PALETTE_COUNT = sizeof(palette) / sizeof(palette[0]);

static Rgb lampColor[LED_COUNT];
static uint8_t lampBrightness[LED_COUNT];
static bool lampEnabled[LED_COUNT];
static uint8_t selectedLamp = 0;

static const uint8_t inputPins[INPUT_COUNT] = {
  INPUT_UP_PIN, INPUT_DOWN_PIN, INPUT_LEFT_PIN,
  INPUT_RIGHT_PIN, INPUT_A_PIN, INPUT_B_PIN
};
static const char *inputNames[INPUT_COUNT] = {
  "UP", "DOWN", "LEFT", "RIGHT", "A", "B"
};

struct TouchState {
  uint32_t baseline = 0;
  uint32_t threshold = 0;
  bool raw = false;
  bool stable = false;
  uint32_t rawChangedAt = 0;
};
static TouchState touch[INPUT_COUNT];

static Preferences prefs;
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
static bool ledReady = false;
static bool settingsDirty = false;
static uint32_t settingsChangedAt = 0;
static uint32_t lastDisplayAt = 0;

static bool initLedOutput() {
  pinMode(LED_DATA_PIN, OUTPUT);
  digitalWrite(LED_DATA_PIN, LOW);
  if (!rmtInit(LED_DATA_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) {
    return false;
  }
  rmtSetEOT(LED_DATA_PIN, 0);
  return true;
}

static void sendLeds() {
  if (!ledReady) return;

  rmt_data_t symbols[LED_COUNT * 24];
  size_t n = 0;
  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    uint8_t bytes[3] = {
      lampEnabled[i] ? (uint8_t)((uint16_t)lampColor[i].r * lampBrightness[i] / 100U) : 0,
      lampEnabled[i] ? (uint8_t)((uint16_t)lampColor[i].g * lampBrightness[i] / 100U) : 0,
      lampEnabled[i] ? (uint8_t)((uint16_t)lampColor[i].b * lampBrightness[i] / 100U) : 0
    };
    for (uint8_t channel = 0; channel < 3; ++channel) {
      for (int8_t bit = 7; bit >= 0; --bit) {
        const bool one = bytes[channel] & (1U << bit);
        symbols[n].level0 = 1;
        symbols[n].duration0 = one ? 14 : 4;
        symbols[n].level1 = 0;
        symbols[n].duration1 = one ? 4 : 14;
        ++n;
      }
    }
  }
  rmtWrite(LED_DATA_PIN, symbols, n, RMT_WAIT_FOR_EVER);
  digitalWrite(LED_DATA_PIN, LOW);
  delayMicroseconds(100);
}

static void markSettingsChanged() {
  settingsDirty = true;
  settingsChangedAt = millis();
}

static void loadSettings() {
  prefs.begin("bloomshoot", true);
  selectedLamp = min((uint8_t)(LED_COUNT - 1), prefs.getUChar("selected", 0));
  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "r%u", i); lampColor[i].r = prefs.getUChar(key, 255);
    snprintf(key, sizeof(key), "g%u", i); lampColor[i].g = prefs.getUChar(key, 255);
    snprintf(key, sizeof(key), "b%u", i); lampColor[i].b = prefs.getUChar(key, 255);
    snprintf(key, sizeof(key), "lvl%u", i);
    lampBrightness[i] = min((uint8_t)100, prefs.getUChar(key, 25));
    snprintf(key, sizeof(key), "on%u", i);
    lampEnabled[i] = prefs.getBool(key, true);
  }
  prefs.end();
}

static void saveSettings() {
  prefs.begin("bloomshoot", false);
  prefs.putUChar("selected", selectedLamp);
  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "r%u", i); prefs.putUChar(key, lampColor[i].r);
    snprintf(key, sizeof(key), "g%u", i); prefs.putUChar(key, lampColor[i].g);
    snprintf(key, sizeof(key), "b%u", i); prefs.putUChar(key, lampColor[i].b);
    snprintf(key, sizeof(key), "lvl%u", i); prefs.putUChar(key, lampBrightness[i]);
    snprintf(key, sizeof(key), "on%u", i); prefs.putBool(key, lampEnabled[i]);
  }
  prefs.end();
  settingsDirty = false;
}

static void calibrateTouchInputs() {
  for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
    uint64_t sum = 0;
    for (uint8_t sample = 0; sample < 32; ++sample) {
      sum += touchRead(inputPins[i]);
      delay(5);
    }
    touch[i].baseline = (uint32_t)(sum / 32U);
    touch[i].threshold = touch[i].baseline + touch[i].baseline / 5U;
    touch[i].raw = false;
    touch[i].stable = false;
    touch[i].rawChangedAt = millis();
  }
}

static bool justPressed(uint8_t index) {
  TouchState &state = touch[index];
  const uint32_t value = touchRead(inputPins[index]);
  const bool isTouched = value >= state.threshold;
  const uint32_t now = millis();

  if (isTouched != state.raw) {
    state.raw = isTouched;
    state.rawChangedAt = now;
  }

  if (state.raw != state.stable &&
      now - state.rawChangedAt >= TOUCH_DEBOUNCE_MS) {
    state.stable = state.raw;
    if (state.stable) return true;
  }

  if (!state.stable) {
    state.baseline = (state.baseline * 127U + value) / 128U;
    state.threshold = state.baseline + state.baseline / 5U;
  }
  return false;
}

static void cycleSelectedColor(int8_t direction) {
  uint8_t current = 0;
  for (uint8_t i = 0; i < PALETTE_COUNT; ++i) {
    if (lampColor[selectedLamp].r == palette[i].rgb.r &&
        lampColor[selectedLamp].g == palette[i].rgb.g &&
        lampColor[selectedLamp].b == palette[i].rgb.b) {
      current = i;
      break;
    }
  }
  current = (current + PALETTE_COUNT + direction) % PALETTE_COUNT;
  lampColor[selectedLamp] = palette[current].rgb;
}

static void handleInput(uint8_t index) {
  switch (index) {
    case 0: // UP
      selectedLamp = (selectedLamp + LED_COUNT - 1) % LED_COUNT;
      break;
    case 1: // DOWN
      selectedLamp = (selectedLamp + 1) % LED_COUNT;
      break;
    case 2: // LEFT
      lampBrightness[selectedLamp] =
        lampBrightness[selectedLamp] < BRIGHTNESS_STEP
          ? 0 : lampBrightness[selectedLamp] - BRIGHTNESS_STEP;
      break;
    case 3: // RIGHT
      lampBrightness[selectedLamp] =
        min((uint8_t)100, (uint8_t)(lampBrightness[selectedLamp] + BRIGHTNESS_STEP));
      break;
    case 4: // A
      cycleSelectedColor(1);
      break;
    case 5: // B
      lampEnabled[selectedLamp] = !lampEnabled[selectedLamp];
      break;
  }
  markSettingsChanged();
  sendLeds();
}

static const char *selectedColorName() {
  for (uint8_t i = 0; i < PALETTE_COUNT; ++i) {
    if (lampColor[selectedLamp].r == palette[i].rgb.r &&
        lampColor[selectedLamp].g == palette[i].rgb.g &&
        lampColor[selectedLamp].b == palette[i].rgb.b) {
      return palette[i].name;
    }
  }
  return "CUSTOM";
}

static void drawDisplay() {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tf);

  display.drawStr(0, 8, "BLOOMSHOOT  LIGHT BOX");
  display.drawHLine(0, 11, 128);

  char line[32];
  snprintf(line, sizeof(line), "LAMP %u / %u   %s",
           selectedLamp + 1, LED_COUNT,
           lampEnabled[selectedLamp] ? "ON" : "OFF");
  display.drawStr(0, 22, line);

  snprintf(line, sizeof(line), "COLOR: %s", selectedColorName());
  display.drawStr(0, 33, line);

  snprintf(line, sizeof(line), "BRIGHTNESS: %u%%", lampBrightness[selectedLamp]);
  display.drawStr(0, 44, line);
  display.drawFrame(91, 37, 35, 8);
  if (lampBrightness[selectedLamp] > 0) {
    const uint8_t width = (uint16_t)lampBrightness[selectedLamp] * 31U / 100U;
    display.drawBox(93, 39, width, 4);
  }

  // Six indicators show which physical lamps are enabled; the outlined
  // indicator is the lamp currently being edited.
  for (uint8_t i = 0; i < LED_COUNT; ++i) {
    const uint8_t x = 7 + i * 20;
    if (lampEnabled[i]) display.drawDisc(x + 3, 52, 3);
    if (i == selectedLamp) display.drawFrame(x - 2, 46, 11, 12);
  }

  display.drawStr(0, 63, "UP/DN LAMP  LT/RT LEVEL  A COLOR  B POWER");
  display.sendBuffer();
}

static void printStartupInfo() {
  Serial.println();
  Serial.println("Bloomshoot light controller");
  Serial.println("Controls: UP/DOWN lamp, LEFT/RIGHT brightness, A color, B power");
  Serial.println("Pins: OLED SDA=8 SCL=6; APA106 DATA=7; touch=4,5,9,10,1,2");
  Serial.println("LED chain: 6 APA106; RGB byte order; start at low brightness.");
  for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
    Serial.printf("TOUCH %s pin=%u baseline=%lu threshold=%lu\n",
                  inputNames[i], inputPins[i],
                  (unsigned long)touch[i].baseline,
                  (unsigned long)touch[i].threshold);
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  loadSettings();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(0x3C << 1);
  display.begin();

  ledReady = initLedOutput();
  if (!ledReady) {
    Serial.println("ERROR: LED RMT output did not initialize.");
  }

  calibrateTouchInputs();
  printStartupInfo();

  sendLeds();
  drawDisplay();
}

void loop() {
  bool changed = false;
  for (uint8_t i = 0; i < INPUT_COUNT; ++i) {
    if (justPressed(i)) {
      handleInput(i);
      changed = true;
    }
  }

  const uint32_t now = millis();
  if (settingsDirty && now - settingsChangedAt >= 1200UL) {
    saveSettings();
  }
  if (changed || now - lastDisplayAt >= 250UL) {
    drawDisplay();
    lastDisplayAt = now;
  }
  delay(8);
}
