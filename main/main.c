#include "st7789.h"
#include "cats565.h"
#include "snake.h"
#include "buzzer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

void app_main(void)
{
    buzzer_init();

    st7789_init();
    st7789_fill_rect(0, LCD_HEIGHT, 0x0000);
    st7789_draw_rgb565((LCD_WIDTH - CATS_W) / 2, (LCD_HEIGHT - CATS_H) / 2, CATS_W, CATS_H, cats565);

    /* Splash stays. Set true to replay the Imperial March on boot. */
    static const bool k_play_splash_melody = false;
    if (k_play_splash_melody) {
        buzzer_sfx_game_start();
    }

    snake_run();
}