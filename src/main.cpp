#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <BluetoothSerial.h>
#include <Preferences.h>

#define SCALE1_CLK   GPIO_NUM_27
#define SCALE1_DATA  GPIO_NUM_22
// Second scale on RGB LED pins (16=GRN, 17=BLU)
#define SCALE2_CLK   GPIO_NUM_16
#define SCALE2_DATA  GPIO_NUM_17

BluetoothSerial SerialBT;

static lgfx::Bus_SPI       bus;
static lgfx::Panel_ST7789  panel;
static lgfx::Light_PWM     light;
static lgfx::Touch_XPT2046 touch;
static LGFX_Device         display;

// ── Scale ──────────────────────────────────────────────────────────────

struct Scale {
    volatile int32_t  raw;
    int32_t  tare;
    volatile bool     new_data;
} s1
#ifdef SCALE2_CLK
  , s2
#endif
  ;

static hw_timer_t* poll_timer = NULL;

struct PollState {
    bool     prev;
    uint32_t code;
    int      bits;
};

static PollState ps1 = { HIGH, 0, 0 };
#ifdef SCALE2_CLK
static PollState ps2 = { HIGH, 0, 0 };
#endif

static void IRAM_ATTR poll_scales() {
    bool clk1 = (GPIO.in >> SCALE1_CLK) & 1;
    if (clk1 && !ps1.prev) {
        int bit = (GPIO.in >> SCALE1_DATA) & 1;
        ps1.code = (ps1.code >> 1) | (bit << 23);
        ps1.bits++;
        if (ps1.bits == 24) {
            s1.raw = ((ps1.code >> 20) & 1) ? (ps1.code & 0xFFFFF) : -(ps1.code & 0xFFFFF);
            s1.new_data = true;
            ps1.bits = 0;
            ps1.code = 0;
        }
    }
    ps1.prev = clk1;

#ifdef SCALE2_CLK
    bool clk2 = (GPIO.in >> SCALE2_CLK) & 1;
    if (clk2 && !ps2.prev) {
        int bit = (GPIO.in >> SCALE2_DATA) & 1;
        ps2.code = (ps2.code >> 1) | (bit << 23);
        ps2.bits++;
        if (ps2.bits == 24) {
            s2.raw = ((ps2.code >> 20) & 1) ? (ps2.code & 0xFFFFF) : -(ps2.code & 0xFFFFF);
            s2.new_data = true;
            ps2.bits = 0;
            ps2.code = 0;
        }
    }
    ps2.prev = clk2;
#endif
}

static void init_scales() {
    pinMode(SCALE1_CLK, INPUT);
    pinMode(SCALE1_DATA, INPUT);
#ifdef SCALE2_CLK
    pinMode(SCALE2_CLK, INPUT);
    pinMode(SCALE2_DATA, INPUT);
#endif
    poll_timer = timerBegin(0, 80, true);
    timerAttachInterrupt(poll_timer, &poll_scales, true);
    timerAlarmWrite(poll_timer, 15, true);
    timerAlarmEnable(poll_timer);
}

// ── Display init ───────────────────────────────────────────────────────

static void init_display() {
    {
        auto cfg = bus.config();
        cfg.freq_write   = 55000000;
        cfg.freq_read    = 16000000;
        cfg.use_lock     = true;
        cfg.dma_channel  = SPI_DMA_CH_AUTO;
        cfg.spi_host     = HSPI_HOST;
        cfg.pin_mosi     = GPIO_NUM_13;
        cfg.pin_miso     = GPIO_NUM_12;
        cfg.pin_sclk     = GPIO_NUM_14;
        cfg.pin_dc       = GPIO_NUM_2;
        cfg.spi_mode     = 0;
        cfg.spi_3wire    = false;
        bus.config(cfg);
        bus.init();
    }
    {
        auto cfg = light.config();
        cfg.pin_bl      = GPIO_NUM_21;
        cfg.freq        = 12000;
        cfg.pwm_channel = 7;
        cfg.offset      = 0;
        cfg.invert      = false;
        light.config(cfg);
        light.init(255);
    }
    {
        panel.bus(&bus);
        auto cfg = panel.config();
        cfg.pin_cs          = GPIO_NUM_15;
        cfg.offset_rotation = 0;
        cfg.bus_shared      = false;
        panel.config(cfg);
        panel.light(&light);
        display.setPanel(&panel);
    }
    display.init();
    display.setRotation(1);
    display.fillScreen(TFT_BLACK);
}

