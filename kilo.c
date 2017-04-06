#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios g_orig_termios;

//// UTILITY

// Print an error message and terminate the program with an error status.
void die(const char* s) {
  perror(s);
  exit(EXIT_FAILURE);
}

//// TERMINAL HANDLING

// Restore original terminal configuration.
void restore_terminal(void) {
  if(-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios)) {
    die("tcsetattr");
  }
}

// Enable "raw" mode for terminal by disabling both local echo and canonical
// input mode, stopping SIGINT and SIGSTP from being sent, disabling software
// flow control and carriage return processing. The read() timeout is also set
// to be as small as possible (1 decisecond).
void enable_raw_mode(void) {
  struct termios raw;

  // Read current attributes
  if(-1 == tcgetattr(STDIN_FILENO, &raw)) {
    die("tcgetattr");
  }

  // Disable echo, canonical input, signals and implementation-defined input
  // processing.
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // Disable XON/XOFF
  raw.c_iflag &= ~(IXON | ICRNL);

  // Disable output processing
  raw.c_oflag &= ~(OPOST);

  // Other "elderly" flags which constitute raw mode
  raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);
  raw.c_cflag |= CS8;

  // Set read() timeout.
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  // Set new attributes
  if(-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)) {
    die("tcsetattr");
  }
}

int main() {
  // Save original terminal config for restore_terminal() to use
  if(-1 == tcgetattr(STDIN_FILENO, &g_orig_termios)) {
    die("tcgetattr");
  }

  // Register restore_terminal() as an atexit handler so that the terminal is
  // restored when we terminate.
  atexit(restore_terminal);

  // Move to "raw" mode for the terminal.
  enable_raw_mode();

  // Input loop.
  while(1) {
    uint8_t c;

    // Try to read character.
    if((-1 == read(STDIN_FILENO, &c, 1)) && (errno != EAGAIN)) {
      die("read");
    }

    if(isprint(c)) {
      printf("%3d, 0x%02x ('%c')\r\n", c, c, c);
    } else {
      printf("%3d, 0x%02x\r\n", c, c);
    }

    if(c == 'q') { break; }
  }

  return EXIT_SUCCESS;
}
