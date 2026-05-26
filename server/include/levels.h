#ifndef LEVELS_H
#define LEVELS_H

#define PATH_MAX 2048
#define MAX_LEVELS 20

typedef struct {
    char **filenames; // level filenames
    int count; // number of levels
    char dirname[PATH_MAX]; // directory path
} level_list_t;

#endif
