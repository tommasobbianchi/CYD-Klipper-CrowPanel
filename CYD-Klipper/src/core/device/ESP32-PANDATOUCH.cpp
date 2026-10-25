// BTT Panda Touch / K-Touch — ESP32-S3, 5.0" 800x480 RGB565 parallel panel (DE mode) + GT911 touch.
// Panel module QX05ZJGI70N-05545Y datasheet (bigtreetech/PandaTouch_PlatformIO docs/): DCLK 23 / 25 / 27 MHz (min/typ/max),
// Th 816 typ (back/front porch 8/8), Tv 512 typ (16/16), pulse widths 4 -> ~59.8 Hz at 25 MHz.
// BTT's template runs it at 14.8 MHz (~32 Hz), out of spec, which is why it tears; we run the datasheet timing.
//
//   RGB data d0..d15 = B0..B4, G0..G5, R0..R4
//   B[17,18,48,47,39]  G[11,12,13,14,15,16]  R[6,7,8,9,10]
//   pclk 5  DE 38  hsync 42  vsync 45 (not routed)  LCD reset 46  backlight 21 (PWM)
//   GT911: SDA 2  SCL 1  INT 40  RST 41, address 0x5D
//
// Display path: ESP-IDF 5 esp_lcd RGB panel with a DRAM bounce buffer (needs the Arduino 3.x core): the panel DMA reads
// 10 lines at a time from internal RAM, so WiFi/JSON traffic on the PSRAM bus no longer shifts the picture.

#ifdef CYD_SCREEN_DRIVER_ESP32_PANDATOUCH

#include "../screen_driver.h"
#include "lvgl.h"
#include "../../conf/global_config.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"

#ifdef CYD_SCREEN_VERTICAL
#error "Vertical screen not supported with the ESP32_PANDATOUCH driver"
#endif

#ifndef PT_PCLK_KHZ
#define PT_PCLK_KHZ 14500          // measured on the panel: clean at 14.5 MHz (BTT ships 14.8); 23+ MHz shifts with this memory config
#endif
#define PT_PCLK_MIN_KHZ 12000      // below the datasheet min (23 MHz): BTT's own firmware runs 14.8 MHz, so allow trimming down to it
#define PT_PCLK_MAX_KHZ 27000      // datasheet max
#ifndef PT_BOUNCE_LINES
#define PT_BOUNCE_LINES 20         // BTT template uses 10; 20 fits because LVGL no longer needs its own draw buffer
#endif

static const int PIN_BL = 21;
static const int PIN_LCD_RST = 46;

// Horizontal / vertical timing (datasheet typical: Th 816 = 800 + 8 + 8, Tv 512 = 480 + 16 + 16; pulses 4 inside the porches)
static const int HSW = 4, HBP = 4, HFP = 8;
static const int VSW = 4, VBP = 12, VFP = 16;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* g_fb = nullptr;      // the panel frame buffer (PSRAM); LVGL renders straight into it (direct mode)
static esp_lcd_panel_handle_t g_panel = nullptr;
static volatile uint32_t g_vsync = 0;
uint32_t pt_vsync_count() { return g_vsync; }          // frames actually scanned out, for the measured refresh rate
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*)
{
    g_vsync++;
    return false;
}

// Pixel clock chosen in the KlipperScreen "Display" settings (kHz, namespace "ks"); the datasheet window bounds it.
uint32_t pt_pclk_khz()
{
    Preferences p; p.begin("ks", true);
    uint32_t v = p.getUInt("pclk_khz", PT_PCLK_KHZ);
    p.end();
    return v < PT_PCLK_MIN_KHZ || v > PT_PCLK_MAX_KHZ ? PT_PCLK_KHZ : v;
}
void pt_pclk_set_khz(uint32_t khz)
{
    Preferences p; p.begin("ks", false);
    p.putUInt("pclk_khz", khz);
    p.end();
}
static void pt_pclk_reset()
{
    Preferences p; p.begin("ks", false);
    p.remove("pclk_khz");
    p.end();
}

