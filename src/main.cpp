#include <Arduino.h>

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
#define BUTTON_1 14  // T-Display S3 onboard button (GPIO14)
#define BUTTON_2 0   // BOOT button (GPIO0)

// *** Deep sleep wake ***
#define WAKE_MASK  (1ULL << GPIO_NUM_0)
#if CONFIG_IDF_TARGET_ESP32
#define WAKE_MODE  ESP_EXT1_WAKEUP_ALL_LOW
#elif CONFIG_IDF_TARGET_ESP32S3
#define WAKE_MODE  ESP_EXT1_WAKEUP_ANY_LOW
#endif

// ===================== Battery ADC Configuration =====================
static const int   PIN_VBAT   = 4;      // T-Display S3 battery voltage pin (GPIO4)
#define DISPLAY_POWER_PIN 15             // Must be HIGH to enable display on battery
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
const char* WIFI_SSID = "KHomeSmart-IOT";
const char* WIFI_PASSWORD = "nongnoom";

// ===================== GIST North API =====================
const char* GIST_SERVER = "app.gistnorth.soc.cmu.ac.th";
const char* GIST_TOKEN  = "dust_c9230016d168863c7fe1c479ef8e09889c935e923c91e759";
const char* GIST_PATH   = "/iot/api/ingest";
const char* DEVICE_NAME = "PM25-PE-2";

// ===================== Time / NTP (UTC+7) =====================
const long GMT_OFFSET_SEC = 7 * 3600;
const int  DAYLIGHT_OFFSET_SEC = 0;

// ===================== Telemetry Interval =====================
const unsigned long SEND_INTERVAL = 3000;

// ===================== Sensor Timing =====================
const unsigned long PMS_WARMUP_TIME = 30000;     // รอ 30 วินาทีหลังเปิดเซ็นเซอร์
const unsigned long AUTO_SLEEP_TIME = 60000;      // Auto sleep หลัง 1 นาที (60 วินาที)
const unsigned long PMS_READ_INTERVAL = 3000;     // อ่านทุก 3 วินาที (หลัง warmup)

// ===================== PM Calibration Configuration =====================
// Calibration formula: calibrated_value = factor × raw_value + offset

// PM2.5 Calibration Parameters
#define PM25_CALIBRATION_FACTOR    1.0f    // Default: ไม่ปรับค่า
#define PM25_CALIBRATION_OFFSET    0.0f    // Default: ไม่ปรับค่า

// PM10 Calibration Parameters
#define PM10_CALIBRATION_FACTOR    1.0f    // Default: ไม่ปรับค่า
#define PM10_CALIBRATION_OFFSET    0.0f    // Default: ไม่ปรับค่า

// ===================== Display =====================
TFT_eSPI tft;
TFT_eSprite sprite = TFT_eSprite(&tft);
#ifndef TFT_BL
  #define TFT_BL 38
#endif

// ===================== PMS9103M Serial =====================
#define PMS_RX_PIN 18  // ESP32 RX ← PMS TXD
#define PMS_TX_PIN 17  // ESP32 TX → PMS RXD
#define PMS_SET_PIN 43 // Control sleep/wake of sensor
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
void sendToGistNorth();
void enterDeepSleep(bool ultra_low_power = false);
void enterUltraLowPowerMode();
void shutdownAllPeripherals();
void drawCriticalBatteryScreen();
uint16_t calibratePM(uint16_t rawValue, float factor, float offset);
uint16_t getCalibratedPM25();
uint16_t getCalibratedPM10();

// ===================== PM Calibration Functions =====================
uint16_t calibratePM(uint16_t rawValue, float factor, float offset) {
  float calibrated = (factor * rawValue) + offset;
  // Clamp values to valid range (0 - 9999)
  if (calibrated < 0.0f) return 0;
  if (calibrated > 9999.0f) return 9999;
  return (uint16_t)calibrated;
}

uint16_t getCalibratedPM25() {
  return calibratePM(pmsData.pm2_5_atm, PM25_CALIBRATION_FACTOR, PM25_CALIBRATION_OFFSET);
}

