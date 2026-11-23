#include "pacman.h"

#include "buzzer.h"
#include "gamepad.h"
#include "st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define CELL 12
#define GRID (LCD_WIDTH / CELL)
#define TICK_MS 420
#define INPUT_POLL_MS 20

#define COLOR_BG 0x0000
#define COLOR_WALL 0x0015
#define COLOR_DOT 0xBDF7
#define COLOR_PAC1 0xFFE0
#define COLOR_PAC2 0xFD20
#define COLOR_GHOST1 0x97F5
#define COLOR_GHOST2 0xBFFC
#define COLOR_GHOST_EYE 0xFFFF

#define WALL 1
#define DOT 0
#define PATH 2

typedef struct {
    int8_t x;
    int8_t y;
    dir_t dir;
    dir_t want;
    bool active;
    uint16_t color;
    int respawn_ms;
} pac_t;

typedef struct {
    int8_t x;
    int8_t y;
    dir_t dir;
    bool active;
    uint16_t color;
    int leave_ms;
} ghost_t;

static uint8_t s_map[GRID][GRID];
static pac_t s_p1;
static pac_t s_p2;
static ghost_t s_ghost[2];
static bool s_sel_down[2];
static int s_dots_left;
static int16_t s_ghost_last_px[2];
static int16_t s_ghost_last_py[2];
static bool s_ghost_last_valid[2];

static int8_t s_p1_spawn_x = 2;
static int8_t s_p1_spawn_y = 15;
static int8_t s_p2_spawn_x = 17;
static int8_t s_p2_spawn_y = 15;
static int8_t s_gh_spawn_x = 9;
static int8_t s_gh_spawn_y = 6;

static void paint_cell(int x, int y, uint16_t color)
{
    st7789_fill_box(x * CELL, y * CELL, CELL, CELL, color);
}

static void paint_dot(int x, int y)
{
    paint_cell(x, y, COLOR_BG);
    int cx = x * CELL + (CELL / 2) - 1;
    int cy = y * CELL + (CELL / 2) - 1;
    st7789_fill_box(cx, cy, 2, 2, COLOR_DOT);
}

static void paint_wall(int x, int y)
{
    paint_cell(x, y, COLOR_WALL);
    st7789_fill_box(x * CELL + 1, y * CELL + 1, CELL - 2, CELL - 2, 0x0010);
}

static void paint_pac_sprite(int px, int py, dir_t dir, uint16_t color)
{
    st7789_fill_box(px + 1, py + 1, CELL - 2, CELL - 2, color);
    switch (dir) {
    case DIR_RIGHT:
        st7789_fill_box(px + CELL / 2, py + 3, CELL / 2 - 1, CELL - 6, color);
        st7789_fill_box(px + 3, py + 3, 3, 3, COLOR_BG);
        break;
    case DIR_LEFT:
        st7789_fill_box(px + 1, py + 3, CELL / 2 - 1, CELL - 6, color);
        st7789_fill_box(px + CELL - 6, py + 3, 3, 3, COLOR_BG);
        break;
    case DIR_UP:
        st7789_fill_box(px + 3, py + 1, CELL - 6, CELL / 2 - 1, color);
        st7789_fill_box(px + 3, py + CELL - 6, 3, 3, COLOR_BG);
        break;
    case DIR_DOWN:
        st7789_fill_box(px + 3, py + CELL / 2, CELL - 6, CELL / 2 - 1, color);
        st7789_fill_box(px + 3, py + 3, 3, 3, COLOR_BG);
        break;
    }
}

static void paint_pac_at(int gx, int gy, dir_t dir, uint16_t color)
{
    paint_pac_sprite(gx * CELL, gy * CELL, dir, color);
}

static void paint_pac(const pac_t *p)
{
    if (!p->active || p->respawn_ms > 0) {
        return;
    }
    paint_pac_at(p->x, p->y, p->dir, p->color);
}

static void paint_ghost_sprite(int px, int py, uint16_t body)
{
    st7789_fill_box(px + 1, py + 1, CELL - 2, CELL - 2, body);
    st7789_fill_box(px + 2, py + CELL - 4, CELL - 4, 3, COLOR_BG);
    st7789_fill_box(px + 3, py + 3, 3, 3, COLOR_GHOST_EYE);
    st7789_fill_box(px + CELL - 6, py + 3, 3, 3, COLOR_GHOST_EYE);
    st7789_fill_box(px + 4, py + 4, 1, 2, COLOR_BG);
    st7789_fill_box(px + CELL - 5, py + 4, 1, 2, COLOR_BG);
}

