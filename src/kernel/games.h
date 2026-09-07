#ifndef GAMES_H
#define GAMES_H

#include "drivers/Input/keyboard_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Timer/tsc_driver.h"
#include "drivers/Video/limine_video_driver.h"

void snake_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc, struct rtc_driver *rtc);

void rps_game(struct limine_video_driver *video, struct keyboard_driver *kbd);

void flappy_bird_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

void racing_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

void pong_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

void tetris_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

void breakout_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

void cube3d_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc, int width_px, int height_px);

void game2048_game(struct limine_video_driver *video, struct keyboard_driver *kbd, struct tsc_driver *tsc);

#endif