uint16_t getCalibratedPM10() {
  return calibratePM(pmsData.pm10_atm, PM10_CALIBRATION_FACTOR, PM10_CALIBRATION_OFFSET);
}

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
  // Simple calculation: raw * 1100mV / 4095 * divider * cal
  float vbat = (raw * 1100.0f / 4095.0f) * DIVIDER * CAL;
  return vbat;
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

  digitalWrite(TFT_BL, LOW);
  tft.writecommand(0x10);
  
  digitalWrite(PMS_SET_PIN, LOW);
  
  pmsSerial.end();
  
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  
#if CONFIG_IDF_TARGET_ESP32
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
  esp_bt_mem_release(ESP_BT_MODE_BTDM);
#endif
  
  analogRead(PIN_VBAT);
  pinMode(PIN_VBAT, INPUT);
  
  for (int i = 0; i < 49; i++) {
    if (i == 0 || i == 3 || i == 45 || i == 46) continue;
    if (i == 19 || i == 20) continue;
    if (i >= 26 && i <= 37) continue;
    if (i == BUTTON_2 || i == BUTTON_1) continue;
    if (i == 5 || i == 6 || i == 7 || i == 8 || i == 9) continue;
    if (i >= 39 && i <= 48) continue;
    if (i == 15 || i == 38) continue;
    if (i == PMS_RX_PIN || i == PMS_TX_PIN || i == PMS_SET_PIN) continue;
    pinMode(i, INPUT);
  }
  

}

// ===================== Enhanced Deep Sleep Functions =====================
void configureDeepSleepWake(bool include_timer = false, uint64_t timer_us = BATTERY_CHECK_INTERVAL_US) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  
  pinMode(BUTTON_2, INPUT_PULLUP);
#if CONFIG_IDF_TARGET_ESP32
  esp_sleep_enable_ext1_wakeup(WAKE_MASK, WAKE_MODE);
#elif CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_enable_ext1_wakeup(WAKE_MASK, WAKE_MODE);
#endif
  
  if (include_timer) {
    esp_sleep_enable_timer_wakeup(timer_us);
  }
  
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_ON);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL, ESP_PD_OPTION_OFF);
}

void enterDeepSleep(bool ultra_low_power) {
  shutdownAllPeripherals();
  
  if (ultra_low_power) {
    configureDeepSleepWake(true, ULTRA_LOW_POWER_INTERVAL_US);
  } else {
    configureDeepSleepWake(true, BATTERY_CHECK_INTERVAL_US);
  }
  
  Serial.flush();
  delay(100);
  
  esp_deep_sleep_start();
}

void enterUltraLowPowerMode() {
  drawCriticalBatteryScreen();
  delay(2000);
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
}

void evaluateBatteryProtection() {
  if (usbMode) {
    lowBatteryWarn = false;
    lowBatteryMode = false;
    criticalBattery = false;
    return;
  }
  
  if (batteryVoltage <= VBAT_CRITICAL) {
    criticalBattery = true;
    enterUltraLowPowerMode();
    return;
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
  
  drawLowBatteryScreen();
  sprite.pushSprite(0, 0);
  delay(2000);
  
  enterDeepSleep(false);
}

void maybeExitLowBatterySafeState() {
  if (!lowBatteryMode && batteryVoltage >= VBAT_RECOVER) {
    criticalBattery = false;
  }
}

// ===================== Display Functions =====================
void drawCriticalBatteryScreen() {
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.setTextFont(4);
  sprite.drawString("CRITICAL!", 160, 40);
  
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(4);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "%.2fV", batteryVoltage);
  sprite.drawString(vbuf, 160, 75);
  
  sprite.setTextFont(2);
  sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
  sprite.drawString("Deep Sleep Mode", 160, 105);
  sprite.drawString("Charge immediately!", 160, 130);
  
  sprite.pushSprite(0, 0);
}

