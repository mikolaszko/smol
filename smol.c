// includes
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// defines
#define SMOL_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

// data
struct editorConfig {
  int cx, cy;
  int screenrows;
  int screencols;
  struct termios orig_termios;
  char command;
};
struct editorConfig E;

// terminal
void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");

  tcgetattr(STDIN_FILENO, &E.orig_termios);
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  // ICRNL - input flag carriage return newline
  // IXON turns off software control flow
  // BRKINT - for break condition
  // INPCK - enables parity checking
  // INSTRIP - causes 8th but of each input byte to be stripped - probably
  // already turned off
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  // OPOST turns off all output processing features
  raw.c_lflag &= ~(OPOST);
  // CS8 - NOT A FLAG - its bit mask
  raw.c_cflag |= (CS8);
  // ICONON turns off canonical mode
  // ISIG does it for SIGINT SIGTSTP;
  // IEXTEN does it for ctrl-v;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

char editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN)
      die("read");
  };
  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }

  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;
  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

// append buffer
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT                                                              \
  { NULL, 0 }

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

// commands
void editorProcessCommand(char c) {
  // :q command for now quiting
  if (E.command == ':' && c == 'q') {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
  } else {
    E.command = c;
  }
}

// output
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    if (y == E.screenrows / 3) {
      char welcome[50];
      char desc[50];
      int welcomelen = snprintf(welcome, sizeof(welcome),
                                "Smol editor -- version %s", SMOL_VERSION);
      int desclen = snprintf(desc, sizeof(desc), "Simple, Fast AF, Nvim-like");
      if (welcomelen > E.screencols)
        welcomelen = E.screencols;
      if (desclen > E.screencols)
        desclen = E.screencols;

      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }

      while (padding--)
        abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcomelen);
      padding = ((E.screencols - welcomelen) / 2) + 1;
      abAppend(ab, "\n", 1);
      while (padding--)
        abAppend(ab, " ", 1);
      abAppend(ab, desc, desclen);
    } else {
      abAppend(ab, "~", 1);
    }

    abAppend(ab, "\x1b[K", 3);
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

// accumulate all of the tildres and escape chars into buf and then write to it
void editorRefreshScreen() {
  struct abuf ab = ABUF_INIT;

  // hide cursor
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  char buf[32];
  // terminal is 1 based so we need to add + 1 to cursor coords
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  // hide cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

// input
void editorMoveCursor(char key) {
  switch (key) {
  case 'h':
    E.cx--;
    break;
  case 'l':
    E.cx++;
    break;
  case 'k':
    E.cy--;
    break;
  case 'j':
    E.cy++;
    break;
  }
}
void editorProcessKeypress() {
  char c = editorReadKey();
  editorProcessCommand(c);

  switch (c) {
  case 'j':
  case 'h':
  case 'k':
  case 'l':
    editorMoveCursor(c);
    break;
    //
  }
}

// init
void initEditor() {
  E.cx = 0;
  E.cy = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}
int main() {
  enableRawMode();
  initEditor();
  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  };
  return 0;
}
