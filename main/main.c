#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"

#define DIAL_TICK_COUNT 40
#define DIAL_DEGREES_PER_TURN 360
#define DIAL_UNITS_PER_TURN 100
#define DIAL_CAP_PERCENT 42
#define DIAL_POINTER_WIDTH 8
#define NUMERIC_KEYBOARD_DRAG_THRESHOLD 12

typedef enum {
    DIAL_IDLE,
    DIAL_ROTATING,
    DIAL_WAIT_FOR_ANGLE,
    DIAL_HOLD_CENTER,
} dial_press_t;

static const char *TAG = "touch_ui";
static uint32_t button_press_count;
static lv_obj_t *numeric_input;
static lv_obj_t *numeric_keyboard;
static lv_point_t numeric_keyboard_press_point;
static lv_point_t numeric_keyboard_start_position;
static bool numeric_keyboard_dragging;
static lv_obj_t *dial_face;
static lv_obj_t *dial_value_label;
static lv_obj_t *dial_pointer;
static lv_obj_t *rocker_faces[2];
static lv_obj_t *rocker_labels[2];
static lv_obj_t *rocker_indicators[2];
static lv_obj_t *rocker_state_label;
static lv_point_precise_t dial_tick_points[DIAL_TICK_COUNT][2];
static lv_point_precise_t dial_pointer_points[2];
static char dial_value_text[32] = "Value: 0";
static bool numeric_input_replace_on_next_key;
static dial_press_t dial_press;
static int32_t dial_last_angle;
static int64_t dial_rotation_degrees;
static int64_t dial_displayed_value;

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

static void invalidate_dial_pointer(void)
{
    lv_area_t coords;
    lv_obj_get_coords(dial_pointer, &coords);
    lv_point_precise_t start = dial_pointer_points[0];
    lv_point_precise_t end = dial_pointer_points[1];
    lv_area_t area = {
        .x1 = coords.x1 + LV_MIN(start.x, end.x),
        .y1 = coords.y1 + LV_MIN(start.y, end.y),
        .x2 = coords.x1 + LV_MAX(start.x, end.x),
        .y2 = coords.y1 + LV_MAX(start.y, end.y),
    };
    // Include the line stroke and antialiasing, as lv_draw_line() does.
    lv_area_increase(&area, DIAL_POINTER_WIDTH, DIAL_POINTER_WIDTH);
    lv_obj_invalidate_area(dial_pointer, &area);
}

static void update_dial_display(void)
{
    int32_t angle = dial_rotation_degrees % DIAL_DEGREES_PER_TURN;
    int32_t partial = angle * DIAL_UNITS_PER_TURN;
    partial += partial < 0 ? -DIAL_DEGREES_PER_TURN / 2 : DIAL_DEGREES_PER_TURN / 2;
    int64_t value = (dial_rotation_degrees / DIAL_DEGREES_PER_TURN) * DIAL_UNITS_PER_TURN +
                    partial / DIAL_DEGREES_PER_TURN;
    if (value != dial_displayed_value) {
        snprintf(dial_value_text, sizeof(dial_value_text), "Value: %" PRId64, value);
        lv_label_set_text_static(dial_value_label, dial_value_text);
        dial_displayed_value = value;
    }

    int32_t center = dial_pointer_points[0].x;
    int32_t pointer_radius = center * 3 / 4;
    lv_point_precise_t end = {
        center + lv_trigo_sin(angle) * pointer_radius / LV_TRIGO_SIN_MAX,
        center - lv_trigo_sin(angle + 90) * pointer_radius / LV_TRIGO_SIN_MAX,
    };
    if (end.x != dial_pointer_points[1].x || end.y != dial_pointer_points[1].y) {
        invalidate_dial_pointer();
        dial_pointer_points[1] = end;
        invalidate_dial_pointer();
    }
}

static lv_point_t dial_point_offset(const lv_point_t *point)
{
    lv_area_t coords;
    lv_obj_get_coords(dial_face, &coords);
    return (lv_point_t){
        point->x - (coords.x1 + coords.x2) / 2,
        point->y - (coords.y1 + coords.y2) / 2,
    };
}

static bool dial_within_radius(lv_point_t offset, int32_t radius)
{
    return offset.x * offset.x + offset.y * offset.y <= radius * radius;
}

