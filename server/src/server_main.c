#include "server_main.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h> 
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <protocol.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "board.h"
#include "game.h"
#include <semaphore.h>

static const char* server_fifo_path;
static int server_fd;
static int max_games;

static int sigusr1_received = 0;

static int server_shutdown = 0;

static session_request_t request_queue[REQUEST_QUEUE_SIZE];
static int rq_head = 0, rq_tail = 0;

static pthread_mutex_t rq_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t rq_items; // request amount
static sem_t rq_slots; // free request slots

pthread_t host_tid;
pthread_t *session_manager_thread_tids;

static client_t **connected_clients = NULL;
static int connected_clients_count = 0;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;

int create_server_pipe(const char* pipe_path) {
    unlink(pipe_path); // Remove old FIFO if it exists
    if (mkfifo(pipe_path, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        unlink(pipe_path);
        return -1;
    }
    return 0;
}

void cleanup_server() {
    debug("Shutting down server\n");
    server_shutdown = 1;

    // Wake host thread
    close(server_fd);

    // Wake all session manager threads
    for (int i = 0; i < max_games; i++)
        sem_post(&rq_items);

    pthread_join(host_tid, NULL);
    for (int i = 0; i < max_games; i++)
        pthread_join(session_manager_thread_tids[i], NULL);

    sem_destroy(&rq_items);
    sem_destroy(&rq_slots);

    unlink(server_fifo_path);

    debug("Server shutdown complete\n");
    close_debug_file();
}

void handle_sigint() {
    debug("Caught SIGINT signal\n");
    server_shutdown = 1;
}

void handle_sigterm() {
    debug("Caught SIGTERM signal\n");
    server_shutdown = 1;
}

void handle_sigusr1() {
    debug("Caught SIGUSR1 signal\n");
    sigusr1_received = 1;
}

void handle_signal(int sig) {
    (void)sig;

    switch (sig) {
        case SIGINT:
            handle_sigint();
            break;

        case SIGTERM:
            handle_sigterm();
            break;

        case SIGUSR1:
            handle_sigusr1();
            break;

        default:
            break;
    }
}

int compare_clients_points(const void *a, const void *b) {
    const client_t *client_a = *(const client_t **)a;
    const client_t *client_b = *(const client_t **)b;

    return client_b->points - client_a->points; 
}

void generate_leaderboard() {
    debug("Generating leaderboard\n");

    pthread_mutex_lock(&clients_lock);

    if (connected_clients_count == 0) {
        debug("No connected clients, skipping leaderboard generation\n");
        pthread_mutex_unlock(&clients_lock);
        return;
    }

    int n;
    if (connected_clients_count < 5) {
        n = connected_clients_count;
    } else {
        n = 5;
    }
    client_t **top_clients = malloc(sizeof(client_t*) * connected_clients_count);

    memcpy(top_clients, connected_clients, sizeof(client_t*) * connected_clients_count);

    qsort(top_clients, connected_clients_count, sizeof(client_t*), compare_clients_points);

    FILE *lb = fopen("leaderboard.txt", "w");
    for (int i = 0; i < n; i++) {
        fprintf(lb, "Player %d: %d points\n", top_clients[i]->id, top_clients[i]->points);
    }
    fclose(lb);
    free(top_clients);

    pthread_mutex_unlock(&clients_lock);
}

void accept_new_client() {
    if (server_shutdown) return;
    char buffer[1 + MAX_PIPE_PATH_LENGTH + MAX_PIPE_PATH_LENGTH];
    ssize_t n = read(server_fd, buffer, sizeof(buffer));

    if (n == 0) return; // continue waiting
    else if (n != sizeof(buffer)) {
        fprintf(stderr, "host_thread: Invalid connection request message size\n");
        return;
    }

    // Parse OP_CODE; only one for server FIFO
    if (buffer[0] != OP_CODE_CONNECT) {
        fprintf(stderr, "host_thread: Unknown/wrong OP_CODE %d\n", buffer[0]);
        return;
    }

    debug("Connection request found\n");

    // Extract pipe names
    char req_pipe[MAX_PIPE_PATH_LENGTH + 1];
    char notif_pipe[MAX_PIPE_PATH_LENGTH + 1];

    memcpy(req_pipe, buffer + 1, MAX_PIPE_PATH_LENGTH);
    memcpy(notif_pipe, buffer + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);

    req_pipe[MAX_PIPE_PATH_LENGTH] = notif_pipe[MAX_PIPE_PATH_LENGTH] = '\0';

    debug("New client wants session: req pipe: %s; notif pipe: %s\n", req_pipe, notif_pipe);

    session_request_t req;
    strcpy(req.req_pipe, req_pipe);
    strcpy(req.notif_pipe, notif_pipe);
    enqueue_request(&req);
}

void cleanup_client(client_t *c) {
    close(c->req_fd);
    close(c->notif_fd);
    pthread_mutex_destroy(&c->state_lock);
    free(c->board);
    free(c);
}

void enqueue_request(session_request_t *req) {
    sem_wait(&rq_slots);
    pthread_mutex_lock(&rq_mutex);

    request_queue[rq_tail] = *req;
    rq_tail = (rq_tail + 1) % REQUEST_QUEUE_SIZE;

    pthread_mutex_unlock(&rq_mutex);
    sem_post(&rq_items);
}

session_request_t dequeue_request(void) {
    session_request_t req;

    sem_wait(&rq_items);
    pthread_mutex_lock(&rq_mutex);

    req = request_queue[rq_head];
    rq_head = (rq_head + 1) % REQUEST_QUEUE_SIZE;

    pthread_mutex_unlock(&rq_mutex);
    sem_post(&rq_slots);
    return req;
}

void serialize_board(board_t *b, char *out) {
    for (int y = 0; y < b->height; y++) {
        for (int x = 0; x < b->width; x++) {
            board_pos_t *p = &b->board[y * b->width + x];

            if (p->content != ' ') out[y * b->width + x] = p->content;
            else if (p->has_dot) out[y * b->width + x] = '.';
            else if (p->has_portal) out[y * b->width + x] = 'O';
            else out[y * b->width + x] = ' ';
        }
    }
}

int send_board_update(client_t *c) {
    if (c->notif_fd == -1) {
        fprintf(stderr, "send_board_update: No pipe connection\n");
        return -1;
    }

    board_t *b = c->board;
    char op = OP_CODE_BOARD;

    pthread_rwlock_rdlock(&b->state_lock);
    int width = b->width;
    int height = b->height;
    int tempo = b->tempo;
    int total = b->width * b->height;
    if (total <= 0) {
        fprintf(stderr, "send_board_update: Invalid board size\n");
        return -1;
    }    
    char buffer[total];
    serialize_board(b, buffer);
    int victory = b->victory;
    int game_over = b->game_over;
    pthread_rwlock_unlock(&b->state_lock);

    pthread_mutex_lock(&c->state_lock);
    int points = c->points;
    pthread_mutex_unlock(&c->state_lock);

    if (write(c->notif_fd, &op, 1) != 1 ||
    write(c->notif_fd, &width, sizeof(int)) != sizeof(int) ||
    write(c->notif_fd, &height, sizeof(int)) != sizeof(int) ||
    write(c->notif_fd, &tempo, sizeof(int)) != sizeof(int) ||
    write(c->notif_fd, &victory, sizeof(int)) != sizeof(int) ||
    write(c->notif_fd, &game_over, sizeof(int)) != sizeof(int) ||
    write(c->notif_fd, &points, sizeof(int)) != sizeof(int)) {
        fprintf(stderr, "send_board_update: Error writing the board\n");
        return -1;
    }
    // needs a write_all function here if board sizes are large, ignoring that now
    if (write(c->notif_fd, buffer, total) != total) {
        perror("write board data");
        return -1;
    }

    return 0;
}

void run_game_session(client_t *client, level_list_t *levels) {
    int current_level = 0; // 0-based
    board_t *board = client->board;
    int id = client->id;

    while (current_level < levels->count) {
        if (server_shutdown) break;
        board->victory = 0;
        load_level(board, levels->filenames[current_level], levels->dirname, client->points);
        start_ghost_threads(board);
        while (!board->game_over && !board->victory) {
            sleep_ms(board->tempo);
            if (server_shutdown) {
                pthread_rwlock_wrlock(&board->state_lock);
                board->game_over = 1;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }
            debug("Updating board for client %d\n", id);
            send_board_update(client);

            char op;
            int n = read(client->req_fd, &op, 1);

            if (n == 0) {
                debug("Client %d disconnected (EOF)\n", id);
                pthread_rwlock_wrlock(&board->state_lock);
                board->game_over = 1;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            } else if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // Nothing sent, ignore
                fprintf(stderr, "run_game_session: client read error");
                pthread_rwlock_wrlock(&board->state_lock);
                board->game_over = 1;
                pthread_rwlock_unlock(&board->state_lock);
                break;
            }
    
            switch (op) {
                case OP_CODE_DISCONNECT:
                    debug("Client %d disconnected (OP_CODE)\n", id);
                    pthread_rwlock_wrlock(&board->state_lock);
                    board->game_over = 1;
                    pthread_rwlock_unlock(&board->state_lock);
                    break;
    
                case OP_CODE_PLAY:
                    char move;
                    n = read(client->req_fd, &move, 1);
                    if (n == 0) {
                        debug("Client %d disconnected (EOF)\n", id);
                        pthread_rwlock_wrlock(&board->state_lock);
                        board->game_over = 1;
                        pthread_rwlock_unlock(&board->state_lock);
                        break;
                    } else if (n == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Nothing sent, ignore
                        fprintf(stderr, "Client %d play read error\n", id);
                        pthread_rwlock_wrlock(&board->state_lock);
                        board->game_over = 1;
                        pthread_rwlock_unlock(&board->state_lock);
                        break;
                    }
                    debug("Client %d sent command: %c\n", id, move);
                    command_t cmd = {.command = move, .turns = 1};
                    pthread_rwlock_wrlock(&board->state_lock);
                    int result = move_pacman(board, 0, &cmd);
                    pthread_rwlock_unlock(&board->state_lock);
                    client->points = board->pacmans[0].points;

                    if (result == REACHED_PORTAL) board->victory = 1;
                    else if (result == DEAD_PACMAN) board->game_over = 1;
                    break;
    
                default:
                    debug("Client %d sent unknown opcode %d\n", id, op);
                    pthread_rwlock_wrlock(&board->state_lock);
                    board->game_over = 1;
                    pthread_rwlock_unlock(&board->state_lock);
                    break;
            }
        }

        if (board->victory) {
            current_level++;
        }

        stop_ghost_threads(board);
        unload_level(board);
    }
}

