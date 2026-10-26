// ESP32-CAM sensor probe (AI-Thinker pinout).
// Purpose: find out what is actually connected on the camera bus.
// Works on arduino-esp32 2.x and 3.x. Open Serial Monitor at 115200.

#include <Wire.h>
#include "driver/ledc.h"

#define PWDN_PIN 32
#define XCLK_PIN 0
#define SDA_PIN  26
#define SCL_PIN  27

static void startXclk(uint32_t hz) {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_1_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = hz;
  t.clk_cfg = LEDC_AUTO_CLK;
  ledc_timer_config(&t);

  ledc_channel_config_t c = {};
  c.gpio_num = XCLK_PIN;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 1;
  c.hpoint = 0;
  ledc_channel_config(&c);
}

static int rd8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

static int rd16(uint8_t addr, uint16_t reg) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission() != 0) return -1;
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

static void wr8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void probe(uint32_t xclkHz) {
  Serial.printf("\n--- Probe with XCLK = %u MHz ---\n", (unsigned)(xclkHz / 1000000));

  // Power-cycle the sensor
  pinMode(PWDN_PIN, OUTPUT);
  digitalWrite(PWDN_PIN, HIGH);
  delay(100);
  startXclk(xclkHz);
  delay(10);
  digitalWrite(PWDN_PIN, LOW);
  delay(100);

  Wire.end();
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      Serial.printf("Device found at 7-bit address 0x%02X\n", a);

      // OV2640 style: bank select 0xFF=1, then PID at 0x0A/0x0B
      wr8(a, 0xFF, 0x01);
      int p1 = rd8(a, 0x0A), p2 = rd8(a, 0x0B);
      Serial.printf("  OV2640-style  (bank1 0x0A/0x0B): %02X %02X\n", p1 & 0xFF, p2 & 0xFF);

      // OV3660 / OV5640 style: 16-bit register 0x300A/0x300B
      int h = rd16(a, 0x300A), l = rd16(a, 0x300B);
      Serial.printf("  OV3660/5640   (0x300A/0x300B):   %02X %02X\n", h & 0xFF, l & 0xFF);

      // GC / BF / SC style: 8-bit 0xF0/0xF1, plus 0x0A/0x0B on some sensors
      int g1 = rd8(a, 0xF0), g2 = rd8(a, 0xF1);
      Serial.printf("  GC-style      (0xF0/0xF1):       %02X %02X\n", g1 & 0xFF, g2 & 0xFF);
    }
  }
  if (!found) Serial.println("NO device answered on SDA=26 / SCL=27.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-CAM sensor probe ===");
  Serial.printf("Chip: %s rev %d, %d MHz, flash %u MB\n",
                ESP.getChipModel(), (int)ESP.getChipRevision(),
                (int)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() / 1048576));
  Serial.printf("PSRAM: %s (%u bytes)\n", psramFound() ? "found" : "NOT found", (unsigned)ESP.getPsramSize());
}

void loop() {
  probe(20000000);
  probe(10000000);
  Serial.println("\n(repeating in 8 s - copy everything above and send it to me)");
  delay(8000);
}