static void dial_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        dial_press = DIAL_IDLE;
        return;
    }
    if (code != LV_EVENT_PRESSED &&
        (dial_press == DIAL_IDLE || (code != LV_EVENT_PRESSING && code != LV_EVENT_LONG_PRESSED))) {
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_point_t offset = dial_point_offset(&point);
    int32_t center_radius = lv_obj_get_content_width(dial_face) * DIAL_CAP_PERCENT / 200;
    bool at_center = dial_within_radius(offset, center_radius);

    if (code == LV_EVENT_PRESSED) {
        if (!dial_within_radius(offset, lv_obj_get_width(dial_face) / 2)) {
            dial_press = DIAL_IDLE;
        }
        else if (at_center) {
            dial_press = DIAL_HOLD_CENTER;
        }
        else {
            dial_last_angle = lv_atan2(offset.y, offset.x);
            dial_press = DIAL_ROTATING;
        }
        return;
    }

    if (dial_press == DIAL_HOLD_CENTER) {
        if (!at_center) {
            dial_press = DIAL_IDLE;
        }
        else if (code == LV_EVENT_LONG_PRESSED) {
            dial_rotation_degrees = 0;
            update_dial_display();
            dial_press = DIAL_IDLE;
        }
        return;
    }
    if (code != LV_EVENT_PRESSING) {
        return;
    }
    if (at_center) {
        dial_press = DIAL_WAIT_FOR_ANGLE;
        return;
    }

    int32_t angle = lv_atan2(offset.y, offset.x);
    if (dial_press == DIAL_ROTATING) {
        // Screen Y increases downward, so a clockwise angle change is positive.
        int32_t delta = angle - dial_last_angle;
        if (delta > DIAL_DEGREES_PER_TURN / 2) {
            delta -= DIAL_DEGREES_PER_TURN;
        }
        else if (delta < -DIAL_DEGREES_PER_TURN / 2) {
            delta += DIAL_DEGREES_PER_TURN;
        }
        if (delta != 0) {
            dial_rotation_degrees += delta;
            update_dial_display();
        }
    }
    dial_last_angle = angle;
    dial_press = DIAL_ROTATING;
}

