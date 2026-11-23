#include "arkanoid.h"

#include "buzzer.h"
#include "gamepad.h"
#include "st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Target ~30 FPS; partial redraws (paddle + ball) leave headroom on SPI. */
#define FRAME_MS 33
#define LEVEL_COUNT 10

#define COLOR_BG 0x0000
#define COLOR_PADDLE 0xFFFF
#define COLOR_BALL 0xFFFF
#define COLOR_FRAME 0x4208
#define COLOR_SILVER 0xC618
#define COLOR_GOLD 0xFFE0
#define COLOR_TROPHY 0xFFE0
#define COLOR_TROPHY_TRIM 0xFD20
#define COLOR_STAR 0xFFFF

#define BRICK_COLS 8
#define BRICK_ROWS 8
#define BRICK_W 28
#define BRICK_H 10
#define BRICK_GAP_X 2
#define BRICK_GAP_Y 2
#define BRICK_TOP_Y 18
#define BRICK_LEFT_X ((LCD_WIDTH - (BRICK_COLS * BRICK_W + (BRICK_COLS - 1) * BRICK_GAP_X)) / 2)

#define PADDLE_W 44
#define PADDLE_H 6
#define PADDLE_Y (LCD_HEIGHT - 16)
#define PADDLE_SPEED 6

#define BALL_SIZE 6
#define BALL_SPEED_X 2
#define BALL_SPEED_Y 2

/* Classic row tint for breakable colored bricks. */
static const uint16_t k_row_color[BRICK_ROWS] = {
    0xC618,
    0xC618,
    0xFD20,
    0xFD20,
    0xF800,
    0xFFE0,
    0x07E0,
    0x001F,
};

typedef enum {
    BRICK_COLOR = 0,
    BRICK_SILVER,
    BRICK_GOLD,
} brick_kind_t;

typedef struct {
    int16_t x;
    int16_t y;
} vec2_t;

typedef struct {
    bool alive;
    brick_kind_t kind;
    uint8_t hits;
    int16_t x;
    int16_t y;
    uint16_t color;
} brick_t;

typedef struct {
    uint8_t rows[BRICK_ROWS];
    uint8_t gold[BRICK_ROWS];
    uint8_t silver[BRICK_ROWS];
} level_def_t;

/* Layouts inspired by early Arkanoid rounds (tunnel, pyramid, cage, invader, etc.). */
static const level_def_t k_levels[LEVEL_COUNT] = {
    /* 1 — full wall (round 1). */
    {.rows = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
    /* 2 — tunnel: dense left, gap on the right to send the ball behind. */
    {.rows = {0xFC, 0xFC, 0xFC, 0xFC, 0xFF, 0xFF, 0xFF, 0xFF}},
    /* 3 — diamond / stacked chevron. */
    {.rows = {0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18}},
    /* 4 — hollow frame. */
    {.rows = {0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF}},
    /* 5 — umbrella arch. */
    {.rows = {0x3C, 0x7E, 0xFF, 0xFF, 0x18, 0x18, 0x24, 0x3C}},
    /* 6 — twin columns (break the center). */
    {.rows = {0xC3, 0xC3, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3, 0xC3}},
    /* 7 — checkerboard. */
    {.rows = {0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55}},
    /* 8 — gold cage in the middle (indestructible core). */
    {
        .rows = {0xFF, 0xFF, 0xE7, 0xC3, 0xC3, 0xE7, 0xFF, 0xFF},
        .gold = {0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x00},
    },
    /* 9 — simple “invader” face. */
    {.rows = {0x00, 0x66, 0xFF, 0xDB, 0xDB, 0xFF, 0x7E, 0x3C}},
    /* 10 — boss gate with silver lintel. */
    {
        .rows = {0xFF, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0xFF},
        .silver = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},
    },
};

static brick_t s_bricks[BRICK_ROWS][BRICK_COLS];
static vec2_t s_paddle;
static vec2_t s_ball;
static vec2_t s_ball_vel;
static bool s_ball_launched;
static bool s_select_was_down;
static int s_level;
static bool s_victory;
static bool s_victory_drawn;

static uint8_t col_mask(int col)
{
    return (uint8_t)(1u << col);
}

