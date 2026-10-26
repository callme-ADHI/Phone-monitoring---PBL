/*
 * ============================================================================
 * Project: Smart Classroom Occupancy & Phone Policy Enforcement Monitor
 * Target : STM32F103C8T6 (Blue Pill)
 * Board  : Generic STM32F1 series -> BluePill F103C8 (or F103CB)
 * 
 * Hardware Connections:
 * ----------------------------------------------------------------------------
 * 1. OLED Display (0.96" SSD1306 I2C 128x64):
 *      - VCC -> 3.3V
 *      - GND -> GND
 *      - SCL -> PB6 (I2C1 SCL)
 *      - SDA -> PB7 (I2C1 SDA)
 * 
 * 2. ESP32 UART Link (Serial1 / USART1):
 *      - STM32 PA10 (USART1 RX) <- ESP32 GPIO17 (TX2)
 *      - STM32 PA9  (USART1 TX) -> ESP32 GPIO16 (RX2)  [Optional return telemetry]
 *      - GND                    -> ESP32 GND            [Crucial: Common Ground!]
 * 
 * 3. Status LEDs:
 *      - Green LED (Compliant / Normal) : PB0 (or On-board PC13)
 *      - Red LED (Violation / Mismatch) : PB1
 *      - Yellow/Buzzer (High Phone Usage): PB10
 * ============================================================================
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Output Indicator Pins
#define LED_COMPLIANT_PIN   PB0
#define LED_VIOLATION_PIN   PB1
#define LED_HIGH_USAGE_PIN  PB10
#define ONBOARD_LED_PIN     PC13  // Inverted logic on Blue Pill (LOW = ON)

// Serial line buffer
char rxBuffer[128];
uint8_t rxIndex = 0;

// Telemetry State parsed from ESP32
int  personCount  = 0;
int  phoneCount   = 0;
bool inSession    = false;
bool highUsage    = false;
char startTime[6] = "08:00";
char endTime[6]   = "18:00";
unsigned long lastPacketMs = 0;

// LED Blink Timer
unsigned long lastBlinkMs = 0;
bool blinkState = false;

void setup() {
  // Debug USB / Hardware Serial (USART1 is PA9-TX, PA10-RX)
  Serial1.begin(115200);

  pinMode(LED_COMPLIANT_PIN, OUTPUT);
  pinMode(LED_VIOLATION_PIN, OUTPUT);
  pinMode(LED_HIGH_USAGE_PIN, OUTPUT);
  pinMode(ONBOARD_LED_PIN, OUTPUT);

  digitalWrite(LED_COMPLIANT_PIN, LOW);
  digitalWrite(LED_VIOLATION_PIN, LOW);
  digitalWrite(LED_HIGH_USAGE_PIN, LOW);
  digitalWrite(ONBOARD_LED_PIN, HIGH); // OFF

  // I2C OLED Initialization
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // Try alternate I2C address 0x3D
    display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 10);
  display.println("CLASSROOM MONITOR");
  display.setCursor(10, 30);
  display.println("STM32 BluePill Node");
  display.setCursor(12, 48);
  display.println("Waiting ESP32 Link...");
  display.display();
  delay(1500);
}

/*
 * Line Protocol Expected from ESP32:
 * "$TELEM,P:<personCount>,M:<phoneCount>,S:<inSession 0/1>,H:<highUsage 0/1>,T1:<HH:MM>,T2:<HH:MM>*\n"
 * Example: "$TELEM,P:3,M:3,S:1,H:0,T1:09:00,T2:10:30*\n"
 */
void parseTelemetry(char* line) {
  if (strncmp(line, "$TELEM,", 7) != 0) return;

  char* pStr = strstr(line, "P:");
  char* mStr = strstr(line, "M:");
  char* sStr = strstr(line, "S:");
  char* hStr = strstr(line, "H:");
  char* t1Str = strstr(line, "T1:");
  char* t2Str = strstr(line, "T2:");

  if (pStr) personCount = atoi(pStr + 2);
  if (mStr) phoneCount  = atoi(mStr + 2);
  if (sStr) inSession   = (atoi(sStr + 2) == 1);
  if (hStr) highUsage   = (atoi(hStr + 2) == 1);

  if (t1Str) {
    strncpy(startTime, t1Str + 3, 5);
    startTime[5] = '\0';
  }
  if (t2Str) {
    strncpy(endTime, t2Str + 3, 5);
    endTime[5] = '\0';
  }

  lastPacketMs = millis();
}

