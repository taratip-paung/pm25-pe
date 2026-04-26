#include <Arduino.h>

// ปิดคำเตือนเรื่องทัช (บอร์ดนี้ไม่มีทัช)
#ifndef TOUCH_CS
#define TOUCH_CS -1
#endif

#include <TFT_eSPI.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "esp_adc_cal.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"

// ===================== Buttons =====================
#define BUTTON_1 35  // ปุ่มบน (GPIO35) [RTC IO, ไม่มี internal pull-up]
#define BUTTON_2 0   // ปุ่มล่าง (GPIO0)  [RTC IO, มี internal pull-up และเป็นปุ่ม BOOT]

// *** Deep sleep wake ***
#define WAKE_MASK  (1ULL << GPIO_NUM_0)   // ใช้ GPIO0 เป็น wake source
#define WAKE_MODE  ESP_EXT1_WAKEUP_ALL_LOW // ตื่นเมื่อกดปุ่ม (LOW)

// ===================== Battery ADC Configuration =====================
static const int   PIN_VBAT   = 34;     // จุดวัดแบตผ่านตัวแบ่งแรงดัน
static const float DIVIDER    = 2.0f;   // เช่น 100k/100k => x2
static const float VREF_mV    = 1100;   // สำหรับ esp_adc_cal
static const float CAL        = 0.99f;  // fine-tune จาก DMM

esp_adc_cal_characteristics_t adc_chars;
static constexpr adc_atten_t CAL_ATTEN = ADC_ATTEN_DB_12;

// ===================== BATTERY PROTECTION LEVELS (ปรับให้สูงขึ้น) =====================
#define VBAT_WARN     3.60f    // เตือนแบตต่ำ (เพิ่มจาก 3.50f)
#define VBAT_CUTOFF   3.40f    // เข้าโหมดประหยัด (เพิ่มจาก 3.30f)  
#define VBAT_CRITICAL 3.35f    // วิกฤต - deep sleep ทันที (ใหม่)
#define VBAT_RECOVER  3.70f    // กลับมาทำงานปกติ (เพิ่มจาก 3.60f)
#define VBAT_MINIMUM  3.30f    // ต่ำสุดที่ยอมให้ระบบทำงาน

// Timer wakeup interval (microseconds)
#define BATTERY_CHECK_INTERVAL_US  (30 * 60 * 1000000ULL)  // 30 นาที
#define ULTRA_LOW_POWER_INTERVAL_US (60 * 60 * 1000000ULL)  // 60 นาที

// ===================== WiFi =====================
const char* WIFI_SSID = "PM25";
const char* WIFI_PASSWORD = "00000000";

// ===================== ThingsBoard =====================
const char* TB_SERVER = "tb4.rtk-landmos.com";
const int   TB_PORT   = 443;  // HTTPS
const char* TB_TOKEN  = "LbFaT8SHI2qH9ux1E3R3";
const char* DEVICE_NAME = "PM25-PE-2";

// ===================== Time / NTP (UTC+7) =====================
const long GMT_OFFSET_SEC = 7 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;

// ===================== Telemetry Interval =====================
const unsigned long SEND_INTERVAL = 60000;  // ส่งทุก 60 วินาที

// ===================== Sensor Timing =====================
const unsigned long PMS_WARMUP_TIME = 30000;     // รอ 30 วินาทีหลังเปิดเซ็นเซอร์
const unsigned long AUTO_SLEEP_TIME = 120000;    // Auto sleep หลัง 2 นาที
const unsigned long PMS_READ_INTERVAL = 5000;    // อ่านทุก 5 วินาที (หลัง warmup)

// ===================== Display =====================
TFT_eSPI tft;
TFT_eSprite sprite = TFT_eSprite(&tft);
#ifndef TFT_BL
  #define TFT_BL 4
#endif

// ===================== PMS9103M Serial =====================
#define PMS_RX_PIN 13  // ESP32 RX ← PMS TXD
#define PMS_TX_PIN 17  // ESP32 TX → PMS RXD
#define PMS_SET_PIN 2  // Control sleep/wake of sensor
HardwareSerial pmsSerial(1);  // Use UART1

// ===================== System State =====================
bool systemActive = false;
bool displayOn = false;
volatile bool buttonPressed = false;
unsigned long lastButtonTime = 0;
const unsigned long DEBOUNCE_TIME = 500;
bool pmsConnected = false;
bool wifiConnected = false;

// Timing state variables
unsigned long systemWakeTime = 0;        // เวลาที่ระบบตื่น
unsigned long pmsWarmupStart = 0;        // เวลาที่เริ่ม warmup PMS
bool pmsWarmedUp = false;                // สถานะ warmup เสร็จหรือยัง
bool waitingForWarmup = false;           // กำลังรอ warmup
unsigned long lastPMSRead = 0;           // เวลาอ่าน PMS ล่าสุด

