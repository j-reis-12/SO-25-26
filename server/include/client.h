#ifndef CLIENT_H
#define CLIENT_H

#include <pthread.h>
#include "board.h"

typedef struct {
    int id;
    int req_fd; // command requests file
    int notif_fd; // board updates file
    char last_command;
    pthread_mutex_t state_lock;
    board_t *board;
    int points;
} client_t;

#endif