void drawLowBatteryScreen() {
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.setTextFont(4);
  sprite.drawString("LOW BATTERY", 160, 45);
  
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(2);
  char vbuf[32];
  snprintf(vbuf, sizeof(vbuf), "VBAT: %.2fV  (%d%%)", batteryVoltage, batteryPercent);
  sprite.drawString(vbuf, 160, 80);
  
  sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
  sprite.drawString("Charging required", 160, 110);
  
  sprite.setTextDatum(TR_DATUM);
  sprite.setTextFont(1);
  sprite.setTextColor(TFT_RED, TFT_BLACK);
  sprite.drawString("WiFi OFF", 315, 5);
  
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString(DEVICE_NAME, 5, 5);
  
  sprite.setTextDatum(BL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString("SAFE MODE", 5, 168);
}

// ===================== Time Functions =====================
void syncTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1700000000 && retries < 20) {
    delay(250);
    now = time(nullptr);
    retries++;
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
        return true;
      }
      pmsBuffer[0] = byteIn;
    }
    delay(10);
  }
  
  digitalWrite(PMS_SET_PIN, LOW);
  return false;
}

bool processPMSData() {
  if (pmsBuffer[0] != 0x42 || pmsBuffer[1] != 0x4D) {
    return false;
  }
  
  uint16_t checksum = 0;
  for (int i = 0; i < 30; i++) checksum += pmsBuffer[i];
  uint16_t bufferChecksum = (pmsBuffer[30] << 8) | pmsBuffer[31];
  
  if (checksum != bufferChecksum) {
    return false;
  }
  
  pmsData.pm1_0_cf1 = (pmsBuffer[4] << 8) | pmsBuffer[5];
  pmsData.pm2_5_cf1 = (pmsBuffer[6] << 8) | pmsBuffer[7];
  pmsData.pm10_cf1  = (pmsBuffer[8] << 8) | pmsBuffer[9];
  pmsData.pm1_0_atm = (pmsBuffer[10] << 8) | pmsBuffer[11];
  pmsData.pm2_5_atm = (pmsBuffer[12] << 8) | pmsBuffer[13];
  pmsData.pm10_atm  = (pmsBuffer[14] << 8) | pmsBuffer[15];
  pmsData.particles_03  = (pmsBuffer[16] << 8) | pmsBuffer[17];
  pmsData.particles_05  = (pmsBuffer[18] << 8) | pmsBuffer[19];
  pmsData.particles_10  = (pmsBuffer[20] << 8) | pmsBuffer[21];
  pmsData.particles_25  = (pmsBuffer[22] << 8) | pmsBuffer[23];
  pmsData.particles_50  = (pmsBuffer[24] << 8) | pmsBuffer[25];
  pmsData.particles_100 = (pmsBuffer[26] << 8) | pmsBuffer[27];
  
  return true;
}

void readPMSSensor() {
  if (!systemActive || lowBatteryMode) return;
  if (!pmsConnected) return;
  
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
  if (lowBatteryMode) return;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_CYAN, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Connecting WiFi...", 160, 65);
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(1);
  char ssidMsg[64];
  snprintf(ssidMsg, sizeof(ssidMsg), "SSID: %s", WIFI_SSID);
  sprite.drawString(ssidMsg, 5, 135);
  sprite.pushSprite(0, 0);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    Serial.printf("[WIFI] Attempt %d/20\n", attempts);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_GREEN, TFT_BLACK);
    sprite.setTextFont(3);
    sprite.drawString("WiFi CONNECTED", 160, 50);
    sprite.setTextFont(1);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString(WiFi.localIP().toString().c_str(), 160, 85);
    sprite.pushSprite(0, 0);
    delay(1000);
  } else {
    wifiConnected = false;
    Serial.println("[WIFI] Connection failed!");
    
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_RED, TFT_BLACK);
    sprite.setTextFont(3);
    sprite.drawString("WiFi FAILED", 160, 50);
    sprite.setTextFont(1);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("Will continue offline", 160, 85);
    sprite.pushSprite(0, 0);
    delay(1000);
  }
}