void screen_setBrightness(unsigned char brightness)
{
    ledcWrite(PIN_BL, brightness);
}

void set_invert_display()
{
}

void screen_lv_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    // LVGL (direct mode) already drew into the frame buffer; make the rows it touched visible to the panel DMA.
    // The user "rotate 180" setting is not supported in direct mode (the panel scans upright by default).
    uintptr_t start = (uintptr_t)(g_fb + (size_t)area->y1 * CYD_SCREEN_WIDTH_PX + area->x1) & ~(uintptr_t)63;
    uintptr_t end = ((uintptr_t)(g_fb + (size_t)area->y2 * CYD_SCREEN_WIDTH_PX + area->x2 + 1) + 63) & ~(uintptr_t)63;
    uintptr_t fb_end = (uintptr_t)(g_fb + (size_t)CYD_SCREEN_WIDTH_PX * CYD_SCREEN_HEIGHT_PX);
    if (end > fb_end) end = fb_end;
    esp_cache_msync((void*)start, end - start, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    lv_disp_flush_ready(disp);
}

void screen_lv_touchRead(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    // Direct GT911 read (a generic touch driver never delivered events on this board).
    static uint16_t touchX = 0, touchY = 0;
    static bool touched = false;

    Wire.beginTransmission(0x5D);
    Wire.write(0x81); Wire.write(0x4E);
    uint8_t tx = Wire.endTransmission(false);
    int rx = tx == 0 ? Wire.requestFrom(0x5D, 1) : -1;
    if (tx == 0 && rx == 1)
    {
        uint8_t st = Wire.read();
        if (st & 0x80) // buffer ready
        {
            touched = (st & 0x0F) > 0;
            if (touched)
            {
                Wire.beginTransmission(0x5D);
                Wire.write(0x81); Wire.write(0x50);
                if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x5D, 4) == 4)
                {
                    uint16_t x = Wire.read(); x |= Wire.read() << 8;
                    uint16_t y = Wire.read(); y |= Wire.read() << 8;
                    // Measured: GT911 coordinates already match the upright image; only the user's 180 deg setting flips them
                    touchX = global_config.rotate_screen ? (CYD_SCREEN_WIDTH_PX - 1 - x) : x;
                    touchY = global_config.rotate_screen ? (CYD_SCREEN_HEIGHT_PX - 1 - y) : y;
                }
            }
            Wire.beginTransmission(0x5D); // clear status
            Wire.write(0x81); Wire.write(0x4E); Wire.write(0);
            Wire.endTransmission();
        }
    }

    if (touchX > CYD_SCREEN_WIDTH_PX || touchY > CYD_SCREEN_HEIGHT_PX)
    {
        LOG_LN("X or Y outside of expected parameters..");
    }
    else
    {
        data->state = touched ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

static void rgb_panel_init(uint32_t pclk_hz)
{
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = pclk_hz;
    cfg.timings.h_res = 800;
    cfg.timings.v_res = 480;
    cfg.timings.hsync_pulse_width = HSW;
    cfg.timings.hsync_back_porch = HBP;
    cfg.timings.hsync_front_porch = HFP;
    cfg.timings.vsync_pulse_width = VSW;
    cfg.timings.vsync_back_porch = VBP;
    cfg.timings.vsync_front_porch = VFP;
    cfg.timings.flags.pclk_active_neg = 1;      // BTT template + measured
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 1;
    cfg.bounce_buffer_size_px = 800 * PT_BOUNCE_LINES;
    cfg.dma_burst_size = 64;
    cfg.hsync_gpio_num = 42;                    // parked: not routed on the FPC, DE mode
    cfg.vsync_gpio_num = 45;
    cfg.de_gpio_num = 38;
    cfg.pclk_gpio_num = 5;
    cfg.disp_gpio_num = -1;
    const int data[16] = {17, 18, 48, 47, 39,        // B0..B4
                          11, 12, 13, 14, 15, 16,    // G0..G5
                          6, 7, 8, 9, 10};           // R0..R4
    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data[i];
    cfg.flags.fb_in_psram = 1;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(g_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(g_panel));
    void* fb = nullptr;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(g_panel, 1, &fb));
    g_fb = (lv_color_t*)fb;
    memset(g_fb, 0, (size_t)CYD_SCREEN_WIDTH_PX * CYD_SCREEN_HEIGHT_PX * sizeof(lv_color_t));
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = on_vsync;
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr));
}