static void paint_ghost_at(int gx, int gy, uint16_t body)
{
    paint_ghost_sprite(gx * CELL, gy * CELL, body);
}

static void paint_ghost(const ghost_t *g)
{
    if (!g->active) {
        return;
    }
    uint16_t body = g->color ? g->color : COLOR_GHOST1;
    paint_ghost_at(g->x, g->y, body);
}

static bool in_bounds(int x, int y)
{
    return x >= 0 && y >= 0 && x < GRID && y < GRID;
}

static bool can_enter(int x, int y)
{
    return in_bounds(x, y) && s_map[y][x] != WALL;
}

static bool has_dot(int x, int y)
{
    return in_bounds(x, y) && s_map[y][x] == DOT;
}

static void build_maze(void)
{
    static const uint8_t layout[20][20] = {
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1},
        {1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 1, 1, 1, 1, 0, 1, 1, 2, 2, 1, 1, 0, 1, 1, 1, 1, 0, 1},
        {1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1},
        {1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1},
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    };

    s_dots_left = 0;
    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            s_map[y][x] = layout[y][x];
            if (has_dot(x, y)) {
                s_dots_left++;
            }
        }
    }
}

static void paint_map_cell(int x, int y)
{
    if (!in_bounds(x, y)) {
        return;
    }
    if (s_map[y][x] == WALL) {
        paint_wall(x, y);
    } else if (has_dot(x, y)) {
        paint_dot(x, y);
    } else {
        paint_cell(x, y, COLOR_BG);
    }
}

static uint32_t s_maze_repaint_row[GRID];

static void maze_repaint_clear(void)
{
    memset(s_maze_repaint_row, 0, sizeof(s_maze_repaint_row));
}

static void maze_repaint_mark_tile(int tx, int ty)
{
    if (tx < 0 || ty < 0 || tx >= GRID || ty >= GRID) {
        return;
    }
    s_maze_repaint_row[ty] |= 1UL << tx;
}

static void maze_repaint_mark_sprite(int px, int py)
{
    const int x0 = px / CELL;
    const int y0 = py / CELL;
    const int x1 = (px + CELL - 1) / CELL;
    const int y1 = (py + CELL - 1) / CELL;

    for (int ty = y0; ty <= y1; ty++) {
        if (ty < 0 || ty >= GRID) {
            continue;
        }
        for (int tx = x0; tx <= x1; tx++) {
            if (tx < 0 || tx >= GRID) {
                continue;
            }
            s_maze_repaint_row[ty] |= 1UL << tx;
        }
    }
}

static int slide_px(int x0, int x1, int elapsed)
{
    if (x0 == x1) {
        return x0 * CELL;
    }
    return x0 * CELL + (x1 - x0) * CELL * elapsed / TICK_MS;
}

static int slide_py(int y0, int y1, int elapsed)
{
    if (y0 == y1) {
        return y0 * CELL;
    }
    return y0 * CELL + (y1 - y0) * CELL * elapsed / TICK_MS;
}

static void maze_repaint_mark_slide(int8_t x0, int8_t y0, int8_t x1, int8_t y1, int elapsed)
{
    maze_repaint_mark_sprite(slide_px(x0, x1, elapsed), slide_py(y0, y1, elapsed));
    maze_repaint_mark_tile(x0, y0);
    maze_repaint_mark_tile(x1, y1);
    if (y0 == y1 && x0 != x1) {
        int lo = x0 < x1 ? x0 : x1;
        int hi = x0 > x1 ? x0 : x1;
        for (int x = lo; x <= hi; x++) {
            maze_repaint_mark_tile(x, y0);
        }
    } else if (x0 == x1 && y0 != y1) {
        int lo = y0 < y1 ? y0 : y1;
        int hi = y0 > y1 ? y0 : y1;
        for (int y = lo; y <= hi; y++) {
            maze_repaint_mark_tile(x0, y);
        }
    }
}

static void maze_repaint_flush(void)
{
    for (int y = 0; y < GRID; y++) {
        uint32_t mask = s_maze_repaint_row[y];
        for (int x = 0; x < GRID; x++) {
            if (mask & (1UL << x)) {
                paint_map_cell(x, y);
            }
        }
    }
}

static void draw_maze(void)
{
    st7789_fill_rect(0, LCD_HEIGHT, COLOR_BG);
    for (int y = 0; y < GRID; y++) {
        for (int x = 0; x < GRID; x++) {
            paint_map_cell(x, y);
        }
    }
}

