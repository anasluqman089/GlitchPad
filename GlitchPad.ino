#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BME280.h>

// PIN
#define BTN1        4
#define BTN2        5
#define BTN3        6
#define BTN4        7
#define ENC_A       15
#define ENC_B       16
#define ENC_SW      17
#define BUZZER      18
#define LED_PIN     3

#define OLED_SDA    8
#define OLED_SCL    9
#define I2C_ADDR    0x3C

// DISPLAY
Adafruit_SSD1306 display(128, 32, &Wire, -1);

// BME280
Adafruit_BME280 bme;
bool bmeOK = false;

// STATE
enum Mode { MODE_RAIN, MODE_MSG, MODE_GLITCH, MODE_TELEM, MODE_BREACH };
Mode currentMode = MODE_RAIN;

int  rainSpeed   = 60;      // lower = faster rain
int  brightness  = 200;     // PWM LED duty
bool ledRed      = false;

// Matrix rain drops
uint8_t dropX[10];
uint8_t dropY[10];

// Encoder
volatile long encoderPos = 0;
long lastEncoderPos = 0;
int  encStep = 0;

// Buttons (debounce)
struct Btn {
  uint8_t pin;
  bool lastState;
  uint32_t lastChange;
  void (*callback)();
};
uint32_t lastDebounceTime(uint32_t t){ return t; }

volatile bool btnFlag[4] = {false,false,false,false};

// Debounce vars
bool btnState[4] = {HIGH,HIGH,HIGH,HIGH};
uint32_t debounceT[4] = {0,0,0,0};

unsigned long lastRainUpdate = 0;
unsigned long lastLedUpdate  = 0;
unsigned long glitchEnd      = 0;

// SOUND HELPERS
void beep(int freq, int dur, int vol = 50) {
  ledcWriteTone(0, freq);
  ledcWrite(0, vol);
  delay(dur);
  ledcWrite(0, 0);
}

void matrixBlip() {
  beep(1200, 30);
  beep(900, 30);
  beep(1500, 40);
}

void staticNoise(int dur) {
  // random glitchy noise burst
  for (uint32_t i = 0; i < dur; i += random(10, 60)) {
    beep(random(200, 3000), 15, 30);
  }
  ledcWrite(0, 0);
}

void siren(int cycles) {
  for (int i = 0; i < cycles; i++) {
    for (int f = 400; f < 1800; f += 20) beep(f, 4, 40);
    for (int f = 1800; f > 400; f -= 20) beep(f, 4, 40);
  }
}

//  LED HELPERS 
void setLED(bool red, uint8_t duty) {
  ledRed = red;
  brightness = duty;
}

// BUTTON HANDLERS
void onBtn1() {  // "WAKE UP NEO..."
  currentMode = MODE_MSG;
  setLED(false, 255);
  matrixBlip();
}

void onBtn2() {  // Random glitch
  currentMode = MODE_GLITCH;
  glitchEnd = millis() + 3000;
  setLED(true, 255);
  staticNoise(1500);
}

void onBtn3() {  // Telemetry
  currentMode = MODE_TELEM;
  setLED(false, 120);
  beep(800, 40); beep(1600, 40);
}

void onBtn4() {  // SYSTEM BREACH
  currentMode = MODE_BREACH;
  setLED(true, 255);
  siren(2);
  glitchEnd = millis() + 6000;
}

//  SETUP 
void setup() {
  Serial.begin(115200);

  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT_PULLUP);
  pinMode(BTN4, INPUT_PULLUP);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);

  // Buzzer PWM (ESP32-S3 LEDC)
  ledcAttach(BUZZER, 1000, 8);

  // LED PWM
  ledcAttach(LED_PIN, 5000, 8);
  ledcWrite(LED_PIN, 0);

  // I2C
  Wire.begin(OLED_SDA, OLED_SCL);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR)) {
    Serial.println("OLED FAIL");
    while (1) { beep(200, 200); delay(400); }
  }
  display.setTextColor(SSD1306_WHITE);

  // BME280
  bmeOK = bme.begin(0x76);
  if (!bmeOK) bmeOK = bme.begin(0x77);

  // Randomize rain drops
  randomSeed(analogRead(0));
  for (int i = 0; i < 10; i++) {
    dropX[i] = random(0, 128);
    dropY[i] = random(0, 32);
  }

  // GLITCH BOOT SEQUENCE
  glitchBoot();
  beep(600, 50); beep(1200, 50); beep(2000, 80);
  currentMode = MODE_RAIN;
  setLED(false, 200);
}

void glitchBoot() {
  display.clearDisplay();
  const char* lines[] = {
    "Ox Alpha BIOS v2.1",
    "LOADING MATRIX....",
    "BME280 SENSOR: OK",
    "HACKING MAINFRAME",
    "FOLLOW THE WHITE RABBIT",
    "WAKE UP, NEO..."
  };
  for (int i = 0; i < 6; i++) {
    display.clearDisplay();
    display.setCursor(0, i * 5);
    // glitchy partial text reveal
    for (int c = 0; lines[i][c]; c++) {
      display.print(lines[i][c]);
      if (random(0, 10) > 7) { display.print(random(0,2) ? '#' : '%'); c++; }
    }
    display.display();
    beep(random(400, 2000), 12, 25);
    delay(120);
  }
  // final glitch flash
  for (int i = 0; i < 5; i++) {
    display.invertDisplay(true);  display.display(); beep(random(1000,3000), 20, 30);
    display.invertDisplay(false); display.display(); delay(40);
  }
}

