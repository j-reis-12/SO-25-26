#ifndef GAME_H
#define GAME_H

#include "board.h"
#include "client.h"
#include "levels.h"

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define PATH_MAX 2048

typedef struct {
    board_t *board;
    client_t *client; // client playing on this board
    level_list_t *levels;
} pacman_thread_arg_t;

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

void start_ghost_threads(board_t *board);
void stop_ghost_threads(board_t *board);

int run_single_level(board_t *board, client_t *client);

void *pacman_thread(void *arg);
void *ghost_thread(void *arg);

#endif