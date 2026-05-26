#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

#define MAX_BOARD_SIZE 2048

struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

static int server_dead = 0;

int read_all(int fd, void *buf, size_t size) {
    char *ptr = buf;
    size_t remaining = size;
    while (remaining > 0) {
        int n = read(fd, ptr, remaining);
        if (n <= 0) return n; // EOF or error
        remaining -= n;
        ptr += n;
    }
    return size - remaining;
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
	// Unlink FIFOs if they already exist
	unlink(req_pipe_path);
	unlink(notif_pipe_path);

	debug("Creating client FIFOs\n");
	if (mkfifo(req_pipe_path, 0666) == -1) {
		perror("mkfifo req");
		return 1;
	}
	if (mkfifo(notif_pipe_path, 0666) == -1) {
		perror("mkfifo notif");
		unlink(req_pipe_path);
		return 1;
	}

	debug("Opening server registration FIFO\n");
	int server_fd = open(server_pipe_path, O_WRONLY);
	if (server_fd == -1) {
		perror("open server fifo");
		unlink(req_pipe_path);
		unlink(notif_pipe_path);
		return 1;
	}

	// Make connection request message
	/* Format: (char) OP_CODE_CONNECT | (char[MAX_PIPE_PATH_LENGTH]) req_pipe
			| (char[MAX_PIPE_PATH_LENGTH]) notif_pipe */
	char msg[1 + MAX_PIPE_PATH_LENGTH + MAX_PIPE_PATH_LENGTH];
	memset(msg, 0, sizeof(msg));
	msg[0] = OP_CODE_CONNECT;
	strncpy(msg + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
	strncpy(msg + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

	debug("Sending connection request to server: req pipe: %s; notif pipe: %s\n",
    req_pipe_path, notif_pipe_path);
	if (write(server_fd, msg, sizeof(msg)) != sizeof(msg)) {
		perror("write server fifo");
		close(server_fd);
		unlink(req_pipe_path);
		unlink(notif_pipe_path);
		return 1;
	}
	close(server_fd);

    debug("Opening notification FIFO for receiving updates: %s\n", notif_pipe_path);
    int notif_fd = open(notif_pipe_path, O_RDONLY);
    if (notif_fd == -1) {
        perror("open notif fifo");
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    }

    debug("Opening request FIFO for sending commands: %s\n", req_pipe_path);
    // Needs nonblock flag to avoid blocking on disconnect
    int req_fd = open(req_pipe_path, O_WRONLY | O_NONBLOCK);
    if (req_fd == -1) {
        perror("open req fifo");
        close(notif_fd);
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    }

	// Read the server response
    // Format: (char) OP_CODE_CONNECT | (char) result
    char response[2];
    int n = read_all(notif_fd, response, 2);
    if (n != 2 || response[0] != OP_CODE_CONNECT) {
        fprintf(stderr, "Invalid server response: %s\n", response);
        close(notif_fd);
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    } else if (response[1] != RESULT_SUCCESS) {
        fprintf(stderr, "Server rejected connection\n");
        close(notif_fd);
        unlink(req_pipe_path);
        unlink(notif_pipe_path);
        return 1;
    }

	// Save session info
	session.id = 0;
	session.req_pipe = req_fd;
	session.notif_pipe = notif_fd;
	strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
	session.req_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';
	strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
	session.notif_pipe_path[MAX_PIPE_PATH_LENGTH] = '\0';

    debug("Request accepted\n");
	return 0;
}

int pacman_play(char command) {
    if (command != 'W' && command != 'A'
    && command != 'S' && command != 'D') {
        fprintf(stderr, "pacman_play: invalid command '%c'\n", command);
        return -1;
    }

	debug("Sending play request to server: %c\n", command);
    char msg[2] = {OP_CODE_PLAY, command};
    if (write(session.req_pipe, msg, 2) != 2) {
        perror("write req fifo");
        return -1;
    }

    return 0;
}

int pacman_disconnect() {
    debug("Disconnecting from server\n");
	if (!server_dead) {
        char op = OP_CODE_DISCONNECT;
        if (write(session.req_pipe, &op, 1) == -1) {
            if (errno == EPIPE) debug("Server already disconnected (EPIPE)\n");
            else {
                perror("write disconnect");
            }
        }
    }
    if (session.req_pipe != -1) close(session.req_pipe);
    if (session.notif_pipe != -1) close(session.notif_pipe);
    unlink(session.req_pipe_path);
    unlink(session.notif_pipe_path);

    session.id = -1;
    debug("Done\n");
  	return 0;
}

Board receive_board_update(void) {
    Board b = {.data = NULL};
    debug("Receiving update\n");
    int notif_fd = session.notif_pipe;

    if (notif_fd == -1) {
        fprintf(stderr, "receive_board_update: not connected\n");
        return b;
    }

    char op;
    int n = read_all(session.notif_pipe, &op, 1);
    if (n == 0) {
        debug("Server disconnected (EOF)\n");
        server_dead = 1;
        return b;
    } else if (n == -1) {
        fprintf(stderr, "receive_board_update: server opcode read_all error");
        return b;
    } else if (op != OP_CODE_BOARD) {
        fprintf(stderr, "receive_board_update: invalid op: %d\n", op);
        return b;
    }

    if (read_all(notif_fd, &b.width, sizeof(int)) != sizeof(int) ||
    read_all(notif_fd, &b.height, sizeof(int)) != sizeof(int) ||
    read_all(notif_fd, &b.tempo, sizeof(int)) != sizeof(int) ||
    read_all(notif_fd, &b.victory, sizeof(int)) != sizeof(int) ||
    read_all(notif_fd, &b.game_over, sizeof(int)) != sizeof(int) ||
    read_all(notif_fd, &b.accumulated_points, sizeof(int)) != sizeof(int)) {
        fprintf(stderr, "receive_board_update: invalid board args\n");
        return b;
    }

    int total = b.width * b.height;
    if (total > MAX_BOARD_SIZE || total <= 0) {
        fprintf(stderr, "receive_board_update: invalid board size received\n");
        return b;
    }

    char *data = malloc(total);
    if (!data) {
        fprintf(stderr, "receive_board_update: error allocating for board data\n");
        return b;
    }
    if (read_all(notif_fd, data, total) != total) {
        fprintf(stderr, "receive_board_update: error reading board data\n");
        return b;
    }

    if (b.data) free(b.data);
    b.data = data;
    return b;
}