static lv_obj_t *create_dial_line(lv_obj_t *parent, lv_color_t color, int32_t width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_size(line, LV_PCT(100), LV_PCT(100));
    lv_obj_center(line);
    lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(line, width, LV_PART_MAIN);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static void create_dial(lv_obj_t *page)
{
    lv_obj_update_layout(page);
    int32_t available_width = lv_obj_get_content_width(page);
    int32_t available_height = lv_obj_get_content_height(page);
    int32_t outer_size = available_width - 32;
    if (outer_size > available_height - 112) {
        outer_size = available_height - 112;
    }
    if (outer_size > 440) {
        outer_size = 440;
    }

    lv_obj_t *bezel = lv_obj_create(page);
    lv_obj_set_size(bezel, outer_size, outer_size);
    lv_obj_align(bezel, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bezel, lv_color_hex(0x42474F), LV_PART_MAIN);
    lv_obj_set_style_border_color(bezel, lv_color_hex(0xAEB4BD), LV_PART_MAIN);
    lv_obj_set_style_border_width(bezel, 3, LV_PART_MAIN);
    lv_obj_set_style_outline_color(bezel, lv_color_hex(0x101215), LV_PART_MAIN);
    lv_obj_set_style_outline_width(bezel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bezel, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bezel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    int32_t face_size = outer_size - 34;
    dial_face = lv_obj_create(bezel);
    lv_obj_set_size(dial_face, face_size, face_size);
    lv_obj_center(dial_face);
    lv_obj_set_style_radius(dial_face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dial_face, lv_color_hex(0x22262C), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(dial_face, lv_color_hex(0x42474F), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(dial_face, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dial_face, lv_color_hex(0x666D77), LV_PART_MAIN);
    lv_obj_set_style_border_width(dial_face, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dial_face, 0, LV_PART_MAIN);
    lv_obj_remove_flag(dial_face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                                   LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(dial_face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(dial_face, dial_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_update_layout(dial_face);
    int32_t content_size = lv_obj_get_content_width(dial_face);
    int32_t center = content_size / 2;
    int32_t outer_radius = content_size * 44 / 100;
    for (uint32_t i = 0; i < DIAL_TICK_COUNT; i++) {
        int32_t angle = i * DIAL_DEGREES_PER_TURN / DIAL_TICK_COUNT;
        bool major_tick = (i % 5) == 0;
        int32_t inner_radius = outer_radius - (major_tick ? 18 : 9);
        int32_t sin_angle = lv_trigo_sin(angle);
        int32_t cos_angle = lv_trigo_sin(angle + 90);
        dial_tick_points[i][0] = (lv_point_precise_t){
            center + sin_angle * outer_radius / LV_TRIGO_SIN_MAX,
            center - cos_angle * outer_radius / LV_TRIGO_SIN_MAX,
        };
        dial_tick_points[i][1] = (lv_point_precise_t){
            center + sin_angle * inner_radius / LV_TRIGO_SIN_MAX,
            center - cos_angle * inner_radius / LV_TRIGO_SIN_MAX,
        };
        lv_obj_t *tick = create_dial_line(dial_face,
                                          major_tick ? lv_color_hex(0xE2E5E9) : lv_color_hex(0x858C96),
                                          major_tick ? 4 : 2);
        lv_line_set_points(tick, dial_tick_points[i], 2);
    }

    dial_pointer_points[0] = (lv_point_precise_t){center, center};
    dial_pointer_points[1] = (lv_point_precise_t){center, center - center * 3 / 4};
    dial_pointer = create_dial_line(dial_face, lv_theme_get_color_primary(page), DIAL_POINTER_WIDTH);
    lv_line_set_points_mutable(dial_pointer, dial_pointer_points, 2);

    lv_obj_t *center_cap = lv_obj_create(dial_face);
    int32_t cap_size = content_size * DIAL_CAP_PERCENT / 100;
    lv_obj_set_size(center_cap, cap_size, cap_size);
    lv_obj_center(center_cap);
    lv_obj_set_style_radius(center_cap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(center_cap, lv_color_hex(0x454B54), LV_PART_MAIN);
    lv_obj_set_style_border_color(center_cap, lv_color_hex(0x858C96), LV_PART_MAIN);
    lv_obj_set_style_border_width(center_cap, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(center_cap, 0, LV_PART_MAIN);
    lv_obj_remove_flag(center_cap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reset_label = lv_label_create(center_cap);
    lv_label_set_text(reset_label, "RESET");
    lv_obj_set_style_text_color(reset_label, lv_color_hex(0xF4F4F4), LV_PART_MAIN);
    lv_obj_center(reset_label);

    dial_value_label = lv_label_create(page);
    lv_label_set_text_static(dial_value_label, dial_value_text);
    lv_obj_set_style_text_font(dial_value_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(dial_value_label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "1 turn = 100  |  Hold center to reset");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void update_rocker_display(bool on)
{
    lv_color_t primary = lv_theme_get_color_primary(rocker_state_label);
    lv_label_set_text(rocker_state_label, on ? "State: ON" : "State: OFF");
    for (uint32_t i = 0; i < 2; i++) {
        bool active = i == (on ? 1U : 0U);
        lv_color_t accent = on ? primary : lv_color_hex(0xF4F4F4);
        lv_obj_set_style_translate_y(rocker_faces[i], active ? 6 : -6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(rocker_faces[i],
                                 lv_color_hex(active ? 0x22262C : 0x666D77), LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(rocker_faces[i],
                                      lv_color_hex(active ? 0x42474F : 0x22262C), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(rocker_faces[i], active ? 0 : 8, LV_PART_MAIN);
        lv_obj_set_style_text_color(rocker_labels[i],
                                   active ? accent : lv_color_hex(0x858C96), LV_PART_MAIN);
        lv_obj_set_style_bg_color(rocker_indicators[i],
                                 active ? accent : lv_color_hex(0x22262C), LV_PART_MAIN);
    }
}

static void rocker_event_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    lv_obj_t *rocker = lv_event_get_current_target(event);
    lv_area_t coords;
    lv_point_t point;
    lv_obj_get_coords(rocker, &coords);
    lv_indev_get_point(indev, &point);
    if (point.x < coords.x1 || point.x > coords.x2 ||
        point.y < coords.y1 || point.y > coords.y2) {
        return;
    }
    update_rocker_display(point.x - coords.x1 >= lv_obj_get_width(rocker) / 2);
}

static void create_rocker_switch(lv_obj_t *page)
{
    lv_obj_update_layout(page);
    int32_t width = lv_obj_get_content_width(page) - 48;
    if (width > 400) {
        width = 400;
    }

    rocker_state_label = lv_label_create(page);
    lv_obj_set_style_text_font(rocker_state_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(rocker_state_label, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *rocker = lv_obj_create(page);
    lv_obj_set_size(rocker, width, 180);
    lv_obj_align(rocker, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_text_font(rocker, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_radius(rocker, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rocker, lv_color_hex(0x101215), LV_PART_MAIN);
    lv_obj_set_style_border_color(rocker, lv_color_hex(0xAEB4BD), LV_PART_MAIN);
    lv_obj_set_style_border_width(rocker, 4, LV_PART_MAIN);
    lv_obj_set_style_outline_color(rocker, lv_color_hex(0x42474F), LV_PART_MAIN);
    lv_obj_set_style_outline_width(rocker, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rocker, 10, LV_PART_MAIN);
    lv_obj_remove_flag(rocker, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                              LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(rocker, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(rocker, rocker_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_update_layout(rocker);
    for (uint32_t i = 0; i < 2; i++) {
        lv_obj_t *face = lv_obj_create(rocker);
        rocker_faces[i] = face;
        lv_obj_set_size(face, (lv_obj_get_content_width(rocker) - 4) / 2,
                       lv_obj_get_content_height(rocker) - 12);
        lv_obj_align(face, i == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(face, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_dir(face, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_border_color(face, lv_color_hex(0x858C96), LV_PART_MAIN);
        lv_obj_set_style_border_width(face, 2, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(face, lv_color_hex(0x101215), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(face, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_shadow_offset_y(face, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
        lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        rocker_labels[i] = lv_label_create(face);
        lv_label_set_text(rocker_labels[i], i == 0 ? "OFF" : "ON");
        lv_obj_align(rocker_labels[i], LV_ALIGN_CENTER, 0, -8);
        lv_obj_remove_flag(rocker_labels[i], LV_OBJ_FLAG_CLICKABLE);

        rocker_indicators[i] = lv_obj_create(face);
        lv_obj_set_size(rocker_indicators[i], 48, 8);
        lv_obj_align(rocker_indicators[i], LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_set_style_radius(rocker_indicators[i], 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(rocker_indicators[i], 0, LV_PART_MAIN);
        lv_obj_remove_flag(rocker_indicators[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text(hint, "Tap left: OFF  |  Tap right: ON");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    update_rocker_display(false);
}

static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_obj_t *status_label = lv_event_get_user_data(event);
        button_press_count++;
        lv_label_set_text_fmt(status_label, "Button pressed: %lu", (unsigned long)button_press_count);
    }
}

static void tabview_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_add_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);
        numeric_input_replace_on_next_key = false;
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

static void numeric_keyboard_drag_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_INDEV_RESET) {
        numeric_keyboard_dragging = false;
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &numeric_keyboard_press_point);
        lv_area_t coords;
        lv_obj_get_coords(numeric_keyboard, &coords);
        numeric_keyboard_start_position = (lv_point_t){coords.x1, coords.y1};
        numeric_keyboard_dragging = false;
        return;
    }

    if (code != LV_EVENT_PRESSING) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int32_t dx = point.x - numeric_keyboard_press_point.x;
    int32_t dy = point.y - numeric_keyboard_press_point.y;
    if (!numeric_keyboard_dragging) {
        if (dx * dx + dy * dy < NUMERIC_KEYBOARD_DRAG_THRESHOLD * NUMERIC_KEYBOARD_DRAG_THRESHOLD) {
            return;
        }
        numeric_keyboard_dragging = true;
        lv_buttonmatrix_set_selected_button(numeric_keyboard, LV_BUTTONMATRIX_BUTTON_NONE);
        lv_obj_set_align(numeric_keyboard, LV_ALIGN_TOP_LEFT);
    }

    lv_obj_t *parent = lv_obj_get_parent(numeric_keyboard);
    lv_area_t parent_content;
    lv_obj_get_content_coords(parent, &parent_content);
    lv_area_t screen;
    lv_obj_get_coords(lv_screen_active(), &screen);
    lv_area_t bounds = {
        .x1 = LV_MAX(parent_content.x1, screen.x1),
        .y1 = LV_MAX(parent_content.y1, screen.y1),
        .x2 = LV_MIN(parent_content.x2, screen.x2),
        .y2 = LV_MIN(parent_content.y2, screen.y2),
    };

    int32_t width = lv_obj_get_width(numeric_keyboard);
    int32_t height = lv_obj_get_height(numeric_keyboard);
    int32_t max_x = LV_MAX(bounds.x1, bounds.x2 - width + 1);
    int32_t max_y = LV_MAX(bounds.y1, bounds.y2 - height + 1);
    int32_t x = LV_CLAMP(bounds.x1, numeric_keyboard_start_position.x + dx, max_x);
    int32_t y = LV_CLAMP(bounds.y1, numeric_keyboard_start_position.y + dy, max_y);
    lv_obj_set_pos(numeric_keyboard, x - parent_content.x1, y - parent_content.y1);
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
    lv_color_t accent = lv_theme_get_color_primary(screen);

    lv_obj_t *tabview = lv_tabview_create(screen);
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_size(tabview, 80);
    lv_obj_set_style_text_font(lv_tabview_get_tab_bar(tabview), &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_add_event_cb(tabview, tabview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *main_page = lv_tabview_add_tab(tabview, "Main");
    lv_obj_t *dial_page = lv_tabview_add_tab(tabview, "Page 2");
    lv_obj_add_flag(main_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(main_page, dismiss_numeric_keyboard_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dial_panel = lv_obj_create(dial_page);
    lv_obj_remove_style_all(dial_panel);
    lv_obj_set_size(dial_panel, LV_PCT(55), LV_PCT(100));
    lv_obj_align(dial_panel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(dial_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    create_dial(dial_panel);

    lv_obj_t *switch_panel = lv_obj_create(dial_page);
    lv_obj_remove_style_all(switch_panel);
    lv_obj_set_size(switch_panel, LV_PCT(45), LV_PCT(100));
    lv_obj_align(switch_panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(switch_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    create_rocker_switch(switch_panel);

    lv_obj_t *title = lv_label_create(main_page);
    lv_label_set_text(title, "ESP32-P4 Touch UI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *status_label = lv_label_create(main_page);
    lv_label_set_text(status_label, "Button pressed: 0");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -70);

    lv_obj_t *button = lv_button_create(main_page);
    lv_obj_set_size(button, 330, 120);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x666D77), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(button, lv_color_hex(0x42474F), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0xAEB4BD), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 3, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, accent, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x22262C), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(button, lv_color_hex(0x101215), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_width(button, 0, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_height(button, 0, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, button_event_cb, LV_EVENT_CLICKED, status_label);
    lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Press");
    lv_obj_set_style_text_font(button_label, &lv_font_montserrat_48, 0);
    lv_obj_center(button_label);

    lv_obj_t *value_label = lv_label_create(main_page);
    lv_label_set_text(value_label, "Value");
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_48, 0);
    lv_obj_align(value_label, LV_ALIGN_CENTER, 0, 140);

    numeric_input = lv_textarea_create(main_page);
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

    numeric_keyboard = lv_keyboard_create(main_page);
    lv_obj_remove_flag(numeric_keyboard, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_keyboard_set_mode(numeric_keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_map(numeric_keyboard, LV_KEYBOARD_MODE_USER_1, numeric_key_map, numeric_key_ctrl_map);
    lv_buttonmatrix_set_button_ctrl_all(numeric_keyboard,
                                        LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT);
    lv_keyboard_set_textarea(numeric_keyboard, numeric_input);
    lv_obj_remove_event_cb(numeric_keyboard, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(numeric_keyboard, numeric_keyboard_drag_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(numeric_keyboard, numeric_keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_size(numeric_keyboard, 440, 400);
    lv_obj_align(numeric_keyboard, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(numeric_keyboard, lv_color_hex(0x101215), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(numeric_keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(numeric_keyboard, lv_color_hex(0xAEB4BD), LV_PART_MAIN);
    lv_obj_set_style_border_width(numeric_keyboard, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(numeric_keyboard, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(numeric_keyboard, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_row(numeric_keyboard, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(numeric_keyboard, 8, LV_PART_MAIN);
    lv_obj_set_style_text_font(numeric_keyboard, &lv_font_montserrat_48, LV_PART_ITEMS);
    lv_obj_set_style_text_color(numeric_keyboard, lv_color_hex(0xF4F4F4), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(numeric_keyboard, lv_color_hex(0x666D77), LV_PART_ITEMS);
    lv_obj_set_style_bg_grad_color(numeric_keyboard, lv_color_hex(0x42474F), LV_PART_ITEMS);
    lv_obj_set_style_bg_grad_dir(numeric_keyboard, LV_GRAD_DIR_VER, LV_PART_ITEMS);
    lv_obj_set_style_border_color(numeric_keyboard, lv_color_hex(0xAEB4BD), LV_PART_ITEMS);
    lv_obj_set_style_border_width(numeric_keyboard, 2, LV_PART_ITEMS);
    lv_obj_set_style_radius(numeric_keyboard, 8, LV_PART_ITEMS);
    lv_obj_remove_style(numeric_keyboard, NULL, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_flag(numeric_keyboard, LV_OBJ_FLAG_HIDDEN);
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