static void bounce_off_brick(int ball_cx, int ball_cy, const brick_t *b)
{
    int brick_cx = b->x + BRICK_W / 2;
    int brick_cy = b->y + BRICK_H / 2;
    int dx = ball_cx - brick_cx;
    int dy = ball_cy - brick_cy;
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    if (dx > dy) {
        s_ball_vel.x = (int16_t)(-s_ball_vel.x);
    } else {
        s_ball_vel.y = (int16_t)(-s_ball_vel.y);
    }
}

static void bricks_load_level(int level_index)
{
    const level_def_t *lv = &k_levels[level_index];

    for (int row = 0; row < BRICK_ROWS; row++) {
        int y = BRICK_TOP_Y + row * (BRICK_H + BRICK_GAP_Y);
        for (int col = 0; col < BRICK_COLS; col++) {
            brick_t *b = &s_bricks[row][col];
            uint8_t bit = col_mask(col);

            b->x = (int16_t)(BRICK_LEFT_X + col * (BRICK_W + BRICK_GAP_X));
            b->y = (int16_t)y;

            if ((lv->rows[row] & bit) == 0) {
                b->alive = false;
                continue;
            }

            b->alive = true;
            if (lv->gold[row] & bit) {
                b->kind = BRICK_GOLD;
                b->hits = 1;
                b->color = COLOR_GOLD;
            } else if (lv->silver[row] & bit) {
                b->kind = BRICK_SILVER;
                b->hits = 2;
                b->color = COLOR_SILVER;
            } else {
                b->kind = BRICK_COLOR;
                b->hits = 1;
                b->color = k_row_color[row];
            }
        }
    }
}

static void draw_brick(const brick_t *b)
{
    if (!b->alive) {
        return;
    }
    st7789_fill_box(b->x, b->y, BRICK_W, BRICK_H, b->color);
    st7789_fill_box(b->x, b->y, BRICK_W, 1, COLOR_FRAME);
    st7789_fill_box(b->x, b->y + BRICK_H - 1, BRICK_W, 1, COLOR_FRAME);
    st7789_fill_box(b->x, b->y, 1, BRICK_H, COLOR_FRAME);
    st7789_fill_box(b->x + BRICK_W - 1, b->y, 1, BRICK_H, COLOR_FRAME);
}

static void erase_brick(const brick_t *b)
{
    st7789_fill_box(b->x, b->y, BRICK_W, BRICK_H, COLOR_BG);
}

static void draw_all_bricks(void)
{
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            draw_brick(&s_bricks[row][col]);
        }
    }
}

static void draw_paddle_at(int16_t x)
{
    if (x < 0) {
        x = 0;
    }
    if (x + PADDLE_W > LCD_WIDTH) {
        x = (int16_t)(LCD_WIDTH - PADDLE_W);
    }
    st7789_fill_box(x, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_PADDLE);
}

static void erase_paddle_at(int16_t x)
{
    if (x < 0) {
        x = 0;
    }
    if (x + PADDLE_W > LCD_WIDTH) {
        x = (int16_t)(LCD_WIDTH - PADDLE_W);
    }
    st7789_fill_box(x, PADDLE_Y, PADDLE_W, PADDLE_H, COLOR_BG);
}

static void draw_ball_at(int16_t x, int16_t y)
{
    st7789_fill_box(x, y, BALL_SIZE, BALL_SIZE, COLOR_BALL);
}

static void erase_ball_at(int16_t x, int16_t y)
{
    st7789_fill_box(x, y, BALL_SIZE, BALL_SIZE, COLOR_BG);
}

static void ball_snap_to_paddle(void)
{
    s_ball.x = (int16_t)(s_paddle.x + PADDLE_W / 2 - BALL_SIZE / 2);
    s_ball.y = (int16_t)(PADDLE_Y - BALL_SIZE - 2);
}

static bool rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static bool breakable_bricks_left(void)
{
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            brick_t *b = &s_bricks[row][col];
            if (b->alive && b->kind != BRICK_GOLD) {
                return true;
            }
        }
    }
    return false;
}

