#include "buzzer.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUZZER_GPIO GPIO_NUM_32
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LEDC_DUTY_ON ((1 << 10) / 2)

#define C4 262
#define E4 330
#define G4 392
#define C5 523
#define E5 659
#define G5 784

typedef struct {
    uint16_t hz;
    uint16_t ms;
} sfx_note_t;

static bool s_ready;

static void buzzer_silence(void)
{
    if (!s_ready) {
        return;
    }
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static void buzzer_ensure(uint16_t hz)
{
    if (!s_ready) {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_DUTY_RES,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = hz ? hz : 1000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t channel = {
            .gpio_num = BUZZER_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0,
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
        s_ready = true;
        return;
    }
    if (hz != 0) {
        ESP_ERROR_CHECK(ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, hz));
    }
}

static void buzzer_set(uint16_t hz)
{
    if (hz == 0) {
        buzzer_silence();
        return;
    }
    buzzer_ensure(hz);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_DUTY_ON));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
}

static void play_seq(const sfx_note_t *notes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (notes[i].hz) {
            buzzer_set(notes[i].hz);
            vTaskDelay(pdMS_TO_TICKS(notes[i].ms));
        } else {
            buzzer_silence();
            vTaskDelay(pdMS_TO_TICKS(notes[i].ms));
        }
    }
    buzzer_silence();
}

void buzzer_init(void)
{
    buzzer_ensure(1000);
    buzzer_silence();
}

void buzzer_tone(uint16_t hz, uint32_t ms)
{
    if (hz == 0) {
        buzzer_silence();
        return;
    }
    buzzer_set(hz);
    vTaskDelay(pdMS_TO_TICKS(ms));
    buzzer_silence();
}

void buzzer_sfx_game_start(void)
{
    static const sfx_note_t seq[] = {
        {C5, 70}, {E5, 70}, {G5, 110},
    };
    play_seq(seq, sizeof(seq) / sizeof(seq[0]));
}

void buzzer_sfx_eat(void)
{
    buzzer_tone(G5, 45);
    buzzer_tone((uint16_t)(G5 + 120), 55);
}

void buzzer_sfx_lose(void)
{
    static const sfx_note_t seq[] = {
        {G4, 120}, {E4, 120}, {C4, 180},
    };
    play_seq(seq, sizeof(seq) / sizeof(seq[0]));
}