void *session_manager_thread(void *arg) {
    level_list_t *levels= (level_list_t*)arg;
    
    while (!server_shutdown) {
        // Wait for a session request from the host thread
        session_request_t req = dequeue_request();
        if (server_shutdown) break;

        debug("Opening client notif FIFO: %s\n", req.notif_pipe);
        // Always open notif first to avoid blocking in req
        int notif_fd = open(req.notif_pipe, O_WRONLY);
        if (notif_fd == -1) {
            perror("open notif fifo");
            return NULL;
        }

        debug("Opening client req FIFO: %s\n", req.req_pipe);
        // Needs non-block flag to update board without waiting for input
        int req_fd = open(req.req_pipe, O_RDONLY | O_NONBLOCK);
        if (req_fd == -1) {
            perror("open req fifo");
            close(notif_fd);
            return NULL;
        }

        client_t *c = malloc(sizeof(client_t));
        memset(c, 0, sizeof(*c));
        pthread_mutex_lock(&id_lock);
        char* id_str = strtok(req.req_pipe, '/');
        id_str = strtok(NULL, '/');
        c->id = atoi(strtok(id_str, "_"));
        pthread_mutex_unlock(&id_lock);
        c->req_fd = req_fd;
        c->notif_fd = notif_fd;
        pthread_mutex_init(&c->state_lock, NULL);

        pthread_mutex_lock(&clients_lock);
        if (connected_clients_count < max_games) {
            connected_clients[connected_clients_count++] = c;
        }
        pthread_mutex_unlock(&clients_lock);

        board_t *b = malloc(sizeof(board_t));
        memset(b, 0, sizeof(*b));
        c->board = b;

        debug("Sending confirmation message to client %d\n", c->id);
        char reply[2] = { OP_CODE_CONNECT, RESULT_SUCCESS };
        if (write(notif_fd, reply, 2) != 2) {
            perror("write notif fifo");
            close(req_fd);
            close(notif_fd);
            return NULL;
        }

        debug("Added new client with id %d\n", c->id);

        run_game_session(c, levels);
        cleanup_client(c);
    }
    return NULL;
}

