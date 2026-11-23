#pragma once

#include <stdint.h>

#define GAME_MENU_SLOTS 6

typedef enum {
    GAME_SNAKE = 0,
    GAME_ARKANOID = 1,
} game_id_t;

/* Blocks until the player confirms an available game. */
game_id_t gamemenu_pick(void);