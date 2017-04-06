#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

//// DATA TYPES

// Append buffer
struct abuf {
  uint8_t* buf;
  ssize_t len;
};

// Initial value for abuf structure.
#define ABUF_INIT { NULL, 0 }

// A row of display text
typedef struct erow {
  ssize_t size;
  uint8_t* chars;
} erow;

// The state of our editor.
struct editor_config {
  // Original terminal config on launch.
  struct termios orig_termios;

  // Terminal window size.
  int screen_rows, screen_cols;

  // Current cursor position.
  int cx, cy;

  // Number of rows in file
  int num_rows;

  // An array of rows
  erow* row;
};

//// GLOBALS

// Short, pithy name for "global editor".
struct editor_config E;

//// MACROS

// Editor version string
#define KILO_VERSION "0.0.1"

// Byte corresponding to CTRL-<key>
#define CTRL_KEY(key) ((key) & 0x1f)

// Co-erce string to uint8_t pointer
#define U8(str) ((uint8_t*)(str))

// Special key values
enum special_keys {
  ARROW_LEFT = 0x1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,

  DEL_KEY,

  HOME_KEY,
  END_KEY,

  PAGE_UP,
  PAGE_DOWN,
};

//// UTILITY

// Print an error message and terminate the program with an error status.
void die(const char* s) {
  // Clear screen and re-position cursor.
  write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

  perror(s);
  exit(EXIT_FAILURE);
}

//// APPEND BUFFER

// Append an array of bytes to an append buffer.
void ab_append(struct abuf *ab, const uint8_t *s, ssize_t len) {
  if(ab->len + len < ab->len) {
    die("overflow?");
  }

  uint8_t *new = realloc(ab->buf, ab->len + len);
  if(new == NULL) { die("realloc"); }

  memcpy(&new[ab->len], s, len);

  ab->buf = new;
  ab->len += len;
}

void ab_free(struct abuf *ab) {
  free(ab->buf);
}

//// TERMINAL HANDLING

