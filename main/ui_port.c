/*
 * LVGL 9 glue: display driver on top of lcd.c, tick source, render task and
 * a lock so other tasks can touch widgets.
 *
 * Buffers mirror the vendor's LVGL 8 setup: two LCD_BUF_LINES-line RGB565
 * buffers in internal DMA-capable RAM, partial render mode.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "lvgl.h"
#if LVGL_VERSION_MAJOR == 9
#include "lvgl_private.h"   /* indev read timer period (no public setter) */
#endif

#include "board.h"
#include "lcd.h"
#include "touch.h"
#include "ui_port.h"
#include "devcon.h"

static const char *TAG = "ui_port";

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static SemaphoreHandle_t s_lock;
static int16_t s_last_x, s_last_y;
static bool s_pressed;

/* ---- rendering / input latency counters (devcon "perf") -----------------
 * flush_*: flush_cb entry -> DMA-done ISR, i.e. how long a chunk occupies the
 * panel.  render_*: time spent inside lv_timer_handler (layout + raster).
 * press_*: first PRESSED sample -> the flush that shows its effect. */
static volatile uint32_t s_perf_frames, s_flush_sum, s_flush_max;
static volatile uint32_t s_render_sum, s_render_max;
static volatile uint32_t s_press_sum, s_press_max, s_press_n;
static volatile uint32_t s_flush_t0, s_press_at;
static volatile bool s_press_pending;
static bool s_was_down;
static uint32_t s_perf_t0;

static inline void note_press(bool down)
{
    if (down && !s_was_down) {
        s_press_at = (uint32_t)esp_timer_get_time();
        s_press_pending = true;
    }
    s_was_down = down;
}

/* Raw controller coordinates -> screen. Vendor uses them as-is for the
 * 240x320 panel; adjust here if the axes turn out mirrored/swapped. */
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY 0
#endif
#ifndef TOUCH_MIRROR_X
#define TOUCH_MIRROR_X 0
#endif
#ifndef TOUCH_MIRROR_Y
#define TOUCH_MIRROR_Y 0
#endif

/* injected touch (developer console): the finger moves from (x0,y0) to
 * (x1,y1) over the hold time */
static int16_t s_inj_x0, s_inj_y0, s_inj_x1, s_inj_y1;
static uint32_t s_inj_from_ms, s_inj_until_ms;

void ui_port_inject_swipe(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t ms)
{
    s_inj_x0 = x0;
    s_inj_y0 = y0;
    s_inj_x1 = x1;
    s_inj_y1 = y1;
    s_inj_from_ms = esp_timer_get_time() / 1000;
    s_inj_until_ms = s_inj_from_ms + ms;
}

