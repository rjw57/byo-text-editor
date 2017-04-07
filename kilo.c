#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
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
  ssize_t r_size;
  uint8_t* chars;
  uint8_t* render;
} erow;

// The state of our editor.
struct editor_config {
  // Original terminal config on launch.
  struct termios orig_termios;

  // Editor window size.
  // Note: this is not necessarily the terminal size. For a start, this does not
  // include the status bar.
  int screen_rows, screen_cols;

  // Current cursor position.
  int cx, cy;

  // Current position within *rendered* line.
  int rx;

  // Current scroll offset.
  int row_off, col_off;

  // Number of rows in file
  int num_rows;

  // An array of rows
  erow* row;

  // The current filename
  char* filename;

  // Status message displayed to user
  char status_msg[80];

  // When status message was last shown
  time_t status_msg_time;
};

//// GLOBALS

// Short, pithy name for "global editor".
struct editor_config E;

//// MACROS

// Editor version string
#define KILO_VERSION "0.0.1"

// Byte corresponding to CTRL-<key>
#define CTRL_KEY(key) ((key) & 0x1f)

// Coerce string to uint8_t pointer
#define U8(str) ((uint8_t*)(str))

// Tab stop size
#define KILO_TAB_STOP 8

// Time (in seconds) to display status messages
#define KILO_MSG_TIMEOUT 5

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

//// Row-wise operations

// Compute the rendered x-position for a given cursor offset in a row
int editor_row_cx_to_rx(erow* row, int cx) {
  int rx = 0;
  for(int j=0; j < cx; ++j) {
    if(row->chars[j] == '\t') {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    ++rx;
  }
  return rx;
}

// Update a row structure after modification by re-computing it's rendered form.
void editor_update_row(erow* row) {
  int tabs = 0;
  for(int j=0; j<row->size; ++j) {
    if(row->chars[j] == '\t') { ++tabs; }
  }

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP-1) + 1);

  int idx = 0;
  for(int j=0; j<row->size; ++j) {
    if(row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while(idx % KILO_TAB_STOP != 0) { row->render[idx++] = ' '; }
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->r_size = idx;
}

// Append a row to the file
void editor_append_row(uint8_t *buf, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

  int at = E.num_rows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, buf, len);
  E.row[at].chars[len] = '\0';

  E.row[at].r_size = 0;
  E.row[at].render = NULL;
  editor_update_row(&E.row[at]);

  ++E.num_rows;
}

//// OUTPUT

// Scroll editor to ensure cursor is on-screen
void editor_scroll(void) {
  // Set rx
  E.rx = 0;
  if(E.cy < E.num_rows) {
    E.rx = editor_row_cx_to_rx(&(E.row[E.cy]), E.cx);
  }

  if(E.cy < E.row_off) {
    E.row_off = E.cy;
  }

  if(E.cy >= E.row_off + E.screen_rows) {
    E.row_off = E.cy - E.screen_rows + 1;
  }

  if(E.rx < E.col_off) {
    E.col_off = E.rx;
  }

  if(E.rx >= E.col_off + E.screen_cols) {
    E.col_off = E.rx - E.screen_cols + 1;
  }

  assert(E.row_off >= 0);
  assert(E.col_off >= 0);
}

// Draw each row of the screen into the output buffer
void editor_draw_rows(struct abuf *ab) {
  for(int y=0; y<E.screen_rows; ++y) {
    int file_row = y + E.row_off;

    if(file_row >= E.num_rows) {
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
      int len = E.row[file_row].r_size - E.col_off;
      if(len < 0) { len = 0; }
      if(len > E.screen_cols) { len = E.screen_cols; }
      ab_append(ab, &(E.row[file_row].render[E.col_off]), len);
    }

    // Clear remainder of line
    ab_append(ab, U8("\x1b[K"), 3);

    // Add newline
    ab_append(ab, U8("\r\n"), 2);
  }
}

// Draw status bar
void editor_draw_status_bar(struct abuf *ab) {
  // reverse video
  ab_append(ab, U8("\x1b[7m"), 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status),
      " %.20s - %d lines", E.filename ? E.filename : "[No Name]",
      E.num_rows);
  if(len > E.screen_cols) { len = E.screen_cols; }
  ab_append(ab, (uint8_t*)status, len);

  int rlen = snprintf(rstatus, sizeof(rstatus),
      "%d/%d ", E.cy+1, E.num_rows);

  while(len < E.screen_cols) {
    if(E.screen_cols - len == rlen) {
      ab_append(ab, (uint8_t*)rstatus, rlen);
      break;
    }

    ab_append(ab, U8(" "), 1);
    ++len;
  }

  // normal video and new line
  ab_append(ab, U8("\x1b[m\r\n"), 6);
}

