#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>

struct Session
{
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char *board_buf;
  size_t board_buf_cap;
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path)
{
  // Create FIFOs
  unlink(req_pipe_path);
  unlink(notif_pipe_path);

  if (mkfifo(req_pipe_path, 0666) == -1)
  {
    perror("mkfifo req");
    return 1;
  }
  if (mkfifo(notif_pipe_path, 0666) == -1)
  {
    perror("mkfifo notif");
    unlink(req_pipe_path);
    return 1;
  }

  // Open server pipe
  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1)
  {
    perror("open server pipe");
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }

  // Send request
  char msg[1 + 40 + 40] = {0};
  msg[0] = OP_CODE_CONNECT;
  strncpy(msg + 1, req_pipe_path, 40);
  strncpy(msg + 1 + 40, notif_pipe_path, 40);

  // Single write prevents interleaving between multiple clients.
  (void)write(server_fd, msg, sizeof(msg));
  close(server_fd);

  // Open pipes
  // Open notif first (server opens it for writing)
  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1)
  {
    perror("open notif pipe");
    return 1;
  }

  // Open req pipe before waiting for confirmation.
  // Otherwise we can deadlock if the server waits to open/read the req FIFO
  // while the client waits for the confirmation.
  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1)
  {
    perror("open req pipe");
    close(session.notif_pipe);
    session.notif_pipe = -1;
    return 1;
  }

  // Read confirmation
  char response[2];
  if (read(session.notif_pipe, response, 2) != 2)
  {
    perror("read confirmation");
    close(session.req_pipe);
    close(session.notif_pipe);
    session.req_pipe = -1;
    session.notif_pipe = -1;
    return 1;
  }

  if (response[0] != OP_CODE_CONNECT || response[1] != 0)
  {
    fprintf(stderr, "Connection failed\n");
    close(session.req_pipe);
    close(session.notif_pipe);
    session.req_pipe = -1;
    session.notif_pipe = -1;
    return 1;
  }

  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  session.id = 1; // Set active

  return 0;
}

void pacman_play(char command)
{
  if (session.id == -1)
    return;
  char msg[2] = {OP_CODE_PLAY, command};
  (void)write(session.req_pipe, msg, sizeof(msg));
}

int pacman_disconnect()
{
  if (session.id == -1)
    return 0;
  char op_code = OP_CODE_DISCONNECT;
  write(session.req_pipe, &op_code, 1);

  close(session.req_pipe);
  close(session.notif_pipe);
  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);
  free(session.board_buf);
  session.board_buf = NULL;
  session.board_buf_cap = 0;
  session.id = -1;
  return 0;
}

Board receive_board_update(void)
{
  Board board = {0};
  if (session.id == -1)
    return board;

  char op_code;
  ssize_t n = read(session.notif_pipe, &op_code, 1);
  if (n <= 0)
  {
    debug("receive_board_update: read opcode returned %zd errno=%d\n", n, errno);
    board.game_over = 1; // Disconnected
    return board;
  }

  if (op_code != OP_CODE_BOARD)
  {
    debug("receive_board_update: unexpected opcode=%d\n", (int)op_code);
    return board;
  }

  read(session.notif_pipe, &board.width, sizeof(int));
  read(session.notif_pipe, &board.height, sizeof(int));
  read(session.notif_pipe, &board.tempo, sizeof(int));
  read(session.notif_pipe, &board.victory, sizeof(int));
  read(session.notif_pipe, &board.game_over, sizeof(int));
  read(session.notif_pipe, &board.accumulated_points, sizeof(int));

  debug(
      "receive_board_update: %dx%d tempo=%d victory=%d game_over=%d points=%d\n",
      board.width,
      board.height,
      board.tempo,
      board.victory,
      board.game_over,
      board.accumulated_points);

  size_t needed = (size_t)board.width * (size_t)board.height;
  if (needed > session.board_buf_cap)
  {
    char *newbuf = realloc(session.board_buf, needed);
    if (!newbuf)
    {
      board.game_over = 1;
      return board;
    }
    session.board_buf = newbuf;
    session.board_buf_cap = needed;
  }

  board.data = session.board_buf;
  read(session.notif_pipe, board.data, board.width * board.height);

  return board;
}