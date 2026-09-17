#include "games.h"

#include "drivers/Input/keyboard_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static void g_print_int(struct limine_video_driver *v, uint32_t n, uint32_t color) {
    if (n == 0) { v->printf("0", color); return; }
    char buf[12]; char *p = buf + 11; *p = '\0';
    while (n > 0) { *--p = '0' + (n % 10); n /= 10; }
    v->printf(p, color);
}

static uint32_t lcg_seed = 0xDEADBEEF;
static uint32_t lcg_rand(void) {
    lcg_seed = lcg_seed * 1664525u + 1013904223u;
    return lcg_seed;
}

static inline uint64_t read_uptime_ms(struct tsc_driver *tsc) {
    return tsc ? tsc->get_tsc_uptime_ms() : 0;
}

static uint8_t kbd_poll_key(struct keyboard_driver *kbd) {
    if (kbd->has_key())
        return kbd->get_key();

    if (kbd->has_extended_key()) {
        uint16_t ext = kbd->get_extended_key();
        return (uint8_t)(0x80 | (ext & 0x7F));
    }
    return 0;
}

static int sc_to_dir(struct keyboard_driver *kbd, uint8_t sc) {
    switch (kbd->scancode_to_key_code(sc)) {
        case KEY_W: case KEY_UP: return 1;
        case KEY_A: case KEY_LEFT: return 2;
        case KEY_S: case KEY_DOWN: return 3;
        case KEY_D: case KEY_RIGHT: return 4;
        default: return 0;
    }
}
static bool sc_is_esc(struct keyboard_driver *kbd, uint8_t sc) {
    return kbd->scancode_to_key_code(sc) == KEY_ESC;
}
static bool sc_is_enter(struct keyboard_driver *kbd, uint8_t sc) {
    return kbd->scancode_to_key_code(sc) == KEY_ENTER;
}
static bool sc_is_space(struct keyboard_driver *kbd, uint8_t sc) {
    return kbd->scancode_to_key_code(sc) == KEY_SPACE;
}
static int sc_to_digit(struct keyboard_driver *kbd, uint8_t sc) {
    switch (kbd->scancode_to_key_code(sc)) {
        case KEY_1: return 1;
        case KEY_2: return 2;
        case KEY_3: return 3;
        default: return 0;
    }
}

static uint8_t wait_key(struct keyboard_driver *kbd) {
    while (1) {
        uint8_t sc = kbd_poll_key(kbd);
        if (sc) return sc;
        for (volatile int i = 0; i < 200000; i++) asm volatile("pause");
    }
}

#define SNAKE_W 40
#define SNAKE_H 20
#define SNAKE_MAX 400
#define FIELD_OFF_X 2
#define FIELD_OFF_Y 3

#define COL_BORDER LIMINE_COLOR_LIGHT_BLUE
#define COL_SNAKE LIMINE_COLOR_LIGHT_GREEN
#define COL_HEAD LIMINE_COLOR_LIME
#define COL_FOOD LIMINE_COLOR_LIGHT_RED
#define COL_BG LIMINE_COLOR_BLACK
#define COL_SCORE LIMINE_COLOR_YELLOW
#define COL_GAMEOVER LIMINE_COLOR_LIGHT_RED
#define COL_TITLE LIMINE_COLOR_CYAN

typedef struct { int16_t x, y; } Vec2;

static Vec2 snake_body[SNAKE_MAX];
static int snake_len;
static int s_dir, s_next_dir;
static Vec2 s_food;
static uint32_t s_score;

static void field_char(struct limine_video_driver *v, int fx, int fy, char c, uint32_t color) {
    v->char_at(c, color,
               (uint16_t)(FIELD_OFF_X + fx),
               (uint16_t)(FIELD_OFF_Y + fy));
}

static void draw_border(struct limine_video_driver *v) {
    for (int x = 0; x <= SNAKE_W + 1; x++) {
        v->char_at('-', COL_BORDER, (uint16_t)(FIELD_OFF_X-1+x), (uint16_t)(FIELD_OFF_Y-1));
        v->char_at('-', COL_BORDER, (uint16_t)(FIELD_OFF_X-1+x), (uint16_t)(FIELD_OFF_Y+SNAKE_H));
    }
    for (int y = 0; y < SNAKE_H; y++) {
        v->char_at('|', COL_BORDER, (uint16_t)(FIELD_OFF_X-1), (uint16_t)(FIELD_OFF_Y+y));
        v->char_at('|', COL_BORDER, (uint16_t)(FIELD_OFF_X+SNAKE_W), (uint16_t)(FIELD_OFF_Y+y));
    }
    v->char_at('+', COL_BORDER, (uint16_t)(FIELD_OFF_X-1), (uint16_t)(FIELD_OFF_Y-1));
    v->char_at('+', COL_BORDER, (uint16_t)(FIELD_OFF_X+SNAKE_W), (uint16_t)(FIELD_OFF_Y-1));
    v->char_at('+', COL_BORDER, (uint16_t)(FIELD_OFF_X-1), (uint16_t)(FIELD_OFF_Y+SNAKE_H));
    v->char_at('+', COL_BORDER, (uint16_t)(FIELD_OFF_X+SNAKE_W), (uint16_t)(FIELD_OFF_Y+SNAKE_H));
}

static void draw_score_snake(struct limine_video_driver *v) {
    v->set_cursor_pos(FIELD_OFF_X-1, FIELD_OFF_Y-2);
    v->printf("SNAKE  Score: ", COL_TITLE);
    g_print_int(v, s_score, COL_SCORE);
    v->printf("  [WASD] move  [ESC] quit   ", COL_TITLE);
}

static void place_food(void) {
    for (int a = 0; a < 1000; a++) {
        int fx = (int)(lcg_rand() % (uint32_t)SNAKE_W);
        int fy = (int)(lcg_rand() % (uint32_t)SNAKE_H);
        bool hit = false;
        for (int i = 0; i < snake_len; i++)
            if (snake_body[i].x == fx && snake_body[i].y == fy) { hit = true; break; }
        if (!hit) { s_food.x = (int16_t)fx; s_food.y = (int16_t)fy; return; }
    }
    s_food.x = 0; s_food.y = 0;
}

static void snake_init(struct rtc_driver *rtc) {
    struct system_time *t = rtc->get_rtc_time();
    lcg_seed = (uint32_t)(t->seconds*1000u + t->minutes*60000u + t->hours);
    if (!lcg_seed) lcg_seed = 0xC0FFEE;
    snake_len = 3;
    for (int i = 0; i < 3; i++) {
        snake_body[i].x = (int16_t)(SNAKE_W/2 - i);
        snake_body[i].y = (int16_t)(SNAKE_H/2);
    }
    s_dir = s_next_dir = 4;
    s_score = 0;
    place_food();
}

