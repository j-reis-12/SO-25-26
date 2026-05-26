#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

#define FILE_MAX 8192
#define MAX_PATH 128

#define MAX_HEIGHT 250
#define MAX_WIDTH 250

typedef struct {
    board_t *board;
    pthread_mutex_t *lock;
    int ghost_index;
    int *result;
} ghost_thread_data;

typedef struct {
    board_t *board;
    pthread_mutex_t *lock;
    int *result;
} pacman_thread_data;

typedef struct {
    board_t *board;
    pthread_mutex_t *lock;
    int *result;
} screen_thread_data;

int has_backup = 0;
board_t *backup_board = NULL;
pthread_mutex_t board_lock;

pthread_t pacman_pthread;
pthread_t screen_pthread;
pthread_t ghost_pthreads[MAX_GHOSTS];

int stop_threads = 0;
int result = CONTINUE_PLAY;

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
    if(game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

int play_board(board_t * game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;
    command_t c; 
    if (pacman->n_moves == 0) { // if is user input
        c.command = get_input();
        debug("play board KEY %c\n", c.command);

        if(c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    }
    else { // else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        debug("play board pre-defined key!!!\n");
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q') {
        return QUIT_GAME;
    }

    if (play->command == 'G') {
        return CREATE_BACKUP;
    }

    pthread_mutex_lock(&board_lock);
    int result = move_pacman(game_board, 0, play);
    pthread_mutex_unlock(&board_lock);

    if (result == REACHED_PORTAL) {
        // Next level
        return NEXT_LEVEL;
    }

    if(result == DEAD_PACMAN) {
        return QUIT_GAME;
    }
    

    if (!game_board->pacmans[0].alive) {
        return QUIT_GAME;
    }      

    return CONTINUE_PLAY;  
}

board_pos_t *process_board(char (*board)[MAX_WIDTH], int dim_h, int dim_w) {
    board_pos_t *result = malloc(sizeof(board_pos_t) * dim_h * dim_w);
    for (int y = 0; y < dim_h; y++) {
        for (int x = 0; x < dim_w; x++) {
            char c = board[y][x];
            switch (c) {
                case 'X': // wall
                    result[y * dim_w + x].content = 'W'; 
                    break;
                case 'o': // dot
                    result[y * dim_w + x].content = ' ';
                    result[y * dim_w + x].has_dot = 1;
                    break;
                case '@': // portal
                    result[y * dim_w + x].content = ' ';
                    result[y * dim_w + x].has_portal = 1;
                    break;
                default: return NULL;
            }
        }
    }
    return result;
}

board_t *process_file(const char *filename, const char *level_dir) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return NULL;
    }

    char buffer[FILE_MAX + 1];
    size_t total = 0;
    while (total < FILE_MAX) {
        ssize_t result = read(fd, buffer + total, FILE_MAX - total);
        if (result < 0) {
            perror("read");
            close(fd);
            return NULL;
        }
        if (result == 0) break;
        total += result;
    }
    buffer[total] = '\0';
    close(fd);

    // split file into lines and process variables
    char *line_start = buffer;
    char *line_end;

    enum { DIM, TEMPO, PAC_OR_MON, BOARD }; // expected line types
    int expect = DIM;

    int dim_w, dim_h;
    int tempo;
    char pac_file[MAX_PATH] = "";
    char mon_files[MAX_GHOSTS][MAX_PATH];
    int n_monst = 0;
    char board_lines[MAX_HEIGHT][MAX_WIDTH];
    int board_count = 0;
    int last = 0;

    while (1) {
        line_end = strchr(line_start, '\n');
        if (line_end) *line_end = '\0';
        else {
            line_end = line_start + strlen(line_start);
            last = 1;
        }

        char *p = line_start;
        if (*p == '#') { line_start = line_end + 1; continue; } // skip comments

        // check expected data lines in order
        switch (expect) {
            case DIM:
                p += 3;
                dim_h = (int)strtol(p, &p, 10);
                dim_w = (int)strtol(p, &p, 10);
                expect = TEMPO;
                break;
            
            case TEMPO:
                tempo = (int)strtol(p + 5, NULL, 10);
                expect = PAC_OR_MON;
                break;
            
            case PAC_OR_MON:
                if (strncmp(p, "PAC", 3) == 0) {
                    // PAC filename
                    p += 4;
                    while (*p==' ') p++;
                    int len_dir = strlen(level_dir);
                    int len_fn = strlen(p);
                    memcpy(pac_file, level_dir, len_dir);
                    pac_file[len_dir] = '/';
                    memcpy(pac_file+len_dir+1, p, len_fn);
                    pac_file[len_dir+1+len_fn] = '\0';

                } else if (strncmp(p, "MON", 3) == 0) {
                    // MON filenames
                    p += 3;
                    while (*p) {
                        while (*p==' ') p++;
                        char *start = p;
                        while (*p && *p!=' ' && *p!='\t' && *p!='\r') p++;
                        if (p > start) {
                            int len_dir = strlen(level_dir);
                            int len_fn = p - start;
                            if (len_dir + 1 + len_fn >= MAX_PATH) len_fn = MAX_PATH - len_dir - 2;
                            memcpy(mon_files[n_monst], level_dir, len_dir);
                            mon_files[n_monst][len_dir] = '/';
                            memcpy(mon_files[n_monst]+len_dir+1, start, len_fn);
                            mon_files[n_monst][len_dir+1+len_fn] = '\0';
                            n_monst++;
                            if (n_monst >= MAX_GHOSTS) break;
                        }
                    }
                    expect = BOARD;
                }
                break;
            
            case BOARD:
                // collect board lines: expect exactly dim_h lines of board content
                strncpy(board_lines[board_count++], p, dim_w);
                break;
        }
        if (last) break;
        line_start = line_end + 1;
    }

    board_t *game_board = malloc(sizeof(board_t));
    game_board->width = dim_w;
    game_board->height = dim_h;
    game_board->board = malloc(sizeof(board_pos_t) * dim_w * dim_h);
    game_board->board = process_board(board_lines, dim_h, dim_w);
    game_board->n_pacmans = 1; // always 1 pacman
    game_board->pacmans = malloc(sizeof(pacman_t) * game_board->n_pacmans);
    game_board->n_ghosts = n_monst;
    game_board->ghosts = malloc(sizeof(ghost_t) * n_monst);
    strncpy(game_board->level_name, filename, MAX_PATH - 1);
    strncpy(game_board->pacman_file, pac_file, MAX_PATH - 1);
    for (int i = 0; i < n_monst; i++)
        strncpy(game_board->ghosts_files[i], mon_files[i], MAX_PATH - 1);
    game_board->tempo = tempo;

    return game_board;
}

