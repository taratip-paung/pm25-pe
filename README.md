# PE_PMS9103M-2

เครื่องวัดคุณภาพอากาศ PM2.5 แบบพกพา ทำงานบนไมโครคอนโทรลเลอร์ **ESP32-S3** (LILYGO T-Display S3) อ่านค่าฝุ่นละอองด้วยเซ็นเซอร์ PMS9103M แสดงผลบนจอ TFT สี 1.9 นิ้ว (170x320) และส่งข้อมูลไปยัง **GIST North IoT API** พร้อมระบบจัดการพลังงานแบตเตอรี่อัจฉริยะ ทำงานแบบต่อเนื่อง (Always On) พร้อม Deep Sleep เมื่อแบตเตอรี่วิกฤต

---

## คุณสมบัติเด่น

- วัดฝุ่นละออง **PM1.0, PM2.5, PM10** ด้วยเซ็นเซอร์เลเซอร์ PMS9103M
- แสดงผลค่า PM2.5 แบบเรียลไทม์พร้อม **ระดับสี** ตามมาตรฐานคุณภาพอากาศ บนจอ **1.9 นิ้ว ST7789 170x320**
- ส่งข้อมูลไปยัง **GIST North IoT API** ผ่าน HTTPS ทุก 1 วินาที
- ระบบ **จัดการพลังงานแบตเตอรี่** แบบหลายระดับ (Warning → Safe Mode → Critical → Ultra Low Power)
- วัดแรงดันแบตเตอรี่ผ่าน ADC + Voltage Divider พร้อมแสดง % และสถานะชาร์จ
- **ระบบทำงานต่อเนื่อง (Always On)** - กดปุ่มเพื่อ Sleep/Wake, Deep Sleep เมื่อแบตวิกฤต
- ซิงค์เวลาผ่าน **NTP** (UTC+7)
- แจ้งเตือนเมื่อแบตเตอรี่ต่ำ / ไม่มี WiFi / ไม่พบเซ็นเซอร์
- **Offline Mode** - ทำงานได้แม้ไม่มี WiFi (แสดงข้อความ "OFFLINE MODE")
- **Sleep Mode** - กดปุ่มเพื่อปิดเซ็นเซอร์และ WiFi (ประหยัดพลังงาน)

---

## ฮาร์ดแวร์ที่ใช้

| อุปกรณ์ | รายละเอียด |
|---------|-----------|
| **ไมโครคอนโทรลเลอร์** | LILYGO T-Display S3 (ESP32-S3R8 + ST7789V 170×320 TFT, 8-bit Parallel) |
| **เซ็นเซอร์ฝุ่น** | PMS9103M Laser Dust Sensor (Plantower) |
| **แบตเตอรี่** | Li-Po Battery (วัดแรงดันผ่าน GPIO4 ADC) |

---

## การเชื่อมต่อวงจร (Pin Mapping)

### PMS9103M Sensor (UART1)

| ฟังก์ชัน | GPIO | หมายเหตุ |
|----------|------|---------|
| PMS TX → ESP32 RX | **GPIO 18** | UART1 RX |
| PMS RX ← ESP32 TX | **GPIO 17** | UART1 TX |
| PMS SET (Sleep/Wake) | **GPIO 43** | HIGH = ทำงาน, LOW = หลับ |

### Battery Management

| ฟังก์ชัน | GPIO | หมายเหตุ |
|----------|------|---------|
| Battery ADC | **GPIO 4** | ADC1_CH3, ผ่าน Voltage Divider |
| Display Power Enable | **GPIO 15** | HIGH = เปิดจอ (จำเป็นเมื่อใช้แบต) |

### Buttons

| ฟังก์ชัน | GPIO | หมายเหตุ |
|----------|------|---------|
| ปุ่มกด (onboard) | **GPIO 14** | เปลี่ยน Active/Sleep mode |
| ปุ่มกด (BOOT) | **GPIO 0** | Deep Sleep Wake Source |

### Display (ST7789 - 8-bit Parallel)

| ฟังก์ชัน | GPIO | หมายเหตุ |
|----------|------|---------|
| TFT CS | GPIO 6 | 8-bit Parallel |
| TFT DC | GPIO 7 | Data/Command |
| TFT RST | GPIO 5 | Reset |
| TFT WR | GPIO 8 | Write |
| TFT RD | GPIO 9 | Read |
| TFT D0-D7 | GPIO 39,40,41,42,45,46,47,48 | Data Bus |
| TFT BL (Backlight) | GPIO 38 | Backlight Control |

