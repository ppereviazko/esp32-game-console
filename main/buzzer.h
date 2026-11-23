#pragma once

#include <stdint.h>

void buzzer_init(void);
void buzzer_tone(uint16_t hz, uint32_t ms);

void buzzer_sfx_game_start(void);
void buzzer_sfx_eat(void);
void buzzer_sfx_lose(void);