void* host_thread(void* arg) {
    server_fifo_path = (const char*)arg;

    // Unblock SIGUSR1 for this thread only
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);


    // Open server FIFO for reading
    server_fd = open(server_fifo_path, O_RDONLY);
    if (server_fd == -1) {
        perror("open server fifo");
        return NULL;
    }

    while (!server_shutdown) {
        accept_new_client();

        if (sigusr1_received) {
            sigusr1_received = 0;
            generate_leaderboard();
        }
    }
    return NULL;
}

level_list_t* scan_level_directory(const char *dirname) {
    level_list_t *list = malloc(sizeof(level_list_t));
    list->filenames = malloc(sizeof(char*) * MAX_LEVELS);
    strncpy(list->dirname, dirname, PATH_MAX);
    list->count = 0;

    DIR* level_dir = opendir(dirname);
    if (!level_dir) {
        free(list->filenames);
        free(list);
        return NULL;
    }

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot) continue;

        if (strcmp(dot, ".lvl") == 0) list->filenames[list->count++] = strdup(entry->d_name);
    }
    if (closedir(level_dir) == -1) {
        fprintf(stderr, "Failed to close directory\n");
        free(list->filenames);
        free(list);
        return NULL;
    }

    return list;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <level_directory> <max_games> <register_fifo_name>\n", argv[0]);
        return -1;
    }

    level_list_t *levels = scan_level_directory(argv[1]);
    if(!levels) {
        fprintf(stderr, "Failed to scan level directory: %s\n", argv[1]);
        return -1;
    }

    max_games = atoi(argv[2]);
    if (max_games <= 0) {
        fprintf(stderr, "Invalid number of games: %s\n", argv[2]);
        return -1;
    }

    connected_clients = malloc(max_games * sizeof(client_t*));

    session_manager_thread_tids = malloc(max_games * sizeof(pthread_t));


    sem_init(&rq_items, 0, 0);
    sem_init(&rq_slots, 0, REQUEST_QUEUE_SIZE);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    // Block SIGUSR1 in main (and all threads created after)
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    open_debug_file("debug.log");

    debug("Starting server FIFO\n");
    const char *server_fifo = argv[3];
    if (create_server_pipe(server_fifo) == -1) {
        fprintf(stderr, "Failed to create server pipe: %s\n", server_fifo);
        return -1;
    }

    debug("Creating host thread\n");
    pthread_create(&host_tid, NULL, host_thread, (void*)server_fifo);

    debug("Creating session manager threads");
    for (int i = 0; i < max_games; i++)
        pthread_create(&session_manager_thread_tids[i], NULL, session_manager_thread, levels);

    while (!server_shutdown) sleep(1); // Server main thread does nothing else.
    cleanup_server();
    return 0;
}