// Battery state flags
bool lowBatteryMode = false;
bool lowBatteryWarn = false;
bool usbMode = false;
bool criticalBattery = false;  // เพิ่มใหม่
static bool wasCharging = false;  // สำหรับ hysteresis

// ===================== Battery Data =====================
float batteryVoltage = 0.0f;
int   batteryPercent = 0;

// ===================== PMS Buffer/Data =====================
struct PMS_DATA {
  uint16_t pm1_0_cf1;
  uint16_t pm2_5_cf1;
  uint16_t pm10_cf1;
  uint16_t pm1_0_atm;
  uint16_t pm2_5_atm;
  uint16_t pm10_atm;
  uint16_t particles_03;
  uint16_t particles_05;
  uint16_t particles_10;
  uint16_t particles_25;
  uint16_t particles_50;
  uint16_t particles_100;
};

PMS_DATA pmsData;
uint8_t pmsBuffer[32];
int bufferIndex = 0;
bool dataReady = false;
unsigned long lastUpdate = 0;
unsigned long lastRead = 0;
unsigned long lastTelemetry = 0;

String lastSuccessSend = String("--");

// ===================== Forward Declarations =====================
void connectWiFi();
void enterSleepMode();
void wakeFromSleep();
void syncTime();
String getShortTimestamp();
bool checkPMSSensor();
void readBattery();
void evaluateBatteryProtection();
void forceLowBatterySafeState();
void maybeExitLowBatterySafeState();
void readPMSSensor();
bool processPMSData();
String getAirQuality(uint16_t pm25);
uint16_t getColorForPM25(uint16_t pm25);
uint16_t getBatteryColor(int percent);
void updateDisplay();
void drawLowBatteryScreen();
void handleButtonPress();
void sendToThingsBoard();
void enterDeepSleep(bool ultra_low_power = false);
void enterUltraLowPowerMode();
void shutdownAllPeripherals();
void drawCriticalBatteryScreen();

// ===================== Battery Reading Functions =====================
static uint32_t readRawAvg(int n = 32) {
  uint32_t acc = 0;
  for (int i = 0; i < n; ++i) {
    acc += analogRead(PIN_VBAT);
    delayMicroseconds(800);
  }
  return acc / n;
}

float readVBat() {
  uint32_t raw = readRawAvg(32);
  uint32_t mv  = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
  float vbat = (mv / 1000.0f) * DIVIDER;
  return vbat * CAL;
}

// USB detection with hysteresis
bool usbPluggedHeuristic(float vbat_read) {
  // ใช้ hysteresis เพื่อป้องกัน flapping
  if (wasCharging) {
    // ถ้าเคยชาร์จ ให้ถือว่ายังชาร์จอยู่จนกว่าแรงดันจะต่ำกว่า 4.10V
    wasCharging = vbat_read > 4.10f;
  } else {
    // ถ้าไม่ได้ชาร์จ ต้องเห็นแรงดันสูงกว่า 4.25V ถึงจะถือว่าเริ่มชาร์จ
    wasCharging = vbat_read > 4.25f;
  }
  return wasCharging;
}

// ===================== SOC mapping =====================
struct SocPt { float v; int pct; };
static const SocPt SOC_OCV[] = {
  {4.20f,100},
  {4.10f,95},
  {4.00f,85},
  {3.90f,70},
  {3.80f,55},
  {3.70f,40},
  {3.60f,25},
  {3.50f,15},
  {3.40f,10},
  {3.35f,5},   // ปรับใหม่
  {3.30f,2},   // ปรับใหม่
  {3.20f,0}
};

static inline int clampPct(int x){ return (x<0)?0:((x>100)?100:x); }

int socFromVoltage(float v) {
  if (v >= SOC_OCV[0].v)   return 100;
  if (v <= SOC_OCV[11].v)  return 0;
  for (size_t i = 0; i < 11; ++i) {
    const float v_hi = SOC_OCV[i].v;
    const float v_lo = SOC_OCV[i+1].v;
    if (v <= v_hi && v >= v_lo) {
      const int   p_hi = SOC_OCV[i].pct;
      const int   p_lo = SOC_OCV[i+1].pct;
      const float t = (v - v_lo) / (v_hi - v_lo);
      const int   p = (int)(p_lo + t * (p_hi - p_lo) + 0.5f);
      return clampPct(p);
    }
  }
  return 0;
}

int smoothSOC(int soc_new) {
  static int soc_s = -1;
  if (soc_s < 0) soc_s = soc_new;
  soc_s = (int)(0.7f * soc_s + 0.3f * soc_new);
  return soc_s;
}