static void draw_victory_trophy(void)
{
    const int cx = LCD_WIDTH / 2;
    const int cy = LCD_HEIGHT / 2 - 8;

    st7789_fill_box(cx - 36, cy - 28, 72, 56, 0x0841);
    st7789_fill_box(cx - 34, cy - 26, 68, 52, COLOR_BG);

    st7789_fill_box(cx - 22, cy + 14, 44, 8, COLOR_TROPHY_TRIM);
    st7789_fill_box(cx - 18, cy + 10, 36, 6, COLOR_TROPHY);
    st7789_fill_box(cx - 12, cy - 6, 24, 18, COLOR_TROPHY);
    st7789_fill_box(cx - 16, cy - 2, 6, 10, COLOR_TROPHY_TRIM);
    st7789_fill_box(cx + 10, cy - 2, 6, 10, COLOR_TROPHY_TRIM);
    st7789_fill_box(cx - 4, cy - 18, 8, 14, COLOR_STAR);

    st7789_fill_box(cx - 2, cy - 26, 4, 8, COLOR_STAR);
    st7789_fill_box(cx - 8, cy - 20, 16, 4, COLOR_STAR);
    st7789_fill_box(cx - 12, cy - 22, 4, 4, COLOR_STAR);
    st7789_fill_box(cx + 8, cy - 22, 4, 4, COLOR_STAR);
}

static void begin_victory(void)
{
    if (s_ball_launched) {
        erase_ball_at(s_ball.x, s_ball.y);
    }
    s_ball_launched = false;
    s_victory = true;
    s_victory_drawn = false;
    buzzer_sfx_game_start();
}

static void draw_victory_if_needed(void)
{
    if (!s_victory_drawn) {
        draw_victory_trophy();
        s_victory_drawn = true;
    }
}

static void scene_reset(void)
{
    st7789_fill_rect(0, LCD_HEIGHT, COLOR_BG);
    bricks_load_level(s_level);
    draw_all_bricks();

    s_paddle.x = (LCD_WIDTH - PADDLE_W) / 2;
    s_paddle.y = PADDLE_Y;
    draw_paddle_at(s_paddle.x);

    s_ball_vel.x = BALL_SPEED_X;
    s_ball_vel.y = -BALL_SPEED_Y;
    s_ball_launched = false;
    s_select_was_down = gamepad_select_pressed();
    s_victory = false;
    s_victory_drawn = false;
    ball_snap_to_paddle();
    draw_ball_at(s_ball.x, s_ball.y);
}

static void launch_ball(void)
{
    if (s_ball_launched || s_victory) {
        return;
    }
    s_ball_launched = true;
    int center = s_paddle.x + PADDLE_W / 2;
    int ball_cx = s_ball.x + BALL_SIZE / 2;
    int dx = ball_cx - center;
    if (dx > 6) {
        s_ball_vel.x = BALL_SPEED_X;
    } else if (dx < -6) {
        s_ball_vel.x = (int16_t)-BALL_SPEED_X;
    } else {
        s_ball_vel.x = (int16_t)((s_paddle.x & 1) ? BALL_SPEED_X : -BALL_SPEED_X);
    }
    s_ball_vel.y = (int16_t)(-BALL_SPEED_Y);
    buzzer_tone(660, 40);
}

static bool edge_select(void)
{
    bool now = gamepad_select_pressed();
    bool edge = now && !s_select_was_down;
    s_select_was_down = now;
    return edge;
}

static void read_paddle(void)
{
    if (s_victory) {
        return;
    }

    int16_t nx = s_paddle.x;
    if (gamepad_pressed(PAD1_LEFT) || gamepad_pressed(PAD2_LEFT)) {
        nx -= PADDLE_SPEED;
    }
    if (gamepad_pressed(PAD1_RIGHT) || gamepad_pressed(PAD2_RIGHT)) {
        nx += PADDLE_SPEED;
    }
    if (nx < 0) {
        nx = 0;
    }
    if (nx + PADDLE_W > LCD_WIDTH) {
        nx = (int16_t)(LCD_WIDTH - PADDLE_W);
    }

    bool paddle_moved = nx != s_paddle.x;
    if (paddle_moved) {
        erase_paddle_at(s_paddle.x);
        if (!s_ball_launched) {
            erase_ball_at(s_ball.x, s_ball.y);
        }
        s_paddle.x = nx;
        draw_paddle_at(s_paddle.x);
        if (!s_ball_launched) {
            ball_snap_to_paddle();
            draw_ball_at(s_ball.x, s_ball.y);
        }
    } else if (!s_ball_launched) {
        int16_t bx = (int16_t)(s_paddle.x + PADDLE_W / 2 - BALL_SIZE / 2);
        int16_t by = (int16_t)(PADDLE_Y - BALL_SIZE - 2);
        if (bx != s_ball.x || by != s_ball.y) {
            erase_ball_at(s_ball.x, s_ball.y);
            s_ball.x = bx;
            s_ball.y = by;
            draw_ball_at(s_ball.x, s_ball.y);
        }
    }
}

