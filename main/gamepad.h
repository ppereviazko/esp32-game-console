#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

/* Pads short the pin to ground. A press reads low.
 * Internal pull-ups are enabled on every pin that has them.
 * Pad 1: up 26, left 25, down 33, right 16, select 17
 * Pad 2: up 14, left 27, down 12, right 13, select 19
 */

#define PAD1_UP GPIO_NUM_26
#define PAD1_LEFT GPIO_NUM_25
#define PAD1_DOWN GPIO_NUM_33
#define PAD1_RIGHT GPIO_NUM_16
#define PAD1_SELECT GPIO_NUM_17

#define PAD2_UP GPIO_NUM_14
#define PAD2_LEFT GPIO_NUM_27
#define PAD2_DOWN GPIO_NUM_12
#define PAD2_RIGHT GPIO_NUM_13
#define PAD2_SELECT GPIO_NUM_19

typedef enum {
    DIR_UP = 0,
    DIR_RIGHT,
    DIR_DOWN,
    DIR_LEFT,
} dir_t;

void gamepad_init(void);
bool gamepad_pressed(gpio_num_t pin);
/* Returns true when a direction button is held. Ignores a 180-degree turn. */
bool gamepad_direction(int player, dir_t current, dir_t *next);
bool gamepad_select_pressed(void);