// ===================== Shutdown All Peripherals =====================
void shutdownAllPeripherals() {
  Serial.println("DEBUG: Shutting down all peripherals...");
  
  // ปิดจอและ backlight
  digitalWrite(TFT_BL, LOW);
  tft.writecommand(0x10);  // Sleep command for display
  
  // ปิดเซ็นเซอร์ PMS
  digitalWrite(PMS_SET_PIN, LOW);
  
  // ปิด Serial ports
  pmsSerial.end();
  
  // ปิด WiFi และ Bluetooth อย่างสมบูรณ์
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  
  // ปิด Bluetooth
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
  
  // ปิด ADC power (commented out deprecated function)
  // adc_power_off() is deprecated in newer ESP-IDF
  // For ESP32, we can use adc_power_release() or just leave ADC in low power
  // adc_power_release();  // This also might not be available in all versions
  
  // Alternative: Just de-init the ADC pins to save power
  analogRead(PIN_VBAT);  // One last read to ensure ADC is initialized
  pinMode(PIN_VBAT, INPUT);  // Set to high impedance input
  
  // ตั้งค่า GPIO ที่ไม่ใช้เป็น INPUT เพื่อประหยัดพลังงาน
  for (int i = 0; i < 34; i++) {
    if (i != BUTTON_2 && i != 6 && i != 7 && i != 8 && i != 11) {
      pinMode(i, INPUT);
    }
  }
  
  Serial.println("DEBUG: All peripherals shutdown complete");
}

// ===================== Enhanced Deep Sleep Functions =====================
void configureDeepSleepWake(bool include_timer = false, uint64_t timer_us = BATTERY_CHECK_INTERVAL_US) {
  // Clear all wakeup sources first
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  
  // Configure button wake (GPIO0)
  pinMode(BUTTON_2, INPUT_PULLUP);
  esp_sleep_enable_ext1_wakeup(WAKE_MASK, WAKE_MODE);
  
  // Optionally add timer wakeup
  if (include_timer) {
    esp_sleep_enable_timer_wakeup(timer_us);
    Serial.printf("DEBUG: Timer wakeup set for %llu seconds\n", timer_us / 1000000ULL);
  }
  
  // Configure power domains
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);  // Keep for RTC
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
}

void enterDeepSleep(bool ultra_low_power) {
  shutdownAllPeripherals();
  
  if (ultra_low_power) {
    Serial.println("DEBUG: Entering ULTRA LOW POWER deep sleep");
    configureDeepSleepWake(true, ULTRA_LOW_POWER_INTERVAL_US);
  } else {
    Serial.println("DEBUG: Entering normal deep sleep with battery monitoring");
    configureDeepSleepWake(true, BATTERY_CHECK_INTERVAL_US);
  }
  
  Serial.printf("DEBUG: Battery voltage before sleep: %.2fV\n", batteryVoltage);
  Serial.flush();
  delay(100);
  
  esp_deep_sleep_start();
}

void enterUltraLowPowerMode() {
  Serial.println("DEBUG: CRITICAL BATTERY - Entering ultra low power mode!");
  
  // แสดงข้อความเตือนสั้นๆ
  drawCriticalBatteryScreen();
  delay(2000);
  
  // เข้า deep sleep แบบ ultra low power
  enterDeepSleep(true);
}

// ===================== Battery Protection Functions =====================
void readBattery() {
  float v = readVBat();
  batteryVoltage = v;
  usbMode = usbPluggedHeuristic(v);

  if (usbMode) {
    int est = socFromVoltage(min(v, 4.20f));
    batteryPercent = smoothSOC(est);
    lowBatteryWarn = false;
    lowBatteryMode = false;
    criticalBattery = false;
  } else {
    int soc = socFromVoltage(v);
    batteryPercent = smoothSOC(soc);
    evaluateBatteryProtection();
  }

  Serial.printf("DEBUG: VBAT=%.2fV, SOC=%d%%, usb=%d, warn=%d, lowMode=%d, critical=%d\n",
                batteryVoltage, batteryPercent, usbMode, lowBatteryWarn, lowBatteryMode, criticalBattery);
}

void evaluateBatteryProtection() {
  if (usbMode) {
    lowBatteryWarn = false;
    lowBatteryMode = false;
    criticalBattery = false;
    return;
  }
  
  // CRITICAL: ต่ำกว่า VBAT_CRITICAL = ultra low power mode ทันที
  if (batteryVoltage <= VBAT_CRITICAL) {
    criticalBattery = true;
    Serial.println("CRITICAL: Battery voltage critical! Immediate shutdown required!");
    enterUltraLowPowerMode();
    return;  // ไม่ควรมาถึงบรรทัดนี้
  }
  
  // ตรวจสอบระดับต่างๆ
  lowBatteryWarn = (batteryVoltage <= VBAT_WARN);
  
  if (batteryVoltage <= VBAT_CUTOFF) {
    if (!lowBatteryMode) {
      lowBatteryMode = true;
      forceLowBatterySafeState();
    }
  } else {
    if (lowBatteryMode && batteryVoltage >= VBAT_RECOVER) {
      lowBatteryMode = false;
      criticalBattery = false;
    }
  }
}