// ===================== Sleep/Wake Functions =====================
void enterSleepMode() {
  if (!systemActive) return;
  
  systemActive = false;
  displayOn = false;
  
  pmsWarmedUp = false;
  waitingForWarmup = false;
  pmsWarmupStart = 0;
  systemWakeTime = 0;
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_BLUE, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Sleep Mode", 160, 85);
  sprite.pushSprite(0, 0);
  delay(400);
  
  digitalWrite(TFT_BL, LOW);
  tft.fillScreen(TFT_BLACK);
  
  digitalWrite(PMS_SET_PIN, LOW);
  
  if (wifiConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
  }
  
  while (pmsSerial.available()) pmsSerial.read();
  
  dataReady = false;
  bufferIndex = 0;
}

void wakeFromSleep() {
  if (systemActive) return;
  
  systemActive = true;
  displayOn = true;
  systemWakeTime = millis();
  
  digitalWrite(TFT_BL, HIGH);
  
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_CYAN, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("Waking up...", 160, 85);
  sprite.pushSprite(0, 0);
  
  readBattery();
  
  if (batteryVoltage <= VBAT_CRITICAL && !usbMode) {
    enterUltraLowPowerMode();
    return;
  }
  
  if (lowBatteryMode && !usbMode) {
    forceLowBatterySafeState();
    return;
  }
  
  if (pmsConnected) {
    digitalWrite(PMS_SET_PIN, HIGH);
    pmsWarmupStart = millis();
    pmsWarmedUp = false;
    waitingForWarmup = true;
    
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("Sensor Warming Up", 160, 65);
    sprite.drawString("Please wait 30s...", 160, 100);
    sprite.pushSprite(0, 0);
  } else {
    sprite.fillSprite(TFT_BLACK);
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_ORANGE, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("No Sensor Connected", 160, 65);
    sprite.drawString("System running offline", 160, 100);
    sprite.pushSprite(0, 0);
  }
  
  connectWiFi();
  if (wifiConnected) {
    syncTime();
  }
  
  dataReady = false;
  bufferIndex = 0;
}

void handleButtonPress() {
  if (!buttonPressed) return;
  
  unsigned long now = millis();
  if (now - lastButtonTime > DEBOUNCE_TIME) {
    if (systemActive) {
      enterSleepMode();
    } else {
      wakeFromSleep();
    }
    lastButtonTime = now;
  }
  buttonPressed = false;
}

