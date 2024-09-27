// includes
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
// defines
#define SMOL_VERSION "0.0.1"
#define SMOL_TAB_STOP 2
#define CTRL_KEY(k) ((k) & 0x1f)
#define SMOL_QUIT_TIMES 1

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

// data
typedef struct erow {
  char *chars;
  int size;
  char *render;
  int rsize;
} erow;

enum mode { V = 86, I = 73, N = 78 };

struct editorConfig {
  int rx;
  int cx, cy;
  int rowoff, coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  struct termios orig_termios;
  char command_seq[3];
  char command;
  enum mode mode;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
};
struct editorConfig E;

// prot
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

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

// row operations
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (SMOL_TAB_STOP - 1) - (rx % SMOL_TAB_STOP);
    rx++;
  }
  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (SMOL_TAB_STOP - 1) + 1);

  // NOTE:
  // this probably can be simplified with memset because thats how optimizer
  // will do it,
  // maybe its nicer to read like this?
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % SMOL_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows)
    return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\n';
  editorUpdateRow(row);
  E.dirty++;
}

// editor operations
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size)
    at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

void editorDelChar() {
  if (E.cy == E.numrows)
    return;
  if (E.cx == 0 && E.cy == 0)
    return;
  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

// editor ops

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline(char c) {
  if (E.cx == 0) {
    editorInsertRow(E.cy + 1, "", 0);
  }
  if (c == '\r') {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  } else {
    editorInsertRow(E.cy + 1, "", 0);
  }
  E.cy++;
  E.cx = 0;
}

// file i/o
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;
  for (j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1;
  }
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  E.dirty = 0;
  free(E.filename);
  printf("%s", filename);
  size_t fnlen = strlen(filename) + 1;
  E.filename = malloc(fnlen);
  memcpy(E.filename, filename, fnlen);
  FILE *fp = fopen(filename, "r");

  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen = 13;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        editorSetStatusMessage("%d bytes written to disk", len);
        E.dirty = 0;
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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

// output
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {

        char welcome[80];
        char desc[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Smol editor -- version %s", SMOL_VERSION);
        int desclen =
            snprintf(desc, sizeof(desc), "Simple, Fast AF, Nvim-like");
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
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}
void editorDrawStatusBar(struct abuf *ab) {
  char *grayish = "\x1b[48;5;240m";
  abAppend(ab, grayish, strlen(grayish));

  char mode[100], rstatus[80];
  int len = snprintf(mode, sizeof(mode), "   Mode: %c | %.20s - %d lines %s",
                     E.mode, E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, mode, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols)
    msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);

  abAppend(ab, "\r\n", 2);
}

// accumulate all of the tildres and escape chars into buf and then write to it
void editorRefreshScreen() {
  editorScroll();
  struct abuf ab = ABUF_INIT;

  // hide cursor
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawMessageBar(&ab);
  editorDrawStatusBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // hide cursor
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

// input
char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';
  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    int c = editorReadKey();
    if (c == BACKSPACE) {
      if (buflen != 0)
        buf[buflen--] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorMoveCursor(char key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  switch (key) {
  case 'h':
    if (E.cx != 0)
      E.cx--;
    break;
  case 'l':
    if (row && E.cx < row->size) {
      E.cx++;
    }
    break;
  case 'k':
    if (E.cy != 0)
      E.cy--;
    break;
  case 'j':
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  case '$':
    if (E.cy < E.numrows) {
      E.cx = E.row[E.cy].size;
    }
    break;
  case '^':
    E.cx = 0;
    break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}
// input but commands
void editorProcessCommand(char c) {
  static int quit_times = SMOL_QUIT_TIMES;
  if (E.mode == I) {
    return;
  }

  int times = 0;
  // MANIFESTO:
  // ugly but very useful and simple
  //
  switch (c) {
  case '$':
    editorMoveCursor('$');
    return;
  case '^':
    editorMoveCursor('^');
    return;
  case 'G':
    E.cy = E.rowoff;
    times = E.numrows;
    while (times--) {
      editorMoveCursor('j');
    }
    return;
  case 'g':
    if (E.command == 'g') {
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows)
        E.cy = E.numrows;

      times = E.numrows;
      while (times--) {
        editorMoveCursor('k');
      }
      return;
    }
    break;
  case 'd':
    if (E.command == 'd') {
      editorDelRow(E.cy);
    }
    break;
  case 'q':
    if (E.command == ':') {
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage(
            "WARN! File has unsaved changes. Press :q %d more times to quit",
            quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      return;
    }
    break;
  case 'b':
    if (E.mode == I) {
      return;
    }
    times = 10;
    while (times--) {
      editorMoveCursor('h');
    }
    break;
  case 'w':
    if (E.mode == I) {
      return;
    }
    if (E.command == ':') {
      editorSave();
    } else {
      times = 10;
      while (times--) {
        editorMoveCursor('l');
      }
    }
    break;
  }

  if (c != ':') {
    quit_times = SMOL_QUIT_TIMES;
  }

  E.command = c;
}

void editorProcessKeypress() {
  char c = editorReadKey();
  editorProcessCommand(c);

  switch (c) {
  case '\r':
    editorInsertNewline('\r');
    break;
  case '\x1b':
    if (E.mode != N) {
      E.mode = N;
    }
    break;
  case BACKSPACE:
    if (E.mode == I) {
      editorDelChar();
    }
    break;
  case 'o':
    if (E.mode == I) {
      editorInsertChar(c);
      return;
    }
    editorInsertNewline('o');
    break;
  case 'j':
  case 'h':
  case 'k':
  case 'l':
    if (E.mode == N) {
      editorMoveCursor(c);
    }
    if (E.mode == I) {
      editorInsertChar(c);
    }
    break;
  //
  case 'i':
    if (E.mode != I) {
      E.mode = I;
      break;
    }
    if (E.mode == I) {
      editorInsertChar(c);
      break;
    }
    break;
  case 'n':
    if (E.mode == I) {
      editorInsertChar(c);
      break;
    }
    if (E.mode != N) {
      E.mode = N;
    }
    break;
  default:
    if (E.mode == I) {
      editorInsertChar(c);
    }
  }
}

// init
void initEditor() {
  E.rx = 0;
  E.mode = N;
  E.cx = 0;
  E.cy = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;
  E.dirty = 0;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");

  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  };
  return 0;
}