void screen_setup()
{
    Serial.printf("PT: psram=%d size=%u free=%u\n", psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    // LCD reset (ESPHome: reset_pin GPIO46) — hold low, then release before the RGB peripheral starts
    pinMode(PIN_LCD_RST, OUTPUT);
    digitalWrite(PIN_LCD_RST, LOW);
    delay(20);
    digitalWrite(PIN_LCD_RST, HIGH);
    delay(150);
    // GT911 reset with INT held low -> I2C address 0x5D; it needs ~50 ms before it answers.
    pinMode(40, OUTPUT);
    pinMode(41, OUTPUT);
    digitalWrite(40, LOW);
    digitalWrite(41, LOW);
    delay(10);
    digitalWrite(41, HIGH);
    delay(10);
    pinMode(40, INPUT);
    delay(60);
    Wire.begin(2, 1, 400000);
    for (uint8_t a : {0x5D, 0x14})
    {
        Wire.beginTransmission(a);
        Wire.write(0x81); Wire.write(0x40); // product id register 0x8140
        bool ack = Wire.endTransmission() == 0;
        char id[5] = {0};
        if (ack && Wire.requestFrom((int)a, 4) == 4)
            for (int i = 0; i < 4; i++) id[i] = Wire.read();
        Serial.printf("PT: i2c 0x%02X ack=%d id='%s'\n", a, ack, id);
    }
    // Safe mode: touching the screen while it boots drops a user-chosen pixel clock back to the datasheet typical.
    Wire.beginTransmission(0x5D); Wire.write(0x81); Wire.write(0x4E);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(0x5D, 1) == 1 && (Wire.read() & 0x8F) > 0x80)
    {
        pt_pclk_reset();
        Serial.println("PT: touch at boot -> pixel clock reset to default");
    }

    uint32_t khz = pt_pclk_khz();
    Serial.printf("PT: pixel clock %u kHz = %.1f Hz, bounce %dx800\n", (unsigned)khz,
                  khz * 1000.0 / ((800 + HSW + HBP + HFP) * (480 + VSW + VBP + VFP)), PT_BOUNCE_LINES);
    rgb_panel_init(khz * 1000);
    Serial.println("PT: rgb panel up");

    ledcAttach(PIN_BL, 44100, 8);
    screen_setBrightness(255);
    Serial.println("PT: brightness set");

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, g_fb, NULL, CYD_SCREEN_WIDTH_PX * CYD_SCREEN_HEIGHT_PX);   // the frame buffer itself

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = CYD_SCREEN_WIDTH_PX;
    disp_drv.ver_res = CYD_SCREEN_HEIGHT_PX;
    disp_drv.flush_cb = screen_lv_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.direct_mode = 1;
    disp_drv.full_refresh = 0;
    // measurement aid: how long a whole refresh cycle (render + flush of all dirty areas) takes
    disp_drv.monitor_cb = [](lv_disp_drv_t*, uint32_t ms, uint32_t px) {
        if (ms >= 40) Serial.printf("PT: refresh %u ms, %u px\n", (unsigned)ms, (unsigned)px);
    };
    lv_disp_drv_register(&disp_drv);

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = screen_lv_touchRead;
    lv_indev_drv_register(&indev_drv);
}

#endif