static int active_players(void)
{
    int n = 0;
    if (s_p1.active) {
        n++;
    }
    if (s_p2.active) {
        n++;
    }
    return n;
}

static void eat_dot(int x, int y)
{
    if (!has_dot(x, y)) {
        return;
    }
    s_map[y][x] = PATH;
    if (s_dots_left > 0) {
        s_dots_left--;
    }
}

static void cell_after_dir(int x, int y, dir_t dir, int *nx, int *ny)
{
    *nx = x;
    *ny = y;
    switch (dir) {
    case DIR_UP:
        (*ny)--;
        break;
    case DIR_DOWN:
        (*ny)++;
        break;
    case DIR_LEFT:
        (*nx)--;
        break;
    case DIR_RIGHT:
        (*nx)++;
        break;
    }
}

static void apply_want(pac_t *p)
{
    int nx;
    int ny;
    cell_after_dir(p->x, p->y, p->want, &nx, &ny);
    if (can_enter(nx, ny)) {
        p->dir = p->want;
    }
}

static void move_pac(pac_t *p)
{
    int nx;
    int ny;

    if (!p->active || p->respawn_ms > 0) {
        return;
    }

    apply_want(p);
    cell_after_dir(p->x, p->y, p->dir, &nx, &ny);
    if (!can_enter(nx, ny)) {
        return;
    }

    p->x = (int8_t)nx;
    p->y = (int8_t)ny;
    eat_dot(nx, ny);
}

static pac_t *nearest_pac(int gx, int gy)
{
    pac_t *best = NULL;
    int best_d = 0x7fffffff;
    pac_t *list[2] = {&s_p1, &s_p2};

    for (int i = 0; i < 2; i++) {
        pac_t *p = list[i];
        if (!p->active || p->respawn_ms > 0) {
            continue;
        }
        int d = (p->x - gx) * (p->x - gx) + (p->y - gy) * (p->y - gy);
        if (d < best_d) {
            best_d = d;
            best = p;
        }
    }
    return best;
}

static dir_t opposite(dir_t d)
{
    return (dir_t)(d ^ 2);
}

static dir_t pick_chase_dir(const ghost_t *g, const pac_t *target)
{
    static const dir_t k_try[] = {DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT};
    dir_t back = opposite(g->dir);
    dir_t best = g->dir;
    int best_d = 0x7fffffff;
    bool any = false;

    for (size_t i = 0; i < sizeof(k_try) / sizeof(k_try[0]); i++) {
        dir_t d = k_try[i];
        if (d == back) {
            continue;
        }
        int nx;
        int ny;
        cell_after_dir(g->x, g->y, d, &nx, &ny);
        if (!can_enter(nx, ny)) {
            continue;
        }
        any = true;
        if (!target) {
            return d;
        }
        int dscore = (target->x - nx) * (target->x - nx) + (target->y - ny) * (target->y - ny);
        if (dscore < best_d) {
            best_d = dscore;
            best = d;
        }
    }
    if (!any) {
        int nx;
        int ny;
        cell_after_dir(g->x, g->y, back, &nx, &ny);
        if (can_enter(nx, ny)) {
            return back;
        }
        return g->dir;
    }
    return best;
}

static void move_ghost(ghost_t *g)
{
    int nx;
    int ny;
    pac_t *target;

    if (!g->active) {
        return;
    }
    if (g->leave_ms > 0) {
        g->leave_ms -= TICK_MS;
        return;
    }

    target = nearest_pac(g->x, g->y);
    if (!target) {
        return;
    }

    g->dir = pick_chase_dir(g, target);
    cell_after_dir(g->x, g->y, g->dir, &nx, &ny);
    if (!can_enter(nx, ny)) {
        return;
    }

    g->x = (int8_t)nx;
    g->y = (int8_t)ny;
}

static void spawn_pac(pac_t *p, int8_t sx, int8_t sy, uint16_t color)
{
    if (!p->active && p->x == 0 && p->y == 0) {
        maze_repaint_clear();
        maze_repaint_mark_sprite(0, 0);
        maze_repaint_flush();
    }
    memset(p, 0, sizeof(*p));
    p->x = sx;
    p->y = sy;
    p->dir = DIR_RIGHT;
    p->want = DIR_RIGHT;
    p->active = true;
    p->color = color;
    eat_dot(sx, sy);
    paint_pac(p);
}