void forceLowBatterySafeState() {
  digitalWrite(PMS_SET_PIN, LOW);
  dataReady = false;
  bufferIndex = 0;
  
  if (wifiConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
  }
  
  Serial.println("DEBUG: Enter LOW BATTERY SAFE STATE");
  
  // แสดงหน้าจอเตือน
  drawLowBatteryScreen();
  sprite.pushSprite(0, 0);
  delay(2000);
  
  // เข้า deep sleep พร้อม timer เพื่อเช็คแบตเป็นระยะ
  enterDeepSleep(false);
}

void maybeExitLowBatterySafeState() {
  if (!lowBatteryMode && batteryVoltage >= VBAT_RECOVER) {
    Serial.println("DEBUG: Battery recovered, exiting safe state");
    criticalBattery = false;
  }
}

// ===================== Display Functions =====================
void drawCriticalBatteryScreen() {
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  
  // Critical warning
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.setTextFont(4);
  sprite.drawString("CRITICAL!", 120, 30);
  
  // Voltage display
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(4);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "%.2fV", batteryVoltage);
  sprite.drawString(vbuf, 120, 60);
  
  // Message
  sprite.setTextFont(2);
  sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
  sprite.drawString("Deep Sleep Mode", 120, 90);
  sprite.drawString("Charge immediately!", 120, 110);
  
  sprite.pushSprite(0, 0);
}

void drawLowBatteryScreen() {
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.setTextFont(4);
  sprite.drawString("LOW BATTERY", 120, 40);
  
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(2);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "VBAT: %.2fV  (%d%%)", batteryVoltage, batteryPercent);
  sprite.drawString(vbuf, 120, 70);
  
  sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
  sprite.drawString("Charging required", 120, 95);
  
  sprite.setTextDatum(TR_DATUM);
  sprite.setTextFont(1);
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.drawString("WiFi OFF", 235, 5);
  
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString(DEVICE_NAME, 5, 5);
  
  sprite.setTextDatum(BL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString("SAFE MODE", 5, 134);
}

// ===================== Time Functions =====================
void syncTime() {
  Serial.println("DEBUG: Syncing time via NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1700000000 && retries < 20) {
    delay(250);
    now = time(nullptr);
    retries++;
  }
  
  if (now >= 1700000000) {
    Serial.println("DEBUG: Time sync OK");
  } else {
    Serial.println("WARNING: Time sync failed");
  }
}