static void init_touch() {
    auto cfg = touch.config();
    cfg.x_min           = 300;
    cfg.x_max           = 3900;
    cfg.y_min           = 3700;
    cfg.y_max           = 200;
    cfg.pin_int         = -1;
    cfg.bus_shared      = false;
    cfg.spi_host        = -1;
    cfg.pin_sclk        = GPIO_NUM_25;
    cfg.pin_mosi        = GPIO_NUM_32;
    cfg.pin_miso        = GPIO_NUM_39;
    cfg.pin_cs          = GPIO_NUM_33;
    cfg.offset_rotation = 0 ^ 2;
    touch.config(cfg);
    display.getPanel()->setTouch(&touch);
    display.getPanel()->initTouch();
}

// ── Config (NVS) ───────────────────────────────────────────────────────

struct AxisConfig {
    bool reversed;
    bool enabled;
    bool x2;
};

AxisConfig z_cfg, x_cfg;

static const char* nvs_ns = "dro";

static void load_config() {
    Preferences p;
    p.begin(nvs_ns, true);
    z_cfg.reversed = p.getBool("z_rev", false);
    z_cfg.enabled  = p.getBool("z_en",  true);
    x_cfg.reversed = p.getBool("x_rev", false);
    x_cfg.enabled  = p.getBool("x_en",  true);
    x_cfg.x2       = p.getBool("x_x2",  false);
    p.end();
}

static void save_config() {
    Preferences p;
    p.begin(nvs_ns, false);
    p.putBool("z_rev", z_cfg.reversed);
    p.putBool("z_en",  z_cfg.enabled);
    p.putBool("x_rev", x_cfg.reversed);
    p.putBool("x_en",  x_cfg.enabled);
    p.putBool("x_x2",  x_cfg.x2);
    p.end();
}

// ── Screen state ──────────────────────────────────────────────────────

enum State { ST_DRO, ST_AXIS_SEL, ST_Z_CFG, ST_X_CFG };
State state = ST_DRO;
bool need_redraw = true;

// ── Display helpers ────────────────────────────────────────────────────

static void draw_btn(const char* label, int x, int y, int w, int h,
                     bool filled, uint16_t fg, uint16_t bg) {
    if (filled) {
        display.fillRect(x, y, w, h, bg);
    } else {
        display.fillRect(x, y, w, h, TFT_BLACK);
    }
    display.drawRect(x, y, w, h, TFT_WHITE);
    display.setTextDatum(middle_center);
    display.setTextColor(fg, filled ? bg : TFT_BLACK);
    display.setTextSize(2);
    display.drawString(label, x + w / 2, y + h / 2);
}

static void draw_toggle_row(const char* label, int x, int y,
                            const char* opt0, const char* opt1,
                            bool val) {
    display.setTextDatum(middle_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.drawString(label, x, y + 14);

    draw_btn(opt0, 170, y, 80, 28, !val, TFT_WHITE, TFT_DARKGREY);
    draw_btn(opt1, 255, y, 80, 28, val,  TFT_WHITE, TFT_DARKGREY);
}

// ── Screen draw functions ──────────────────────────────────────────────

// Apply config AFTER tare subtraction: (raw - tare) is the relative offset
static int32_t apply_cfg(int32_t raw_minus_tare, const AxisConfig& cfg) {
    if (!cfg.enabled) return 0;
    return cfg.reversed ? -raw_minus_tare : raw_minus_tare;
}

static void zero_s1() { s1.tare = s1.raw; }
#ifdef SCALE2_CLK
static void zero_s2() { s2.tare = s2.raw; }
#endif

static void draw_dro_screen() {
    display.fillScreen(TFT_BLACK);

    // DRO lines
    {
        int32_t c = apply_cfg(s1.raw - s1.tare, z_cfg);
        float mm = c / 100.0f;
        char buf[24];
        snprintf(buf, sizeof(buf), "Z:%7.2f mm", mm);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.setTextSize(3);
        display.drawString(buf, 140, 100);

        display.fillRect(270, 85, 40, 30, TFT_BLACK);
        display.drawRect(270, 85, 40, 30, TFT_WHITE);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.setTextSize(2);
        display.drawString("0", 290, 100);
    }

#ifdef SCALE2_CLK
    {
        int32_t c = apply_cfg(s2.raw, x_cfg);
        if (x_cfg.x2) c *= 2;
        float mm = (c - s2.tare) / 100.0f;
        char buf[24];
        snprintf(buf, sizeof(buf), "X:%7.2f mm", mm);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_GREEN, TFT_BLACK);
        display.setTextSize(3);
        display.drawString(buf, 140, 140);

        display.fillRect(270, 125, 40, 30, TFT_BLACK);
        display.drawRect(270, 125, 40, 30, TFT_WHITE);
        display.setTextDatum(middle_center);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.setTextSize(2);
        display.drawString("0", 290, 140);
    }
#endif

    // Menu button
    draw_btn("Menu", 270, 5, 45, 25, false, TFT_WHITE, TFT_BLACK);
    need_redraw = false;
}