void snake_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc, struct rtc_driver *rtc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    snake_init(rtc);
    draw_border(video);
    draw_score_snake(video);
    for (int i = 0; i < snake_len; i++)
        field_char(video, snake_body[i].x, snake_body[i].y,
                   i==0?'@':'o', i==0?COL_HEAD:COL_SNAKE);
    field_char(video, s_food.x, s_food.y, '*', COL_FOOD);

    uint64_t logic_int = 150;
    uint64_t input_int = 5;

    uint64_t last_logic = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);
    bool game_over = false;

    while (!game_over) {
        uint64_t now = read_uptime_ms(tsc);

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                int d = sc_to_dir(kbd, sc);
                if (d) {
                    bool ok = true;
                    if (s_dir==1&&d==3) ok=false;
                    if (s_dir==3&&d==1) ok=false;
                    if (s_dir==2&&d==4) ok=false;
                    if (s_dir==4&&d==2) ok=false;
                    if (ok) s_next_dir = d;
                }
                if (sc_is_esc(kbd, sc)) { game_over = true; continue; }
            }
        }

        if (now - last_logic >= logic_int) {
            last_logic = now;
            s_dir = s_next_dir;

            Vec2 tail = snake_body[snake_len-1];
            for (int i = snake_len-1; i > 0; i--) snake_body[i] = snake_body[i-1];

            Vec2 head = snake_body[0];
            switch (s_dir) {
                case 1: head.y--; break;
                case 2: head.x--; break;
                case 3: head.y++; break;
                case 4: head.x++; break;
            }
            snake_body[0] = head;

            if (head.x<0||head.x>=SNAKE_W||head.y<0||head.y>=SNAKE_H) {
                game_over = true; continue;
            }
            for (int i = 1; i < snake_len; i++)
                if (snake_body[i].x==head.x && snake_body[i].y==head.y) {
                    game_over = true; break;
                }
            if (game_over) continue;

            bool ate = (head.x==s_food.x && head.y==s_food.y);
            if (ate) {
                s_score += 10;
                if (snake_len < SNAKE_MAX) { snake_body[snake_len]=tail; snake_len++; }
                place_food();
                field_char(video, s_food.x, s_food.y, '*', COL_FOOD);
                draw_score_snake(video);
            } else {
                field_char(video, tail.x, tail.y, ' ', COL_BG);
            }
            field_char(video, snake_body[1].x, snake_body[1].y, 'o', COL_SNAKE);
            field_char(video, head.x, head.y, '@', COL_HEAD);
        }
    }

    int cx = FIELD_OFF_X + SNAKE_W/2 - 5;
    int cy = FIELD_OFF_Y + SNAKE_H/2 - 1;
    video->set_cursor_pos((uint32_t)cx, (uint32_t)cy);
    video->printf("  GAME OVER!  ", COL_GAMEOVER);
    video->set_cursor_pos((uint32_t)cx, (uint32_t)(cy+1));
    video->printf("  Score: ", COL_SCORE);
    g_print_int(video, s_score, COL_SCORE);
    video->set_cursor_pos((uint32_t)cx, (uint32_t)(cy+2));
    video->printf("  Press ENTER ", LIMINE_COLOR_LIGHT_GRAY);

    while (1) { uint8_t sc = wait_key(kbd); if (sc_is_enter(kbd, sc)||sc_is_esc(kbd, sc)) break; }
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define RPS_ROCK 0
#define RPS_SCISSORS 1
#define RPS_PAPER 2
static const char *rps_names[3] = { "ROCK", "SCISSORS", "PAPER" };

static void draw_choice(struct limine_video_driver *v, int choice, int col_x, int row_y, uint32_t color) {
    static const char *art[3][5] = {
        {" *** ","*   *","*   *"," *** "," [R] "},
        {"V   V"," V V ","  V  "," | | ","[SC] "},
        {"#####","#    ","#### ","#    ","[PP] "},
    };
    for (int row = 0; row < 5; row++) {
        const char *line = art[choice][row];
        for (int c = 0; line[c]; c++)
            v->char_at(line[c], color, (uint16_t)(col_x+c), (uint16_t)(row_y+row));
    }
}

void rps_game(struct limine_video_driver *video, struct keyboard_driver *kbd)
{
    int player_wins=0, cpu_wins=0, draws=0;
    while (1) {
        video->clear(LIMINE_COLOR_BLACK);
        video->set_cursor_visible(false);

        video->set_cursor_pos(10,1); video->printf("=== ROCK - SCISSORS - PAPER ===", LIMINE_COLOR_CYAN);
        video->set_cursor_pos(10,2);
        video->printf("You: ", LIMINE_COLOR_LIGHT_GREEN); g_print_int(video,(uint32_t)player_wins, LIMINE_COLOR_LIGHT_GREEN);
        video->printf("  CPU: ", LIMINE_COLOR_LIGHT_RED); g_print_int(video,(uint32_t)cpu_wins, LIMINE_COLOR_LIGHT_RED);
        video->printf("  Draws: ", LIMINE_COLOR_LIGHT_GRAY); g_print_int(video,(uint32_t)draws, LIMINE_COLOR_LIGHT_GRAY);

        video->set_cursor_pos(2,4); video->printf("Choose:", LIMINE_COLOR_WHITE);
        draw_choice(video, RPS_ROCK, 2, 6, LIMINE_COLOR_LIGHT_GRAY);
        draw_choice(video, RPS_SCISSORS, 12, 6, LIMINE_COLOR_LIGHT_GRAY);
        draw_choice(video, RPS_PAPER, 22, 6, LIMINE_COLOR_LIGHT_GRAY);
        video->set_cursor_pos(2,12);
        video->printf("[1] Rock   [2] Scissors   [3] Paper   [ESC] Quit", LIMINE_COLOR_YELLOW);

        int player_choice = -1;
        while (player_choice < 0) {
            uint8_t sc = wait_key(kbd);
            if (sc_is_esc(kbd, sc)) { video->set_cursor_visible(true); video->clear(LIMINE_COLOR_BLACK); return; }
            int d = sc_to_digit(kbd, sc);
            if (d>=1&&d<=3) player_choice = d-1;
        }

        int cpu_choice = (int)(lcg_rand() % 3u);
        draw_choice(video, player_choice, 2+player_choice*10, 6, LIMINE_COLOR_LIGHT_GREEN);
        video->set_cursor_pos(2,13); video->printf("CPU chose: ", LIMINE_COLOR_WHITE);
        video->printf(rps_names[cpu_choice], LIMINE_COLOR_LIGHT_RED);
        draw_choice(video, cpu_choice, 35, 6, LIMINE_COLOR_LIGHT_RED);

        int result = 0;
        if (player_choice != cpu_choice) {
            result = ((player_choice==RPS_ROCK &&cpu_choice==RPS_SCISSORS)||
                      (player_choice==RPS_SCISSORS&&cpu_choice==RPS_PAPER) ||
                      (player_choice==RPS_PAPER &&cpu_choice==RPS_ROCK)) ? 1 : -1;
        }
        video->set_cursor_pos(2,15);
        if (result==0) { video->printf(">>> DRAW! <<<", LIMINE_COLOR_YELLOW); draws++; }
        else if (result==1) { video->printf(">>> YOU WIN! <<<", LIMINE_COLOR_LIGHT_GREEN); player_wins++; }
        else { video->printf(">>> CPU WINS! <<<",LIMINE_COLOR_LIGHT_RED); cpu_wins++; }

        video->set_cursor_pos(2,17);
        video->printf("Press ENTER to play again, ESC to quit", LIMINE_COLOR_LIGHT_GRAY);
        while (1) {
            uint8_t sc = wait_key(kbd);
            if (sc_is_esc(kbd, sc)) { video->set_cursor_visible(true); video->clear(LIMINE_COLOR_BLACK); return; }
            if (sc_is_enter(kbd, sc)) break;
        }
    }
}

#define FB_W 50
#define FB_H 20
#define FB_BIRD_X 10
#define MAX_PIPES 10

typedef struct { int x; int gap_y; } Pipe;
static Pipe fb_pipes[MAX_PIPES];
static int fb_pipe_count;
static int fb_bird_y, fb_vel, fb_score;

static void fb_spawn_pipe(void) {
    if (fb_pipe_count >= MAX_PIPES) return;
    fb_pipes[fb_pipe_count].x = FB_W - 1;
    fb_pipes[fb_pipe_count].gap_y = (int)(lcg_rand()%(uint32_t)(FB_H-6))+3;
    fb_pipe_count++;
}

