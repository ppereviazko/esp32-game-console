#include "snake.h"

#include "buzzer.h"
#include "gamepad.h"
#include "st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CELL 12
#define GRID (LCD_WIDTH / CELL)
#define MAX_LEN (GRID * GRID)
#define START_LEN 4
#define TICK_MS 160

#define COLOR_BG 0x0000
#define COLOR_GREEN 0x07E0
#define COLOR_RED 0xF800
#define COLOR_FOOD 0xFFE0

typedef struct {
    int8_t x;
    int8_t y;
} cell_t;

typedef struct {
    cell_t body[MAX_LEN];
    int len;
    dir_t dir;
    uint16_t color;
    bool joined;
    bool alive;
} snake_t;

static uint32_t s_rng = 0x6d2b79f5;

static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static void paint_cell(int x, int y, uint16_t color)
{
    st7789_fill_box(x * CELL, y * CELL, CELL, CELL, color);
}

static bool occupies(const snake_t *s, int x, int y, int ignore_tail)
{
    if (!s->joined || !s->alive || s->len <= 0) {
        return false;
    }
    int last = s->len - ignore_tail;
    for (int i = 0; i < last; i++) {
        if (s->body[i].x == x && s->body[i].y == y) {
            return true;
        }
    }
    return false;
}

static void place_food(cell_t *food, const snake_t *a, const snake_t *b)
{
    for (;;) {
        int x = (int)(rnd() % GRID);
        int y = (int)(rnd() % GRID);
        if (occupies(a, x, y, 0) || occupies(b, x, y, 0)) {
            continue;
        }
        food->x = (int8_t)x;
        food->y = (int8_t)y;
        paint_cell(x, y, COLOR_FOOD);
        return;
    }
}

static void spawn(snake_t *s, int x, int y, dir_t dir, uint16_t color)
{
    s->len = START_LEN;
    s->dir = dir;
    s->color = color;
    s->joined = true;
    s->alive = true;
    for (int i = 0; i < START_LEN; i++) {
        int step = dir == DIR_RIGHT ? -i : i;
        s->body[i].x = (int8_t)(x + step);
        s->body[i].y = (int8_t)y;
        paint_cell(s->body[i].x, s->body[i].y, color);
    }
}

static cell_t step_head(const snake_t *s)
{
    cell_t head = s->body[0];
    switch (s->dir) {
    case DIR_UP:
        head.y = (int8_t)((head.y + GRID - 1) % GRID);
        break;
    case DIR_DOWN:
        head.y = (int8_t)((head.y + 1) % GRID);
        break;
    case DIR_LEFT:
        head.x = (int8_t)((head.x + GRID - 1) % GRID);
        break;
    case DIR_RIGHT:
        head.x = (int8_t)((head.x + 1) % GRID);
        break;
    }
    return head;
}

static void advance(snake_t *s, cell_t head, bool grow)
{
    if (!grow) {
        cell_t tail = s->body[s->len - 1];
        paint_cell(tail.x, tail.y, COLOR_BG);
    } else if (s->len < MAX_LEN) {
        s->len++;
    }
    for (int i = s->len - 1; i > 0; i--) {
        s->body[i] = s->body[i - 1];
    }
    s->body[0] = head;
    paint_cell(head.x, head.y, s->color);
}

static bool pad_wants_in(int player, dir_t *dir)
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
        if (gamepad_pressed(keys[i].pin)) {
            *dir = keys[i].dir;
            return true;
        }
    }
    return false;
}

static void kill_snake(snake_t *s)
{
    if (!s->joined) {
        return;
    }
    for (int i = 0; i < s->len; i++) {
        paint_cell(s->body[i].x, s->body[i].y, COLOR_BG);
    }
    s->joined = false;
    s->alive = false;
    s->len = 0;
}