String getShortTimestamp() {
  time_t now = time(nullptr);
  if (now < 1700000000) return String("--");
  
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d/%02d %02d:%02d",
           timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

// ===================== Button ISR =====================
void IRAM_ATTR buttonISR() {
  buttonPressed = true;
}

// ===================== PMS Sensor Functions =====================
bool checkPMSSensor() {
  Serial.println("DEBUG: Checking for PMS9103M sensor...");
  
  digitalWrite(PMS_SET_PIN, HIGH);
  delay(2000);
  
  while (pmsSerial.available()) pmsSerial.read();
  
  unsigned long checkStart = millis();
  int bytesReceived = 0;
  
  while (millis() - checkStart < 3000) {
    if (pmsSerial.available()) {
      uint8_t byteIn = pmsSerial.read();
      bytesReceived++;
      if (bytesReceived > 1 && pmsBuffer[0] == 0x42 && byteIn == 0x4D) {
        while (pmsSerial.available()) pmsSerial.read();
        Serial.println("DEBUG: PMS9103M sensor detected!");
        return true;
      }
      pmsBuffer[0] = byteIn;
    }
    delay(10);
  }
  
  Serial.println("WARNING: PMS9103M sensor NOT detected");
  digitalWrite(PMS_SET_PIN, LOW);
  return false;
}

bool processPMSData() {
  if (pmsBuffer[0] != 0x42 || pmsBuffer[1] != 0x4D) {
    Serial.println("DEBUG: Invalid header");
    return false;
  }
  
  uint16_t checksum = 0;
  for (int i = 0; i < 30; i++) checksum += pmsBuffer[i];
  uint16_t bufferChecksum = (pmsBuffer[30] << 8) | pmsBuffer[31];
  
  if (checksum != bufferChecksum) {
    Serial.printf("DEBUG: Checksum error\n");
    return false;
  }
  
  pmsData.pm1_0_cf1 = (pmsBuffer[4] << 8) | pmsBuffer[5];
  pmsData.pm2_5_cf1 = (pmsBuffer[6] << 8) | pmsBuffer[7];
  pmsData.pm10_cf1  = (pmsBuffer[8] << 8) | pmsBuffer[9];
  pmsData.pm1_0_atm = (pmsBuffer[10] << 8) | pmsBuffer[11];
  pmsData.pm2_5_atm = (pmsBuffer[12] << 8) | pmsBuffer[13];
  pmsData.pm10_atm  = (pmsBuffer[14] << 8) | pmsBuffer[15];
  
  Serial.println("\n=== PMS9103M Data ===");
  Serial.printf("PM1.0: %d µg/m³\n", pmsData.pm1_0_atm);
  Serial.printf("PM2.5: %d µg/m³\n", pmsData.pm2_5_atm);
  Serial.printf("PM10:  %d µg/m³\n", pmsData.pm10_atm);
  Serial.println("=====================");
  
  return true;
}

void readPMSSensor() {
  if (!systemActive || !pmsConnected || lowBatteryMode) return;
  
  // ถ้ายังอยู่ในช่วง warmup ให้อ่านแต่ไม่ใช้ค่า
  if (waitingForWarmup && !pmsWarmedUp) {
    // Clear buffer but don't process
    while (pmsSerial.available()) {
      pmsSerial.read();
    }
    return;
  }
  
  // ตรวจสอบ interval การอ่าน (อ่านทุก 5 วินาทีหลัง warmup)
  if (pmsWarmedUp && (millis() - lastPMSRead < PMS_READ_INTERVAL)) {
    return;
  }
  
  while (pmsSerial.available()) {
    uint8_t byteIn = pmsSerial.read();
    
    if (bufferIndex == 0 && byteIn != 0x42) continue;
    if (bufferIndex == 1 && byteIn != 0x4D) {
      bufferIndex = 0;
      continue;
    }
    
    pmsBuffer[bufferIndex] = byteIn;
    bufferIndex++;
    
    if (bufferIndex >= 32) {
      Serial.println("DEBUG: Frame received");
      if (processPMSData()) {
        dataReady = true;
        lastRead = millis();
        lastPMSRead = millis();
      }
      bufferIndex = 0;
    }
  }
}

// ===================== WiFi Functions =====================
void connectWiFi() {
  if (!systemActive || lowBatteryMode) return;
  
  Serial.println("\nDEBUG: Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Connecting WiFi...", 120, 67);
  sprite.pushSprite(0, 0);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nDEBUG: WiFi connected!");
    Serial.print("DEBUG: IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    Serial.println("\nWARNING: Failed to connect to WiFi");
  }
}

// ===================== Sleep/Wake Functions =====================
void enterSleepMode() {
  if (!systemActive) return;
  
  Serial.println("\n=== ENTERING SLEEP MODE ===");
  systemActive = false;
  displayOn = false;
  
  // Reset warmup flags
  pmsWarmedUp = false;
  waitingForWarmup = false;
  pmsWarmupStart = 0;
  systemWakeTime = 0;
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_BLUE, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Sleep Mode", 120, 67);
  sprite.pushSprite(0, 0);
  delay(400);
  
  digitalWrite(TFT_BL, LOW);
  tft.fillScreen(TFT_BLACK);
  
  digitalWrite(PMS_SET_PIN, LOW);
  Serial.println("DEBUG: PMS sensor set to sleep mode");
  
  if (wifiConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
    Serial.println("DEBUG: WiFi disconnected");
  }
  
  while (pmsSerial.available()) pmsSerial.read();
  
  dataReady = false;
  bufferIndex = 0;
  
  Serial.println("DEBUG: System in sleep mode");
  Serial.println("===========================\n");
}

void wakeFromSleep() {
  if (systemActive) return;
  
  Serial.println("\n=== WAKING FROM SLEEP ===");
  systemActive = true;
  displayOn = true;
  systemWakeTime = millis();  // บันทึกเวลาที่ตื่น
  
  digitalWrite(TFT_BL, HIGH);
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_CYAN, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Waking up...", 120, 67);
  sprite.pushSprite(0, 0);
  
  readBattery();
  
  // ตรวจสอบแบตเตอรี่ก่อนเริ่มทำงาน
  if (batteryVoltage <= VBAT_CRITICAL && !usbMode) {
    enterUltraLowPowerMode();
    return;
  }
  
  if (lowBatteryMode && !usbMode) {
    forceLowBatterySafeState();
    return;
  }
  
  // เปิดเซ็นเซอร์ PMS และเริ่ม warmup
  if (pmsConnected) {
    digitalWrite(PMS_SET_PIN, HIGH);
    Serial.println("DEBUG: PMS sensor waking up - starting 30s warmup period");
    pmsWarmupStart = millis();
    pmsWarmedUp = false;
    waitingForWarmup = true;
    
    // แสดงสถานะ warmup
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("Sensor Warming Up", 120, 50);
    sprite.drawString("Please wait 30s...", 120, 80);
    sprite.pushSprite(0, 0);
  }
  
  connectWiFi();
  if (wifiConnected) {
    syncTime();
  }
  
  dataReady = false;
  bufferIndex = 0;
  
  Serial.println("DEBUG: System active");
  Serial.println("=========================\n");
}

void handleButtonPress() {
  if (!buttonPressed) return;
  
  unsigned long now = millis();
  if (now - lastButtonTime > DEBOUNCE_TIME) {
    Serial.println("DEBUG: Button pressed - toggling sleep mode");
    if (systemActive) {
      enterSleepMode();
    } else {
      wakeFromSleep();
    }
    lastButtonTime = now;
  }
  buttonPressed = false;
}

// ===================== ThingsBoard Functions =====================
void sendToThingsBoard() {
  if (!wifiConnected || !systemActive || lowBatteryMode) return;
  
  Serial.println("\nDEBUG: Sending data to ThingsBoard...");
  
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  
  String url = String("https://") + TB_SERVER + "/api/v1/" + TB_TOKEN + "/telemetry";
  
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    
    StaticJsonDocument<512> doc;
    
    if (pmsConnected && dataReady) {
      doc["pm1_0"] = pmsData.pm1_0_atm;
      doc["pm2_5"] = pmsData.pm2_5_atm;
      doc["pm10"]  = pmsData.pm10_atm;
    }
    
    doc["battery_voltage"] = batteryVoltage;
    doc["battery_percent"] = batteryPercent;
    doc["battery_warn"]    = lowBatteryWarn;
    doc["battery_lowmode"] = lowBatteryMode;
    doc["battery_critical"] = criticalBattery;
    doc["charging"]        = usbMode;
    doc["device"]          = DEVICE_NAME;
    doc["rssi"]            = WiFi.RSSI();
    doc["active"]          = systemActive;
    doc["pms_connected"]   = pmsConnected && !lowBatteryMode;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    Serial.print("DEBUG: Sending JSON: ");
    Serial.println(jsonString);
    
    int httpCode = https.POST(jsonString);
    
    if (httpCode > 0) {
      Serial.printf("DEBUG: HTTP Response code: %d\n", httpCode);
      if (httpCode == HTTP_CODE_OK || httpCode == 200) {
        Serial.println("DEBUG: Data sent successfully!");
        lastSuccessSend = getShortTimestamp();
      }
    } else {
      Serial.printf("ERROR: HTTP request failed: %s\n", https.errorToString(httpCode).c_str());
    }
    
    https.end();
  }
}