void flappy_bird_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    fb_bird_y=FB_H/2; fb_vel=0; fb_score=0; fb_pipe_count=0;
    fb_spawn_pipe();

    uint64_t logic_int = 150;
    uint64_t input_int = 5;
    uint64_t last_logic = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);
    bool over = false;
    bool flap = false;

    while (!over) {
        uint64_t now = read_uptime_ms(tsc);

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                enum key_code kc = kbd->scancode_to_key_code(sc);
                if (kc == KEY_W || kc == KEY_SPACE) flap = true;
                if (kc == KEY_ESC) {
                    video->set_cursor_visible(true);
                    video->clear(LIMINE_COLOR_BLACK);
                    return;
                }
            }
        }

        if (now - last_logic >= logic_int) {
            last_logic = now;

            if (flap) { fb_vel = -3; flap = false; }
            fb_vel += 1;
            if (fb_vel > 3) fb_vel = 3;
            fb_bird_y += fb_vel;
            if (fb_bird_y < 0) { fb_bird_y = 0; fb_vel = 0; }
            if (fb_bird_y >= FB_H) { fb_bird_y = FB_H-1; fb_vel = 0; }

            for (int i = 0; i < fb_pipe_count; i++) {
                fb_pipes[i].x--;
                if (fb_pipes[i].x == FB_BIRD_X) {
                    int gap = fb_pipes[i].gap_y;
                    if (fb_bird_y < gap-2 || fb_bird_y > gap+2) over = true;
                    else fb_score++;
                }
            }
            if (fb_pipe_count>0 && fb_pipes[0].x<0) {
                for (int i=0; i<fb_pipe_count-1; i++) fb_pipes[i]=fb_pipes[i+1];
                fb_pipe_count--;
            }
            if ((lcg_rand()%60)==0) fb_spawn_pipe();

            video->clear(LIMINE_COLOR_BLACK);
            video->char_at('@', LIMINE_COLOR_YELLOW, FB_BIRD_X, fb_bird_y);
            for (int i=0; i<fb_pipe_count; i++)
                for (int y=0; y<FB_H; y++)
                    if (y<fb_pipes[i].gap_y-2 || y>fb_pipes[i].gap_y+2)
                        video->char_at('|', LIMINE_COLOR_LIGHT_GREEN, fb_pipes[i].x, y);
            video->set_cursor_pos(2,1);
            video->printf("Flappy Score: ", LIMINE_COLOR_WHITE);
            g_print_int(video, (uint32_t)fb_score, LIMINE_COLOR_YELLOW);
            video->printf("  [W/SPACE] flap  [ESC] quit", LIMINE_COLOR_LIGHT_GRAY);
        }
    }

    video->set_cursor_pos(10,10); video->printf("  GAME OVER!  ", LIMINE_COLOR_LIGHT_RED);
    video->set_cursor_pos(10,11); video->printf("  Score: ", LIMINE_COLOR_YELLOW);
    g_print_int(video, (uint32_t)fb_score, LIMINE_COLOR_YELLOW);
    video->set_cursor_pos(10,12); video->printf("  Press ENTER/ESC ", LIMINE_COLOR_LIGHT_GRAY);
    while (1) { uint8_t sc = wait_key(kbd); if (sc_is_esc(kbd, sc)||sc_is_enter(kbd, sc)) break; }
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define RC_W 30
#define RC_H 20

static int rc_car_x;
static int rc_obstacles[RC_H];

void racing_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    rc_car_x = RC_W/2;
    for (int i=0; i<RC_H; i++) rc_obstacles[i]=-1;

    uint64_t logic_int = 80;
    uint64_t input_int = 5;
    uint64_t last_logic = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);

    int move_dir = 0;
    int rc_score = 0;
    bool rc_over = false;

    while (!rc_over) {
        uint64_t now = read_uptime_ms(tsc);

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                enum key_code kc = kbd->scancode_to_key_code(sc);
                if (kc==KEY_A || kc==KEY_LEFT) move_dir = -1;
                if (kc==KEY_D || kc==KEY_RIGHT) move_dir = 1;
                if (sc_is_esc(kbd, sc)) { rc_over = true; continue; }
            }
        }

        if (now - last_logic >= logic_int) {
            last_logic = now;

            rc_car_x += move_dir;
            move_dir = 0;
            if (rc_car_x < 0) rc_car_x = 0;
            if (rc_car_x >= RC_W) rc_car_x = RC_W-1;

            for (int y=RC_H-1; y>0; y--) rc_obstacles[y]=rc_obstacles[y-1];
            if ((lcg_rand()%5)==0)
                rc_obstacles[0]=(int)(lcg_rand()%(uint32_t)(RC_W-2))+1;
            else
                rc_obstacles[0]=-1;

            if (rc_obstacles[RC_H-1]==rc_car_x) rc_over=true;

            video->clear(LIMINE_COLOR_BLACK);
            for (int y=0; y<RC_H; y++) {
                video->char_at('|', LIMINE_COLOR_LIGHT_BLUE, 0, y);
                video->char_at('|', LIMINE_COLOR_LIGHT_BLUE, RC_W-1, y);
            }
            for (int y=0; y<RC_H; y++)
                if (rc_obstacles[y]>=0)
                    video->char_at('#', LIMINE_COLOR_LIGHT_RED, rc_obstacles[y], y);
            video->char_at('A', LIMINE_COLOR_LIGHT_GREEN, rc_car_x, RC_H-1);

            rc_score++;
            video->set_cursor_pos(2,1);
            video->printf("Score: ", LIMINE_COLOR_WHITE);
            g_print_int(video, (uint32_t)rc_score, LIMINE_COLOR_YELLOW);
            video->printf("  [A/D] steer  [ESC] quit", LIMINE_COLOR_LIGHT_GRAY);
        }
    }

    video->set_cursor_pos(10,10); video->printf("  CRASH!  ", LIMINE_COLOR_LIGHT_RED);
    video->set_cursor_pos(10,11); video->printf("  Score: ", LIMINE_COLOR_YELLOW);
    g_print_int(video, (uint32_t)rc_score, LIMINE_COLOR_YELLOW);
    video->set_cursor_pos(10,12); video->printf("  Press ENTER/ESC ", LIMINE_COLOR_LIGHT_GRAY);
    while (1) { uint8_t sc = wait_key(kbd); if (sc_is_esc(kbd, sc)||sc_is_enter(kbd, sc)) break; }
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define PN_W 60
#define PN_H 22
#define PN_OX 2
#define PN_OY 2
#define PN_PAD_H 3
#define PN_WIN 5