static void place_ghost(ghost_t *g, int8_t gx, int8_t gy, uint16_t color)
{
    g->x = gx;
    g->y = gy;
    g->dir = DIR_UP;
    g->color = color;
    g->active = true;
    paint_ghost(g);
}

static void sync_ghosts(void)
{
    int need = active_players();

    for (int i = 0; i < 2; i++) {
        if (i < need) {
            if (!s_ghost[i].active) {
                if (s_ghost[i].x == 0 && s_ghost[i].y == 0) {
                    maze_repaint_clear();
                    maze_repaint_mark_sprite(0, 0);
                    maze_repaint_flush();
                }
                place_ghost(&s_ghost[i],
                            (int8_t)(s_gh_spawn_x + i),
                            s_gh_spawn_y,
                            (i == 0) ? COLOR_GHOST1 : COLOR_GHOST2);
                s_ghost[i].leave_ms = 800 + i * 400;
            }
        } else if (s_ghost[i].active) {
            maze_repaint_clear();
            maze_repaint_mark_tile(s_ghost[i].x, s_ghost[i].y);
            maze_repaint_flush();
            memset(&s_ghost[i], 0, sizeof(s_ghost[i]));
        }
    }
}

static bool edge_player_select(int player)
{
    gpio_num_t pin = player == 0 ? PAD1_SELECT : PAD2_SELECT;
    bool now = gamepad_pressed(pin);
    bool fire = now && !s_sel_down[player];
    s_sel_down[player] = now;
    return fire;
}

static bool read_pad_dir(int player, dir_t *out)
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
            *out = keys[i].dir;
            return true;
        }
    }
    return false;
}

static void kill_pac(pac_t *p, int8_t sx, int8_t sy)
{
    if (!p->active) {
        return;
    }
    maze_repaint_clear();
    maze_repaint_mark_tile(p->x, p->y);
    maze_repaint_flush();
    p->x = sx;
    p->y = sy;
    p->dir = DIR_RIGHT;
    p->want = DIR_RIGHT;
    p->respawn_ms = 2000;
    buzzer_sfx_lose();
}

static void check_collisions(void)
{
    for (int gi = 0; gi < 2; gi++) {
        ghost_t *g = &s_ghost[gi];
        if (!g->active || g->leave_ms > 0) {
            continue;
        }
        pac_t *list[2] = {&s_p1, &s_p2};
        int8_t sp_x[2] = {s_p1_spawn_x, s_p2_spawn_x};
        int8_t sp_y[2] = {s_p1_spawn_y, s_p2_spawn_y};
        for (int pi = 0; pi < 2; pi++) {
            pac_t *p = list[pi];
            if (!p->active || p->respawn_ms > 0) {
                continue;
            }
            if (p->x == g->x && p->y == g->y) {
                kill_pac(p, sp_x[pi], sp_y[pi]);
            }
        }
    }
}

static void tick_respawn(pac_t *p)
{
    if (!p->active || p->respawn_ms <= 0) {
        return;
    }
    p->respawn_ms -= TICK_MS;
    if (p->respawn_ms <= 0) {
        p->respawn_ms = 0;
        paint_pac(p);
        buzzer_tone(660, 40);
    }
}

static bool round_won(void)
{
    return s_dots_left <= 0 && active_players() > 0;
}

static void celebrate_win(void)
{
    for (int y = 0; y < GRID; y += 2) {
        st7789_fill_box(0, y * CELL, LCD_WIDTH, CELL, COLOR_PAC1);
        vTaskDelay(pdMS_TO_TICKS(60));
        st7789_fill_box(0, y * CELL, LCD_WIDTH, CELL, COLOR_BG);
    }
    buzzer_sfx_eat();
    vTaskDelay(pdMS_TO_TICKS(500));
}

static void round_reset(void)
{
    memset(&s_p1, 0, sizeof(s_p1));
    memset(&s_p2, 0, sizeof(s_p2));
    memset(s_ghost, 0, sizeof(s_ghost));
    build_maze();
    draw_maze();
}

static void redraw_actors(void)
{
    paint_pac(&s_p1);
    paint_pac(&s_p2);
    for (int i = 0; i < 2; i++) {
        paint_ghost(&s_ghost[i]);
    }
}

static void poll_player_join(void)
{
    if (edge_player_select(0) && !s_p1.active) {
        spawn_pac(&s_p1, s_p1_spawn_x, s_p1_spawn_y, COLOR_PAC1);
        buzzer_sfx_game_start();
        sync_ghosts();
    }
    if (edge_player_select(1) && !s_p2.active) {
        spawn_pac(&s_p2, s_p2_spawn_x, s_p2_spawn_y, COLOR_PAC2);
        buzzer_tone(988, 50);
        sync_ghosts();
    }
}

