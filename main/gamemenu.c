#include "gamemenu.h"

#include "buzzer.h"
#include "gamepad.h"
#include "st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MENU_COLS 3
#define MENU_ROWS 2
#define SLOT_W 72
#define SLOT_H 94
#define SLOT_GAP 6
#define MENU_LEFT ((LCD_WIDTH - (MENU_COLS * SLOT_W + (MENU_COLS - 1) * SLOT_GAP)) / 2)
#define MENU_TOP ((LCD_HEIGHT - (MENU_ROWS * SLOT_H + (MENU_ROWS - 1) * SLOT_GAP)) / 2)

#define COLOR_BG 0x0000
#define COLOR_SLOT 0x1082
#define COLOR_SLOT_EMPTY 0x0841
#define COLOR_BORDER 0x39E7
#define COLOR_BORDER_SEL 0xFFE0
#define COLOR_SNAKE 0x07E0
#define COLOR_FOOD 0xFFE0
#define COLOR_PADDLE 0xFFFF
#define COLOR_PAC 0xFFE0
#define COLOR_GHOST 0xF800

static const bool k_playable[GAME_MENU_SLOTS] = {
    true,
    true,
    true,
    false,
    false,
    false,
};

static bool s_sel_down;
static bool s_nav_down[4];

static int slot_x(int col)
{
    return MENU_LEFT + col * (SLOT_W + SLOT_GAP);
}

static int slot_y(int row)
{
    return MENU_TOP + row * (SLOT_H + SLOT_GAP);
}

static void slot_rect(int index, int *x, int *y)
{
    int row = index / MENU_COLS;
    int col = index % MENU_COLS;
    *x = slot_x(col);
    *y = slot_y(row);
}

static bool any_dir_pressed(dir_t *out)
{
    const struct {
        gpio_num_t a;
        gpio_num_t b;
        dir_t dir;
    } keys[] = {
        {PAD1_UP, PAD2_UP, DIR_UP},
        {PAD1_DOWN, PAD2_DOWN, DIR_DOWN},
        {PAD1_LEFT, PAD2_LEFT, DIR_LEFT},
        {PAD1_RIGHT, PAD2_RIGHT, DIR_RIGHT},
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (gamepad_pressed(keys[i].a) || gamepad_pressed(keys[i].b)) {
            *out = keys[i].dir;
            return true;
        }
    }
    return false;
}

static bool edge_select(void)
{
    bool now = gamepad_select_pressed();
    bool fire = now && !s_sel_down;
    s_sel_down = now;
    return fire;
}

static bool edge_dir(dir_t *out)
{
    dir_t dir = DIR_UP;
    bool now = any_dir_pressed(&dir);
    bool fire = false;
    if (now) {
        if (!s_nav_down[dir]) {
            fire = true;
            *out = dir;
        }
        s_nav_down[dir] = true;
    } else {
        for (int i = 0; i < 4; i++) {
            s_nav_down[i] = false;
        }
    }
    return fire;
}

static void draw_icon_snake(int ox, int oy)
{
    const int cell = 8;
    const int pad = 6;
    int base_x = ox + pad;
    int base_y = oy + pad + 8;

    for (int i = 0; i < 4; i++) {
        st7789_fill_box(base_x + i * cell, base_y, cell - 1, cell - 1, COLOR_SNAKE);
    }
    st7789_fill_box(base_x + 3 * cell, base_y - cell, cell - 1, cell - 1, COLOR_SNAKE);
    st7789_fill_box(base_x + 3 * cell, base_y - 2 * cell, cell - 1, cell - 1, COLOR_SNAKE);
    st7789_fill_box(base_x + cell, base_y + cell, cell - 1, cell - 1, COLOR_FOOD);
}

