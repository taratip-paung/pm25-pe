# PE_PMS9103M-2

เครื่องวัดคุณภาพอากาศ PM2.5 แบบพกพา ทำงานบนไมโครคอนโทรลเลอร์ ESP32 (LILYGO T-Display) อ่านค่าฝุ่นละอองด้วยเซ็นเซอร์ PMS9103M แสดงผลบนจอ TFT สี และส่งข้อมูลไปยัง ThingsBoard IoT Platform พร้อมระบบจัดการพลังงานแบตเตอรี่อัจฉริยะ

---

## คุณสมบัติเด่น

- วัดฝุ่นละออง **PM1.0, PM2.5, PM10** ด้วยเซ็นเซอร์เลเซอร์ PMS9103M
- แสดงผลค่า PM2.5 แบบเรียลไทม์พร้อม **ระดับสี** ตามมาตรฐานคุณภาพอากาศ
- ส่งข้อมูล Telemetry ไปยัง **ThingsBoard** ผ่าน HTTPS ทุก 60 วินาที
- ระบบ **จัดการพลังงานแบตเตอรี่** แบบหลายระดับ (Warning → Safe Mode → Critical)
- วัดแรงดันแบตเตอรี่ผ่าน ADC + Voltage Divider พร้อมแสดง % และสถานะชาร์จ
- **Deep Sleep / Auto Sleep** เพื่อประหยัดพลังงาน (ตื่นด้วยปุ่มกดหรือ Timer)
- ซิงค์เวลาผ่าน **NTP** (UTC+7)
- แจ้งเตือนเมื่อแบตเตอรี่ต่ำ / ไม่มี WiFi / ไม่พบเซ็นเซอร์

---

## ฮาร์ดแวร์ที่ใช้

| อุปกรณ์ | รายละเอียด |
|---------|-----------|
| **ไมโครคอนโทรลเลอร์** | LILYGO T-Display (ESP32 + ST7789 135×240 TFT) |
| **เซ็นเซอร์ฝุ่น** | PMS9103M Laser Dust Sensor (Plantower) |
| **แบตเตอรี่** | Li-Po Battery (วัดแรงดันผ่าน Voltage Divider 100k/100k) |

---

## การเชื่อมต่อวงจร (Pin Mapping)

| ฟังก์ชัน | GPIO | หมายเหตุ |
|----------|------|---------|
| PMS TX → ESP32 RX | **GPIO 13** | UART1 RX |
| PMS RX ← ESP32 TX | **GPIO 17** | UART1 TX |
| PMS SET (Sleep/Wake) | **GPIO 2** | HIGH = ทำงาน, LOW = หลับ |
| Battery ADC | **GPIO 34** | ตัวแบ่งแรงดัน x2 |
| ปุ่มกดบน | **GPIO 35** | RTC IO, ไม่มี internal pull-up |
| ปุ่มกดล่าง (BOOT) | **GPIO 0** | RTC IO, Deep Sleep Wake Source |
| TFT MOSI | GPIO 19 | |
| TFT SCLK | GPIO 18 | |
| TFT CS | GPIO 5 | |
| TFT DC | GPIO 16 | |
| TFT BL (Backlight) | GPIO 4 | |

---

## ซอฟต์แวร์ที่ต้องใช้

- **[PlatformIO](https://platformio.org/)** (แนะนำ) หรือ Arduino IDE
- **ESP32 Platform** (Espressif32)

### ไลบรารีที่ใช้

| ไลบรารี | เวอร์ชัน | 用途 |
|---------|---------|------|
| [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) | ^2.5.43 | ควบคุมจอ TFT ST7789 |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^6.21.3 | สร้าง JSON payload สำหรับ ThingsBoard |

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

### 3. แก้ไขการตั้งค่า

แก้ไขค่า config ในไฟล์ `src/main.cpp` ก่อน upload:

```cpp
// WiFi
const char* WIFI_SSID = "ชื่อWiFiของคุณ";
const char* WIFI_PASSWORD = "รหัสWiFi";

// ThingsBoard
const char* TB_SERVER = "tb4.rtk-landmos.com";
const char* TB_TOKEN  = "Tokenของอุปกรณ์คุณ";
const char* DEVICE_NAME = "PM25-PE-2";
```

> **สำคัญ:** อย่า commit credentials จริงลง public repository

---

## การตั้งค่าเพิ่มเติม

### ระดับแบตเตอรี่

```cpp
#define VBAT_WARN     3.60f   // เตือนแบตต่ำ
#define VBAT_CUTOFF   3.40f   // เข้าโหมดประหยัด
#define VBAT_CRITICAL 3.35f   // วิกฤต → Deep Sleep ทันที
#define VBAT_RECOVER  3.70f   // กลับมาทำงานปกติ
```

### ระยะเวลา

```cpp
const unsigned long SEND_INTERVAL = 60000;      // ส่งข้อมูลทุก 60 วินาที
const unsigned long PMS_WARMUP_TIME = 30000;     // รอ warmup เซ็นเซอร์ 30 วินาที
const unsigned long AUTO_SLEEP_TIME = 120000;    // Auto Sleep หลัง 2 นาที
const unsigned long PMS_READ_INTERVAL = 5000;    // อ่านเซ็นเซอร์ทุก 5 วินาที
```

---

## โหมดการทำงาน

```
┌─────────────┐  กดปุ่ม   ┌──────────────┐  30 วินาที  ┌───────────────┐  2 นาที  ┌──────────┐
│  Deep Sleep │ ────────→ │  Wake Up     │ ──────────→ │  อ่านค่า PM    │ ───────→ │ Auto     │
│  (ประหยัด)  │ ←──────── │  + WiFi +NTP │             │  ส่ง ThingsBoard│          │  Sleep   │
└─────────────┘  หมดเวลา  └──────────────┘             └───────────────┘          └──────────┘
       ↑                                                                          │
       │  แบตต่ำมาก                                                              │
       ├──────────────────────────────────────────────────────────────────────────┘
       │       Ultra Low Power Deep Sleep (ตรวจแบตทุก 60 นาที)
       │
       ├── Critical (< 3.35V)  → Deep Sleep ทันที, Timer 60 นาที
       ├── Cutoff  (< 3.40V)   → Safe Mode + Deep Sleep, Timer 30 นาที
       └── Warning (< 3.60V)   → แจ้งเตือนหน้าจอ, ทำงานต่อ
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
├── platformio.ini          # การตั้งค่า PlatformIO + Build Flags
├── src/
│   └── main.cpp            # โค้ดหลักทั้งหมด
├── lib/
│   └── TFT_eSPI/
│       └── User_Setup.h    # การตั้งค่าจอ TFT (override)
├── include/
├── test/
├── .gitignore
├── LICENSE                 # MIT License
└── README.md               # เอกสารนี้
```

---

## ข้อมูลที่ส่งไป ThingsBoard

```json
{
  "pm1_0": 12,
  "pm2_5": 25,
  "pm10": 35,
  "battery_voltage": 3.85,
  "battery_percent": 60,
  "battery_warn": false,
  "battery_lowmode": false,
  "battery_critical": false,
  "charging": false,
  "device": "PM25-PE-2",
  "rssi": -52,
  "active": true,
  "pms_connected": true
}
```

---

## License

โปรเจคนี้ใช้สัญญาอนุญาต [MIT License](LICENSE)