void pong_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    int pl_y=PN_H/2-1, cpu_y=PN_H/2-1;
    int bx=PN_W/2, by=PN_H/2, bdx=1, bdy=1;
    int pl_score=0, cpu_score=0;

    uint64_t logic_int = 80;
    uint64_t input_int = 5;
    uint64_t last_logic = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);
    int pl_move = 0;
    bool pn_over = false;

    while (!pn_over) {
        uint64_t now = read_uptime_ms(tsc);

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                enum key_code kc = kbd->scancode_to_key_code(sc);
                if (kc==KEY_W || kc==KEY_UP) pl_move=-1;
                if (kc==KEY_S || kc==KEY_DOWN) pl_move= 1;
                if (sc_is_esc(kbd, sc)) { pn_over=true; continue; }
            }
        }

        if (now - last_logic >= logic_int) {
            last_logic = now;

            pl_y += pl_move;
            pl_move = 0;
            if (pl_y < 0) pl_y = 0;
            if (pl_y > PN_H-PN_PAD_H) pl_y = PN_H-PN_PAD_H;

            int mid = cpu_y + PN_PAD_H/2;
            if (mid < by && cpu_y < PN_H-PN_PAD_H) cpu_y++;
            if (mid > by && cpu_y > 0) cpu_y--;

            bx += bdx; by += bdy;
            if (by<=0) { by=1; bdy= 1; }
            if (by>=PN_H-1) { by=PN_H-2; bdy=-1; }

            if (bx<=1) {
                if (by>=pl_y && by<pl_y+PN_PAD_H) {
                    bx=2; bdx=1;
                    if (by==pl_y) bdy=-1;
                    if (by==pl_y+PN_PAD_H-1) bdy= 1;
                } else { cpu_score++; bx=PN_W/2; by=PN_H/2; bdx=1; bdy=1; }
            }
            if (bx>=PN_W-2) {
                if (by>=cpu_y && by<cpu_y+PN_PAD_H) {
                    bx=PN_W-3; bdx=-1;
                    if (by==cpu_y) bdy=-1;
                    if (by==cpu_y+PN_PAD_H-1) bdy= 1;
                } else { pl_score++; bx=PN_W/2; by=PN_H/2; bdx=-1; bdy=-1; }
            }

            video->clear(LIMINE_COLOR_BLACK);
            for (int x=0; x<PN_W; x++) {
                video->char_at('-', LIMINE_COLOR_LIGHT_BLUE, (uint16_t)(PN_OX+x), (uint16_t)(PN_OY));
                video->char_at('-', LIMINE_COLOR_LIGHT_BLUE, (uint16_t)(PN_OX+x), (uint16_t)(PN_OY+PN_H+1));
            }
            for (int y=1; y<=PN_H; y+=2)
                video->char_at(':', LIMINE_COLOR_LIGHT_GRAY,
                               (uint16_t)(PN_OX+PN_W/2), (uint16_t)(PN_OY+y));
            for (int i=0; i<PN_PAD_H; i++) {
                video->char_at('[', LIMINE_COLOR_LIGHT_GREEN,
                               (uint16_t)(PN_OX+1), (uint16_t)(PN_OY+1+pl_y+i));
                video->char_at(']', LIMINE_COLOR_LIGHT_RED,
                               (uint16_t)(PN_OX+PN_W-2), (uint16_t)(PN_OY+1+cpu_y+i));
            }
            video->char_at('*', LIMINE_COLOR_YELLOW,
                           (uint16_t)(PN_OX+bx), (uint16_t)(PN_OY+1+by));

            video->set_cursor_pos(PN_OX, 0);
            video->printf("PONG  You: ", LIMINE_COLOR_CYAN);
            g_print_int(video, (uint32_t)pl_score, LIMINE_COLOR_LIGHT_GREEN);
            video->printf("  CPU: ", LIMINE_COLOR_CYAN);
            g_print_int(video, (uint32_t)cpu_score, LIMINE_COLOR_LIGHT_RED);
            video->printf("  [W/S] move  first to ", LIMINE_COLOR_LIGHT_GRAY);
            g_print_int(video, PN_WIN, LIMINE_COLOR_YELLOW);
            video->printf(" wins  [ESC] quit", LIMINE_COLOR_LIGHT_GRAY);

            if (pl_score>=PN_WIN || cpu_score>=PN_WIN) {
                int wx=PN_OX+PN_W/2-7, wy=PN_OY+PN_H/2;
                video->set_cursor_pos((uint32_t)wx,(uint32_t)wy);
                if (pl_score>=PN_WIN) video->printf("   YOU WIN!    ", LIMINE_COLOR_LIGHT_GREEN);
                else video->printf("   CPU WINS!   ", LIMINE_COLOR_LIGHT_RED);
                video->set_cursor_pos((uint32_t)wx,(uint32_t)(wy+1));
                video->printf("   Press ENTER ", LIMINE_COLOR_LIGHT_GRAY);
                while (1) { uint8_t s=wait_key(kbd); if(sc_is_enter(kbd, s)||sc_is_esc(kbd, s)) break; }
                pn_over = true;
            }
        }
    }
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define TT_W 12
#define TT_H 20
#define TT_OX 2
#define TT_OY 1

static uint8_t tt_field[TT_H][TT_W];

static const int8_t tt_shapes[7][4][4][2] = {
    {{{0,0},{1,0},{2,0},{3,0}},{{1,0},{1,1},{1,2},{1,3}},{{0,1},{1,1},{2,1},{3,1}},{{0,0},{0,1},{0,2},{0,3}}},
    {{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}},{{0,0},{1,0},{0,1},{1,1}}},
    {{{1,0},{0,1},{1,1},{2,1}},{{0,0},{0,1},{1,1},{0,2}},{{0,0},{1,0},{2,0},{1,1}},{{1,0},{0,1},{1,1},{1,2}}},
    {{{1,0},{2,0},{0,1},{1,1}},{{0,0},{0,1},{1,1},{1,2}},{{1,0},{2,0},{0,1},{1,1}},{{0,0},{0,1},{1,1},{1,2}}},
    {{{0,0},{1,0},{1,1},{2,1}},{{1,0},{0,1},{1,1},{0,2}},{{0,0},{1,0},{1,1},{2,1}},{{1,0},{0,1},{1,1},{0,2}}},
    {{{0,0},{0,1},{1,1},{2,1}},{{0,0},{1,0},{0,1},{0,2}},{{0,0},{1,0},{2,0},{2,1}},{{1,0},{1,1},{0,2},{1,2}}},
    {{{2,0},{0,1},{1,1},{2,1}},{{0,0},{0,1},{0,2},{1,2}},{{0,0},{1,0},{2,0},{0,1}},{{0,0},{1,0},{1,1},{1,2}}},
};
static const uint32_t tt_colors[7] = {
    LIMINE_COLOR_CYAN, LIMINE_COLOR_YELLOW, LIMINE_COLOR_LIGHT_MAGENTA,
    LIMINE_COLOR_LIGHT_GREEN, LIMINE_COLOR_LIGHT_RED,
    LIMINE_COLOR_LIGHT_BLUE, LIMINE_COLOR_ORANGE,
};

static int tt_piece, tt_rot, tt_px, tt_py;

static bool tt_fits(int p, int r, int px, int py) {
    for (int i=0; i<4; i++) {
        int nx=px+tt_shapes[p][r][i][0], ny=py+tt_shapes[p][r][i][1];
        if (nx<0||nx>=TT_W||ny<0||ny>=TT_H) return false;
        if (tt_field[ny][nx]) return false;
    }
    return true;
}
static void tt_lock(void) {
    for (int i=0; i<4; i++) {
        int nx=tt_px+tt_shapes[tt_piece][tt_rot][i][0];
        int ny=tt_py+tt_shapes[tt_piece][tt_rot][i][1];
        if (ny>=0&&ny<TT_H&&nx>=0&&nx<TT_W)
            tt_field[ny][nx]=(uint8_t)(tt_piece+1);
    }
}
static int tt_clear_lines(void) {
    int cl=0;
    for (int y=TT_H-1; y>=0;) {
        bool full=true;
        for (int x=0; x<TT_W; x++) if(!tt_field[y][x]){full=false;break;}
        if (full) {
            for (int row=y; row>0; row--)
                for (int x=0; x<TT_W; x++) tt_field[row][x]=tt_field[row-1][x];
            for (int x=0; x<TT_W; x++) tt_field[0][x]=0;
            cl++;
        } else y--;
    }
    return cl;
}
static bool tt_new_piece(void) {
    tt_piece=(int)(lcg_rand()%7u); tt_rot=0;
    tt_px=TT_W/2-1; tt_py=0;
    return tt_fits(tt_piece,tt_rot,tt_px,tt_py);
}
static void tt_draw(struct limine_video_driver *v) {
    for (int y=0; y<=TT_H+1; y++) {
        v->char_at('|', LIMINE_COLOR_LIGHT_BLUE, (uint16_t)(TT_OX-1), (uint16_t)(TT_OY+y));
        v->char_at('|', LIMINE_COLOR_LIGHT_BLUE, (uint16_t)(TT_OX+TT_W), (uint16_t)(TT_OY+y));
    }
    for (int x=0; x<=TT_W+1; x++)
        v->char_at('-', LIMINE_COLOR_LIGHT_BLUE, (uint16_t)(TT_OX-1+x), (uint16_t)(TT_OY+TT_H+1));
    for (int y=0; y<TT_H; y++)
        for (int x=0; x<TT_W; x++) {
            uint8_t c=tt_field[y][x];
            v->char_at(c?'#':' ', c?tt_colors[c-1]:LIMINE_COLOR_BLACK,
                       (uint16_t)(TT_OX+x), (uint16_t)(TT_OY+1+y));
        }
    for (int i=0; i<4; i++) {
        int nx=tt_px+tt_shapes[tt_piece][tt_rot][i][0];
        int ny=tt_py+tt_shapes[tt_piece][tt_rot][i][1];
        if (ny>=0&&ny<TT_H)
            v->char_at('@', tt_colors[tt_piece],
                       (uint16_t)(TT_OX+nx), (uint16_t)(TT_OY+1+ny));
    }
}

