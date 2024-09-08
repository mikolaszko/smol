// includes
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

// defines
//
#define CTRL_KEY(k) ((k) & 0x1f)

// data
struct termios orig_termios;
enum mode { N, I, V };
char command;

// terminal
void die(const char *s) {
  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr");

  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);

  struct termios raw = orig_termios;
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

// commands
void editorProcessCommand(char c) {
  if (command == ':' && c == 'w') {
    exit(0);
  } else {
    command = c;
  }
}

// input
void editorProcessKeypress() {
  char c = editorReadKey();
  editorProcessCommand(c);
}

// init
int main() {
  enableRawMode();
  while (1) {
    editorProcessKeypress();
  };
  return 0;
}