// ===================== GIST North API =====================
void sendToGistNorth() {
  if (!wifiConnected || !systemActive || lowBatteryMode) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = String("https://") + GIST_SERVER + GIST_PATH;

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Authorization", String("Bearer ") + GIST_TOKEN);

    StaticJsonDocument<256> doc;

    if (pmsConnected && dataReady) {
      doc["pm25"] = (float)getCalibratedPM25();
      doc["pm25_raw"] = (float)pmsData.pm2_5_atm;
      doc["pm10"] = (float)getCalibratedPM10();
      doc["pm10_raw"] = (float)pmsData.pm10_atm;

      Serial.printf("[CALIBRATE] PM2.5 raw=%d -> cal=%d | PM10 raw=%d -> cal=%d\n",
                    pmsData.pm2_5_atm, getCalibratedPM25(),
                    pmsData.pm10_atm, getCalibratedPM10());

      Serial.printf("[PMS HEX]");
      for (int i = 0; i < 32; i++) {
        Serial.printf(" %02X", pmsBuffer[i]);
      }
      Serial.println();
    } else {
      doc["pm25"] = 0.0;
      doc["pm25_raw"] = 0.0;
      doc["pm10"] = 0.0;
      doc["pm10_raw"] = 0.0;
    }

    String jsonString;
    serializeJson(doc, jsonString);

    String ts = getShortTimestamp();
    Serial.printf("[%s] SEND -> %s | pm25=%.1f (raw=%.1f) pm10=%.1f (raw=%.1f) | ",
                  ts.c_str(), GIST_SERVER,
                  pmsConnected ? (float)getCalibratedPM25() : 0.0f,
                  pmsConnected ? (float)pmsData.pm2_5_atm : 0.0f,
                  pmsConnected ? (float)getCalibratedPM10() : 0.0f,
                  pmsConnected ? (float)pmsData.pm10_atm : 0.0f);

    int httpCode = https.POST(jsonString);

    if (httpCode > 0) {
      String payload = https.getString();
      Serial.printf("RESPONSE: %d\n", httpCode);
      Serial.printf("FULL RESPONSE: %s\n", payload.c_str());
      
      if (httpCode == HTTP_CODE_OK || httpCode == 200) {
        lastSuccessSend = ts;
      }
    } else {
      Serial.printf("ERROR: %s\n", https.errorToString(httpCode).c_str());
    }

    https.end();
  } else {
    Serial.printf("[%s] SEND FAILED: cannot connect to %s\n",
                  getShortTimestamp().c_str(), GIST_SERVER);
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
  
  if (!pmsConnected) {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_ORANGE, TFT_BLACK);
    sprite.setTextFont(4);
    sprite.drawString("NO PMS", 160, 60);
    sprite.setTextFont(2);
    sprite.drawString("Sensor Not Connected", 160, 95);
  } 
  else if (waitingForWarmup && !pmsWarmedUp) {
    unsigned long elapsed = (millis() - pmsWarmupStart) / 1000;
    unsigned long remaining = (PMS_WARMUP_TIME / 1000) - elapsed;
    
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.setTextFont(4);
    sprite.drawString("WARMING UP", 160, 45);
    
    sprite.setTextFont(6);
    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    char countdown[10];
    snprintf(countdown, sizeof(countdown), "%lu", remaining);
    sprite.drawString(countdown, 160, 80);
    
    sprite.setTextFont(2);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("seconds remaining", 160, 120);
  }
  else if (dataReady) {
    uint16_t calibratedPM25 = getCalibratedPM25();
    uint16_t calibratedPM10 = getCalibratedPM10();
    uint16_t color = getColorForPM25(calibratedPM25);

    static uint16_t lastRawPM25 = 0xFFFF;
    static uint16_t lastRawPM10 = 0xFFFF;

    if (pmsData.pm2_5_atm != lastRawPM25 || pmsData.pm10_atm != lastRawPM10) {
      Serial.printf("[PM_DATA] PM2.5 raw=%d -> cal=%d | PM10 raw=%d -> cal=%d\n",
                    pmsData.pm2_5_atm, calibratedPM25,
                    pmsData.pm10_atm, calibratedPM10);
      lastRawPM25 = pmsData.pm2_5_atm;
      lastRawPM10 = pmsData.pm10_atm;
    }
    
    sprite.setTextDatum(ML_DATUM);
    sprite.setTextColor(color, TFT_BLACK);
    String pm25str = String(calibratedPM25);
    if (pm25str.length() <= 2)      sprite.setTextFont(8);
    else if (pm25str.length() == 3) sprite.setTextFont(7);
    else                            sprite.setTextFont(6);
    sprite.drawString(pm25str, 10, 85);
    
    sprite.setTextDatum(MR_DATUM);
    sprite.setTextFont(4);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.drawString("ug/m3", 310, 55);
    
    sprite.setTextFont(2);
    sprite.setTextColor(color, TFT_BLACK);
    sprite.drawString(getAirQuality(calibratedPM25), 310, 90);
    
    sprite.setTextFont(1);
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.drawString(lastSuccessSend, 310, 115);
  }
  else {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setTextFont(2);
    if (!pmsConnected) {
      sprite.drawString("System Running", 160, 70);
      sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
      sprite.drawString("No Sensor Data", 160, 100);
    } else {
      sprite.drawString("Waiting for data...", 160, 85);
    }
  }
  
  sprite.setTextDatum(TR_DATUM);
  sprite.setTextFont(1);
  sprite.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED, TFT_BLACK);
  sprite.drawString(wifiConnected ? "WiFi OK" : "No WiFi", 315, 5);
  
  sprite.setTextColor(getBatteryColor(batteryPercent), TFT_BLACK);
  char battStr[32];
  snprintf(battStr, sizeof(battStr), "Batt: %d%% %.2fV%s",
           batteryPercent, batteryVoltage, usbMode ? " CHG" : "");
  sprite.drawString(battStr, 315, 15);
  
  if (lowBatteryWarn) {
    sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    sprite.drawString("Battery Low!", 315, 25);
  } else {
    unsigned long elapsed = (millis() - systemWakeTime) / 1000;
    unsigned long remaining = (AUTO_SLEEP_TIME / 1000) - elapsed;
    char countdown[16];
    snprintf(countdown, sizeof(countdown), "Sleep in %lus", remaining);
    sprite.setTextColor(TFT_CYAN, TFT_BLACK);
    sprite.drawString(countdown, 315, 25);
  }
  
  sprite.setTextDatum(TL_DATUM);
  sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
  sprite.drawString(DEVICE_NAME, 5, 5);
  
  if (!wifiConnected) {
    sprite.setTextDatum(MC_DATUM);
    sprite.setTextColor(TFT_RED, TFT_BLACK);
    sprite.setTextFont(2);
    sprite.drawString("OFFLINE MODE", 160, 120);
  }
  
  sprite.pushSprite(0, 0);
}