void tetris_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    for (int y=0; y<TT_H; y++) for (int x=0; x<TT_W; x++) tt_field[y][x]=0;
    if (!tt_new_piece()) return;

    uint64_t grav_int = 500;
    uint64_t input_int = 5;
    uint64_t last_grav = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);

    uint32_t tt_score = 0;
    bool tt_over = false;
    static const uint32_t lpts[5]={0,100,300,500,800};

    video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+2); video->printf("TETRIS", LIMINE_COLOR_CYAN);
    video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+4); video->printf("A/D  left/right",LIMINE_COLOR_LIGHT_GRAY);
    video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+5); video->printf("W    rotate", LIMINE_COLOR_LIGHT_GRAY);
    video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+6); video->printf("S    soft drop", LIMINE_COLOR_LIGHT_GRAY);
    video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+7); video->printf("ESC  quit", LIMINE_COLOR_LIGHT_GRAY);

    while (!tt_over) {
        uint64_t now = read_uptime_ms(tsc);
        bool need_draw = false;

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                if (sc_is_esc(kbd, sc)) { tt_over=true; continue; }

                enum key_code kc = kbd->scancode_to_key_code(sc);

                if ((kc==KEY_A || kc==KEY_LEFT) &&
                    tt_fits(tt_piece,tt_rot,tt_px-1,tt_py))
                    { tt_px--; need_draw=true; }

                if ((kc==KEY_D || kc==KEY_RIGHT) &&
                    tt_fits(tt_piece,tt_rot,tt_px+1,tt_py))
                    { tt_px++; need_draw=true; }

                if (kc==KEY_W || kc==KEY_UP) {
                    int nr=(tt_rot+1)%4;
                    if (tt_fits(tt_piece,nr,tt_px,tt_py))
                        { tt_rot=nr; need_draw=true; }
                }

                if (kc==KEY_S || kc==KEY_DOWN) {
                    if (tt_fits(tt_piece,tt_rot,tt_px,tt_py+1))
                        { tt_py++; need_draw=true; }
                    else {
                        tt_lock();
                        int cl=tt_clear_lines();
                        if(cl<=4) tt_score+=lpts[cl];
                        if(!tt_new_piece()) tt_over=true;
                        last_grav=now; need_draw=true;
                    }
                }
            }
        }

        if (now - last_grav >= grav_int) {
            last_grav = now;
            if (tt_fits(tt_piece,tt_rot,tt_px,tt_py+1)) {
                tt_py++; need_draw=true;
            } else {
                tt_lock();
                int cl=tt_clear_lines();
                if(cl<=4) tt_score+=lpts[cl];
                if(!tt_new_piece()) tt_over=true;
                need_draw=true;
            }
        }

        if (need_draw) {
            tt_draw(video);
            video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+9);
            video->printf("Score:      ", LIMINE_COLOR_YELLOW);
            video->set_cursor_pos(TT_OX+TT_W+3, TT_OY+10);
            g_print_int(video, tt_score, LIMINE_COLOR_YELLOW);
            video->printf("      ", LIMINE_COLOR_BLACK);
        }
    }

    int cx=TT_OX+TT_W/2-6, cy=TT_OY+TT_H/2;
    video->set_cursor_pos((uint32_t)cx,(uint32_t)cy);
    video->printf("  GAME OVER!  ", LIMINE_COLOR_LIGHT_RED);
    video->set_cursor_pos((uint32_t)cx,(uint32_t)(cy+1));
    video->printf("  Score: ", LIMINE_COLOR_YELLOW); g_print_int(video,tt_score,LIMINE_COLOR_YELLOW);
    video->set_cursor_pos((uint32_t)cx,(uint32_t)(cy+2));
    video->printf("  Press ENTER ", LIMINE_COLOR_LIGHT_GRAY);
    while(1){uint8_t sc=wait_key(kbd);if(sc_is_enter(kbd, sc)||sc_is_esc(kbd, sc))break;}
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define BK_W 40
#define BK_H 22
#define BK_OX 2
#define BK_OY 1
#define BK_ROWS 4
#define BK_COLS 10
#define BK_BRICK_W 4
#define BK_BRICK_Y0 2
#define BK_PAD_W 6
#define BK_PAD_Y (BK_H-2)

static uint8_t bk_bricks[BK_ROWS][BK_COLS];
static const uint32_t bk_colors[BK_ROWS]={
    LIMINE_COLOR_LIGHT_RED, LIMINE_COLOR_ORANGE,
    LIMINE_COLOR_YELLOW, LIMINE_COLOR_LIGHT_GREEN,
};

void breakout_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    for (int r=0;r<BK_ROWS;r++) for(int c=0;c<BK_COLS;c++) bk_bricks[r][c]=(uint8_t)(r+1);

    int pad_x=BK_W/2-BK_PAD_W/2;
    int bx=BK_W/2, by=BK_H/2, bdx=1, bdy=-1;
    uint32_t bk_score=0;
    int total=BK_ROWS*BK_COLS, broken=0, lives=3;
    bool bk_over=false, bk_win=false;

    uint64_t logic_int = 60;
    uint64_t input_int = 5;
    uint64_t last_logic = read_uptime_ms(tsc);
    uint64_t last_input = read_uptime_ms(tsc);
    int pad_move = 0;

    while (!bk_over) {
        uint64_t now = read_uptime_ms(tsc);

        if (now - last_input >= input_int) {
            last_input = now;
            uint8_t sc = kbd_poll_key(kbd);
            if (sc) {
                enum key_code kc = kbd->scancode_to_key_code(sc);
                if ((kc==KEY_A || kc==KEY_LEFT) && pad_x>1) pad_move=-1;
                if ((kc==KEY_D || kc==KEY_RIGHT) && pad_x<BK_W-BK_PAD_W-1) pad_move= 1;
                if (sc_is_esc(kbd, sc)) { bk_over=true; continue; }
            }
        }

        if (now - last_logic >= logic_int) {
            last_logic = now;

            pad_x += pad_move;
            pad_move = 0;
            if (pad_x<1) pad_x=1;
            if (pad_x>BK_W-BK_PAD_W-1) pad_x=BK_W-BK_PAD_W-1;

            bx+=bdx; by+=bdy;
            if (bx<=0) {bx=1; bdx= 1;}
            if (bx>=BK_W-1) {bx=BK_W-2; bdx=-1;}
            if (by<=0) {by=1; bdy= 1;}

            if (by>=BK_H) {
                lives--;
                bx=BK_W/2; by=BK_H/2; bdx=1; bdy=-1;
                if (lives<=0){bk_over=true;}
            }

            if (by==BK_PAD_Y && bx>=pad_x && bx<pad_x+BK_PAD_W) {
                bdy=-1;
                int rel=bx-pad_x;
                if (rel<BK_PAD_W/3) bdx=-1;
                else if (rel>BK_PAD_W*2/3) bdx= 1;
            }

            for (int r=0; r<BK_ROWS&&!bk_win; r++) {
                for (int c=0; c<BK_COLS; c++) {
                    if (!bk_bricks[r][c]) continue;
                    int bx0=c*BK_BRICK_W, bky=BK_BRICK_Y0+r;
                    if (bx>=bx0&&bx<bx0+BK_BRICK_W-1&&by==bky) {
                        bk_bricks[r][c]=0; bdy=-bdy;
                        bk_score+=(uint32_t)(BK_ROWS-r)*10u;
                        broken++;
                        if(broken>=total){bk_win=true;bk_over=true;}
                        goto bk_done;
                    }
                }
            }
            bk_done:;

            video->clear(LIMINE_COLOR_BLACK);
            for(int x=0;x<BK_W;x++){
                video->char_at('=',LIMINE_COLOR_LIGHT_BLUE,(uint16_t)(BK_OX+x),(uint16_t)(BK_OY));
                video->char_at('=',LIMINE_COLOR_LIGHT_BLUE,(uint16_t)(BK_OX+x),(uint16_t)(BK_OY+BK_H+1));
            }
            for(int y=0;y<=BK_H+1;y++){
                video->char_at('|',LIMINE_COLOR_LIGHT_BLUE,(uint16_t)(BK_OX-1), (uint16_t)(BK_OY+y));
                video->char_at('|',LIMINE_COLOR_LIGHT_BLUE,(uint16_t)(BK_OX+BK_W),(uint16_t)(BK_OY+y));
            }
            for(int r=0;r<BK_ROWS;r++)
                for(int c=0;c<BK_COLS;c++) {
                    if(!bk_bricks[r][c]) continue;
                    for(int dx=0;dx<BK_BRICK_W-1;dx++)
                        video->char_at('=',bk_colors[r],
                                       (uint16_t)(BK_OX+c*BK_BRICK_W+dx),
                                       (uint16_t)(BK_OY+1+BK_BRICK_Y0+r));
                }
            for(int i=0;i<BK_PAD_W;i++)
                video->char_at('=',LIMINE_COLOR_LIGHT_GREEN,
                               (uint16_t)(BK_OX+pad_x+i),(uint16_t)(BK_OY+1+BK_PAD_Y));
            video->char_at('o',LIMINE_COLOR_YELLOW,(uint16_t)(BK_OX+bx),(uint16_t)(BK_OY+1+by));

            video->set_cursor_pos(BK_OX,0);
            video->printf("BREAKOUT  Score: ",LIMINE_COLOR_CYAN);
            g_print_int(video,bk_score,LIMINE_COLOR_YELLOW);
            video->printf("  Lives: ",LIMINE_COLOR_LIGHT_GRAY);
            g_print_int(video,(uint32_t)lives,LIMINE_COLOR_LIGHT_RED);
            video->printf("  [A/D] move  [ESC] quit",LIMINE_COLOR_LIGHT_GRAY);
        }
    }

    int cx=BK_OX+BK_W/2-7, cy=BK_OY+BK_H/2;
    video->set_cursor_pos((uint32_t)cx,(uint32_t)cy);
    if(bk_win) video->printf("   YOU WIN!    ",LIMINE_COLOR_LIGHT_GREEN);
    else video->printf("  GAME OVER!   ",LIMINE_COLOR_LIGHT_RED);
    video->set_cursor_pos((uint32_t)cx,(uint32_t)(cy+1));
    video->printf("  Score: ",LIMINE_COLOR_YELLOW); g_print_int(video,bk_score,LIMINE_COLOR_YELLOW);
    video->set_cursor_pos((uint32_t)cx,(uint32_t)(cy+2));
    video->printf("  Press ENTER  ",LIMINE_COLOR_LIGHT_GRAY);
    while(1){uint8_t sc=wait_key(kbd);if(sc_is_enter(kbd, sc)||sc_is_esc(kbd, sc))break;}
    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define CB3_FONT_W 8
