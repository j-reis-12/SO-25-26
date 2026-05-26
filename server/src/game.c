#include "game.h"
#include "board.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/wait.h>
#include <stdbool.h>

void start_ghost_threads(board_t *board) {
    for (int i = 0; i < board->n_ghosts; i++) {
        ghost_thread_arg_t *g = malloc(sizeof(*g));
        g->board = board;
        g->ghost_index = i;
        pthread_create(&board->ghost_tids[i], NULL, ghost_thread, g);
    }
}

void stop_ghost_threads(board_t *board) {
    pthread_rwlock_wrlock(&board->state_lock);
    board->shutdown = 1;
    pthread_rwlock_unlock(&board->state_lock);

    for (int i = 0; i < board->n_ghosts; i++) pthread_join(board->ghost_tids[i], NULL);

    board->shutdown = 0; // no lock needed, all threads exited
}

int run_single_level(board_t *board, client_t *client) {
    pacman_t *pacman = &board->pacmans[0];

    while (true) {
        if (!pacman->alive) return QUIT_GAME;

        sleep_ms(board->tempo * (1 + pacman->passo));

        command_t* play;
        command_t c;
        pthread_mutex_lock(&client->state_lock);
        c.command = client->last_command;
        client->last_command = '\0';
        pthread_mutex_unlock(&client->state_lock);

        if(c.command == '\0') continue;
        c.turns = 1;
        play = &c;

        pthread_rwlock_rdlock(&board->state_lock);
        int result = move_pacman(board, 0, play);
        pthread_rwlock_unlock(&board->state_lock);

        if (result == REACHED_PORTAL) return NEXT_LEVEL;
        else if(result == DEAD_PACMAN) return QUIT_GAME;
    }
}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;

    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (board->shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}