int compare_filenames(const void *a, const void *b) {
    // cast to appropriate type
    const char *fa = *(const char **)a;
    const char *fb = *(const char **)b;

    // extract numeric part from filenames
    int num_a = atoi(fa);
    int num_b = atoi(fb);

    return (num_a - num_b);
}

void *ghost_thread(void *args) {
    ghost_thread_data *data = (ghost_thread_data *)args;
    board_t *board = data->board;
    int ghost_index = data->ghost_index;
    pthread_mutex_t *lock = data->lock;
    int *result = data->result;
    debug("Ghost %d moving outside...\n", ghost_index);
    debug("Pacman alive status: %d\n", board->pacmans[0].alive);

    while ((!stop_threads) && (*result != QUIT_GAME) && (*result != NEXT_LEVEL)) {
        debug("Ghost %d moving inside...\n", ghost_index);
        pthread_mutex_lock(lock);
        ghost_t* ghost = &board->ghosts[ghost_index]; 
        move_ghost(board, ghost_index, &ghost->moves[ghost->current_move % ghost->n_moves]);
        debug("Ghost %d moved to %d %d\n", ghost_index, ghost->pos_y, ghost->pos_x);
        pthread_mutex_unlock(lock);
        sleep_ms(board->tempo);
    }

    free(data);
    return NULL;

}

void *pacman_thread(void *args) {
    pacman_thread_data *data = args;
    board_t *board = data->board;
    pthread_mutex_t *lock = data->lock;
    pacman_t *pac = &board->pacmans[0];
    int *result = data->result;

    while ((!stop_threads) && pac->alive) {
        command_t cmd;
        if (pac->n_moves == 0) { // input do utilizador
            char c = get_input();
            if (c == '\0') {
                sleep_ms(50);
                continue;
            }
            cmd.command = c;
            cmd.turns = 1;
        } else { // movimentos do ficheiro
            cmd = pac->moves[pac->current_move % pac->n_moves];
        }

        pthread_mutex_lock(lock);
        int r = move_pacman(board, 0, &cmd);

        // actualizar resultado que a main vai ler
        if (r == REACHED_PORTAL) *result = NEXT_LEVEL;
        else if (r == DEAD_PACMAN) *result = QUIT_GAME;
        else if (cmd.command == 'Q') *result = QUIT_GAME;
        else if (cmd.command == 'G') {
            if (!has_backup) {
                *result = CREATE_BACKUP;
            }
        }    
        else *result = CONTINUE_PLAY;
        debug("Pacman moved to %d %d, result=%d\n", pac->pos_y, pac->pos_x, *result);
        pthread_mutex_unlock(lock);

        if (*result != CONTINUE_PLAY) break;

        sleep_ms(board->tempo);
    }

    free(data);
    return NULL;
}   

void *screen_thread(void *args) {
    screen_thread_data *data = args;
    int *result = data->result;
    while ((!stop_threads) && (*result != QUIT_GAME) && (*result != NEXT_LEVEL)) {
        debug("Screen refreshing... result=%d\n", *result);
        pthread_mutex_lock(data->lock);
        draw_board(data->board, DRAW_MENU);
        refresh_screen();
        pthread_mutex_unlock(data->lock);

        sleep_ms(data->board->tempo);
    }
    free(data);
    return NULL;
}