---

## ซอฟต์แวร์ที่ต้องใช้

- **[PlatformIO](https://platformio.org/)** (แนะนำ) หรือ Arduino IDE
- **ESP32-S3 Platform** (Espressif32)

### ไลบรารีที่ใช้

| ไลบรารี | เวอร์ชัน | 用途 |
|---------|---------|------|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | ^2.5.43 | ควบคุมจอ TFT ST7789 (8-bit Parallel) |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^6.21.3 | สร้าง JSON payload สำหรับ GIST North API |

---

## การติดตั้งและใช้งาน

### 1. Clone โปรเจค

```bash
git clone https://github.com/taratip-paung/pm25-pe.git
cd pm25-pe
```

### 2. เปิดด้วย PlatformIO

```bash
# เปิดใน VSCode + PlatformIO หรือใช้ CLI
pio run -t upload
pio device monitor -b 115200
```

**หมายเหตุ:** หากต้องการเปลี่ยน COM port สำหรับ upload/monitor ให้แก้ไข `platformio.ini`:
```ini
upload_port = COM8    # เปลี่ยน COM port ที่ต้องการ
monitor_port = COM8   # เปลี่ยน COM port ที่ต้องการ
```

### 3. แก้ไขการตั้งค่า

แก้ไขค่า config ในไฟล์ `src/main.cpp` ก่อน upload:

```cpp
// WiFi
const char* WIFI_SSID = "ชื่อWiFiของคุณ";
const char* WIFI_PASSWORD = "รหัสWiFi";

// GIST North API
const char* GIST_SERVER = "app.gistnorth.soc.cmu.ac.th";
const char* GIST_TOKEN  = "Tokenของอุปกรณ์คุณ";
const char* GIST_PATH   = "/iot/api/ingest";
const char* DEVICE_NAME = "PM25-PE-2";
```

> **สำคัญ:** อย่า commit credentials จริงลง public repository

---

## การทำงานของระบบ

ระบบทำงาน **ต่อเนื่อง (Always On)** เมื่อเปิดเครื่อง:
- เชื่อมต่อ WiFi และซิงค์เวลา NTP
- เปิดเซ็นเซอร์ PMS และรอ warm-up 30 วินาที
- อ่านค่า PM ทุก 3 วินาที
- ส่งข้อมูลไป GIST North API ทุก 1 วินาที (จริงๆ ~3 วินาที เนื่องจากเวลา HTTPS request)
- แสดง PMS HEX raw data บน Serial Monitor ทุกครั้งที่ส่ง API
- ตรวจสอบแบตเตอรี่ทุก 5 วินาที

**สามารถกดปุ่มเพื่อ:**
- ปุ่ม (GPIO 14) หรือ BOOT (GPIO 0) → สลับระหว่าง Active Mode / Sleep Mode

**Deep Sleep เมื่อแบตวิกฤต:**
- Battery Critical (< 3.35V) → Ultra Low Power Deep Sleep (60 นาที)
- Battery Cutoff (< 3.40V) → Safe Mode + Deep Sleep (30 นาที)

---

## การตั้งค่าเพิ่มเติม

### การตั้งค่าจอ TFT (Display Configuration)

การตั้งค่าจอ TFT อยู่ใน `platformio.ini` ผ่าน build flags:

```ini
build_flags =
  -D USER_SETUP_LOADED
  -D ST7789_DRIVER
  -D TFT_PARALLEL_8_BIT      # ใช้ 8-bit Parallel Interface
  -D TFT_WIDTH=170           # กว้าง 170 pixel
  -D TFT_HEIGHT=320          # สูง 320 pixel
  -D TFT_RGB_ORDER=TFT_RGB   # ลำดับสี RGB
  -D TFT_INVERSION_ON        # Invert display
  # ... และอื่นๆ
```

**สำคัญ:** การตั้งค่าเหล่านี้ override User_Setup.h ของไลบรารี TFT_eSPI

### การตั้งค่า PlatformIO

**Board & Platform:**
```ini
[env:lilygo-t-display-s3]
platform = espressif32
board = lilygo-t-display-s3
framework = arduino
```

**Upload & Monitor:**
```ini
upload_speed  = 921600      # ความเร็ว upload (สูงสุด 921600)
upload_protocol = esptool  # Protocol สำหรับ upload
upload_port = COM8          # COM port สำหรับ upload
monitor_speed = 115200      # ความเร็ว serial monitor
monitor_port = COM8         # COM port สำหรับ monitor
```

**Library Dependencies:**
```ini
lib_deps =
  bodmer/TFT_eSPI @ ^2.5.43      # ไลบรารีจอ TFT
  bblanchon/ArduinoJson @ ^6.21.3 # ไลบรารี JSON
```

### ระดับแบตเตอรี่

```cpp
#define VBAT_WARN     3.60f   // เตือนแบตต่ำ (แสดงข้อความบนจอ)
#define VBAT_CUTOFF   3.40f   // เข้า Safe Mode → Deep Sleep 30 นาที
#define VBAT_CRITICAL 3.35f   // วิกฤต → Ultra Low Power Deep Sleep 60 นาที
#define VBAT_RECOVER  3.70f   // กลับมาทำงานปกติ (หลังจากอยู่ใน Safe Mode)
#define VBAT_MINIMUM  3.30f   // ระดับแบตต่ำสุดที่ระบบยอมให้ทำงาน
```

**กลไกความปลอดภัยแบตเตอรี่:**
- ใช้ Hysteresis เพื่อป้องกันการกระพริบระหว่างสถานะ
- ตรวจจับการชาร์จ USB (เมื่อแรงดัน > 4.25V และลดลงเหลือ < 4.10V)
- เมื่อชาร์จ: ระบบทำงานปกติ, ไม่มีการเข้าโหมดประหยัดพลังงาน

### ระยะเวลา

```cpp
const unsigned long SEND_INTERVAL = 1000;       // ส่งข้อมูลทุก 1 วินาที (จริงๆ ~3 วินาที)
const unsigned long PMS_WARMUP_TIME = 30000;     // รอ warmup เซ็นเซอร์ 30 วินาที
const unsigned long PMS_READ_INTERVAL = 3000;    // อ่านเซ็นเซอร์ทุก 3 วินาที (หลัง warmup)
const unsigned long BATTERY_CHECK_INTERVAL = 5000; // ตรวจสอบแบตทุก 5 วินาที
```

**Deep Sleep Timer:**
```cpp
#define BATTERY_CHECK_INTERVAL_US  (30 * 60 * 1000000ULL)  // 30 นาที (Safe Mode)
#define ULTRA_LOW_POWER_INTERVAL_US (60 * 60 * 1000000ULL)  // 60 นาที (Critical Mode)
```

**WiFi Connection:**
```cpp
// รอ connection สูงสุด 20 attempts (10 วินาที)
// ถ้าไม่สามารถเชื่อมต่อได้ จะทำงานใน Offline Mode
```

---

## โหมดการทำงาน

```
┌─────────────────┐
│  Power On Boot  │
│  + WiFi + NTP   │
│  + Sensor Init  │
└────────┬────────┘
         │ 30s
         ▼
┌─────────────────┐
│  Warmup PMS     │
│  (30 วินาที)    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│  ทำงานต่อเนื่อง    │
│  - อ่าน PM ทุก 3 วินาที  │
│  - ส่ง API + แสดง HEX ทุก ~3 วินาที  │
│  - อัพเดทจอทุก 5 วินาที  │
└────────┬────────┘
         │
         ├─ กดปุ่ม → Sleep (ปิดเซ็นเซอร์ + WiFi)
         │              กดปุ่มอีกครั้ง → Wake
         │
         └─ ตรวจแบตทุก 5 วินาที
             ├─ Warning (< 3.60V)   → แจ้งเตือนหน้าจอ, ทำงานต่อ
             ├─ Cutoff  (< 3.40V)   → Safe Mode + Deep Sleep 30 นาที
             └─ Critical (< 3.35V)  → Ultra Low Power Deep Sleep 60 นาที
```

---

## ระดับคุณภาพอากาศ (แสดงผลบนจอ)

| PM2.5 (µg/m³) | ระดับ | สีบนจอ |
|----------------|-------|--------|
| 0 – 12 | Good (ดี) | เขียว |
| 13 – 35 | Moderate (ปานกลาง) | เหลือง |
| 36 – 55 | Unhealthy-S (ไม่ดีสำหรับกลุ่มเสี่ยง) | ส้ม |
| 56 – 150 | Unhealthy (ไม่ดี) | แดง |
| 151 – 250 | Very Unhealthy (ไม่ดีมาก) | ม่วง |
| > 250 | Hazardous (อันตราย) | น้ำตาลเข้ม |

---

## โครงสร้างไฟล์โปรเจค

```
PE_PMS9103M-2/
├── platformio.ini          # การตั้งค่า PlatformIO + Build Flags (TFT + WiFi + Upload)
├── src/
│   └── main.cpp            # โค้ดหลักทั้งหมด (~1148 บรรทัด)
│                           # - Battery Management (Voltage ADC, SOC, Protection)
│                           # - PMS9103M Sensor (UART, Data Processing)
│                           # - WiFi & NTP (Connection, Time Sync)
│                           # - Display (TFT_eSPI, UI, Air Quality Colors)
│                           # - GIST North API (HTTPS, JSON Payload)
│                           # - Deep Sleep (Battery Protection, Wake Sources)
├── lib/
│   └── TFT_eSPI/
│       └── User_Setup.h    # TFT configuration (ถูก override โดย platformio.ini)
├── include/
├── test/
├── .gitignore
├── LICENSE                 # MIT License
└── README.md               # เอกสารนี้
```

---

## ข้อมูลที่ส่งไป GIST North API

**Endpoint:** `POST https://app.gistnorth.soc.cmu.ac.th/iot/api/ingest`

**Headers:**
- `Content-Type: application/json`
- `Authorization: Bearer <GIST_TOKEN>`

**Payload:**
```json
{
  "pm25": 19,
  "pm10": 21
}
```

**Serial Monitor Output (แสดงพร้อม API):**
```
[PMS HEX] 42 4D 00 1C 00 0E 00 13 00 14 00 0E 00 13 00 14 01 5A 00 EB 00 5B 00 2E 00 0E 00 04 00 02 01 AB
[29/04 14:18] SEND -> app.gistnorth.soc.cmu.ac.th | pm25=19.0 pm10=21.0 | RESPONSE: 200
```

**สถานะการส่ง:**
- **Online Mode:** ส่งข้อมูลทุก 1 วินาที หาก WiFi เชื่อมต่อ
- **Offline Mode:** ไม่ส่งข้อมูล แสดง "OFFLINE MODE" บนหน้าจอ
- **Low Battery Mode:** ปิด WiFi, ไม่ส่งข้อมูล
- **Sensor Disconnected:** ส่ง pm25 = 0, pm10 = 0

> **หมายเหตุ:** ค่า `temp` และ `rh` ไม่ได้ส่ง เนื่องจากอุปกรณ์ไม่มีเซ็นเซอร์วัด หากต้องการเพิ่มเซ็นเซอร์ DHT22 หรือ BME280 สามารถแก้ไขโค้ดใน `sendToGistNorth()` ได้

---

## การแก้ปัญหา (Troubleshooting)

### จอไม่แสดงผล
- ตรวจสอบว่า `DISPLAY_POWER_PIN` (GPIO 15) ถูกตั้งค่าเป็น HIGH
- ตรวจสอบการตั้งค่า TFT ใน `platformio.ini` (build_flags)
- ลองเปลี่ยนค่า `TFT_INVERSION_ON` เป็น `TFT_INVERSION_OFF` ถ้าสีกลับหัว

### เซ็นเซอร์ PMS ไม่ตรวจจับ
- ตรวจสอบการต่อสาย UART:
  - PMS TXD → ESP32 RX (GPIO 13)
  - PMS RXD → ESP32 TX (GPIO 17)
  - PMS SET → ESP32 GPIO 2
- ตรวจสอบว่า PMS_SET_PIN เป็น HIGH เมื่ออ่านข้อมูล
- ดู Serial Monitor สำหรับข้อความ "PMS sensor NOT connected"

### WiFi เชื่อมต่อไม่ได้
- ตรวจสอบ SSID และ PASSWORD ในโค้ด (src/main.cpp:51-52)
- ตรวจสอบว่า router รองรับ 2.4GHz (ESP32 ไม่รองรับ 5GHz)
- รอสูงสุด 10 วินาทีสำหรับการเชื่อมต่อ

### อ่านค่าแบตเตอรี่ไม่ถูกต้อง
- ปรับค่า `CAL` (line 34) ใน main.cpp
- ตรวจสอบค่า `DIVIDER` (line 32) ให้ตรงกับวงจรจริง
- วัดด้วย DMM และเปรียบเทียบกับค่าที่อ่านได้

### Deep Sleep ไม่ทำงาน
- ตรวจสอบว่า `RTC_SLOW_MEM` เปิดใช้งานใน deep sleep configuration
- ตรวจสอบว่าไม่มี peripheral ที่ทำให้ตื่น (RTC_GPIO)
- ตรวจสอบ wiring ของปุ่มกด (BOOT button = GPIO 0)

---

## License

โปรเจคนี้ใช้สัญญาอนุญาต [MIT License](LICENSE)
