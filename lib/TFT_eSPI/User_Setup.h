// User_Setup.h สำหรับ TTGO T-Display (ST7789 135x240)
#define USER_SETUP_ID 25

#define ST7789_DRIVER
#define TFT_WIDTH  135
#define TFT_HEIGHT 240

// พินของ TTGO T-Display
#define TFT_MOSI 19
#define TFT_SCLK 18
#define TFT_CS    5
#define TFT_DC   16
#define TFT_RST  23
#define TFT_BL    4
#define TFT_BACKLIGHT_ON HIGH

// ฟอนต์ที่ใช้
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ความเร็ว SPI (ค่ามาตรฐานที่นิ่งกับบอร์ดนี้)
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