void start_threads(board_t *board, int *result) {

    pacman_thread_data *pac_data = malloc(sizeof(pacman_thread_data));
    pac_data->board = board;
    pac_data->lock = &board_lock;
    pac_data->result = result;
    pthread_create(&pacman_pthread, NULL, pacman_thread, pac_data);
        

    for (int i = 0; i < board->n_ghosts; i++) {
        ghost_thread_data *ghost_data = malloc(sizeof(ghost_thread_data));
        ghost_data->board = board;
        ghost_data->lock = &board_lock;
        ghost_data->ghost_index = i;
        ghost_data->result = result;
        pthread_create(&ghost_pthreads[i], NULL, ghost_thread, ghost_data);
    }

    screen_thread_data *s_data = malloc(sizeof(screen_thread_data));
    s_data->board = board;
    s_data->lock = &board_lock;
    s_data->result = result;
    pthread_create(&screen_pthread, NULL, screen_thread, s_data);

}

void kill_threads(board_t *board) {
    stop_threads = 1;
    pthread_join(screen_pthread, NULL);
    pthread_join(pacman_pthread, NULL);
    for (int i=0; i<board->n_ghosts; i++) {
        pthread_join(ghost_pthreads[i], NULL);
    }

}

void create_backup(board_t *game_board, int *result, const char *level_dir, char *level_files[], int n_levels, int current_level_index, int points) {
    pthread_mutex_lock(&board_lock);

    kill_threads(game_board);

    has_backup = 1;
    backup_board = malloc(sizeof(board_t));
    memcpy(backup_board, game_board, sizeof(board_t));
    // se o board contém ponteiros (caso do seu código), você precisa copiar também o conteúdo profundo (deep copy)
    pthread_mutex_unlock(&board_lock);

    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // FILHO
        stop_threads = 0;
        int child_result = CONTINUE_PLAY;

        // começa no nível atual
        start_threads(game_board, &child_result);
        
        int i = current_level_index + 1; // próximo nível
        while (i <= n_levels) {
            if (child_result == QUIT_GAME) exit(1);
            if (child_result == NEXT_LEVEL) {
                kill_threads(game_board);
                free(game_board);

                game_board = process_file(level_files[i], level_dir);
                load_level(game_board, points);
                child_result = CONTINUE_PLAY;
                start_threads(game_board, &child_result);
                i++;
            }
            sleep_ms(game_board->tempo);
        }
        debug("child result: %d\n", child_result);
        exit(0);
    }

    int status;
    wait(&status); // espera o filho morrer
    debug("status: %d\n", status);
    debug("%d\n", has_backup);
    if (WEXITSTATUS(status) == 1) {
        debug("%d depois\n", *result);
        // restaura estado do jogo a partir do backup
        stop_threads = 0;
        memcpy(game_board, backup_board, sizeof(board_t));
        start_threads(game_board, result);

        // se fez deep copy, restaura também os ponteiros corretamenteg
        terminal_cleanup();
        terminal_init();

        has_backup = 0;
        free(backup_board);
        backup_board = NULL;
        return;
    } else {
        terminal_cleanup();
        terminal_init();
        exit(0);
    }
}


int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return 1;
    }

    const char *level_dir = argv[1];
    DIR *dir = opendir(level_dir);
    if (!dir) {
        perror("opendir");
        return EXIT_FAILURE;
    }

    struct dirent *entry;
    char *level_files[MAX_LEVELS];
    int n_levels = 0;

    // read level files from directory
    while ((entry = readdir(dir))) {
        char *filename = entry->d_name;
        char *extension = strrchr(filename, '.');
        if (!extension || strcmp(extension, ".lvl") != 0) continue;

        // add filename to level files list
        char *copy = strdup(filename);
        level_files[n_levels++] = copy;
    }
    closedir(dir);

    // sort level files numerically
    qsort(level_files, n_levels, sizeof(char *), compare_filenames);

    open_debug_file("debug.log");
    terminal_init();

    pthread_mutex_init(&board_lock, NULL);

    for (int i = 0; i < n_levels; i++) {
        char full_path[MAX_PATH];
        snprintf(full_path, MAX_PATH, "%s/%s", level_dir, level_files[i]);
        board_t *game_board = process_file(full_path, level_dir);
        if (!game_board) {
            perror("process_file");
            return EXIT_FAILURE;
        }

        int accumulated_points = 0;
        int result = CONTINUE_PLAY;

        load_level(game_board, accumulated_points);

        start_threads(game_board, &result);

        while (result != QUIT_GAME) {
            if (result == CREATE_BACKUP) {
                create_backup(game_board, &result, level_dir, level_files, n_levels, i, accumulated_points);
                result = CONTINUE_PLAY; // reset     
            } else if (result == NEXT_LEVEL) {
                debug("Pacman reached portal, next level!\n");
                // Pacman chegou ao portal, passa para próximo nível
                break; // sai do loop atual, main vai para próximo nível
            }
        }

        pthread_join(screen_pthread, NULL);
        pthread_join(pacman_pthread, NULL);
        for (int i=0; i<game_board->n_ghosts; i++) {
            pthread_join(ghost_pthreads[i], NULL);
        }

        terminal_cleanup();
        return 0;
        

    }
    
    terminal_cleanup();
    close_debug_file();
    has_backup = 0;
    return 0;
}