void ui_port_inject_touch(int16_t x, int16_t y, uint32_t hold_ms)
{
    ui_port_inject_swipe(x, y, x, y, hold_ms);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    uint32_t now = esp_timer_get_time() / 1000;
    if (now < s_inj_until_ms) {
        int32_t t = now - s_inj_from_ms, d = s_inj_until_ms - s_inj_from_ms;
        s_last_x = s_inj_x0 + (s_inj_x1 - s_inj_x0) * t / d;
        s_last_y = s_inj_y0 + (s_inj_y1 - s_inj_y0) * t / d;
        note_press(true);
        s_pressed = true;
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        return;
    }
    if (touch_read(&x, &y)) {
        if (TOUCH_SWAP_XY) { uint16_t t = x; x = y; y = t; }
        if (TOUCH_MIRROR_X) x = LCD_H_RES - 1 - x;
        if (TOUCH_MIRROR_Y) y = LCD_V_RES - 1 - y;
        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;
        s_last_x = x;
        s_last_y = y;
        note_press(true);
        s_pressed = true;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        note_press(false);
        s_pressed = false;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = s_last_x;
    data->point.y = s_last_y;
}

bool ui_port_touch_state(int16_t *x, int16_t *y)
{
    *x = s_last_x;
    *y = s_last_y;
    return s_pressed;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    s_flush_t0 = (uint32_t)esp_timer_get_time();
    devcon_mirror(a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    lcd_draw_bitmap(a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
    /* lv_display_flush_ready() is called from the DMA-done ISR */
}

#ifdef LCD_ALIGN_PX
/* The C706 panel wants its CASET/RASET window on LCD_ALIGN_PX boundaries
 * (the vendor driver warns about anything else): grow every dirty area. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~(LCD_ALIGN_PX - 1);
    a->y1 &= ~(LCD_ALIGN_PX - 1);
    a->x2 = ((a->x2 + LCD_ALIGN_PX) & ~(LCD_ALIGN_PX - 1)) - 1;
    a->y2 = ((a->y2 + LCD_ALIGN_PX) & ~(LCD_ALIGN_PX - 1)) - 1;
    if (a->x2 >= LCD_H_RES) a->x2 = LCD_H_RES - 1;
    if (a->y2 >= LCD_V_RES) a->y2 = LCD_V_RES - 1;
}
#endif

static bool IRAM_ATTR on_flush_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    uint32_t dt = now - s_flush_t0;
    s_perf_frames = s_perf_frames + 1;
    s_flush_sum = s_flush_sum + dt;
    if (dt > s_flush_max) s_flush_max = dt;
    if (s_press_pending) {
        uint32_t lat = now - s_press_at;
        s_press_sum = s_press_sum + lat;
        s_press_n = s_press_n + 1;
        if (lat > s_press_max) s_press_max = lat;
        s_press_pending = false;
    }
    lv_display_flush_ready(s_disp);
    return false;
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        ui_lock();
        uint32_t t0 = (uint32_t)esp_timer_get_time();
        uint32_t wait_ms = lv_timer_handler();
        uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
        s_render_sum = s_render_sum + dt;
        if (dt > s_render_max) s_render_max = dt;
        ui_unlock();
        if (wait_ms > 16) wait_ms = 16;
        if (wait_ms < 1) wait_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t ui_port_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    void *buf1 = heap_caps_malloc(LCD_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(LCD_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display");
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf1, buf2, LCD_MAX_TRANSFER, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
#ifdef LCD_ALIGN_PX
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#endif
    lcd_set_done_cb(on_flush_done, NULL);

    if (touch_chip() != TOUCH_NONE) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
#if LVGL_VERSION_MAJOR == 9
        /* LVGL ties the touch read timer to LV_DEF_REFR_PERIOD (16 ms). Sample
         * twice as often so a drag is registered sooner; rendering keeps its
         * own 16 ms cadence. */
        if (s_indev->read_timer) lv_timer_set_period(s_indev->read_timer, 8);
#endif
    }

    /* LVGL needs a fat stack; keep it in internal RAM (default). */
    s_perf_t0 = (uint32_t)esp_timer_get_time();
    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16 * 1024, NULL, 5, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "task");
    return ESP_OK;
}

void ui_port_mark_input(void)
{
    s_press_at = (uint32_t)esp_timer_get_time();
    s_press_pending = true;
}

void ui_port_full_refresh(void)
{
    ui_lock();
    lv_obj_invalidate(lv_screen_active());
    ui_unlock();
}

void ui_port_perf_report(void)
{
    uint32_t ms = (uint32_t)((esp_timer_get_time() - s_perf_t0) / 1000);
    uint32_t n = s_perf_frames ? s_perf_frames : 1;
    uint32_t pn = s_press_n ? s_press_n : 1;
    printf("perf up=%lums frames=%lu fps=%.1f render_avg=%.2f render_max=%.2f "
           "flush_avg=%.2f flush_max=%.2f press_n=%lu press_avg=%.1f press_max=%.1f\n",
           (unsigned long)ms, (unsigned long)s_perf_frames,
           ms ? (double)s_perf_frames * 1000.0 / ms : 0.0,
           (double)s_render_sum / n / 1000.0, (double)s_render_max / 1000.0,
           (double)s_flush_sum / n / 1000.0, (double)s_flush_max / 1000.0,
           (unsigned long)s_press_n, (double)s_press_sum / pn / 1000.0,
           (double)s_press_max / 1000.0);
}

void ui_port_perf_reset(void)
{
    s_perf_frames = 0; s_flush_sum = 0; s_flush_max = 0;
    s_render_sum = 0; s_render_max = 0;
    s_press_sum = 0; s_press_max = 0; s_press_n = 0;
    s_press_pending = false;
    s_perf_t0 = (uint32_t)esp_timer_get_time();
}

void ui_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void ui_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}