// Draw status message (if any)
void editor_draw_message_bar(struct abuf* ab) {
  // Clear any existing line
  ab_append(ab, U8("\x1b[K"), 3);

  int msg_len = strlen(E.status_msg);
  if(msg_len > E.screen_cols) { msg_len = E.screen_cols; }
  if(msg_len && (time(NULL) - E.status_msg_time < KILO_MSG_TIMEOUT)) {
    ab_append(ab, (uint8_t*)E.status_msg, msg_len);
  }
}

// Refresh screen display
void editor_refresh_screen(void) {
  struct abuf ab = ABUF_INIT;

  // Set scroll position
  editor_scroll();

  // Hide cursor
  ab_append(&ab, U8("\x1b[?25l"), 6);

  // Cursor -> top-left
  ab_append(&ab, U8("\x1b[H"), 3);

  // Draw the screen
  editor_draw_rows(&ab);
  editor_draw_status_bar(&ab);
  editor_draw_message_bar(&ab);

  // Cursor -> current position
  uint8_t buf[32];
  int buf_len = snprintf((char*)buf, sizeof(buf),
      "\x1b[%d;%dH", (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
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

// Set a status message. Takes a format string and arguments a la printf().
void editor_set_status_message(const char* fmt, ...) {
  va_list ap;

  // Format message
  va_start(ap, fmt);
  vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
  va_end(ap);

  // Record time
  E.status_msg_time = time(NULL);
}

//// FILE I/O

// Read a file into the editor.
void editor_open(const char* filename) {
  FILE *fp = fopen(filename, "r");
  if(!fp) { die("fopen"); }

  free(E.filename);
  E.filename = strdup(filename);

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
  // Get row under current cursor
  erow *row = (E.cy >= E.num_rows) ? NULL : &(E.row[E.cy]);

  switch(key) {
    case ARROW_LEFT:
      if(E.cx > 0) {
        // simple case
        E.cx--;
      } else if(E.cy > 0) {
        // end of previous line
        E.cy--;
        if(E.cy < E.num_rows) { E.cx = E.row[E.cy].size; }
      }
      break;
    case ARROW_RIGHT:
      if(row && (E.cx < row->size)) {
        // simple case
        E.cx++;
      } else if(row && (E.cx == row->size) && (E.cy < E.num_rows)) {
        // start of next line
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if(E.cy > 0) { E.cy--; }
      break;
    case ARROW_DOWN:
      if(E.cy < E.num_rows) { E.cy++; }
      break;
  }

  // Get (possibly) new row under cursor
  row = (E.cy >= E.num_rows) ? NULL : &(E.row[E.cy]);

  // Snap cx to new row
  int rowlen = row ? row->size : 0;
  if(E.cx > rowlen) { E.cx = rowlen; }
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
      if(E.cy < E.num_rows) {
        E.cx = E.row[E.cy].size;
      }
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        // Position cursor at top or bottom of page depending on keypress
        if(c == PAGE_UP) {
          E.cy = E.row_off;
        } else if(c == PAGE_DOWN) {
          E.cy = E.row_off + E.screen_rows - 1;
        }

        // Simulate multiple arrow key presses. Takes care of correcting any
        // cursor positions due to line lengths, etc.
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

  // Make room for status bar and message
  E.screen_rows -= 2;

  if(E.screen_rows < 1) {
    die("terminal too small");
  }

  // Reset cursor position
  E.rx = E.cx = E.cy = 0;

  // Reset scroll position
  E.col_off = E.row_off = 0;

  // No rows
  E.num_rows = 0;
  E.row = NULL;

  // No file
  E.filename = NULL;

  // No status
  E.status_msg[0] = '\0';
  E.status_msg_time = 0;
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

  // Set a helpful status message
  editor_set_status_message("HELP: Ctrl-Q = quit");

  // Input loop.
  while(1) {
    editor_refresh_screen();
    editor_process_key();
  }

  return EXIT_SUCCESS;
}