static void poll_want(void)
{
    dir_t next;

    poll_player_join();
    if (s_p1.active && s_p1.respawn_ms <= 0 && read_pad_dir(0, &next)) {
        s_p1.want = next;
    }
    if (s_p2.active && s_p2.respawn_ms <= 0 && read_pad_dir(1, &next)) {
        s_p2.want = next;
    }
}

/* Visual slide only; logic cells were already updated. */
static void animate_tick(int8_t p1_x0,
                         int8_t p1_y0,
                         int8_t p1_xe,
                         int8_t p1_ye,
                         int8_t p2_x0,
                         int8_t p2_y0,
                         int8_t p2_xe,
                         int8_t p2_ye,
                         int8_t g0_x0,
                         int8_t g0_y0,
                         int8_t g0_xe,
                         int8_t g0_ye,
                         int8_t g1_x0,
                         int8_t g1_y0,
                         int8_t g1_xe,
                         int8_t g1_ye,
                         const bool pac_active_start[2],
                         const bool ghost_active_start[2])
{
    int elapsed = 0;
    int8_t gx0[2] = {g0_x0, g1_x0};
    int8_t gy0[2] = {g0_y0, g1_y0};
    int8_t gxe[2] = {g0_xe, g1_xe};
    int8_t gye[2] = {g0_ye, g1_ye};

    s_ghost_last_valid[0] = false;
    s_ghost_last_valid[1] = false;

    while (elapsed < TICK_MS) {
        int8_t pk_x0;
        int8_t pk_y0;
        int8_t pk_x1;
        int8_t pk_y1;
        poll_want();

        maze_repaint_clear();
        if (s_p1.active) {
            if (pac_active_start[0]) {
                pk_x0 = p1_x0;
                pk_y0 = p1_y0;
                pk_x1 = p1_xe;
                pk_y1 = p1_ye;
            } else {
                pk_x0 = s_p1.x;
                pk_y0 = s_p1.y;
                pk_x1 = s_p1.x;
                pk_y1 = s_p1.y;
            }
            maze_repaint_mark_sprite(slide_px(pk_x0, pk_x1, elapsed), slide_py(pk_y0, pk_y1, elapsed));
        }
        if (s_p2.active) {
            if (pac_active_start[1]) {
                pk_x0 = p2_x0;
                pk_y0 = p2_y0;
                pk_x1 = p2_xe;
                pk_y1 = p2_ye;
            } else {
                pk_x0 = s_p2.x;
                pk_y0 = s_p2.y;
                pk_x1 = s_p2.x;
                pk_y1 = s_p2.y;
            }
            maze_repaint_mark_sprite(slide_px(pk_x0, pk_x1, elapsed), slide_py(pk_y0, pk_y1, elapsed));
        }
        for (int i = 0; i < 2; i++) {
            int8_t gh_x0;
            int8_t gh_y0;
            int8_t gh_x1;
            int8_t gh_y1;

            if (!s_ghost[i].active) {
                continue;
            }
            if (ghost_active_start[i]) {
                gh_x0 = gx0[i];
                gh_y0 = gy0[i];
                gh_x1 = gxe[i];
                gh_y1 = gye[i];
            } else {
                gh_x0 = s_ghost[i].x;
                gh_y0 = s_ghost[i].y;
                gh_x1 = s_ghost[i].x;
                gh_y1 = s_ghost[i].y;
            }
            if (s_ghost_last_valid[i]) {
                maze_repaint_mark_sprite(s_ghost_last_px[i], s_ghost_last_py[i]);
            }
            maze_repaint_mark_slide(gh_x0, gh_y0, gh_x1, gh_y1, elapsed);
        }
        maze_repaint_flush();

        if (s_p1.active) {
            if (pac_active_start[0]) {
                pk_x0 = p1_x0;
                pk_y0 = p1_y0;
                pk_x1 = p1_xe;
                pk_y1 = p1_ye;
            } else {
                pk_x0 = s_p1.x;
                pk_y0 = s_p1.y;
                pk_x1 = s_p1.x;
                pk_y1 = s_p1.y;
            }
            paint_pac_sprite(slide_px(pk_x0, pk_x1, elapsed),
                             slide_py(pk_y0, pk_y1, elapsed),
                             s_p1.dir,
                             s_p1.color);
        }
        if (s_p2.active) {
            if (pac_active_start[1]) {
                pk_x0 = p2_x0;
                pk_y0 = p2_y0;
                pk_x1 = p2_xe;
                pk_y1 = p2_ye;
            } else {
                pk_x0 = s_p2.x;
                pk_y0 = s_p2.y;
                pk_x1 = s_p2.x;
                pk_y1 = s_p2.y;
            }
            paint_pac_sprite(slide_px(pk_x0, pk_x1, elapsed),
                             slide_py(pk_y0, pk_y1, elapsed),
                             s_p2.dir,
                             s_p2.color);
        }
        for (int i = 0; i < 2; i++) {
            int16_t gpx;
            int16_t gpy;
            uint16_t body;
            int8_t gh_x0;
            int8_t gh_y0;
            int8_t gh_x1;
            int8_t gh_y1;

            if (!s_ghost[i].active) {
                continue;
            }
            if (ghost_active_start[i]) {
                gh_x0 = gx0[i];
                gh_y0 = gy0[i];
                gh_x1 = gxe[i];
                gh_y1 = gye[i];
            } else {
                gh_x0 = s_ghost[i].x;
                gh_y0 = s_ghost[i].y;
                gh_x1 = s_ghost[i].x;
                gh_y1 = s_ghost[i].y;
            }
            body = s_ghost[i].color ? s_ghost[i].color : COLOR_GHOST1;
            gpx = (int16_t)slide_px(gh_x0, gh_x1, elapsed);
            gpy = (int16_t)slide_py(gh_y0, gh_y1, elapsed);
            paint_ghost_sprite(gpx, gpy, body);
            s_ghost_last_px[i] = gpx;
            s_ghost_last_py[i] = gpy;
            s_ghost_last_valid[i] = true;
        }

        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
        elapsed += INPUT_POLL_MS;
    }
}

