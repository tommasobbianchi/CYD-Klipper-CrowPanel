// BTT Panda Touch / K-Touch — ESP32-S3, 800x480 RGB565 parallel panel (MIPI-RGB) + GT911 touch.
// Pin map from bigtreetech/PandaTouch docs/PINOUT.md + src/pt/pt_board.h (confirmed on hardware
// 2026-09-17) and the board's ESPHome config (pandatouch.yaml).
//
// Pins:
//   RGB data d0..d15 = B0..B4, G0..G5, R0..R4
//   B[17,18,48,47,39]  G[11,12,13,14,15,16]  R[6,7,8,9,10]
//   pclk 5  DE(henable) 38  hsync 42  vsync 45   backlight 21 (LEDC)
//   GT911: SDA 2  SCL 1  INT 40  RST 41
//   pclk 14.8 MHz, HS 4/16/16, VS 4/32/32

#ifdef CYD_SCREEN_DRIVER_ESP32_PANDATOUCH

#include "../screen_driver.h"
#include "lvgl.h"
#include "../../conf/global_config.h"
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Arduino.h>
#include <Wire.h>

#ifdef CYD_SCREEN_VERTICAL
#error "Vertical screen not supported with the ESP32_PANDATOUCH driver"
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[CYD_SCREEN_HEIGHT_PX * CYD_SCREEN_WIDTH_PX / 10];

// Pixel clock under test: overridden by an untracked pt_pclk.h while tuning the refresh rate.
#if __has_include("pt_pclk.h")
#include "pt_pclk.h"
#endif
#ifndef PT_PCLK_HZ
#define PT_PCLK_HZ 16000000 // measured on hardware: >=18 MHz shifts, 14 MHz loses sync (white), 16 MHz is clean
#endif

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Panel_RGB _panel_instance;
    lgfx::Bus_RGB _bus_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_GT911 _touch_instance;

public:
    LGFX()
    {
        // panel geometry
        {
            auto cfg = _panel_instance.config();
            cfg.memory_width = 800;
            cfg.memory_height = 480;
            cfg.panel_width = 800;
            cfg.panel_height = 480;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            _panel_instance.config(cfg);
        }

        // framebuffer in PSRAM (required for the RGB panel)
        {
            auto cfg = _panel_instance.config_detail();
            cfg.use_psram = 2; // 0 = SRAM only, 1 = half PSRAM / half SRAM, 2 = PSRAM only
            _panel_instance.config_detail(cfg);
        }

        // RGB bus
        {
            auto cfg = _bus_instance.config();
            cfg.panel = &_panel_instance;
            cfg.freq_write = PT_PCLK_HZ;

            cfg.pin_d0 = 17;  // B0
            cfg.pin_d1 = 18;  // B1
            cfg.pin_d2 = 48;  // B2
            cfg.pin_d3 = 47;  // B3
            cfg.pin_d4 = 39;  // B4
            cfg.pin_d5 = 11;  // G0
            cfg.pin_d6 = 12;  // G1
            cfg.pin_d7 = 13;  // G2
            cfg.pin_d8 = 14;  // G3
            cfg.pin_d9 = 15;  // G4
            cfg.pin_d10 = 16; // G5
            cfg.pin_d11 = 6;  // R0
            cfg.pin_d12 = 7;  // R1
            cfg.pin_d13 = 8;  // R2
            cfg.pin_d14 = 9;  // R3
            cfg.pin_d15 = 10; // R4

            cfg.pin_pclk = 5;
            cfg.pin_vsync = 45;
            cfg.pin_hsync = 42;
            cfg.pin_henable = 38; // DE

            cfg.hsync_polarity = 0;
            cfg.hsync_front_porch = 16;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch = 16;
            cfg.vsync_polarity = 0;
            cfg.vsync_front_porch = 32;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch = 32;
            cfg.pclk_idle_high = 0;
            cfg.de_idle_high = 0;

            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        // backlight (GPIO21)
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = 21;
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.light(&_light_instance);
        }

        // GT911 touch
        {
            auto cfg = _touch_instance.config();
            cfg.x_min = 0;
            cfg.x_max = 799;
            cfg.y_min = 0;
            cfg.y_max = 479;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port = 1;
            cfg.i2c_addr = 0x5D;
            cfg.pin_int = 40;
            cfg.pin_rst = -1; // reset done manually in screen_setup()
            cfg.pin_sda = 2;
            cfg.pin_scl = 1;
            cfg.freq = 400000;
            _touch_instance.config(cfg);
            // touch handled directly over Wire in screen_lv_touchRead()
        }

        setPanel(&_panel_instance);
    }
};

LGFX tft;

void screen_setBrightness(unsigned char brightness)
{
    tft.setBrightness(brightness);
}

void set_invert_display()
{
    // handled by panel_config.invert_colors if needed
}

void screen_lv_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void screen_lv_touchRead(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    // Direct GT911 read (LovyanGFX's touch path never delivered events on this board).
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
                    uint16_t y = Wire.read(); y |= Wire.read() << 8;                    // Measured: GT911 coordinates already match the upright image; only the user's 180 deg setting flips them
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

void screen_setup()
{
    Serial.printf("PT: psram=%d size=%u free=%u\n", psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    // LCD reset (ESPHome: reset_pin GPIO46) — hold low, then release before the RGB peripheral starts
    pinMode(46, OUTPUT);
    digitalWrite(46, LOW);
    delay(20);
    digitalWrite(46, HIGH);
    delay(150);
    // GT911 reset with INT held low -> I2C address 0x5D; it needs ~50 ms before it answers.
    // LovyanGFX only pulses RST and retries for ~6 ms, which is too short, so do it here and set pin_rst = -1.
    pinMode(40, OUTPUT);
    pinMode(41, OUTPUT);
    digitalWrite(40, LOW);
    digitalWrite(41, LOW);
    delay(10);
    digitalWrite(41, HIGH);
    delay(10);
    pinMode(40, INPUT);
    delay(60);
    // Diagnostic: is a GT911 answering on SDA2/SCL1, and at which address? (Wire released before LovyanGFX takes the pins)
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
    Serial.println("PT: tft.begin()");
    bool ok = tft.begin();
    Serial.printf("PT: tft.begin() -> %d\n", ok);
    // This panel is mounted 180 deg from LovyanGFX's default: invert the user setting.
    tft.setRotation(global_config.rotate_screen ? 0 : 2);
    Serial.printf("PT: touch init ok=%d\n", tft.touch() != nullptr);

    delay(500);

    tft.setBrightness(255);
    Serial.println("PT: brightness set");

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, CYD_SCREEN_WIDTH_PX * CYD_SCREEN_HEIGHT_PX / 10);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = CYD_SCREEN_WIDTH_PX;
    disp_drv.ver_res = CYD_SCREEN_HEIGHT_PX;
    disp_drv.flush_cb = screen_lv_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = screen_lv_touchRead;
    lv_indev_drv_register(&indev_drv);
}

#endif