static void draw_icon_pacman(int ox, int oy)
{
    int cx = ox + SLOT_W / 2;
    int cy = oy + SLOT_H / 2 - 4;
    st7789_fill_box(cx - 10, cy - 10, 20, 20, COLOR_PAC);
    st7789_fill_box(cx + 2, cy - 6, 10, 12, COLOR_SLOT);
    st7789_fill_box(cx - 5, cy - 18, 10, 10, COLOR_GHOST);
    st7789_fill_box(cx - 3, cy - 15, 2, 2, COLOR_BG);
    st7789_fill_box(cx + 1, cy - 15, 2, 2, COLOR_BG);
}

static void draw_icon_arkanoid(int ox, int oy)
{
    const uint16_t colors[] = {0xC618, 0xFD20, 0xF800, 0xFFE0};
    const int brick_w = 12;
    const int brick_h = 5;
    const int gap = 2;
    int base_x = ox + 8;
    int base_y = oy + 10;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int x = base_x + col * (brick_w + gap);
            int y = base_y + row * (brick_h + gap);
            st7789_fill_box(x, y, brick_w, brick_h, colors[row]);
        }
    }
    st7789_fill_box(ox + 20, oy + SLOT_H - 24, 32, 4, COLOR_PADDLE);
    st7789_fill_box(ox + 33, oy + SLOT_H - 32, 5, 5, COLOR_PADDLE);
}

static void draw_slot_frame(int index, bool selected)
{
    int x;
    int y;
    slot_rect(index, &x, &y);

    bool empty = !k_playable[index];
    uint16_t fill = empty ? COLOR_SLOT_EMPTY : COLOR_SLOT;
    st7789_fill_box(x, y, SLOT_W, SLOT_H, fill);

    uint16_t border = selected ? COLOR_BORDER_SEL : COLOR_BORDER;
    st7789_fill_box(x, y, SLOT_W, 2, border);
    st7789_fill_box(x, y + SLOT_H - 2, SLOT_W, 2, border);
    st7789_fill_box(x, y, 2, SLOT_H, border);
    st7789_fill_box(x + SLOT_W - 2, y, 2, SLOT_H, border);

    if (index == GAME_SNAKE) {
        draw_icon_snake(x, y);
    } else if (index == GAME_ARKANOID) {
        draw_icon_arkanoid(x, y);
    } else if (index == GAME_PACMAN) {
        draw_icon_pacman(x, y);
    }
}

static void draw_menu(int selected)
{
    st7789_fill_rect(0, LCD_HEIGHT, COLOR_BG);
    for (int i = 0; i < GAME_MENU_SLOTS; i++) {
        draw_slot_frame(i, i == selected);
    }
}

static void move_cursor(int *selected, dir_t dir)
{
    int row = *selected / MENU_COLS;
    int col = *selected % MENU_COLS;

    switch (dir) {
    case DIR_LEFT:
        if (col > 0) {
            col--;
        }
        break;
    case DIR_RIGHT:
        if (col < MENU_COLS - 1) {
            col++;
        }
        break;
    case DIR_UP:
        if (row > 0) {
            row--;
        }
        break;
    case DIR_DOWN:
        if (row < MENU_ROWS - 1) {
            row++;
        }
        break;
    default:
        break;
    }
    *selected = row * MENU_COLS + col;
}

game_id_t gamemenu_pick(void)
{
    gamepad_init();
    s_sel_down = gamepad_select_pressed();
    for (int i = 0; i < 4; i++) {
        s_nav_down[i] = false;
    }

    int selected = GAME_SNAKE;
    draw_menu(selected);

    for (;;) {
        dir_t dir;
        if (edge_dir(&dir)) {
            int prev = selected;
            move_cursor(&selected, dir);
            if (prev != selected) {
                draw_slot_frame(prev, false);
                draw_slot_frame(selected, true);
                buzzer_tone(880, 20);
            }
        }
        if (edge_select()) {
            if (k_playable[selected]) {
                buzzer_sfx_game_start();
                return (game_id_t)selected;
            }
            buzzer_tone(220, 80);
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}