//  ENCODER
void ICACHE_RAM_ATTR encoderISR() {
  static uint8_t oldState = 0;
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);
  uint8_t state = (a << 1) | b;
  if (state != oldState) {
    if ((oldState == 0 && state == 2) ||
        (oldState == 2 && state == 3) ||
        (oldState == 3 && state == 1) ||
        (oldState == 1 && state == 0)) encoderPos++;
    else encoderPos--;
    oldState = state;
  }
}

// MATRIX RAIN
void drawRain() {
  display.clearDisplay();
  for (int i = 0; i < 10; i++) {
    display.drawPixel(dropX[i], dropY[i], SSD1306_WHITE);
    display.drawPixel(dropX[i], dropY[i] - 1, SSD1306_WHITE);
    display.drawPixel(dropX[i], dropY[i] - 2, SSD1306_WHITE);
    dropY[i] += random(1, 3);
    if (dropY[i] > 32) {
      dropY[i] = 0;
      dropX[i] = random(0, 128);
    }
  }
  // BME280 data glitched into the rain
  if (bmeOK && (millis() / 2000) % 2) {
    display.setCursor(35, 12);
    display.setTextSize(1);
    display.printf("%.1fC", bme.readTemperature());
  } else if (bmeOK) {
    display.setCursor(35, 12);
    display.printf("%.0f%%", bme.readHumidity());
  }
  display.display();
}

// GLITCH EFFECT
void drawGlitch() {
  display.clearDisplay();
  // Random corrupted blocks
  for (int i = 0; i < 40; i++) {
    int x = random(0, 120), y = random(0, 28);
    display.fillRect(x, y, random(2, 12), random(2, 6), random(0, 2));
  }
  // Corrupted matrix characters
  display.setTextSize(1);
  for (int i = 0; i < 12; i++) {
    display.setCursor(random(0, 120), random(0, 24));
    display.print((char)random(33, 127));
  }
  display.setCursor(30, 14);
  display.print("ERR##0xDEAD");
  display.display();
  if (millis() > glitchEnd) { currentMode = MODE_RAIN; setLED(false, 200); }
}

// TELEMETRY SCREEN
void drawTelemetry() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(">> SYSTEM READOUT");
  if (bmeOK) {
    display.setCursor(0, 10);
    display.printf("TEMP: %.1f C", bme.readTemperature());
    display.setCursor(0, 19);
    display.printf("HUM : %.1f %%  P: %.1f hPa",
                   bme.readHumidity(), bme.readPressure() / 100.0);
  } else {
    display.setCursor(0, 12);
    display.print("BME280: OFFLINE!");
  }
  display.display();
}

//BREACH SCREEN
void drawBreach() {
  display.clearDisplay();
  if (millis() % 400 < 200) {
    display.fillScreen(SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setTextSize(1);
  display.setCursor(20, 4);  display.print("!! WARNING !!");
  display.setCursor(8, 14);  display.print("SYSTEM BREACH DETECTED");
  display.setCursor(30, 24); display.print("0xH4CK3D_NUMB3R5");
  display.setTextColor(SSD1306_WHITE);
  display.display();
  if (millis() > glitchEnd) { currentMode = MODE_RAIN; setLED(false, 200); ledcWrite(0,0); }
}

// MESSAGE SCREEN
void drawMessage() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(16, 8);
  display.print("WAKE UP, NEO...");
  display.setCursor(16, 18);
  display.print("THE MATRIX HAS YOU");
  display.display();
}

// LOOP
void loop() {
  // Buttons (debounced)
  uint8_t pins[4] = {BTN1, BTN2, BTN3, BTN4};
  void (*cbs[4])() = {onBtn1, onBtn2, onBtn3, onBtn4};
  for (int i = 0; i < 4; i++) {
    bool s = digitalRead(pins[i]);
    if (s != btnState[i] && millis() - debounceT[i] > 50) {
      debounceT[i] = millis();
      btnState[i] = s;
      if (s == LOW) { cbs[i](); beep(2000, 10, 30); }
    }
  }

  //Encoder SW (push) = toggle rain speed / brightness
  static uint32_t swT = 0;
  if (digitalRead(ENC_SW) == LOW && millis() - swT > 300) {
    swT = millis();
    rainSpeed = (rainSpeed == 60) ? 30 : 60;
    brightness = (brightness == 200) ? 255 : 200;
    beep(1500, 30);
  }

  //Encoder rotation = adjust LED brightness
  if (encoderPos != lastEncoderPos) {
    long diff = encoderPos - lastEncoderPos;
    lastEncoderPos = encoderPos;
    brightness = constrain(brightness + diff * 10, 0, 255);
    beep(1000 + (brightness * 4), 8, 20);
  }

  //LED breathing effect
  if (millis() - lastLedUpdate > 30) {
    lastLedUpdate = millis();
    int b = brightness;
    if (!ledRed) {  // green = red+green channels simulated by flicker
      ledcWrite(LED_PIN, b - random(0, 20));
    } else {
      ledcWrite(LED_PIN, millis() % 200 < 100 ? b : b / 4); // alarm strobe
    }
  }

  // Screen modes
  if (millis() - lastRainUpdate > rainSpeed / 10) {
    lastRainUpdate = millis();
    switch (currentMode) {
      case MODE_RAIN:   drawRain();     break;
      case MODE_MSG:    drawMessage();  break;
      case MODE_GLITCH: drawGlitch();   break;
      case MODE_TELEM:  drawTelemetry();break;
      case MODE_BREACH: drawBreach();   break;
    }
    // occasional random micro-glitch in any mode
    if (random(0, 500) > 496) {
      display.invertDisplay(true); beep(random(500,2500), 10, 25);
      delay(30);
      display.invertDisplay(false);
    }
  }
}