static void round_play(void)
{
    round_reset();

    for (;;) {
        if (active_players() == 0) {
            poll_player_join();
            vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
            continue;
        }

        bool pac_active_start[2] = {s_p1.active, s_p2.active};
        int8_t p1_x0 = s_p1.x;
        int8_t p1_y0 = s_p1.y;
        int8_t p2_x0 = s_p2.x;
        int8_t p2_y0 = s_p2.y;
        bool ghost_active_start[2] = {s_ghost[0].active, s_ghost[1].active};
        int8_t g0_x0 = s_ghost[0].x;
        int8_t g0_y0 = s_ghost[0].y;
        int8_t g1_x0 = s_ghost[1].x;
        int8_t g1_y0 = s_ghost[1].y;

        move_pac(&s_p1);
        move_pac(&s_p2);
        for (int i = 0; i < 2; i++) {
            move_ghost(&s_ghost[i]);
        }

        int8_t g0_xe = s_ghost[0].x;
        int8_t g0_ye = s_ghost[0].y;
        int8_t g1_xe = s_ghost[1].x;
        int8_t g1_ye = s_ghost[1].y;

        int8_t p1_x1 = s_p1.x;
        int8_t p1_y1 = s_p1.y;
        int8_t p2_x1 = s_p2.x;
        int8_t p2_y1 = s_p2.y;

        check_collisions();

        int8_t p1_xe = s_p1.respawn_ms > 0 ? p1_x1 : s_p1.x;
        int8_t p1_ye = s_p1.respawn_ms > 0 ? p1_y1 : s_p1.y;
        int8_t p2_xe = s_p2.respawn_ms > 0 ? p2_x1 : s_p2.x;
        int8_t p2_ye = s_p2.respawn_ms > 0 ? p2_y1 : s_p2.y;

        animate_tick(p1_x0,
                     p1_y0,
                     p1_xe,
                     p1_ye,
                     p2_x0,
                     p2_y0,
                     p2_xe,
                     p2_ye,
                     g0_x0,
                     g0_y0,
                     g0_xe,
                     g0_ye,
                     g1_x0,
                     g1_y0,
                     g1_xe,
                     g1_ye,
                     pac_active_start,
                     ghost_active_start);

        tick_respawn(&s_p1);
        tick_respawn(&s_p2);

        if (round_won()) {
            celebrate_win();
            return;
        }
    }
}

void pacman_run(void)
{
    gamepad_init();
    s_sel_down[0] = gamepad_pressed(PAD1_SELECT);
    s_sel_down[1] = gamepad_pressed(PAD2_SELECT);

    for (;;) {
        round_play();
    }
}