#define CB3_FONT_H 16

#define CB3_MAX_W 512
#define CB3_MAX_H 144
#define CB3_DEFAULT_W 64
#define CB3_DEFAULT_H 32
#define CB3_MIN_W 20
#define CB3_MIN_H 12
#define CB3_DIST 3.4

static char cb3_chbuf[CB3_MAX_H][CB3_MAX_W];
static uint32_t cb3_colbuf[CB3_MAX_H][CB3_MAX_W];

typedef struct { double x, y, z; } cb3_vec3;
typedef struct { cb3_vec3 a, b, c; } cb3_tri_t;

static const cb3_tri_t cb3_cube_tris[12] = {
    {{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}}, {{-1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}},
    {{ 1,-1,-1}, {-1,-1,-1}, {-1, 1,-1}}, {{ 1,-1,-1}, {-1, 1,-1}, { 1, 1,-1}},
    {{ 1,-1, 1}, { 1,-1,-1}, { 1, 1,-1}}, {{ 1,-1, 1}, { 1, 1,-1}, { 1, 1, 1}},
    {{-1,-1,-1}, {-1,-1, 1}, {-1, 1, 1}}, {{-1,-1,-1}, {-1, 1, 1}, {-1, 1,-1}},
    {{-1, 1, 1}, { 1, 1, 1}, { 1, 1,-1}}, {{-1, 1, 1}, { 1, 1,-1}, {-1, 1,-1}},
    {{-1,-1,-1}, { 1,-1,-1}, { 1,-1, 1}}, {{-1,-1,-1}, { 1,-1, 1}, {-1,-1, 1}},
};

static const cb3_tri_t cb3_tetra_tris[4] = {
    {{ 1, 1, 1}, { 1,-1,-1}, {-1, 1,-1}},
    {{ 1, 1, 1}, {-1,-1, 1}, { 1,-1,-1}},
    {{ 1, 1, 1}, {-1, 1,-1}, {-1,-1, 1}},
    {{ 1,-1,-1}, {-1,-1, 1}, {-1, 1,-1}},
};

static const cb3_tri_t cb3_octa_tris[8] = {
    {{ 1,0,0}, {0, 1,0}, {0,0, 1}}, {{ 1,0,0}, {0,0, 1}, {0,-1,0}},
    {{ 1,0,0}, {0,-1,0}, {0,0,-1}}, {{ 1,0,0}, {0,0,-1}, {0, 1,0}},
    {{-1,0,0}, {0,0, 1}, {0, 1,0}}, {{-1,0,0}, {0,-1,0}, {0,0, 1}},
    {{-1,0,0}, {0,0,-1}, {0,-1,0}}, {{-1,0,0}, {0, 1,0}, {0,0,-1}},
};

static const cb3_tri_t cb3_pyramid_tris[6] = {
    {{0, 1.3, 0}, {-1,-0.6,-1}, { 1,-0.6,-1}},
    {{0, 1.3, 0}, { 1,-0.6,-1}, { 1,-0.6, 1}},
    {{0, 1.3, 0}, { 1,-0.6, 1}, {-1,-0.6, 1}},
    {{0, 1.3, 0}, {-1,-0.6, 1}, {-1,-0.6,-1}},
    {{-1,-0.6,-1}, { 1,-0.6,-1}, { 1,-0.6, 1}},
    {{-1,-0.6,-1}, { 1,-0.6, 1}, {-1,-0.6, 1}},
};

typedef struct {
    const cb3_tri_t *tris;
    int tri_count;
    double scale;
    double ox, oy, oz;

} cb3_shape_t;

static const cb3_shape_t cb3_shapes[4] = {
    { cb3_cube_tris, 12, 1.00, 0.0, 0.0, 0.0 },
    { cb3_tetra_tris, 4, 0.65, -4.2, 2.6, 0.0 },
    { cb3_octa_tris, 8, 0.65, 4.2, 2.6, 0.0 },
    { cb3_pyramid_tris, 6, 0.65, 0.0, -3.4, 0.0 },
};

static const uint32_t cb3_palette[] = {
    LIMINE_COLOR_LIGHT_RED, LIMINE_COLOR_ORANGE,
    LIMINE_COLOR_YELLOW, LIMINE_COLOR_LIGHT_GREEN,
    LIMINE_COLOR_LIGHT_CYAN, LIMINE_COLOR_LIGHT_BLUE,
    LIMINE_COLOR_LIGHT_MAGENTA, LIMINE_COLOR_HOT_PINK,
    LIMINE_COLOR_GOLD,
};
#define CB3_PALETTE_N ((int)(sizeof(cb3_palette) / sizeof(cb3_palette[0])))

static inline int cb3_clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline void cb3_rotate(double x, double y, double z,
                               double cA, double sA, double cB, double sB, double cC, double sC,
                               double *ox, double *oy, double *oz)
{

    double y1 = y*cA - z*sA;
    double z1 = y*sA + z*cA;
    double x1 = x;

    double x2 = x1*cB + z1*sB;
    double z2 = -x1*sB + z1*cB;
    double y2 = y1;

    double x3 = x2*cC - y2*sC;
    double y3 = x2*sC + y2*cC;
    double z3 = z2;
    *ox = x3; *oy = y3; *oz = z3;
}

static inline void cb3_plot(int cb_w, int cb_h, int sx, int sy, uint32_t color) {
    if (sx < 0 || sx >= cb_w || sy < 0 || sy >= cb_h) return;
    cb3_chbuf[sy][sx] = '#';
    cb3_colbuf[sy][sx] = color;
}

