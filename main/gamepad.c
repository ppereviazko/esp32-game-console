#include "gamepad.h"

#include "esp_log.h"

static const char *TAG = "pad";

static const gpio_num_t k_pullup_pins[] = {
    PAD1_UP, PAD1_LEFT, PAD1_DOWN, PAD1_RIGHT, PAD1_SELECT,
    PAD2_UP, PAD2_LEFT, PAD2_DOWN, PAD2_RIGHT, PAD2_SELECT,
};

void gamepad_init(void)
{
    uint64_t pull_mask = 0;
    for (size_t i = 0; i < sizeof(k_pullup_pins) / sizeof(k_pullup_pins[0]); i++) {
        pull_mask |= 1ULL << k_pullup_pins[i];
    }
    const gpio_config_t pull = {
        .pin_bit_mask = pull_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pull));
    ESP_LOGI(TAG, "pads ready");
}

bool gamepad_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

bool gamepad_direction(int player, dir_t current, dir_t *next)
{
    const gpio_num_t up = player == 0 ? PAD1_UP : PAD2_UP;
    const gpio_num_t right = player == 0 ? PAD1_RIGHT : PAD2_RIGHT;
    const gpio_num_t down = player == 0 ? PAD1_DOWN : PAD2_DOWN;
    const gpio_num_t left = player == 0 ? PAD1_LEFT : PAD2_LEFT;
    const struct {
        gpio_num_t pin;
        dir_t dir;
    } keys[] = {
        {up, DIR_UP},
        {right, DIR_RIGHT},
        {down, DIR_DOWN},
        {left, DIR_LEFT},
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!gamepad_pressed(keys[i].pin)) {
            continue;
        }
        if ((keys[i].dir ^ current) == 2) {
            continue;
        }
        *next = keys[i].dir;
        return true;
    }
    return false;
}

bool gamepad_select_pressed(void)
{
    return gamepad_pressed(PAD1_SELECT) || gamepad_pressed(PAD2_SELECT);
}
