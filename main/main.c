#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "touch_ui";
static uint32_t button_press_count;
static lv_obj_t *numeric_input;
static lv_obj_t *numeric_keyboard;
static bool numeric_input_replace_on_next_key;

static const char * const numeric_key_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", "\n",
    "Enter", ""
};

static const lv_buttonmatrix_ctrl_t numeric_key_ctrl_map[] = {
    1, 1, 1,
    1, 1, 1,
    1, 1, 1,
    1, 1,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | 3
};

static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_obj_t *status_label = lv_event_get_user_data(event);
        button_press_count++;
        lv_label_set_text_fmt(status_label, "Button pressed: %lu", (unsigned long)button_press_count);
    }
}

static void numeric_input_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_textarea_set_cursor_pos(numeric_input, LV_TEXTAREA_CURSOR_LAST);
        numeric_input_replace_on_next_key = true;
        lv_obj_clear_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void numeric_keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint32_t button_id = lv_keyboard_get_selected_button(numeric_keyboard);
    const char *key = lv_keyboard_get_button_text(numeric_keyboard, button_id);
    if (key == NULL) {
        return;
    }

    if (strcmp(key, "Enter") == 0) {
        lv_obj_add_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (numeric_input_replace_on_next_key) {
        lv_textarea_set_text(numeric_input, "");
        numeric_input_replace_on_next_key = false;
    }

    if (strcmp(key, ".") == 0 && strchr(lv_textarea_get_text(numeric_input), '.') != NULL) {
        return;
    }

    lv_textarea_set_cursor_pos(numeric_input, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(numeric_input, key);
}

static void dismiss_numeric_keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED ||
        lv_obj_has_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    lv_obj_t *target = lv_event_get_target(event);
    if (target != numeric_input && target != numeric_keyboard) {
        lv_obj_add_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Press");
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_48, 0);
    lv_obj_center(button_label);

    lv_obj_t *value_label = lv_label_create(screen);
    lv_label_set_text(value_label, "Value");
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_48, 0);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, 140);

    numeric_input = lv_textarea_create(screen);
    lv_textarea_set_one_line(numeric_input, true);
    lv_textarea_set_accepted_chars(numeric_input, "0123456789.");
    lv_textarea_set_max_length(numeric_input, 12);
    lv_textarea_set_text(numeric_input, "0");
    lv_obj_set_size(numeric_input, 360, 80);
    lv_obj_align(numeric_input, LV_ALIGN_CENTER, 0, 215);
    lv_obj_set_style_text_font(numeric_input, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_align(numeric_input, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(numeric_input, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(numeric_input, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(numeric_input, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(numeric_input, 2, LV_PART_MAIN);
    lv_obj_add_flag(numeric_input, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(numeric_input, numeric_input_event_cb, LV_EVENT_CLICKED, NULL);

    numeric_keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_mode(numeric_keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_map(numeric_keyboard, LV_KEYBOARD_MODE_USER_1, numeric_key_map, numeric_key_ctrl_map);
    lv_keyboard_set_textarea(numeric_keyboard, numeric_input);
    lv_obj_remove_event_cb(numeric_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(numeric_keyboard, numeric_keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(numeric_keyboard, 440, 400);
    lv_obj_align(numeric_keyboard, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_text_font(numeric_keyboard, &lv_font_montserrat_48, LV_PART_ITEMS);
    lv_obj_add_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, dismiss_numeric_keyboard_event_cb, LV_EVENT_CLICKED, NULL);
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