static void cb3_draw_edge(int cb_w, int cb_h,
                           double x0, double y0,
                           double x1, double y1,
                           uint32_t color)
{
    double dx = x1 - x0, dy = y1 - y0;
    double dist = sqrt(dx * dx + dy * dy);
    int steps = (int)(dist * 2.0) + 1;
    if (steps > 4000) steps = 4000;
    for (int i = 0; i <= steps; i++) {
        double t = (double)i / (double)steps;
        double sxd = x0 + dx * t;
        double syd = y0 + dy * t;
        int sx = (int)(sxd + 0.5);
        int sy = (int)(syd + 0.5);
        cb3_plot(cb_w, cb_h, sx, sy, color);
        cb3_plot(cb_w, cb_h, sx + 1, sy, color);
        cb3_plot(cb_w, cb_h, sx, sy + 1, color);
    }
}

void cube3d_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc, int width_px, int height_px)
{
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    uint64_t px_w = 0, px_h = 0;
    video->get_display_resolution(&px_w, &px_h);
    int max_cols = px_w ? (int)(px_w / CB3_FONT_W) : 100;
    int max_rows = px_h ? (int)(px_h / CB3_FONT_H) : 40;
    if (max_cols < CB3_MIN_W + 2) max_cols = CB3_MIN_W + 2;
    if (max_rows < CB3_MIN_H + 4) max_rows = CB3_MIN_H + 4;

    int cb_w = width_px > 0 ? width_px / CB3_FONT_W : max_cols - 2;
    int cb_h = height_px > 0 ? height_px / CB3_FONT_H : max_rows - 4;
    cb_w = cb3_clamp_i(cb_w, CB3_MIN_W, CB3_MAX_W);
    cb_h = cb3_clamp_i(cb_h, CB3_MIN_H, CB3_MAX_H);
    if (cb_w > max_cols - 2) cb_w = max_cols - 2;
    if (cb_h > max_rows - 4) cb_h = max_rows - 4;
    if (cb_w < CB3_MIN_W) cb_w = CB3_MIN_W;
    if (cb_h < CB3_MIN_H) cb_h = CB3_MIN_H;

    int ox = (max_cols - cb_w) / 2; if (ox < 0) ox = 0;
    int oy = 2;

    double angleA = 0.3, angleB = 0.6, angleC = 0.1;

    double k1 = (double)(cb_w < cb_h * 2 ? cb_w : cb_h * 2) * 0.42 / 1.3;
    double k2 = k1 * ((double)CB3_FONT_W / (double)CB3_FONT_H);

    uint64_t last_frame = read_uptime_ms(tsc);
    uint64_t fps_window_start = last_frame;
    uint32_t frames_in_window = 0;
    uint32_t fps_display = 0;

    bool over = false;
    while (!over) {
        uint8_t sc = kbd_poll_key(kbd);
        if (sc && sc_is_esc(kbd, sc)) { over = true; break; }

        uint64_t now = read_uptime_ms(tsc);
        double dt = (double)(now - last_frame) / 1000.0;
        last_frame = now;
        if (dt <= 0.0) dt = 0.001;
        if (dt > 0.25) dt = 0.25;

        angleA = fmod(angleA + 1.1 * dt, 2.0 * M_PI);
        angleB = fmod(angleB + 0.7 * dt, 2.0 * M_PI);
        angleC = fmod(angleC + 0.5 * dt, 2.0 * M_PI);

        double cA = cos(angleA), sA = sin(angleA);
        double cB = cos(angleB), sB = sin(angleB);
        double cC = cos(angleC), sC = sin(angleC);

        for (int y = 0; y < cb_h; y++) {
            for (int x = 0; x < cb_w; x++) {
                cb3_chbuf[y][x] = ' ';
            }
        }

        int color_idx = 0;
        for (int s = 0; s < 4; s++) {
            const cb3_shape_t *shp = &cb3_shapes[s];
            for (int t = 0; t < shp->tri_count; t++) {
                const cb3_tri_t *tri = &shp->tris[t];
                uint32_t color = cb3_palette[color_idx % CB3_PALETTE_N];
                color_idx++;

                const cb3_vec3 *verts[3] = { &tri->a, &tri->b, &tri->c };
                double sxv[3], syv[3];
                bool visible = true;
                for (int k = 0; k < 3; k++) {
                    double lx = verts[k]->x * shp->scale;
                    double ly = verts[k]->y * shp->scale;
                    double lz = verts[k]->z * shp->scale;

                    double rx, ry, rz;
                    cb3_rotate(lx, ly, lz, cA, sA, cB, sB, cC, sC, &rx, &ry, &rz);
                    rx += shp->ox; ry += shp->oy; rz += shp->oz;

                    double z_cam = rz + CB3_DIST;
                    if (z_cam <= 0.2) { visible = false; break; }
                    double inv_z = 1.0 / z_cam;

                    sxv[k] = (double)cb_w / 2.0 + rx * inv_z * k1;
                    syv[k] = (double)cb_h / 2.0 - ry * inv_z * k2;
                }
                if (!visible) continue;

                cb3_draw_edge(cb_w, cb_h, sxv[0], syv[0], sxv[1], syv[1], color);
                cb3_draw_edge(cb_w, cb_h, sxv[1], syv[1], sxv[2], syv[2], color);
                cb3_draw_edge(cb_w, cb_h, sxv[2], syv[2], sxv[0], syv[0], color);
            }
        }

        video->clear(LIMINE_COLOR_BLACK);
        for (int y = 0; y < cb_h; y++) {
            for (int x = 0; x < cb_w; x++) {
                if (cb3_chbuf[y][x] == ' ') continue;
                video->char_at(cb3_chbuf[y][x], cb3_colbuf[y][x], (uint16_t)(ox + x), (uint16_t)(oy + y));
            }
        }

        frames_in_window++;
        if (now - fps_window_start >= 500) {
            fps_display = (uint32_t)(frames_in_window * 1000 / (now - fps_window_start));
            frames_in_window = 0;
            fps_window_start = now;
        }

        video->set_cursor_pos((uint32_t)ox, 0);
        video->printf("3D-CUBE  FPS: ", LIMINE_COLOR_CYAN);
        g_print_int(video, fps_display, LIMINE_COLOR_LIGHT_GREEN);
        video->printf("  size: ", LIMINE_COLOR_LIGHT_GRAY);
        g_print_int(video, (uint32_t)(cb_w * CB3_FONT_W), LIMINE_COLOR_AMBER);
        video->printf("x", LIMINE_COLOR_LIGHT_GRAY);
        g_print_int(video, (uint32_t)(cb_h * CB3_FONT_H), LIMINE_COLOR_AMBER);
        video->printf("px (", LIMINE_COLOR_LIGHT_GRAY);
        g_print_int(video, (uint32_t)cb_w, LIMINE_COLOR_AMBER);
        video->printf("x", LIMINE_COLOR_LIGHT_GRAY);
        g_print_int(video, (uint32_t)cb_h, LIMINE_COLOR_AMBER);
        video->printf(" cells)  [ESC] quit", LIMINE_COLOR_LIGHT_GRAY);
    }

    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}

#define G2_N 4
#define G2_CELL_W 7
#define G2_CELL_H 3
#define G2_OX 2
#define G2_OY 2

static int g2_board[G2_N][G2_N];

static uint32_t g2_tile_color(int v) {
    switch (v) {
        case 2: return LIMINE_COLOR_LIGHT_GRAY;
        case 4: return LIMINE_COLOR_WHITE;
        case 8: return LIMINE_COLOR_ORANGE;
        case 16: return LIMINE_COLOR_LIGHT_RED;
        case 32: return LIMINE_COLOR_RED;
        case 64: return LIMINE_COLOR_HOT_PINK;
        case 128: return LIMINE_COLOR_YELLOW;
        case 256: return LIMINE_COLOR_GOLD;
        case 512: return LIMINE_COLOR_LIGHT_GREEN;
        case 1024: return LIMINE_COLOR_LIGHT_CYAN;
        case 2048: return LIMINE_COLOR_LIGHT_MAGENTA;
        default: return LIMINE_COLOR_LIGHT_BLUE;
    }
}

static void g2_spawn(void) {
    int empty_r[G2_N * G2_N], empty_c[G2_N * G2_N], ec = 0;
    for (int r = 0; r < G2_N; r++)
        for (int c = 0; c < G2_N; c++)
            if (!g2_board[r][c]) { empty_r[ec] = r; empty_c[ec] = c; ec++; }
    if (!ec) return;
    int idx = (int)(lcg_rand() % (uint32_t)ec);
    int v = ((lcg_rand() % 10u) == 0) ? 4 : 2;
    g2_board[empty_r[idx]][empty_c[idx]] = v;
}