// ===================== Air Quality Helper Functions =====================
String getAirQuality(uint16_t pm25) {
  if (pm25 <= 12)        return "Good";
  else if (pm25 <= 35)   return "Moderate";
  else if (pm25 <= 55)   return "Unhealthy-S";
  else if (pm25 <= 150)  return "Unhealthy";
  else if (pm25 <= 250)  return "V.Unhealthy";
  else                   return "Hazardous";
}

uint16_t getColorForPM25(uint16_t pm25) {
  if (pm25 <= 12)        return TFT_GREEN;
  else if (pm25 <= 35)   return TFT_YELLOW;
  else if (pm25 <= 55)   return TFT_ORANGE;
  else if (pm25 <= 150)  return TFT_RED;
  else if (pm25 <= 250)  return TFT_PURPLE;
  else                   return TFT_MAROON;
}

uint16_t getBatteryColor(int percent) {
  if (percent > 50)      return TFT_GREEN;
  else if (percent > 25) return TFT_YELLOW;
  else if (percent > 10) return TFT_ORANGE;
  else                   return TFT_RED;
}

// ===================== Main Display Update =====================
void updateDisplay() {
  if (!displayOn || !systemActive) return;
  
  if (lowBatteryMode) {
    drawLowBatteryScreen();
    sprite.pushSprite(0, 0);
    return;
  }
  
  sprite.fillSprite(TFT_BLACK);
  
  // ถ้าไม่มีเซ็นเซอร์ PMS
  if (!pmsConnected) {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_ORANGE, TFT_BLACK);
    sprite.setTextFont(4);
    sprite.drawString("NO PMS", 120, 50);
    sprite.setTextFont(2);
    sprite.drawString("Sensor Not Connected", 120, 85);
  } 
  // ถ้ากำลัง warmup
  else if (waitingForWarmup && !pmsWarmedUp) {
    unsigned long elapsed = (millis() - pmsWarmupStart) / 1000;
    unsigned long remaining = (PMS_WARMUP_TIME / 1000) - elapsed;
    
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.setTextFont(4);
    sprite.drawString("WARMING UP", 120, 40);
    
    sprite.setTextFont(6);
    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    char countdown[10];
    snprintf(countdown, sizeof(countdown), "%lu", remaining);
    sprite.drawString(countdown, 120, 70);
    
    sprite.setTextFont(2);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("seconds remaining", 120, 105);
  }
  // แสดงค่า PM2.5 ปกติ
  else if (dataReady) {
    uint16_t color = getColorForPM25(pmsData.pm2_5_atm);
    
    sprite.setTextDatum(ML_DATUM);
    sprite.setTextColor(color, TFT_BLACK);
    String pm25str = String(pmsData.pm2_5_atm);
    if (pm25str.length() <= 2)      sprite.setTextFont(8);
    else if (pm25str.length() == 3) sprite.setTextFont(7);
    else                            sprite.setTextFont(6);
    sprite.drawString(pm25str, 10, 67);
    
    sprite.setTextDatum(MR_DATUM);
    sprite.setTextFont(4);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("ug/m3", 230, 50);
    
    sprite.setTextFont(2);
    sprite.setTextColor(color, TFT_BLACK);
    sprite.drawString(getAirQuality(pmsData.pm2_5_atm), 230, 85);
    
    sprite.setTextFont(1);
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.drawString(lastSuccessSend, 230, 105);
  }
  // รอข้อมูล
  else {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("Waiting for data...", 120, 67);
  }
  
  // Status indicators (ทุกกรณี)
  sprite.setTextDatum(TR_DATUM);
  sprite.setTextFont(1);
  sprite.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, TFT_BLACK);
  sprite.drawString(wifiConnected ? "WiFi OK" : "No WiFi", 235, 5);
  
  sprite.setTextColor(getBatteryColor(batteryPercent), TFT_BLACK);
  char battStr[32];
  snprintf(battStr, sizeof(battStr), "Batt: %d%% %.2fV%s",
           batteryPercent, batteryVoltage, usbMode ? " CHG" : "");
  sprite.drawString(battStr, 235, 15);
  
  if (lowBatteryWarn) {
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.drawString("Battery Low!", 235, 25);
  } else {
    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    sprite.drawString("Press to Sleep", 235, 25);
  }
  
  // Auto-sleep countdown (ถ้าใกล้จะ sleep)
  if (systemWakeTime > 0) {
    unsigned long awakeTime = millis() - systemWakeTime;
    if (awakeTime > (AUTO_SLEEP_TIME - 10000)) {  // แสดง 10 วินาทีสุดท้าย
      unsigned long remainingSec = (AUTO_SLEEP_TIME - awakeTime) / 1000;
      sprite.setTextDatum(BC_DATUM);
      sprite.setTextFont(1);
      sprite.setTextColor(TFT_ORANGE, TFT_BLACK);
      char sleepMsg[32];
      snprintf(sleepMsg, sizeof(sleepMsg), "Auto sleep in %lus", remainingSec);
      sprite.drawString(sleepMsg, 120, 134);
    }
  }
  
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString(DEVICE_NAME, 5, 5);
  
  if (!wifiConnected && pmsWarmedUp) {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_RED, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("OFFLINE MODE", 120, 110);
  }
  
  sprite.pushSprite(0, 0);
  Serial.println("DEBUG: Display updated");
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== PMS9103M + Enhanced Battery Protection System ===");
  Serial.printf("Battery thresholds: WARN=%.2fV, CUTOFF=%.2fV, CRITICAL=%.2fV, RECOVER=%.2fV\n",
                VBAT_WARN, VBAT_CUTOFF, VBAT_CRITICAL, VBAT_RECOVER);
  
  // Check wakeup reason first
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  // ADC init
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);
  esp_adc_cal_characterize(ADC_UNIT_1, CAL_ATTEN, ADC_WIDTH_BIT_12, VREF_mV, &adc_chars);
  
  // Read battery immediately
  readBattery();
  Serial.printf("DEBUG: Initial battery reading: %.2fV (%d%%)\n", batteryVoltage, batteryPercent);
  
  // Initialize display first for messages
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  sprite.createSprite(240, 135);
  sprite.fillSprite(TFT_BLACK);
  
  // Handle different wakeup scenarios
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("DEBUG: Wakeup by timer - checking battery");
      
      // If still low battery and not charging, go back to sleep
      if (!usbMode && batteryVoltage < VBAT_RECOVER) {
        Serial.printf("DEBUG: Battery still low: %.2fV < %.2fV\n", batteryVoltage, VBAT_RECOVER);
        
        // Show brief status
        digitalWrite(TFT_BL, HIGH);
        sprite.fillSprite(TFT_BLACK);
        sprite.setTextDatum(MC_DATUM);
        sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        sprite.setTextFont(2);
        sprite.drawString("Battery Check", 120, 40);
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%.2fV (%d%%)", batteryVoltage, batteryPercent);
        sprite.drawString(vbuf, 120, 67);
        sprite.drawString("Still too low", 120, 94);
        sprite.pushSprite(0, 0);
        delay(1000);
        digitalWrite(TFT_BL, LOW);
        
        // Go back to deep sleep
        if (batteryVoltage <= VBAT_CRITICAL) {
          enterDeepSleep(true);  // Ultra low power
        } else {
          enterDeepSleep(false); // Normal battery check interval
        }
        return;  // Should not reach here
      }
      
      // Battery recovered or charging - continue boot
      Serial.println("DEBUG: Battery OK or charging - continuing boot");
      break;
      
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("DEBUG: Wakeup by button press");
      
      // Check if battery is too low to operate
      if (!usbMode && batteryVoltage < VBAT_CUTOFF) {
        Serial.printf("DEBUG: Battery too low for operation: %.2fV\n", batteryVoltage);
        
        // Show warning
        digitalWrite(TFT_BL, HIGH);
        drawCriticalBatteryScreen();
        delay(3000);
        digitalWrite(TFT_BL, LOW);
        
        // Go back to sleep
        if (batteryVoltage <= VBAT_CRITICAL) {
          enterDeepSleep(true);
        } else {
          enterDeepSleep(false);
        }
        return;
      }
      break;
      
    default:
      Serial.println("DEBUG: Normal boot or undefined wakeup");
      
      // Check battery on first boot
      if (!usbMode && batteryVoltage <= VBAT_CRITICAL) {
        Serial.println("DEBUG: Initial boot with critical battery!");
        enterUltraLowPowerMode();
        return;
      }
      break;
  }
  
  // If we get here, battery is OK to proceed
  
  // Buttons setup
  pinMode(BUTTON_1, INPUT_PULLUP);
  pinMode(BUTTON_2, INPUT_PULLUP);
  
  // PMS setup
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  pinMode(PMS_SET_PIN, OUTPUT);
  digitalWrite(PMS_SET_PIN, LOW);
  
  // Check PMS sensor if battery is OK
  if (!lowBatteryMode) {
    pmsConnected = checkPMSSensor();
    if (pmsConnected) {
      digitalWrite(PMS_SET_PIN, LOW);  // Sleep initially
    }
  } else {
    pmsConnected = false;
  }
  
  // Setup interrupts for normal operation
  attachInterrupt(digitalPinToInterrupt(BUTTON_1), buttonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_2), buttonISR, FALLING);
  
  Serial.println("DEBUG: Starting in SLEEP MODE");
  Serial.println("DEBUG: Press button to wake up");
  Serial.printf("DEBUG: Data send interval: %lu seconds\n", SEND_INTERVAL/1000);
  Serial.println("========================\n");
  
  systemActive = false;
  displayOn = false;
}

