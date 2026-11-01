/*
  SmartDesk Alarm — Hack Club Fabricate / BLARE
  Seeed Studio XIAO ESP32C3 + 2.25" ST7789 (76 x 284)
  Libraries: Adafruit GFX Library, Adafruit ST7735 and ST7789 Library
  Built-in ESP32 libraries: WiFi, Preferences, SPI, time

  Control: SW1 UP, SW2 DOWN, SW3 OK, SW4 BACK.
  The 2.25" display is 284 x 76 when rotated into landscape.

  NOTE: check PIN_TFT_RST and PIN_TFT_BL against your KiCad PCB!
  Use -1 if RST is physically tied to 3V3, BL tied to GND.

  Avoid uploading real Wi-Fi passwords to a public GitHub repository.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_arduino_version.h>
#include <time.h>
#include <sys/time.h>

// ============= SETTINGS TO EDIT =============
const char* WIFI_SSID = "";          // Your Wi-Fi network name (2.4 GHz)
const char* WIFI_PASSWORD = "";      // Your Wi-Fi password; do not publish it

// All numbers below are GPIO numbers, NOT J1 connector pin numbers.
const int BUTTON_UP = 2;              // D0, SW1
const int BUTTON_DOWN = 3;            // D1, SW2
const int BUTTON_OK = 4;              // D2, SW3
const int BUTTON_BACK = 5;            // D3, SW4
const int PIN_TFT_DC = 6;             // D4
const int PIN_TFT_CS = 7;             // D5
const int PIN_TFT_RST = 8;            // D8 — change to -1 if wired to 3V3
const int PIN_TFT_SCK = 9;            // D9 -> SCL (SPI clock)
const int PIN_TFT_MOSI = 10;          // D10 -> SDA (SPI data)
const int PIN_BUZZER = 20;            // D7
const bool ACTIVE_BUZZER = false;      // Set true if your buzzer only needs ON/OFF voltage
const int PIN_TFT_BL = 21;            // D6 — change to -1 if wired to GND
// This 2.25" kit typically has ACTIVE-LOW backlight (LOW = ON).

const char* TZ_CZECH = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ============= GLOBALS =============
Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Preferences prefs;

const uint16_t BG = ST77XX_BLACK;
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t CYAN = 0x07FF;
const uint16_t YELLOW = 0xFFE0;
const uint16_t RED = 0xF800;
const uint16_t MUTED = 0x8410;
const uint16_t SELECT_BG = 0x18A3;
const int SCREEN_W = 284;
const int SCREEN_H = 76;

enum Page { HOME, MENU, SCHOOL_EDIT, WEEKEND_EDIT, TIMER_EDIT, MANUAL_EDIT };
enum RingKind { RING_NONE, RING_SCHOOL, RING_WEEKEND, RING_TIMER };
Page page = HOME;
RingKind ringing = RING_NONE;

int schoolHour = 7, schoolMinute = 0;
bool schoolEnabled = true;
int weekendHour = 9, weekendMinute = 0;
bool weekendEnabled = false;
int timerMinutes = 10;
bool timerRunning = false;
unsigned long timerDeadline = 0; // millis() based; resets at power loss

int menuIndex = 0;
int editField = 0;
const char* MENU_ITEMS[] = {
  "SKOLNI BUDIK", "VIKEND BUDIK", "CASOVAC",
  "RUCNI DATUM/CAS", "SYNC WI-FI", "TEST MATEMATIKY"
};
const int MENU_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// Manual local date/time editing (not stored in flash: no battery-backed RTC).
int manualYear = 2026, manualMonth = 1, manualDay = 1;
int manualHour = 7, manualMinute = 0;

// Once-per-day triggers, including if the time is corrected backward.
int lastSchoolDay = -1;
int lastWeekendDay = -1;

// Math challenge; answerIndex is among 3 options.
int problemA = 0, problemB = 0, problemOp = 0;
int options[3] = {0, 0, 0};
int selectedOption = 0;
int correctOption = 0;
int solvedCount = 0;

const int keyPins[4] = {BUTTON_UP, BUTTON_DOWN, BUTTON_OK, BUTTON_BACK};
bool stableKeys[4] = {HIGH, HIGH, HIGH, HIGH};
bool lastRawKeys[4] = {HIGH, HIGH, HIGH, HIGH};
unsigned long changedAt[4] = {0, 0, 0, 0};

unsigned long lastDraw = 0;
unsigned long lastWiFiAttempt = 0;
uint32_t lastBuzzerFrequency = 0;
unsigned long lastSecond = 0;
bool redraw = true;

// ============= TIME =============
bool validTime() {
  return time(nullptr) >= 1704067200;  // at least January 2024
}

bool localNow(tm& info) {
  if (!validTime()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &info);
  return true;
}

void beginWiFi() {
  if (WIFI_SSID[0] == '\0') return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();
}

void checkWiFi() {
  if (WIFI_SSID[0] == '\0') return;
  // Do not block the UI while trying to reconnect.
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiAttempt > 60000UL) {
    WiFi.reconnect();
    lastWiFiAttempt = millis();
  }
}

void startManualEditor() {
  tm now;
  if (localNow(now)) {
    manualYear = now.tm_year + 1900;
    manualMonth = now.tm_mon + 1;
    manualDay = now.tm_mday;
    manualHour = now.tm_hour;
    manualMinute = now.tm_min;
  }
  editField = 0;
  page = MANUAL_EDIT;
  redraw = true;
}

void setManualClock() {
  tm edited = {};
  edited.tm_year = manualYear - 1900;
  edited.tm_mon = manualMonth - 1;
  edited.tm_mday = manualDay;
  edited.tm_hour = manualHour;
  edited.tm_min = manualMinute;
  edited.tm_sec = 0;
  edited.tm_isdst = -1;
  time_t timestamp = mktime(&edited);
  timeval tv = {timestamp, 0};
  settimeofday(&tv, nullptr);
  // Keep the current time until it can next be synchronized by Wi-Fi.
  page = MENU;
  redraw = true;
}

// ============= SOUND =============
void prepareBuzzer() {
  if (ACTIVE_BUZZER) {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_BUZZER, 2400, 8);
  ledcWrite(PIN_BUZZER, 0);
#else
  ledcSetup(0, 2400, 8);
  ledcAttachPin(PIN_BUZZER, 0);
  ledcWrite(0, 0);
#endif
}

void buzzerTone(uint32_t freq) {
  if (freq == lastBuzzerFrequency) return;
  lastBuzzerFrequency = freq;
  if (ACTIVE_BUZZER) {
    digitalWrite(PIN_BUZZER, freq > 0 ? HIGH : LOW);
    return;
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(PIN_BUZZER, freq);
#else
  ledcWriteTone(0, freq);
#endif
}

void stopBuzzer() { buzzerTone(0); }

void soundTick() {
  if (ringing == RING_NONE) {
    stopBuzzer();
    return;
  }
  unsigned long now = millis();
  // Alternating frequency, with short silences.
  unsigned long phase = now % 750UL;
  if (phase < 240) buzzerTone(2200);
  else if (phase < 480) buzzerTone(2800);
  else stopBuzzer();
}

// ============= STORE SETTINGS =============
void loadSettings() {
  prefs.begin("smartalarm", false);
  schoolHour = constrain(prefs.getInt("schH", 7), 0, 23);
  schoolMinute = constrain(prefs.getInt("schM", 0), 0, 59);
  schoolEnabled = prefs.getBool("schOn", true);
  weekendHour = constrain(prefs.getInt("wkH", 9), 0, 23);
  weekendMinute = constrain(prefs.getInt("wkM", 0), 0, 59);
  weekendEnabled = prefs.getBool("wkOn", false);
  timerMinutes = constrain(prefs.getInt("timM", 10), 1, 180);
}

void saveSettings() {
  prefs.putInt("schH", schoolHour);
  prefs.putInt("schM", schoolMinute);
  prefs.putBool("schOn", schoolEnabled);
  prefs.putInt("wkH", weekendHour);
  prefs.putInt("wkM", weekendMinute);
  prefs.putBool("wkOn", weekendEnabled);
  prefs.putInt("timM", timerMinutes);
}

// ============= MATH PROBLEMS =============
void newMathProblem() {
  problemOp = random(0, 3); // 0 +, 1 -, 2 *
  problemA = random(2, 20);
  problemB = random(2, 15);
  if (problemOp == 2) {
    problemA = random(2, 10);
    problemB = random(2, 10);
  }
  if (problemOp == 1 && problemA < problemB) {
    int tmp = problemA;
    problemA = problemB;
    problemB = tmp;
  }
  int answer = (problemOp == 0) ? (problemA + problemB) :
               (problemOp == 1) ? (problemA - problemB) :
                                  (problemA * problemB);
  correctOption = random(0, 3);
  options[correctOption] = answer;
  for (int i = 0; i < 3; ++i) {
    if (i == correctOption) continue;
    int candidate;
    do {
      candidate = answer + random(-12, 13);
    } while (candidate < 0 || candidate == answer ||
             (i > 0 && i != correctOption && candidate == options[0]));
    // The previous check handles most duplicates; now ensure uniqueness.
    bool duplicate;
    do {
      duplicate = false;
      for (int j = 0; j < i; ++j) {
        if (options[j] == candidate) duplicate = true;
      }
      if (duplicate) candidate = answer + random(-20, 21);
      if (candidate < 0 || candidate == answer) duplicate = true;
    } while (duplicate);
    options[i] = candidate;
  }
  selectedOption = 0;
  redraw = true;
}

void startRing(RingKind kind) {
  ringing = kind;
  page = HOME;
  solvedCount = 0;
  if (kind == RING_SCHOOL) newMathProblem();
  redraw = true;
}

void endRing() {
  ringing = RING_NONE;
  stopBuzzer();
  redraw = true;
}

// ============= TIMER & DAILY ALARMS =============
void checkAlarms() {
  tm now;
  if (localNow(now)) {
    int dayId = (now.tm_year + 1900) * 366 + now.tm_yday;
    bool schoolDay = now.tm_wday >= 1 && now.tm_wday <= 5;
    bool weekendDay = (now.tm_wday == 0 || now.tm_wday == 6);
    if (ringing == RING_NONE) {
      if (schoolDay && schoolEnabled && lastSchoolDay != dayId &&
          now.tm_hour == schoolHour && now.tm_min == schoolMinute) {
        lastSchoolDay = dayId;
        startRing(RING_SCHOOL);
      } else if (weekendDay && weekendEnabled && lastWeekendDay != dayId &&
                 now.tm_hour == weekendHour && now.tm_min == weekendMinute) {
        lastWeekendDay = dayId;
        startRing(RING_WEEKEND);
      }
    }
  }
  if (timerRunning && (int32_t)(millis() - timerDeadline) >= 0) {
    timerRunning = false;
    if (ringing == RING_NONE) startRing(RING_TIMER);
  }
}

unsigned long remainingTimerSeconds() {
  if (!timerRunning || (int32_t)(millis() - timerDeadline) >= 0) return 0;
  return (timerDeadline - millis() + 999UL) / 1000UL;
}

// ============= SCREEN DRAWING =============
void textAt(int x, int y, const String& content,
            uint16_t color = WHITE, int size = 1) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(content);
}

void centered(int y, const String& content, uint16_t color, int size) {
  int textWidth = content.length() * 6 * size;
  textAt(max(0, (SCREEN_W - textWidth) / 2), y, content, color, size);
}

void frame(const String& title, const String& hint) {
  tft.fillScreen(BG);
  textAt(4, 2, title, CYAN);
  tft.drawFastHLine(0, 12, SCREEN_W, MUTED);
  tft.drawFastHLine(0, 64, SCREEN_W, MUTED);
  textAt(4, 67, hint, MUTED);
}

String two(int number) {
  return (number < 10 ? "0" : "") + String(number);
}

void drawHome() {
  tft.fillScreen(BG);
  tm now;
  if (localNow(now)) {
    const char* DAYS[7] = {"NE", "PO", "UT", "ST", "CT", "PA", "SO"};
    String date = String(DAYS[now.tm_wday]) + " " + two(now.tm_mday) + "." +
                  two(now.tm_mon + 1) + "." + String(now.tm_year + 1900);
    textAt(4, 3, date, CYAN);
    String clockText = two(now.tm_hour) + ":" + two(now.tm_min) + ":" + two(now.tm_sec);
    centered(23, clockText, WHITE, 3);
  } else {
    textAt(4, 3, "CAS NENASTAVEN", RED);
    centered(24, "--:--:--", YELLOW, 3);
  }
  if (WiFi.status() == WL_CONNECTED) textAt(256, 3, "WF", CYAN);
  tft.drawFastHLine(0, 60, SCREEN_W, MUTED);
  String foot = "SK " + two(schoolHour) + ":" + two(schoolMinute) +
                (schoolEnabled ? " ON" : " OFF");
  if (timerRunning) {
    unsigned long sec = remainingTimerSeconds();
    foot = "TIMER " + two(sec / 60) + ":" + two(sec % 60);
  }
  textAt(4, 65, foot, YELLOW);
  textAt(218, 65, "OK=MENU", MUTED);
}

void drawMenu() {
  frame("MENU", "UP/DN:VYBER  OK:OTEVRI  ZPET");
  int top = max(0, min(menuIndex - 1, MENU_COUNT - 3));
  for (int i = 0; i < 3; i++) {
    int index = top + i;
    if (index >= MENU_COUNT) break;
    int y = 16 + 16 * i;
    if (index == menuIndex) tft.fillRoundRect(3, y - 2, 277, 15, 3, SELECT_BG);
    textAt(9, y, String(index == menuIndex ? "> " : "  ") + MENU_ITEMS[index],
           index == menuIndex ? YELLOW : WHITE);
  }
}

void drawAlarmEdit(bool school) {
  int h = school ? schoolHour : weekendHour;
  int m = school ? schoolMinute : weekendMinute;
  bool enabled = school ? schoolEnabled : weekendEnabled;
  frame(school ? "SKOLNI BUDIK (PO-PA)" : "VIKENDOVY BUDIK (SO-NE)",
        "UP/DN:ZMENA  OK:DAL  ZPET:MENU");
  if (editField == 0) centered(26, "> " + two(h) + " : " + two(m), YELLOW, 2);
  else if (editField == 1) centered(26, two(h) + " : " + two(m) + " <", YELLOW, 2);
  else centered(26, String("ZAPNUTO: ") + (enabled ? "ANO" : "NE"), YELLOW, 2);
}

void drawTimer() {
  frame("CASOVAC", "UP/DN:MINUTY   OK:START/STOP");
  if (timerRunning) {
    unsigned long sec = remainingTimerSeconds();
    centered(24, two(sec / 60) + ":" + two(sec % 60), YELLOW, 3);
  } else {
    centered(24, String(timerMinutes) + " MIN", CYAN, 3);
  }
}

void drawManual() {
  const char* names[5] = {"ROK", "MESIC", "DEN", "HODINA", "MINUTA"};
  int vals[5] = {manualYear, manualMonth, manualDay, manualHour, manualMinute};
  frame("RUCNI NASTAVENI CASU", "UP/DN:ZMENA  OK:DAL/ULOZ  ZPET");
  textAt(13, 28, names[editField], WHITE, 2);
  textAt(165, 28, String(vals[editField]), YELLOW, 2);
}

void drawRing() {
  tft.fillScreen(BG);
  if (ringing == RING_SCHOOL) {
    textAt(4, 3, "SKOLNI BUDIK - PRIKLAD " + String(solvedCount + 1) + "/3", RED);
    char symbol = (problemOp == 0) ? '+' : (problemOp == 1) ? '-' : 'x';
    centered(20, String(problemA) + " " + symbol + " " + String(problemB) + " = ?", YELLOW, 2);
    for (int i = 0; i < 3; i++) {
      int x = 8 + i * 94;
      if (i == selectedOption) tft.fillRoundRect(x - 3, 50, 89, 22, 4, SELECT_BG);
      textAt(x + 4, 54, String(options[i]), i == selectedOption ? CYAN : WHITE, 2);
    }
  } else {
    centered(12, ringing == RING_TIMER ? "CASOVAC HOTOV!" : "VIKENDOVY BUDIK", RED, 2);
    centered(48, "STISKNI TLACITKO", YELLOW, 1);
  }
}

void draw() {
  if (ringing != RING_NONE) drawRing();
  else if (page == HOME) drawHome();
  else if (page == MENU) drawMenu();
  else if (page == SCHOOL_EDIT) drawAlarmEdit(true);
  else if (page == WEEKEND_EDIT) drawAlarmEdit(false);
  else if (page == TIMER_EDIT) drawTimer();
  else if (page == MANUAL_EDIT) drawManual();
  redraw = false;
  lastDraw = millis();
}

// ============= BUTTONS =============
// Returns 0 UP, 1 DOWN, 2 OK, 3 BACK, or -1 (no new press).
int pollButtons() {
  for (int i = 0; i < 4; i++) {
    bool raw = digitalRead(keyPins[i]);
    if (raw != lastRawKeys[i]) {
      lastRawKeys[i] = raw;
      changedAt[i] = millis();
    }
    if (millis() - changedAt[i] >= 30UL && raw != stableKeys[i]) {
      stableKeys[i] = raw;
      if (raw == LOW) return i;  // INPUT_PULLUP: pressed = LOW
    }
  }
  return -1;
}

void handleButton(int key) {
  if (ringing != RING_NONE) {
    if (ringing != RING_SCHOOL) {
      endRing();
      return;
    }
    if (key == 0) selectedOption = (selectedOption + 2) % 3;
    if (key == 1) selectedOption = (selectedOption + 1) % 3;
    if (key == 2) {
      if (selectedOption == correctOption) {
        solvedCount++;
        if (solvedCount >= 3) endRing();
        else newMathProblem();
      } else {
        newMathProblem();
      }
    }
    // BACK deliberately does not dismiss the school alarm.
    redraw = true;
    return;
  }

  if (page == HOME) {
    if (key == 2) { page = MENU; menuIndex = 0; }
  } else if (page == MENU) {
    if (key == 0) menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    if (key == 1) menuIndex = (menuIndex + 1) % MENU_COUNT;
    if (key == 3) page = HOME;
    if (key == 2) {
      editField = 0;
      if (menuIndex == 0) page = SCHOOL_EDIT;
      if (menuIndex == 1) page = WEEKEND_EDIT;
      if (menuIndex == 2) page = TIMER_EDIT;
      if (menuIndex == 3) startManualEditor();
      if (menuIndex == 4) {
        beginWiFi();
        // NTP synchronization runs automatically after Wi-Fi connects.
      }
      if (menuIndex == 5) startRing(RING_SCHOOL); // immediate test
    }
  } else if (page == SCHOOL_EDIT || page == WEEKEND_EDIT) {
    bool isSchool = (page == SCHOOL_EDIT);
    int& h = isSchool ? schoolHour : weekendHour;
    int& m = isSchool ? schoolMinute : weekendMinute;
    bool& en = isSchool ? schoolEnabled : weekendEnabled;
    int change = (key == 0) ? 1 : (key == 1) ? -1 : 0;
    if (change != 0) {
      if (editField == 0) h = (h + change + 24) % 24;
      if (editField == 1) m = (m + change + 60) % 60;
      if (editField == 2) en = !en;
    }
    if (key == 2) {
      editField++;
      if (editField >= 3) { saveSettings(); page = MENU; }
    }
    if (key == 3) { saveSettings(); page = MENU; }
  } else if (page == TIMER_EDIT) {
    if (!timerRunning && key == 0) timerMinutes = min(180, timerMinutes + 1);
    if (!timerRunning && key == 1) timerMinutes = max(1, timerMinutes - 1);
    if (key == 2) {
      if (timerRunning) timerRunning = false;
      else {
        timerDeadline = millis() + (unsigned long)timerMinutes * 60000UL;
        timerRunning = true;
      }
      saveSettings();
    }
    if (key == 3) { saveSettings(); page = MENU; }
  } else if (page == MANUAL_EDIT) {
    int change = (key == 0) ? 1 : (key == 1) ? -1 : 0;
    if (change != 0) {
      if (editField == 0) manualYear = constrain(manualYear + change, 2024, 2099);
      if (editField == 1) manualMonth = (manualMonth - 1 + change + 12) % 12 + 1;
      if (editField == 2) manualDay = (manualDay - 1 + change + 31) % 31 + 1;
      if (editField == 3) manualHour = (manualHour + change + 24) % 24;
      if (editField == 4) manualMinute = (manualMinute + change + 60) % 60;
    }
    if (key == 2) {
      editField++;
      if (editField >= 5) setManualClock();
    }
    if (key == 3) page = MENU;
  }
  redraw = true;
}

// ============= ARDUINO ENTRY POINTS =============
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(keyPins[i], INPUT_PULLUP);
  if (PIN_TFT_BL >= 0) {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, LOW);  // Fabricate kit: active-low BL
  }
  prepareBuzzer();
  loadSettings();
  randomSeed(micros());

  SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(76, 284);
  tft.setRotation(1);        // 284 x 76
  tft.setTextWrap(false);
  tft.fillScreen(BG);
  centered(26, "SMARTDESK ALARM", CYAN, 2);
  delay(700);

  // Keep Czech DST rules even when running offline with manually entered time.
  setenv("TZ", TZ_CZECH, 1);
  tzset();
  if (WIFI_SSID[0] != '\0') {
    configTzTime(TZ_CZECH, "pool.ntp.org", "time.google.com");
    beginWiFi();
  }
  redraw = true;
}

void loop() {
  int key = pollButtons();
  if (key >= 0) handleButton(key);
  checkWiFi();
  checkAlarms();
  soundTick();

  // Update the clock and timer roughly once per second.
  if (millis() - lastSecond >= 1000UL) {
    lastSecond = millis();
    if (page == HOME || page == TIMER_EDIT) redraw = true;
  }
  if (redraw) draw();
  delay(5);
}