// ===================== Setup =====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  Serial.println("[BOOT] ADC init...");
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  // esp_adc_cal_characterize(ADC_UNIT_1, CAL_ATTEN, ADC_WIDTH_BIT_12, VREF_mV, &adc_chars);

  Serial.println("[BOOT] Reading battery...");
  readBattery();
  Serial.printf("[BOOT] Battery: %.2fV %d%% USB=%d\n", batteryVoltage, batteryPercent, usbMode);

  if (batteryVoltage < 3.0f || batteryVoltage > 5.0f) {
    Serial.println("[BOOT] WARNING: Abnormal battery reading, assuming charging");
    usbMode = true;
    batteryVoltage = 4.20f;
    batteryPercent = 100;
  }

  if (!usbMode && batteryVoltage <= VBAT_CRITICAL) {
    Serial.println("[BOOT] CRITICAL BATTERY - entering ultra low power mode");
    enterUltraLowPowerMode();
    return;
  }
  
  pinMode(DISPLAY_POWER_PIN, OUTPUT);
  digitalWrite(DISPLAY_POWER_PIN, HIGH);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  Serial.println("[BOOT] Display init...");
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  sprite.createSprite(320, 170);
  sprite.fillSprite(TFT_BLACK);
  Serial.println("[BOOT] Display OK");
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_TIMER:
      if (!usbMode && batteryVoltage < VBAT_RECOVER) {
        digitalWrite(TFT_BL, HIGH);
        sprite.fillSprite(TFT_BLACK);
        sprite.setTextDatum(MC_DATUM);
        sprite.setTextColor(TFT_YELLOW, TFT_BLACK);
        sprite.setTextFont(2);
        sprite.drawString("Battery Check", 160, 50);
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%.2fV (%d%%)", batteryVoltage, batteryPercent);
        sprite.drawString(vbuf, 160, 80);
        sprite.drawString("Still too low", 160, 110);
        sprite.pushSprite(0, 0);
        delay(1000);
        digitalWrite(TFT_BL, LOW);
        
        if (batteryVoltage <= VBAT_CRITICAL) {
          enterDeepSleep(true);
        } else {
          enterDeepSleep(false);
        }
        return;
      }
      break;
      
    case ESP_SLEEP_WAKEUP_EXT1:
      if (!usbMode && batteryVoltage < VBAT_CUTOFF) {
        digitalWrite(TFT_BL, HIGH);
        drawCriticalBatteryScreen();
        delay(3000);
        digitalWrite(TFT_BL, LOW);
        
        if (batteryVoltage <= VBAT_CRITICAL) {
          enterDeepSleep(true);
        } else {
          enterDeepSleep(false);
        }
        return;
      }
      break;
      
    default:
      if (!usbMode && batteryVoltage <= VBAT_CRITICAL) {
        enterUltraLowPowerMode();
        return;
      }
      break;
  }
  
  pinMode(BUTTON_1, INPUT_PULLUP);
  pinMode(BUTTON_2, INPUT_PULLUP);
  
  Serial.println("[BOOT] Serial init...");
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  pinMode(PMS_SET_PIN, OUTPUT);
  digitalWrite(PMS_SET_PIN, LOW);

  if (!lowBatteryMode) {
    Serial.println("[BOOT] Checking PMS sensor...");
    pmsConnected = checkPMSSensor();
    if (pmsConnected) {
      Serial.println("[BOOT] PMS sensor OK");
      digitalWrite(PMS_SET_PIN, LOW);
    } else {
      Serial.println("[BOOT] PMS sensor NOT connected");
    }
  } else {
    pmsConnected = false;
  }
  
  attachInterrupt(digitalPinToInterrupt(BUTTON_1), buttonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(BUTTON_2), buttonISR, FALLING);
  
  // Automatically activate system on boot
  systemActive = true;
  displayOn = true;
  systemWakeTime = millis();
  
  // Display boot status
  sprite.fillSprite(TFT_BLACK);
  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(TFT_WHITE, TFT_BLACK);
  sprite.setTextFont(2);
  sprite.drawString("System Booting...", 160, 50);
  sprite.pushSprite(0, 0);
  
  // Connect WiFi on boot
  connectWiFi();
  if (wifiConnected) {
    syncTime();
  }
  
  // Start sensor if connected
  if (pmsConnected) {
    digitalWrite(PMS_SET_PIN, HIGH);
    pmsWarmupStart = millis();
    pmsWarmedUp = false;
    waitingForWarmup = true;
  }
  
  dataReady = false;
  bufferIndex = 0;
}