static bool g2_compress_merge(int line[G2_N], uint32_t *score) {
    int tmp[G2_N] = {0};
    int n = 0;
    for (int i = 0; i < G2_N; i++) if (line[i]) tmp[n++] = line[i];

    for (int i = 0; i < n - 1; i++) {
        if (tmp[i] && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            *score += (uint32_t)tmp[i];
            for (int j = i + 1; j < n - 1; j++) tmp[j] = tmp[j + 1];
            tmp[n - 1] = 0;
            n--;
        }
    }

    bool changed = false;
    for (int i = 0; i < G2_N; i++) {
        int nv = (i < n) ? tmp[i] : 0;
        if (line[i] != nv) changed = true;
        line[i] = nv;
    }
    return changed;
}

static bool g2_move_left(uint32_t *score) {
    bool any = false;
    for (int r = 0; r < G2_N; r++) {
        int line[G2_N];
        for (int c = 0; c < G2_N; c++) line[c] = g2_board[r][c];
        if (g2_compress_merge(line, score)) any = true;
        for (int c = 0; c < G2_N; c++) g2_board[r][c] = line[c];
    }
    return any;
}
static bool g2_move_right(uint32_t *score) {
    bool any = false;
    for (int r = 0; r < G2_N; r++) {
        int line[G2_N];
        for (int c = 0; c < G2_N; c++) line[c] = g2_board[r][G2_N - 1 - c];
        if (g2_compress_merge(line, score)) any = true;
        for (int c = 0; c < G2_N; c++) g2_board[r][G2_N - 1 - c] = line[c];
    }
    return any;
}
static bool g2_move_up(uint32_t *score) {
    bool any = false;
    for (int c = 0; c < G2_N; c++) {
        int line[G2_N];
        for (int r = 0; r < G2_N; r++) line[r] = g2_board[r][c];
        if (g2_compress_merge(line, score)) any = true;
        for (int r = 0; r < G2_N; r++) g2_board[r][c] = line[r];
    }
    return any;
}
static bool g2_move_down(uint32_t *score) {
    bool any = false;
    for (int c = 0; c < G2_N; c++) {
        int line[G2_N];
        for (int r = 0; r < G2_N; r++) line[r] = g2_board[G2_N - 1 - r][c];
        if (g2_compress_merge(line, score)) any = true;
        for (int r = 0; r < G2_N; r++) g2_board[G2_N - 1 - r][c] = line[r];
    }
    return any;
}

static bool g2_no_moves(void) {
    for (int r = 0; r < G2_N; r++) {
        for (int c = 0; c < G2_N; c++) {
            if (!g2_board[r][c]) return false;
            if (c < G2_N - 1 && g2_board[r][c] == g2_board[r][c + 1]) return false;
            if (r < G2_N - 1 && g2_board[r][c] == g2_board[r + 1][c]) return false;
        }
    }
    return true;
}

static void g2_draw_number(struct limine_video_driver *video, int val, int cell_x0, int cell_y, int cell_w, uint32_t color) {
    if (val == 0) return;
    char buf[8];
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    int n = val;
    while (n > 0) { *--p = (char)('0' + n % 10); n /= 10; }
    int len = (int)((buf + sizeof(buf) - 1) - p);
    int start = cell_x0 + (cell_w - len) / 2;
    if (start < cell_x0) start = cell_x0;
    for (int i = 0; i < len; i++)
        video->char_at(p[i], color, (uint16_t)(start + i), (uint16_t)cell_y);
}

static void g2_render(struct limine_video_driver *video, uint32_t score, bool won_shown) {
    video->clear(LIMINE_COLOR_BLACK);

    int grid_w = G2_N * G2_CELL_W + 1;
    int grid_h = G2_N * G2_CELL_H + 1;

    for (int r = 0; r <= G2_N; r++)
        for (int x = 0; x < grid_w; x++)
            video->char_at('-', LIMINE_COLOR_DARK_GRAY, (uint16_t)(G2_OX + x), (uint16_t)(G2_OY + r * G2_CELL_H));
    for (int c = 0; c <= G2_N; c++)
        for (int y = 0; y < grid_h; y++)
            video->char_at('|', LIMINE_COLOR_DARK_GRAY, (uint16_t)(G2_OX + c * G2_CELL_W), (uint16_t)(G2_OY + y));
    for (int r = 0; r <= G2_N; r++)
        for (int c = 0; c <= G2_N; c++)
            video->char_at('+', LIMINE_COLOR_DARK_GRAY, (uint16_t)(G2_OX + c * G2_CELL_W), (uint16_t)(G2_OY + r * G2_CELL_H));

    for (int r = 0; r < G2_N; r++) {
        for (int c = 0; c < G2_N; c++) {
            int v = g2_board[r][c];
            if (!v) continue;
            g2_draw_number(video, v, G2_OX + c * G2_CELL_W + 1, G2_OY + r * G2_CELL_H + 1, G2_CELL_W - 1, g2_tile_color(v));
        }
    }

    video->set_cursor_pos((uint32_t)G2_OX, 0);
    video->printf("2048  Score: ", LIMINE_COLOR_CYAN);
    g_print_int(video, score, LIMINE_COLOR_YELLOW);
    if (won_shown) video->printf("  *** 2048! ***", LIMINE_COLOR_LIGHT_GREEN);

    video->set_cursor_pos((uint32_t)G2_OX, (uint32_t)(G2_OY + grid_h + 1));
    video->printf("[WASD/Arrows] move  [ESC] quit", LIMINE_COLOR_LIGHT_GRAY);
}

void game2048_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc)
{
    (void)tsc;
    video->clear(LIMINE_COLOR_BLACK);
    video->set_cursor_visible(false);

    for (int r = 0; r < G2_N; r++)
        for (int c = 0; c < G2_N; c++)
            g2_board[r][c] = 0;

    uint32_t score = 0;
    bool won_shown = false;
    g2_spawn();
    g2_spawn();

    bool over = false;
    g2_render(video, score, won_shown);

    while (!over) {
        uint8_t sc = wait_key(kbd);
        if (sc_is_esc(kbd, sc)) { over = true; break; }

        enum key_code kc = kbd->scancode_to_key_code(sc);
        bool moved = false;
        if (kc == KEY_W || kc == KEY_UP) moved = g2_move_up(&score);
        else if (kc == KEY_S || kc == KEY_DOWN) moved = g2_move_down(&score);
        else if (kc == KEY_A || kc == KEY_LEFT) moved = g2_move_left(&score);
        else if (kc == KEY_D || kc == KEY_RIGHT) moved = g2_move_right(&score);
        else continue;

        if (moved) {
            g2_spawn();
            if (!won_shown)
                for (int r = 0; r < G2_N; r++)
                    for (int c = 0; c < G2_N; c++)
                        if (g2_board[r][c] >= 2048) won_shown = true;

            g2_render(video, score, won_shown);

            if (g2_no_moves()) over = true;
        }
    }

    int cx = G2_OX + (G2_N * G2_CELL_W) / 2 - 6, cy = G2_OY + (G2_N * G2_CELL_H) / 2;
    video->set_cursor_pos((uint32_t)cx, (uint32_t)cy);
    video->printf(" GAME OVER! ", LIMINE_COLOR_LIGHT_RED);
    video->set_cursor_pos((uint32_t)cx, (uint32_t)(cy + 1));
    video->printf(" Score: ", LIMINE_COLOR_YELLOW); g_print_int(video, score, LIMINE_COLOR_YELLOW);
    video->set_cursor_pos((uint32_t)cx, (uint32_t)(cy + 2));
    video->printf(" Press ENTER ", LIMINE_COLOR_LIGHT_GRAY);
    while (1) { uint8_t s = wait_key(kbd); if (sc_is_enter(kbd, s) || sc_is_esc(kbd, s)) break; }

    video->set_cursor_visible(true);
    video->clear(LIMINE_COLOR_BLACK);
}