void updateIndicators() {
  unsigned long now = millis();
  bool isLinkAlive = (now - lastPacketMs < 4000);

  // Fast blink toggle every 250ms for alert
  if (now - lastBlinkMs > 250) {
    lastBlinkMs = now;
    blinkState = !blinkState;
  }

  if (!isLinkAlive) {
    // Waiting for ESP32 link - pulse onboard and yellow LED
    digitalWrite(LED_COMPLIANT_PIN, LOW);
    digitalWrite(LED_VIOLATION_PIN, LOW);
    digitalWrite(LED_HIGH_USAGE_PIN, blinkState ? HIGH : LOW);
    digitalWrite(ONBOARD_LED_PIN, blinkState ? LOW : HIGH);
    return;
  }

  // ALERT CONDITION: Person count is greater than phone count
  bool isAlert = (personCount > phoneCount);

  if (isAlert) {
    // Alert mode: Red violation LED blinks rapidly, Green compliant LED is OFF
    digitalWrite(LED_COMPLIANT_PIN, LOW);
    digitalWrite(LED_VIOLATION_PIN, blinkState ? HIGH : LOW);
    digitalWrite(LED_HIGH_USAGE_PIN, highUsage ? (blinkState ? HIGH : LOW) : LOW);
    digitalWrite(ONBOARD_LED_PIN, blinkState ? LOW : HIGH);
  } else {
    // Normal mode: Green LED is steady ON, Red violation LED is OFF
    digitalWrite(LED_COMPLIANT_PIN, HIGH);
    digitalWrite(LED_VIOLATION_PIN, LOW);
    digitalWrite(LED_HIGH_USAGE_PIN, highUsage ? HIGH : LOW);
    digitalWrite(ONBOARD_LED_PIN, HIGH); // OFF (inverted logic)
  }
}

void drawCenteredString(const char* buf, int xStart, int width, int y, int size) {
  int charWidth = size * 6;
  int textWidth = strlen(buf) * charWidth;
  int x = xStart + (width - textWidth) / 2;
  if (x < xStart) x = xStart;
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(buf);
}

void updateOLED() {
  unsigned long now = millis();
  bool isLinkAlive = (now - lastPacketMs < 4000);

  display.clearDisplay();

  // If ESP32 link has dropped
  if (!isLinkAlive) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    drawCenteredString("ESP32 LINK", 0, 128, 12, 1);
    drawCenteredString("WAITING...", 0, 128, 28, 2);
    drawCenteredString("Check PA10 <- TX2", 0, 128, 50, 1);
    display.display();
    return;
  }

  bool isAlert = (personCount > phoneCount);

  if (isAlert) {
    // ══════════════════════════════════════════════════════════════════════════
    // ALERT VIEW (Only shown when Person Count > Phone Count)
    // ══════════════════════════════════════════════════════════════════════════
    // Top Inverted Alert Banner
    if (blinkState) {
      display.fillRect(0, 0, 128, 14, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    } else {
      display.drawRect(0, 0, 128, 14, SSD1306_WHITE);
      display.setTextColor(SSD1306_WHITE);
    }
    drawCenteredString("! ! !  ALERT  ! ! !", 0, 128, 3, 1);

    display.setTextColor(SSD1306_WHITE);

    // Current metrics summary
    char countBuf[32];
    snprintf(countBuf, sizeof(countBuf), "PERSON:%d  PHONE:%d", personCount, phoneCount);
    drawCenteredString(countBuf, 0, 128, 18, 1);

    display.drawFastHLine(8, 29, 112, SSD1306_WHITE);

    // Main Alert Headline
    display.setTextSize(2);
    display.setCursor(8, 33);
    display.print("PERSON>PHONE");

    // Missing devices count
    int missing = personCount - phoneCount;
    char missBuf[32];
    snprintf(missBuf, sizeof(missBuf), "Missing: %d Phone%s", missing, missing == 1 ? "" : "s");
    drawCenteredString(missBuf, 0, 128, 53, 1);

  } else {
    // ══════════════════════════════════════════════════════════════════════════
    // NORMAL VIEW (Clean, perfectly aligned 2-column display)
    // ══════════════════════════════════════════════════════════════════════════
    display.setTextColor(SSD1306_WHITE);

    // Header: Room Monitor
    drawCenteredString("ROOM MONITOR", 0, 128, 2, 1);
    display.drawFastHLine(0, 12, 128, SSD1306_WHITE);

    // Vertical column divider between Person (left) and Phone (right)
    display.drawFastVLine(63, 13, 38, SSD1306_WHITE);

    // Left Column: PERSON
    drawCenteredString("PERSON", 0, 63, 16, 1);
    char pBuf[8];
    snprintf(pBuf, sizeof(pBuf), "%d", personCount);
    drawCenteredString(pBuf, 0, 63, 27, 3);

    // Right Column: PHONES
    drawCenteredString("PHONES", 64, 63, 16, 1);
    char mBuf[8];
    snprintf(mBuf, sizeof(mBuf), "%d", phoneCount);
    drawCenteredString(mBuf, 64, 63, 27, 3);

    // Bottom Divider & Status
    display.drawFastHLine(0, 52, 128, SSD1306_WHITE);

    if (personCount == phoneCount) {
      drawCenteredString("STATUS: EXACT MATCH", 0, 128, 55, 1);
    } else {
      drawCenteredString("STATUS: NORMAL", 0, 128, 55, 1);
    }
  }

  display.display();
}

void loop() {
  // Read Serial1 stream from ESP32
  while (Serial1.available() > 0) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (rxIndex > 0) {
        rxBuffer[rxIndex] = '\0';
        parseTelemetry(rxBuffer);
        rxIndex = 0;
      }
    } else if (rxIndex < sizeof(rxBuffer) - 1) {
      rxBuffer[rxIndex++] = c;
    }
  }

  updateIndicators();
  updateOLED();
  delay(50);
}