static void draw_axis_sel_screen() {
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.drawString("Select Axis", 160, 30);

    draw_btn("Z Axis", 60, 70, 200, 50, false, TFT_CYAN, TFT_BLACK);
    draw_btn("X Axis", 60, 130, 200, 50, false, TFT_CYAN, TFT_BLACK);
    draw_btn("Back", 60, 200, 200, 35, false, TFT_WHITE, TFT_BLACK);
    need_redraw = false;
}

static void draw_z_cfg_screen() {
    display.fillScreen(TFT_BLACK);
    draw_btn("< Back", 10, 5, 80, 28, false, TFT_WHITE, TFT_BLACK);

    display.setTextDatum(middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.drawString("Z Axis Settings", 160, 40);

    draw_toggle_row("Direction", 30, 80, "Normal", "Reverse", z_cfg.reversed);
    draw_toggle_row("Enabled",  30, 130, "Yes",    "No",     !z_cfg.enabled);
    need_redraw = false;
}

static void draw_x_cfg_screen() {
    display.fillScreen(TFT_BLACK);
    draw_btn("< Back", 10, 5, 80, 28, false, TFT_WHITE, TFT_BLACK);

    display.setTextDatum(middle_center);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextSize(2);
    display.drawString("X Axis Settings", 160, 40);

    draw_toggle_row("Direction", 30, 80,  "Normal", "Reverse", x_cfg.reversed);
    draw_toggle_row("Enabled",  30, 130, "Yes",    "No",      !x_cfg.enabled);
    draw_toggle_row("X Double", 30, 180, "Off",    "On",      x_cfg.x2);
    need_redraw = false;
}

// ── Touch handlers ─────────────────────────────────────────────────────

static bool tp_in(int x, int y, int w, int h, const lgfx::touch_point_t& tp) {
    return tp.x >= x && tp.x < x + w && tp.y >= y && tp.y < y + h;
}

static void reset_poll_state() {
    // Force re-initialization of poll statics on next call
    extern void _poll_scales_reset();
}

static void enter_menu() {
    timerAlarmDisable(poll_timer);
}

static void exit_menu() {
    s1.new_data = false;
#ifdef SCALE2_CLK
    s2.new_data = false;
#endif
    timerAlarmEnable(poll_timer);
}

static void handle_dro_touch(const lgfx::touch_point_t& tp) {
    if (tp_in(270, 5, 45, 25, tp)) {
        enter_menu();
        state = ST_AXIS_SEL;
        need_redraw = true;
        return;
    }
    if (tp_in(270, 85, 40, 30, tp)) {
        zero_s1();
        need_redraw = true;
        return;
    }
#ifdef SCALE2_CLK
    if (tp_in(270, 125, 40, 30, tp)) {
        zero_s2();
        need_redraw = true;
        return;
    }
#endif
}

static void handle_axis_sel_touch(const lgfx::touch_point_t& tp) {
    if (tp_in(60, 70, 200, 50, tp)) {
        state = ST_Z_CFG;
        need_redraw = true;
    } else if (tp_in(60, 130, 200, 50, tp)) {
        state = ST_X_CFG;
        need_redraw = true;
    } else if (tp_in(60, 200, 200, 35, tp)) {
        exit_menu();
        state = ST_DRO;
        need_redraw = true;
    }
}

static void handle_z_cfg_touch(const lgfx::touch_point_t& tp) {
    if (tp_in(10, 5, 80, 28, tp)) {
        save_config();
        state = ST_AXIS_SEL;
        need_redraw = true;
        return;
    }
    // Direction row at y=80
    if (tp.y >= 80 && tp.y < 110) {
        if (tp.x >= 170 && tp.x < 255) {
            z_cfg.reversed = false;  // Normal
            need_redraw = true;
        } else if (tp.x >= 260 && tp.x < 345) {
            z_cfg.reversed = true;   // Reverse
            need_redraw = true;
        }
        return;
    }
    // Enabled row at y=130
    if (tp.y >= 130 && tp.y < 160) {
        if (tp.x >= 170 && tp.x < 255) {
            z_cfg.enabled = true;    // Yes
            need_redraw = true;
        } else if (tp.x >= 260 && tp.x < 345) {
            z_cfg.enabled = false;   // No
            need_redraw = true;
        }
        return;
    }
}

static void handle_x_cfg_touch(const lgfx::touch_point_t& tp) {
    if (tp_in(10, 5, 80, 28, tp)) {
        save_config();
        state = ST_AXIS_SEL;
        need_redraw = true;
        return;
    }
    // Direction row at y=80
    if (tp.y >= 80 && tp.y < 110) {
        if (tp.x >= 170 && tp.x < 255) {
            x_cfg.reversed = false;
            need_redraw = true;
        } else if (tp.x >= 260 && tp.x < 345) {
            x_cfg.reversed = true;
            need_redraw = true;
        }
        return;
    }
    // Enabled row at y=130
    if (tp.y >= 130 && tp.y < 160) {
        if (tp.x >= 170 && tp.x < 255) {
            x_cfg.enabled = true;
            need_redraw = true;
        } else if (tp.x >= 260 && tp.x < 345) {
            x_cfg.enabled = false;
            need_redraw = true;
        }
        return;
    }
    // X Double row at y=180
    if (tp.y >= 180 && tp.y < 210) {
        if (tp.x >= 170 && tp.x < 255) {
            x_cfg.x2 = false;
            need_redraw = true;
        } else if (tp.x >= 260 && tp.x < 345) {
            x_cfg.x2 = true;
            need_redraw = true;
        }
        return;
    }
}

// ── Setup / Loop ───────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("ready");
    load_config();
    init_display();
    init_touch();
    init_scales();
    SerialBT.begin("ReDRO");
    need_redraw = true;
}

