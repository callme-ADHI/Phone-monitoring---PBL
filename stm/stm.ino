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
  bool isLinkAlive = (now - lastPacketMs < 5000);

  // Mismatch logic: more phones than people or distinct count difference
  bool isMismatch = (phoneCount != personCount) || (phoneCount > personCount);

  // Fast blink toggle every 300ms
  if (now - lastBlinkMs > 300) {
    lastBlinkMs = now;
    blinkState = !blinkState;
  }

  if (!isLinkAlive) {
    // Waiting for link - slow blink yellow
    digitalWrite(LED_COMPLIANT_PIN, LOW);
    digitalWrite(LED_VIOLATION_PIN, LOW);
    digitalWrite(LED_HIGH_USAGE_PIN, blinkState ? HIGH : LOW);
    digitalWrite(ONBOARD_LED_PIN, blinkState ? LOW : HIGH);
    return;
  }

  // Active Policy Determination
  if (inSession) {
    if (highUsage) {
      // Critical: Phone streaming/high usage during lecture! Rapid Red + Yellow blink
      digitalWrite(LED_COMPLIANT_PIN, LOW);
      digitalWrite(LED_VIOLATION_PIN, blinkState ? HIGH : LOW);
      digitalWrite(LED_HIGH_USAGE_PIN, blinkState ? HIGH : LOW);
      digitalWrite(ONBOARD_LED_PIN, blinkState ? LOW : HIGH);
    } else if (isMismatch) {
      // Discrepancy (e.g. phones > people, or extra devices) -> Red Blink
      digitalWrite(LED_COMPLIANT_PIN, LOW);
      digitalWrite(LED_VIOLATION_PIN, blinkState ? HIGH : LOW);
      digitalWrite(LED_HIGH_USAGE_PIN, LOW);
      digitalWrite(ONBOARD_LED_PIN, blinkState ? LOW : HIGH);
    } else {
      // In session and compliant
      digitalWrite(LED_COMPLIANT_PIN, HIGH);
      digitalWrite(LED_VIOLATION_PIN, LOW);
      digitalWrite(LED_HIGH_USAGE_PIN, LOW);
      digitalWrite(ONBOARD_LED_PIN, HIGH); // OFF
    }
  } else {
    // Off Hours / Recess: Green steady, high usage steady yellow if any
    digitalWrite(LED_COMPLIANT_PIN, HIGH);
    digitalWrite(LED_VIOLATION_PIN, LOW);
    digitalWrite(LED_HIGH_USAGE_PIN, highUsage ? HIGH : LOW);
    digitalWrite(ONBOARD_LED_PIN, HIGH);
  }
}

void updateOLED() {
  unsigned long now = millis();
  bool isLinkAlive = (now - lastPacketMs < 5000);

  display.clearDisplay();

  // Header Bar: Class Status & Window
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (inSession) {
    display.print("[CLASS IN SESSION]");
  } else {
    display.print("[RECESS / OFF-HR]");
  }

  display.setCursor(0, 11);
  display.print("Time: ");
  display.print(startTime);
  display.print("-");
  display.print(endTime);

  display.drawLine(0, 21, 127, 21, SSD1306_WHITE);

  // Big Metrics: People vs Phones
  display.setCursor(0, 25);
  display.print("PEOPLE: ");
  display.setTextSize(2);
  display.setCursor(48, 24);
  display.print(personCount);

  display.setTextSize(1);
  display.setCursor(76, 25);
  display.print("PHONES:");
  display.setTextSize(2);
  display.setCursor(118, 24);
  display.print(phoneCount);

  display.drawLine(0, 41, 127, 41, SSD1306_WHITE);

  // Footer: Mismatch & Usage Status
  display.setTextSize(1);
  display.setCursor(0, 45);

  if (!isLinkAlive) {
    display.print("STATUS: LINK WAITING..");
    display.setCursor(0, 55);
    display.print("Connect ESP32 Serial");
  } else {
    // Compare counts
    if (phoneCount > personCount) {
      display.print("ALERT: PHONE MISMATCH!");
      display.setCursor(0, 55);
      display.print("Phones > Persons (");
      display.print(phoneCount - personCount);
      display.print(" extra)");
    } else if (personCount > phoneCount) {
      display.print("STATUS: ");
      display.print(personCount - phoneCount);
      display.print(" Without Phone");
      display.setCursor(0, 55);
      if (highUsage) {
        display.print("WARN: HIGH PHONE USAGE");
      } else {
        display.print("Policy: COMPLIANT");
      }
    } else {
      display.print("STATUS: EXACT MATCH (");
      display.print(personCount);
      display.print(")");
      display.setCursor(0, 55);
      if (highUsage && inSession) {
        display.print("WARN: HIGH PHONE USAGE!");
      } else {
        display.print("Policy: COMPLIANT");
      }
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
