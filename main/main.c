#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "touch_ui";
static uint32_t button_press_count;

static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_obj_t *status_label = lv_event_get_user_data(event);
        button_press_count++;
        lv_label_set_text_fmt(status_label, "Button pressed: %lu", (unsigned long)button_press_count);
    }
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "ESP32-P4 Touch UI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Button pressed: 0");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 330, 120);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_event_cb(button, button_event_cb, LV_EVENT_CLICKED, status_label);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Press");
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_48, 0);
    lv_obj_center(button_label);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_270,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    if (bsp_display_start_with_config(&cfg) == NULL) {
        ESP_LOGE(TAG, "Failed to initialize the display");
        return;
    }

    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_ERROR_CHECK(bsp_display_lock(-1));
    create_ui();
    bsp_display_unlock();
}