// Restore original terminal configuration.
void restore_terminal(void) {
  // Clear screen and re-position cursor.
  write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

  if(-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios)) {
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

// Obtain the current terminal size. Returns -1 iff there was an error. One or
// both of rows/cols can be NULL in which case that value is not returned.
int get_window_size(int *rows, int *cols) {
  struct winsize ws;

  if((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0)) {
    return -1;
  }

  if(cols != NULL) {
    *cols = ws.ws_col;
  }

  if(rows != NULL) {
    *rows = ws.ws_row;
  }
  return 0;
}

// Read the next key from the keyboard.
int editor_read_key(void) {
  int n_read;
  uint8_t c;

  // Keep polling until we read a byte
  while((n_read = read(STDIN_FILENO, &c, 1)) != 1) {
    // Under Cygwin, read() sets EAGAIN rather then returning 0 bytes.
    if((n_read == -1) && (errno != EAGAIN)) { die("read"); }
  }

  // Handle escape sequences
  if(c == '\x1b') {
    uint8_t seq[3];

    // Try to read next two bytes
    if(read(STDOUT_FILENO, &seq[0], 1) != 1) { return '\x1b'; }
    if(read(STDOUT_FILENO, &seq[1], 1) != 1) { return '\x1b'; }

    if(seq[0] == '[') {
      if((seq[1] >= '0') && (seq[1] <= '9')) {
        if(read(STDOUT_FILENO, &seq[2], 1) != 1) { return '\x1b'; }
        if(seq[2] == '~') {
          switch(seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        switch(seq[1]) {
          // Arrow keys
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;

          // Home and end
          case 'H': return HOME_KEY;
          case 'F': return HOME_KEY;
        }
      }
    } else if(seq[0] == 'O') {
      switch(seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    // Unknown sequence
    return '\x1b';
  }

  return c;
}

//// OUTPUT

// Draw each row of the screen into the output buffer
void editor_draw_rows(struct abuf *ab) {
  for(int y=0; y<E.screen_rows; ++y) {
    if(y >= E.num_rows) {
      // off bottom of file

      // only display welcome message if no lines
      if((E.num_rows == 0) && (y == E.screen_rows / 3)) {
        uint8_t welcome[30];
        int welcomelen = snprintf((char*)welcome, sizeof(welcome),
            "Kilo editor -- version %s", KILO_VERSION);
        if(welcomelen > E.screen_cols) { welcomelen = E.screen_cols; }
        int padding = (E.screen_cols - welcomelen) >> 1;
        if(padding) {
          ab_append(ab, U8("~"), 1);
          --padding;
        }
        while(padding--) { ab_append(ab, U8(" "), 1); }
        ab_append(ab, welcome, welcomelen);
      } else {
        ab_append(ab, U8("~"), 1);
      }
    } else {
      int len = E.row[y].size;
      if(len > E.screen_cols) { len = E.screen_cols; }
      ab_append(ab, E.row[y].chars, len);
    }

    // Clear remainder of line
    ab_append(ab, U8("\x1b[K"), 3);

    // For all but last line, add newline
    if(y + 1 < E.screen_rows) {
      ab_append(ab, U8("\r\n"), 2);
    }
  }
}

// Refresh screen display
void editor_refresh_screen(void) {
  struct abuf ab = ABUF_INIT;

  // Hide cursor
  ab_append(&ab, U8("\x1b[?25l"), 6);

  // Cursor -> top-left
  ab_append(&ab, U8("\x1b[H"), 3);

  // Draw each row of the screen
  editor_draw_rows(&ab);

  // Cursor -> current position
  uint8_t buf[32];
  int buf_len = snprintf((char*)buf, sizeof(buf),
      "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  ab_append(&ab, buf, buf_len);

  // Show cursor
  ab_append(&ab, U8("\x1b[?25h"), 6);

  // Output buffer
  if(-1 == write(STDOUT_FILENO, ab.buf, ab.len)) {
    die("write");
  }

  // Free buffer
  ab_free(&ab);
}

//// Row-wise operations

// Append a row to the file
void editor_append_row(uint8_t *buf, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

  int at = E.num_rows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, buf, len);
  E.row[at].chars[len] = '\0';

  ++E.num_rows;
}

//// FILE I/O

// Read a file into the editor.
void editor_open(const char* filename) {
  FILE *fp = fopen(filename, "r");
  if(!fp) { die("fopen"); }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while((linelen = getline(&line, &linecap, fp)) != -1) {
    if((linelen > 0) && (
          (line[linelen - 1] == '\n') || (line[linelen - 1] == '\r'))) {
      --linelen;
    }

    editor_append_row((uint8_t*) line, linelen);
  }

  free(line);
  fclose(fp);
}

//// INPUT HANDLING

// Cursor movement
void editor_move_cursor(int key) {
  switch(key) {
    case ARROW_LEFT:
      if(E.cx > 0) { E.cx--; }
      break;
    case ARROW_RIGHT:
      if(E.cx < E.screen_cols - 1) { E.cx++; }
      break;
    case ARROW_UP:
      if(E.cy > 0) { E.cy--; }
      break;
    case ARROW_DOWN:
      if(E.cy < E.screen_rows - 1) { E.cy++; }
      break;
  }
}

// Read and process one key from the keyboard.
void editor_process_key(void) {
  int c = editor_read_key();

  switch(c) {
    case CTRL_KEY('q'):
      exit(EXIT_SUCCESS);
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      E.cx = E.screen_cols - 1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        int times = E.screen_rows;
        while(times--) {
          editor_move_cursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
        }
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;

    default:
      // NOP
      break;
  }
}

//// MAIN LOOP

void init_editor() {
  // Obtain terminal window size
  if(-1 == get_window_size(&E.screen_rows, &E.screen_cols)) {
    die("window size");
  }

  // Reset cursor position
  E.cx = E.cy = 0;

  // No rows
  E.num_rows = 0;
  E.row = NULL;
}

int main(int argc, char** argv) {
  // Save original terminal config for restore_terminal() to use
  if(-1 == tcgetattr(STDIN_FILENO, &E.orig_termios)) {
    die("tcgetattr");
  }

  // Register restore_terminal() as an atexit handler so that the terminal is
  // restored when we terminate.
  atexit(restore_terminal);

  // Move to "raw" mode for the terminal.
  enable_raw_mode();

  // Initialise editor
  init_editor();

  // Load file if specified
  if(argc >= 2) {
    editor_open(argv[1]);
  }

  // Input loop.
  while(1) {
    editor_refresh_screen();
    editor_process_key();
  }

  return EXIT_SUCCESS;
}