// ===================== Main Loop =====================
void loop() {
  handleButtonPress();
  
  static unsigned long lastBattRefresh = 0;
  if (millis() - lastBattRefresh >= 5000) {
    readBattery();
    
    if (!usbMode && batteryVoltage <= VBAT_CRITICAL) {
      enterUltraLowPowerMode();
      return;
    }
    
    if (!lowBatteryMode && batteryVoltage <= VBAT_CUTOFF && !usbMode) {
      forceLowBatterySafeState();
      return;
    }
    
    if (lowBatteryMode && (batteryVoltage >= VBAT_RECOVER || usbMode)) {
      maybeExitLowBatterySafeState();
    }
    
    lastBattRefresh = millis();
  }
  
  if (systemActive) {
    if (systemWakeTime > 0 && (millis() - systemWakeTime >= AUTO_SLEEP_TIME)) {
      enterSleepMode();
      return;
    }
    
    if (waitingForWarmup && !pmsWarmedUp) {
      if (millis() - pmsWarmupStart >= PMS_WARMUP_TIME) {
        pmsWarmedUp = true;
        waitingForWarmup = false;
        
        while (pmsSerial.available()) {
          pmsSerial.read();
        }
        bufferIndex = 0;
        dataReady = false;
      }
    }
    
    if (lowBatteryMode) {
      if (millis() - lastUpdate >= 3000) {
        updateDisplay();
        lastUpdate = millis();
      }
      delay(10);
      return;
    }
    
    if (pmsConnected) {
      readPMSSensor();
    }

    if (pmsConnected && dataReady && !waitingForWarmup && pmsWarmedUp) {
      static uint16_t lastRawPM25 = 0xFFFF;
      static uint16_t lastRawPM10 = 0xFFFF;
      static bool lastPrinted = false;
      if (!lastPrinted || pmsData.pm2_5_atm != lastRawPM25 || pmsData.pm10_atm != lastRawPM10) {
        Serial.printf("[PM_DATA] PM2.5 raw=%d -> cal=%d | PM10 raw=%d -> cal=%d\n",
                      pmsData.pm2_5_atm, getCalibratedPM25(),
                      pmsData.pm10_atm, getCalibratedPM10());
        lastRawPM25 = pmsData.pm2_5_atm;
        lastRawPM10 = pmsData.pm10_atm;
        lastPrinted = true;
      }
    }

    unsigned long displayInterval = (waitingForWarmup && !pmsWarmedUp) ? 1000 : 5000;
    if (millis() - lastUpdate >= displayInterval) {
      updateDisplay();
      lastUpdate = millis();
    }
    
    if (millis() - lastTelemetry >= SEND_INTERVAL) {
      sendToGistNorth();
      lastTelemetry = millis();
    }
  }
  
  delay(10);
}