void loop() {
    // ── Draw current screen if needed ──
    if (need_redraw) {
        switch (state) {
            case ST_DRO:     draw_dro_screen();     break;
            case ST_AXIS_SEL: draw_axis_sel_screen(); break;
            case ST_Z_CFG:   draw_z_cfg_screen();   break;
            case ST_X_CFG:   draw_x_cfg_screen();   break;
        }
    }

    // ── Scale data processing (only in DRO mode) ──
    if (state == ST_DRO) {
        bool updated = false;
        if (s1.new_data) {
            s1.new_data = false;
            int32_t c = apply_cfg(s1.raw - s1.tare, z_cfg);
            Serial.printf("Z raw=%d\n", c);
            updated = true;
            {
                char last[20] = "";
                static char prev[20] = "";
                float mm = c / 100.0f;
                char buf[20];
                snprintf(buf, sizeof(buf), "Z:%7.2f mm", mm);
                if (strcmp(buf, prev) != 0) {
                    strcpy(prev, buf);
                    display.fillRect(10, 85, 250, 30, TFT_BLACK);
                    display.setTextDatum(middle_center);
                    display.setTextColor(TFT_GREEN, TFT_BLACK);
                    display.setTextSize(3);
                    display.drawString(buf, 140, 100);
                }
            }
        }
#ifdef SCALE2_CLK
        if (s2.new_data) {
            s2.new_data = false;
            int32_t c = apply_cfg(s2.raw - s2.tare, x_cfg);
            if (x_cfg.x2) c *= 2;
            Serial.printf("X raw=%d\n", c);
            updated = true;
            {
                static char prev[20] = "";
                float mm = c / 100.0f;
                char buf[20];
                snprintf(buf, sizeof(buf), "X:%7.2f mm", mm);
                if (strcmp(buf, prev) != 0) {
                    strcpy(prev, buf);
                    display.fillRect(10, 125, 250, 30, TFT_BLACK);
                    display.setTextDatum(middle_center);
                    display.setTextColor(TFT_GREEN, TFT_BLACK);
                    display.setTextSize(3);
                    display.drawString(buf, 140, 140);
                }
            }
        }
#endif
        if (updated) {
#ifdef SCALE2_CLK
            SerialBT.printf("x%d;y%d;z0;w0;\n", s2.raw, s1.raw);
#else
            SerialBT.printf("x%d;y0;z0;w0;\n", s1.raw);
#endif
        }
    }

    // ── Serial commands ──
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'z') zero_s1();
#ifdef SCALE2_CLK
        if (c == 'x') zero_s2();
#endif
    }

    // ── Touch ──
    lgfx::touch_point_t tp;
    if (display.getTouch(&tp, 1)) {
        switch (state) {
            case ST_DRO:     handle_dro_touch(tp);     break;
            case ST_AXIS_SEL: handle_axis_sel_touch(tp); break;
            case ST_Z_CFG:   handle_z_cfg_touch(tp);   break;
            case ST_X_CFG:   handle_x_cfg_touch(tp);   break;
        }
        delay(50);
    }

    delay(5);
}