static void round_play(void)
{
    snake_t green = {0};
    snake_t red = {0};
    cell_t food = {0, 0};
    bool food_on = false;

    st7789_fill_rect(0, LCD_HEIGHT, COLOR_BG);

    int green_charge = 0;
    int red_charge = 0;

    for (;;) {
        dir_t next = DIR_RIGHT;
        if (!green.joined && pad_wants_in(0, &next)) {
            spawn(&green, 5, GRID / 2 - 3, next, COLOR_GREEN);
            green_charge = 3;
            buzzer_sfx_game_start();
            if (!food_on) {
                place_food(&food, &green, &red);
                food_on = true;
            }
            continue;
        }
        if (green.joined && gamepad_direction(0, green.dir, &next)) {
            green.dir = next;
        }
        next = DIR_RIGHT;
        if (!red.joined && pad_wants_in(1, &next)) {
            spawn(&red, 5, GRID / 2 + 3, next, COLOR_RED);
            red_charge = 3;
            buzzer_sfx_game_start();
            if (!food_on) {
                place_food(&food, &green, &red);
                food_on = true;
            }
            continue;
        }
        if (red.joined && gamepad_direction(1, red.dir, &next)) {
            red.dir = next;
        }

        vTaskDelay(pdMS_TO_TICKS(TICK_MS / 3));
        if (green.joined) {
            green_charge += gamepad_pressed(PAD1_SELECT) ? 3 : 1;
        }
        if (red.joined) {
            red_charge += gamepad_pressed(PAD2_SELECT) ? 3 : 1;
        }

        bool g_go = green.joined && green_charge >= 3;
        bool r_go = red.joined && red_charge >= 3;
        if (g_go) {
            green_charge -= 3;
        }
        if (r_go) {
            red_charge -= 3;
        }
        if (!g_go && !r_go) {
            if (!green.joined && !red.joined) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }

        cell_t gh = {0, 0};
        cell_t rh = {0, 0};
        bool g_eat = false;
        bool r_eat = false;
        if (g_go) {
            gh = step_head(&green);
            g_eat = gh.x == food.x && gh.y == food.y;
        }
        if (r_go) {
            rh = step_head(&red);
            r_eat = rh.x == food.x && rh.y == food.y;
        }

        bool g_hit = false;
        bool r_hit = false;
        if (g_go) {
            g_hit = occupies(&green, gh.x, gh.y, g_eat ? 0 : 1) ||
                    occupies(&red, gh.x, gh.y, (r_go && !r_eat) ? 1 : 0);
        }
        if (r_go) {
            r_hit = occupies(&red, rh.x, rh.y, r_eat ? 0 : 1) ||
                    occupies(&green, rh.x, rh.y, (g_go && !g_eat) ? 1 : 0);
        }
        if (g_go && r_go && gh.x == rh.x && gh.y == rh.y) {
            g_hit = true;
            r_hit = true;
        }
        if (g_hit) {
            kill_snake(&green);
            green_charge = 0;
        }
        if (r_hit) {
            kill_snake(&red);
            red_charge = 0;
        }
        if (g_hit || r_hit) {
            buzzer_sfx_lose();
        }
        if (!g_hit && g_go) {
            advance(&green, gh, g_eat);
        }
        if (!r_hit && r_go) {
            advance(&red, rh, r_eat);
        }
        if (g_eat || r_eat) {
            buzzer_sfx_eat();
            place_food(&food, &green, &red);
        }

        if (g_hit || r_hit) {
            if (green.joined && red.joined && green.alive != red.alive) {
                uint16_t winner = green.alive ? COLOR_GREEN : COLOR_RED;
                st7789_fill_box(0, 0, LCD_WIDTH, 8, winner);
                st7789_fill_box(0, LCD_HEIGHT - 8, LCD_WIDTH, 8, winner);
                vTaskDelay(pdMS_TO_TICKS(400));
                st7789_fill_box(0, 0, LCD_WIDTH, 8, COLOR_BG);
                st7789_fill_box(0, LCD_HEIGHT - 8, LCD_WIDTH, 8, COLOR_BG);
            }
        }
    }
}

void snake_run(void)
{
    gamepad_init();
    for (;;) {
        round_play();
    }
}
