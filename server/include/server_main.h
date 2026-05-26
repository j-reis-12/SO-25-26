#ifndef SERVER_MAIN_H
#define SERVER_MAIN_H

#include <pthread.h>
#include "board.h"
#include "client.h"
#include "levels.h"
#include "protocol.h"

#define REQUEST_QUEUE_SIZE 64

typedef struct {
    char req_pipe[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe[MAX_PIPE_PATH_LENGTH + 1];
} session_request_t;

/* Creates a named pipe (FIFO) with the given name. */
int create_server_pipe(const char* pipe_name);

/* Closes any opened server FIFOs and unlink pipe paths. */
void cleanup_server();

/* Signal handler for termination/etc. */
void handle_signal(int sig);

/* Closes any opened server FIFOs and unlink pipe paths. */
void cleanup_client(client_t *c);

/* Adds a session request to the queue. */
void enqueue_request(session_request_t *req);

/* Extracts a session request from the queue. */
session_request_t dequeue_request(void);

/* Checks for connection requests */
void accept_new_client();

/* Flattens the board into a string. */
void serialize_board(board_t *b, char *out);

/* Sends the updated board to a client's notif file. */
int send_board_update(client_t *s);

/* Server-client thread function to handle an individual client's session. */
void* session_manager_thread(void *arg);

/* Server host thread function to handle client connections. */
void* host_thread(void* arg);

/* Returns a list of all level files scanned from a given directory. */
level_list_t* scan_level_directory(const char *dirname);

#endif