// ===================== Main Loop =====================
void loop() {
  handleButtonPress();
  
  // Battery monitoring every 5 seconds
  static unsigned long lastBattRefresh = 0;
  if (millis() - lastBattRefresh >= 5000) {
    readBattery();
    
    // Check for critical battery during operation
    if (!usbMode && batteryVoltage <= VBAT_CRITICAL) {
      Serial.println("CRITICAL: Battery dropped below critical level during operation!");
      enterUltraLowPowerMode();
      return;
    }
    
    // Check for low battery mode transitions
    if (!lowBatteryMode && batteryVoltage <= VBAT_CUTOFF && !usbMode) {
      forceLowBatterySafeState();
      return;
    }
    
    // Check for recovery
    if (lowBatteryMode && (batteryVoltage >= VBAT_RECOVER || usbMode)) {
      maybeExitLowBatterySafeState();
    }
    
    lastBattRefresh = millis();
  }
  
  if (systemActive) {
    // Check for auto-sleep after 2 minutes
    if (systemWakeTime > 0 && (millis() - systemWakeTime >= AUTO_SLEEP_TIME)) {
      Serial.println("DEBUG: Auto-sleep after 2 minutes of activity");
      enterSleepMode();
      return;
    }
    
    // Check PMS warmup status
    if (waitingForWarmup && !pmsWarmedUp) {
      if (millis() - pmsWarmupStart >= PMS_WARMUP_TIME) {
        pmsWarmedUp = true;
        waitingForWarmup = false;
        Serial.println("DEBUG: PMS warmup complete - starting normal operation");
        
        // Clear any buffered data during warmup
        while (pmsSerial.available()) {
          pmsSerial.read();
        }
        bufferIndex = 0;
        dataReady = false;
      }
    }
    
    // Don't operate in low battery mode
    if (lowBatteryMode) {
      if (millis() - lastUpdate >= 3000) {
        updateDisplay();
        lastUpdate = millis();
      }
      delay(10);
      return;
    }
    
    // Normal operation - read PMS if warmed up
    if (pmsConnected) {
      readPMSSensor();
    }
    
    // Update display more frequently during warmup (every second)
    // Normal update every 5 seconds after warmup
    unsigned long displayInterval = (waitingForWarmup && !pmsWarmedUp) ? 1000 : 5000;
    if (millis() - lastUpdate >= displayInterval) {
      updateDisplay();
      lastUpdate = millis();
    }
    
    // Send telemetry (only if warmed up and have data)
    if (pmsWarmedUp && millis() - lastTelemetry >= SEND_INTERVAL) {
      sendToThingsBoard();
      lastTelemetry = millis();
    }
  }
  
  delay(10);
}