static void reset_ball_on_paddle(void)
{
    erase_ball_at(s_ball.x, s_ball.y);
    s_ball_launched = false;
    ball_snap_to_paddle();
    draw_ball_at(s_ball.x, s_ball.y);
    buzzer_sfx_lose();
}

static void hit_brick(brick_t *b, int ball_cx, int ball_cy)
{
    if (b->kind == BRICK_GOLD) {
        bounce_off_brick(ball_cx, ball_cy, b);
        buzzer_tone(300, 12);
        return;
    }

    if (b->kind == BRICK_SILVER && b->hits > 1) {
        b->hits--;
        b->color = 0x8410;
        draw_brick(b);
        bounce_off_brick(ball_cx, ball_cy, b);
        buzzer_tone(520, 20);
        return;
    }

    b->alive = false;
    erase_brick(b);
    bounce_off_brick(ball_cx, ball_cy, b);
    buzzer_sfx_eat();

    if (!breakable_bricks_left()) {
        begin_victory();
    }
}

static void tick_physics(void)
{
    if (!s_ball_launched || s_victory) {
        return;
    }

    int16_t ox = s_ball.x;
    int16_t oy = s_ball.y;
    int16_t nx = (int16_t)(s_ball.x + s_ball_vel.x);
    int16_t ny = (int16_t)(s_ball.y + s_ball_vel.y);

    if (nx < 0) {
        nx = 0;
        s_ball_vel.x = (int16_t)(-s_ball_vel.x);
    } else if (nx + BALL_SIZE > LCD_WIDTH) {
        nx = (int16_t)(LCD_WIDTH - BALL_SIZE);
        s_ball_vel.x = (int16_t)(-s_ball_vel.x);
    }

    if (ny < 0) {
        ny = 0;
        s_ball_vel.y = (int16_t)(-s_ball_vel.y);
    }

    if (ny + BALL_SIZE >= LCD_HEIGHT) {
        reset_ball_on_paddle();
        return;
    }

    int ball_cx = nx + BALL_SIZE / 2;
    int ball_cy = ny + BALL_SIZE / 2;

    if (s_ball_vel.y > 0 && ny + BALL_SIZE >= PADDLE_Y && ny + BALL_SIZE <= PADDLE_Y + PADDLE_H + 2 &&
        ball_cx >= s_paddle.x && ball_cx <= s_paddle.x + PADDLE_W) {
        ny = (int16_t)(PADDLE_Y - BALL_SIZE);
        s_ball_vel.y = (int16_t)(-BALL_SPEED_Y);
        int hit = ball_cx - (s_paddle.x + PADDLE_W / 2);
        if (hit > 4) {
            s_ball_vel.x = BALL_SPEED_X;
        } else if (hit < -4) {
            s_ball_vel.x = (int16_t)-BALL_SPEED_X;
        }
        buzzer_tone(440, 15);
    }

    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            brick_t *b = &s_bricks[row][col];
            if (!b->alive) {
                continue;
            }
            if (rects_overlap(nx, ny, BALL_SIZE, BALL_SIZE, b->x, b->y, BRICK_W, BRICK_H)) {
                hit_brick(b, ball_cx, ball_cy);
                goto moved;
            }
        }
    }

moved:
    if (nx != ox || ny != oy) {
        erase_ball_at(ox, oy);
        s_ball.x = nx;
        s_ball.y = ny;
        draw_ball_at(s_ball.x, s_ball.y);
    }
}

static void advance_after_victory(void)
{
    s_level = (s_level + 1) % LEVEL_COUNT;
    scene_reset();
}

void arkanoid_run(void)
{
    gamepad_init();
    s_level = 0;
    scene_reset();

    for (;;) {
        if (s_victory) {
            draw_victory_if_needed();
            if (edge_select()) {
                advance_after_victory();
            }
        } else {
            if (edge_select()) {
                launch_ball();
            }
            read_paddle();
            tick_physics();
        }
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}