#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
  int idx; // position of this row within the file
  ssize_t size;
  ssize_t r_size;
  uint8_t* chars;
  uint8_t* render;
  uint8_t* hl; // token types for each byte in render
  int hl_open_comment; // does this row end in an un-closed multiline comment?
} erow;

// Syntax highlighting flags
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

// Syntax highlighting file type detection
struct editor_syntax {
  char *filetype; // detected file type
  char **filematch; // filename patterns
  int flags; // what highlighting to perform
  char *singleline_comment_start; // how do single line comments start?
  char *multiline_comment_start;
  char *multiline_comment_end;
  char **keywords; // NULL-terminated array of keywords (2nd-ary term. with "|")
};

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

  // Desired rendered x position, line length allowing
  int desired_rx;

  // Current position within *rendered* line.
  int rx;

  // Current scroll offset.
  int row_off, col_off;

  // Non zero if buffer has been modified w.r.t. the file on disk.
  int dirty;

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

  // Syntax highlighting information for current file
  struct editor_syntax* syntax;

  // Flag indicating the terminal has resized
  int term_resized;
};

//// GLOBALS

// Short, pithy name for "global editor".
struct editor_config E;

// Filetype tables
char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };
char *C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return",
  "else", "struct", "union", "typedef", "static", "enum", "class",
  "case",

  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|",
  
  NULL
};

struct editor_syntax HLDB[] = {
  {
    "c", C_HL_extensions, HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS,
    "//", "/*", "*/", C_HL_keywords,
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

//// MACROS

// Editor version string
#define KILO_VERSION "0.0.1"

// Tab stop size
#define KILO_TAB_STOP 8

// Time (in seconds) to display status messages
#define KILO_MSG_TIMEOUT 5

// Number of times quit command must be issued if the buffer is dirty
#define KILO_QUIT_TIMES 3

// Byte corresponding to CTRL-<key>
#define CTRL_KEY(key) ((key) & 0x1f)

// Coerce string to uint8_t pointer
#define U8(str) ((uint8_t*)(str))

// Special key values. Those which do not map to a single ASCII value all have
// values > 0xff so that they are not byte values.
enum special_keys {
  ENTER_KEY = '\r', // formally: carriage return
  ESCAPE_KEY = '\x1b',
  BACKSPACE = 127,

  ARROW_LEFT = 0x1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,

  DEL_KEY,

  HOME_KEY,
  END_KEY,

  PAGE_UP,
  PAGE_DOWN,

  TERM_RESIZE_KEY,
};

// Syntax highlighting tokens
enum highlight_tokens {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_MLCOMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH, // search match
};

//// PROTOTYPES

// a callback taking the current input and last key pressed
typedef void (*prompt_cb)(char*, int);

char* editor_prompt(char* prompt, prompt_cb cb);

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

    // Handle terminal resize as a "special" key
    if(E.term_resized) { return TERM_RESIZE_KEY; }
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

//// SYNTAX HIGHLIGHTING

// returns non-zero if c is a separator character
int is_separator(int c) {
  return isspace(c) || (c == '\0') || (strchr(",.()+-/*=~%<>[];", c) != NULL);
}

// Update syntax highlighting for a single row
void editor_update_syntax(erow* row) {
  // re-allocate hl buffer
  row->hl = realloc(row->hl, row->r_size);
  memset(row->hl, HL_NORMAL, row->r_size);

  // if there's no syntax highlighting info, that's all
  if(E.syntax == NULL) { return; }

  // was previous character a separator?
  int prev_sep = 1;

  // are we within a string? if so, set this to the terminating char.
  int in_string = 0;

  // Are we within a multiline comment from the previous row?
  int in_comment = (row->idx > 0) && (E.row[row->idx-1].hl_open_comment);

  // what (if any) prefix denotes single and multi line comments
  char* scs = E.syntax->singleline_comment_start;
  char* mcs = E.syntax->multiline_comment_start;
  char* mce = E.syntax->multiline_comment_end;
  
  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  // keywords
  char **keywords = E.syntax->keywords;

  int i=0;
  while(i < row->r_size) {
    char c = row->render[i];
    uint8_t prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    if(scs_len && !in_string && !in_comment) {
      if(!strncmp((char*)&row->render[i], scs, scs_len)) {
        // rest of line is a comment
        memset(&row->hl[i], HL_COMMENT, row->r_size - i);
        break;
      }
    }

    if(mcs_len && mce_len && !in_string) {
      if(in_comment) {
        row->hl[i] = HL_MLCOMMENT;
        if(!strncmp((char*)&row->render[i], mce, mce_len)) {
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          ++i;
          continue;
        }
      } else if(!strncmp((char*)&row->render[i], mcs, mcs_len)) {
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
        i += mcs_len;
        in_comment = 1;
        continue;
      }
    }

    if(E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if(in_string) {
        row->hl[i] = HL_STRING;
        
        // handle escaped terminators
        if((c == '\\') && (i + 1 < row->r_size)) {
          row->hl[i + 1] = HL_STRING;
          i += 2;
          continue;
        }
        
        if(c == in_string) { in_string = 0; }
        ++i;
        prev_sep = 1;
        continue;
      } else {
        if((c == '"') || (c == '\'')) {
          in_string = c;
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if((isdigit(c) && (prev_sep || (prev_hl == HL_NUMBER))) ||
         ((c == '.') && (prev_hl == HL_NUMBER))) {
        row->hl[i] = HL_NUMBER;
        ++i;
        prev_sep = 0;
        continue;
      }
    }

    if(prev_sep) {
      int j=0;
      for(j=0; keywords[j]; ++j) {
        int klen = strlen(keywords[j]);
        int kw2 = keywords[j][klen-1] == '|';
        if(kw2) { klen--; }

        if(klen + i > row->r_size) { continue; }

        if(!strncmp((char*)&row->render[i], keywords[j], klen) &&
             is_separator(row->render[i + klen])) {
           memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
           i += klen;
           break;
        }
      }
      if(keywords[j] != NULL) { prev_sep = 0; continue; }
    }

    prev_sep = is_separator(c);
    ++i;
  }

  // look to see if the multiline comment flag changed
  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;
  if(changed && (row->idx + 1 < E.num_rows)) {
    // update syntax for row underneath us if necessary
    editor_update_syntax(&E.row[row->idx+1]);
  }
}

// Convert a syntax highlight token to the corresponding ANSI color code
int editor_syntax_to_colour(int hl) {
  switch(hl) {
    case HL_MLCOMMENT:
    case HL_COMMENT:
      return 36; // cyan
    case HL_KEYWORD1: return 33; // yellow
    case HL_KEYWORD2: return 32; // green
    case HL_STRING: return 35; // magenta
    case HL_NUMBER: return 31; // red
    case HL_MATCH: return 34; // blue
    default: return 37; // white
  }
}

// Match current filename to a set of syntax highlighting rules
void editor_select_syntax_highlight(void) {
  // reset any existing association
  E.syntax = NULL;

  // if there's no filename, that's it
  if(E.filename == NULL) { return; }

  // loop over each entry
  for(unsigned int j=0; j<HLDB_ENTRIES; ++j) {
    struct editor_syntax* s = &HLDB[j];

    // loop over filematch entries
    for(int i=0; s->filematch[i] != NULL; ++i) {
      char* p = strstr(E.filename, s->filematch[i]);
      if(p != NULL) {
        int patlen = strlen(s->filematch[i]);
        if((s->filematch[i][0] != '.') || (p[patlen] == '\0')) {
          // it's a match!
          E.syntax = s;

          // re-highlight file
          for(int file_row=0; file_row < E.num_rows; ++file_row) {
            editor_update_syntax(&E.row[file_row]);
          }
          return;
        }
      }
    }
  }
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

// Compute the cursor offset for the given rendered x-position
int editor_row_rx_to_cx(erow* row, int rx_target) {
  int rx = 0, cx = 0;
  for(cx=0; cx < row->size; ++cx) {
    if(row->chars[cx] == '\t') {
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    }
    ++rx;

    if(rx > rx_target) { return cx; }
  }
  return cx;
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

  // re-compute syntax highlighting
  editor_update_syntax(row);
}

// Free resources associated with a row
void editor_free_row(erow* row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

// Delete a row from the file
void editor_del_row(int at) {
  // Bounds check
  if((at < 0) || (at >= E.num_rows)) { return; }

  // Free resources for the row
  editor_free_row(&E.row[at]);

  // Shuffle other rows up
  memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.num_rows - at - 1));
  E.num_rows--;

  // Each row now needs its idx reducing
  for(int i=at; i<E.num_rows; ++i) {
    E.row[at].idx--;
  }

  // Set dirty bit
  E.dirty = 1;
}

// Insert a row in the file. If buf is non-NULL it is the contents of the new
// row.
void editor_insert_row(int at, uint8_t *buf, size_t len) {
  // bounds check
  if((at < 0) || (at > E.num_rows)) { return; }

  // Make room for new row and shuffle array
  E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));
  memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.num_rows - at));
  E.num_rows++;

  // For each row below ours, idx needs incrementing
  for(int i=at+1; i<E.num_rows; ++i) {
    E.row[i].idx++;
  }

  // Initialise row
  if(!buf) { len = 0; }
  E.row[at].idx = at;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, buf, len);
  E.row[at].chars[len] = '\0';

  // Re-render row
  E.row[at].r_size = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  editor_update_row(&E.row[at]);

  // set dirty bit
  E.dirty = 1;

  // reset multiline comment flag
  E.row[at].hl_open_comment = 0;
}

// Append an array of bytes to a row
void editor_row_append_string(erow *row, uint8_t* s, size_t len) {
  // make space for new characters
  row->chars = realloc(row->chars, row->size + len + 1);

  // copy bytes to end of row
  memcpy(&row->chars[row->size], s, len);
  row->size += len;

  // add terminating NUL
  row->chars[row->size] = '\0';

  // re-render row
  editor_update_row(row);

  // set dirty bit
  E.dirty = 1;
}

// Insert a character into an existing row
void editor_row_insert_char(erow *row, int at, uint8_t c) {
  // clip at to lie within row or just beyond it
  if((at < 0) || (at > row->size)) { at = row->size; }

  // make room in buffer
  row->chars = realloc(row->chars, row->size + 2);

  // shift characters from insertion point forward one
  memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
  row->size++;

  // insert new character
  row->chars[at] = c;

  // re-render row
  editor_update_row(row);

  // set dirty bit
  E.dirty = 1;
}

// Remove character at an index within row
void editor_row_del_char(erow *row, int at) {
  // check bounds
  if((at < 0) || (at >= row->size)) { return; }

  // shuffle characters beyond deletion point backwards
  memmove(&row->chars[at], &row->chars[at+1], row->size - at);
  row->size--;

  // re-render row
  editor_update_row(row);

  // set sirty bit
  E.dirty = 1;
}

//// EDITING OPERATIONS

// insert a character at cursor
void editor_insert_char(uint8_t c) {
  // insert a blank row at end of file if we're on the last line
  if(E.cy == E.num_rows) {
    editor_insert_row(E.num_rows, U8(""), 0);
  }

  // insert the character
  editor_row_insert_char(&E.row[E.cy], E.cx, c);

  // advance cursor
  E.cx++;
}

// delete character to the left of cursor
void editor_del_char(void) {
  // don't do anything at extreme ends of file
  if(E.cy == E.num_rows) { return; }
  if((E.cx == 0) && (E.cy == 0)) { return; }

  erow *row = &E.row[E.cy];

  if(E.cx > 0) {
    editor_row_del_char(row, E.cx - 1);
    E.cx--;
  } else {
    // join row to previous row. We know cy > 0 due to check above but we assert
    // here for documentation and sanity checking
    assert(E.cy > 0);

    // Move cursor horizontally to end of previous row
    E.cx = E.row[E.cy - 1].size;

    // Join row to previous row
    editor_row_append_string(&E.row[E.cy-1], row->chars, row->size);
    editor_del_row(E.cy);
    E.cy--;

  }
}

// Insert a newline at current cursor
void editor_insert_new_line(void) {
  int new_cx = 0;

  if(E.cx == 0) {
    // Simply insert a new row *above* this one
    editor_insert_row(E.cy, U8(""), 0);
  } else {
    // The check above should guard against cy being == num_rows but sanity
    // check.
    assert(E.cy < E.num_rows);

    // Find current row
    erow *row = &E.row[E.cy];

    // How many blank characters start this row?
    int n_blank = 0;
    for(int i=0; i<row->size; ++i) {
      if(!isblank(row->chars[i])) { break; }
      n_blank++;
    }

    // Don't go beyond the current cursor position
    if(n_blank > E.cx) { n_blank = E.cx; }

    // Split row at insert point by first inserting a new row with the
    // blank characters which begin this row ...
    editor_insert_row(E.cy + 1, row->chars, n_blank);
    row = &E.row[E.cy]; // insert row might have realloc()-ed array

    // ... then append rightmost portion of current row ...
    editor_row_append_string(&E.row[E.cy + 1], &row->chars[E.cx], row->size - E.cx);

    // ... and truncate the current row
    row->size = (E.cx == n_blank) ? 0 : E.cx;
    row->chars[row->size] = '\0';
    editor_update_row(row);

    // the new cx should be the number of blank characters
    new_cx = n_blank;
  }

  // Move cursor to start of new row
  E.cy++;
  E.cx = new_cx;
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
      // within file
      int len = E.row[file_row].r_size - E.col_off;
      if(len < 0) { len = 0; }
      if(len > E.screen_cols) { len = E.screen_cols; }

      // get rendered string from start of output line
      uint8_t* c = &E.row[file_row].render[E.col_off];

      // get highlight tokens
      uint8_t* hl = &E.row[file_row].hl[E.col_off];

      // append string with colours
      int current_colour = -1;
      for(int j=0; j<len; ++j) {
        if(!isprint(c[j])) {
          // display control characters in reverse video
          uint8_t sym = (c[j] < 26) ? '@' + c[j] : '?';
          ab_append(ab, U8("\x1b[7m"), 4);
          ab_append(ab, &sym, 1);
          ab_append(ab, U8("\x1b[m"), 3);

          // Restore colour if necessary
          if(current_colour != -1) {
            uint8_t buf[16];
            int clen = snprintf((char*)buf, sizeof(buf), "\x1b[%dm", current_colour);
            ab_append(ab, buf, clen);
          }
        } else if(hl[j] == HL_NORMAL) {
          // only reset colour if necessary
          if(current_colour != -1) {
            ab_append(ab, U8("\x1b[39m"), 5);
            current_colour = -1;
          }
          ab_append(ab, &c[j], 1);
        } else {
          int colour = editor_syntax_to_colour(hl[j]);
          if(colour != current_colour) {
            uint8_t buf[16];
            int clen = snprintf((char*)buf, sizeof(buf), "\x1b[%dm", colour);
            ab_append(ab, buf, clen);
            current_colour = colour;
          }
          ab_append(ab, &c[j], 1);
        }
      }

      // reset colour before next line
      ab_append(ab, U8("\x1b[39m"), 5);
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
      " %.20s - %d lines %s", E.filename ? E.filename : "[No Name]",
      E.num_rows, E.dirty ? "(modified)" : "");
  if(len > E.screen_cols) { len = E.screen_cols; }
  ab_append(ab, (uint8_t*)status, len);

  int rlen = snprintf(rstatus, sizeof(rstatus),
      "%s | %d/%d ",
      E.syntax ? E.syntax->filetype : "no ft",
      E.cy+1, E.num_rows);

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

  // match syntax highlighting
  editor_select_syntax_highlight();

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while((linelen = getline(&line, &linecap, fp)) != -1) {
    if((linelen > 0) && (
          (line[linelen - 1] == '\n') || (line[linelen - 1] == '\r'))) {
      --linelen;
    }

    editor_insert_row(E.num_rows, (uint8_t*) line, linelen);
  }

  free(line);
  fclose(fp);

  // Reset dirty bit
  E.dirty = 0;
}

// Write all the rows to a single string. Returns the length of the allocated
// buffer in buflen. Set buflen to NULL if you don't care about the length. The
// returned string should be free()-ed when done.
char* editor_rows_to_string(int *buflen) {
  int totlen = 0;

  // Work out the total length of all the rows
  for(int j=0; j < E.num_rows; ++j) {
    totlen += E.row[j].size + 1; // + 1 for newline
  }
  if(buflen) { *buflen = totlen; }

  // Allocate a buffer to hold the string
  char *buf = malloc(totlen);

  // Walk each row copying the bytes
  char *p = buf;
  for(int j=0; j < E.num_rows; ++j) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n'; // Add newline
    ++p;
  }

  return buf;
}

// Write the editor contents to the current filename.
void editor_save(void) {
  if(E.filename == NULL) {
    // Prompt user for filename
    E.filename = editor_prompt("Save as: %s", NULL);
    if(E.filename == NULL) {
      editor_set_status_message("Save aborted");
      return;
    }
  }

  // Match syntax highlighting
  editor_select_syntax_highlight();

  // Convert editor rows to a string for saving
  int len;
  char *buf = editor_rows_to_string(&len);

  // Open the file for writing and truncate it should the file have already
  // existed.
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if(fd != -1) {
    if(-1 != ftruncate(fd, len)) {
      // Write contents of file
      if(len == write(fd, buf, len)) {
        close(fd);
        free(buf);
        editor_set_status_message("%d bytes written", len);

        // Reset dirty bit
        E.dirty = 0;
        return;
      }
    }
    close(fd);
  }

  // Free string storage
  free(buf);

  // This path through the function indicates an error saving, report this.
  editor_set_status_message("error saving: %s", strerror(errno));
}

//// FIND

void editor_find_callback(char *query, int key) {
  static int start_match_rx = 0;
  static int start_match_row = 0;
  static int direction = 1;

  // static variables used to save the row number and contents of the
  // previous contents of hl before the search match is highlighted
  static int saved_hl_row;
  static uint8_t* saved_hl = NULL;

  // If there was a saved hl line from a previous match, restore it
  if(saved_hl) {
    memcpy(E.row[saved_hl_row].hl, saved_hl, E.row[saved_hl_row].r_size);
    free(saved_hl);
    saved_hl = NULL;
  }

  if((key == ARROW_RIGHT) || (key == ARROW_DOWN)) {
    direction = 1;
  } else if((key == ARROW_LEFT) || (key == ARROW_UP)) {
    direction = -1;
  } else if(iscntrl(key) || (key >= 0x100)) {
    start_match_rx = start_match_row = 0;
    direction = 1;
    return;
  } else {
    start_match_rx = 0;
    start_match_row = 0;
    direction = 1;
  }

  int current_rx = start_match_rx, current_row = start_match_row;

  // search each row in turn
  for(int i=0; i<E.num_rows; ++i) {
    erow* row = &E.row[current_row];
    char* match = strstr((char*)(&row->render[current_rx]), query);

    // if match found, move cursor
    if(match) {
      int match_rx = match - (char*)row->render;
      start_match_rx = match_rx + strlen(query);
      start_match_row = current_row;

      E.cy = current_row;
      E.cx = editor_row_rx_to_cx(row, match_rx);

      // cause editor_scroll() to place matching line at top of screen
      E.row_off = E.num_rows;

      // Save hl before tagging
      saved_hl_row = current_row;
      saved_hl = malloc(row->r_size);
      memcpy(saved_hl, row->hl, row->r_size);

      // Tag matched region
      memset(&row->hl[match_rx], HL_MATCH, strlen(query));

      return;
    } else {
      current_rx = 0;
      current_row += direction;
      if(current_row == -1) {
        current_row = E.num_rows - 1;
      } else if(current_row == E.num_rows) {
        current_row = 0;
      }
    }
  }
}

void editor_find(void) {
  // save cursor and scroll position in case user cancels search
  int scx = E.cx, scy = E.cy, srow_off = E.row_off, scol_off = E.col_off;

  // show search prompt
  char* query = editor_prompt("Search: %s (ESC/Ctrl-C cancels, Arrows continue)",
      editor_find_callback);
  if(query) {
    free(query);
  } else {
    // user cancelled
    E.cx = scx;
    E.cy = scy;
    E.col_off = scol_off;
    E.row_off = srow_off;
  }
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

  // Was this a vertical movement?
  if((key == ARROW_UP) || (key == ARROW_DOWN)) {
    E.cx = row ? editor_row_rx_to_cx(row, E.desired_rx) : 0;
  }

  // Snap cx to new row
  int rowlen = row ? row->size : 0;
  if(E.cx > rowlen) {
    E.cx = rowlen;
  }
}

// Read and process one key from the keyboard.
void editor_process_key(void) {
  static int quit_times = KILO_QUIT_TIMES; // quit time counter
  int c = editor_read_key();

  // Was this a vertical movement command?
  int was_vert = 0;

  // Reset quit times count if anything other than quit is pressed
  if(c != CTRL_KEY('q')) { quit_times = KILO_QUIT_TIMES; }

  switch(c) {
    case TERM_RESIZE_KEY:
      // all we need to do is re-render screen
      E.term_resized = 0;
      break;
    
    case CTRL_KEY('q'):
      if(E.dirty && (quit_times > 0)) {
        editor_set_status_message("File has unsaved changes. "
            "Press Ctrl-Q %d more time%s to quit.",
            quit_times, quit_times == 1 ? "" : "s");
        --quit_times;
        return;
      }
      exit(EXIT_SUCCESS);
      break;

    case CTRL_KEY('s'):
      editor_save();
      break;

    case CTRL_KEY('f'):
      editor_find();
      break;

    case CTRL_KEY('k'):
      editor_del_row(E.cy);
      break;

    // Enter
    case ENTER_KEY:
      editor_insert_new_line();
      break;

    // the various spellings of "backspace"
    case CTRL_KEY('h'):
    case BACKSPACE:
    case DEL_KEY:
      if(c == DEL_KEY) { editor_move_cursor(ARROW_RIGHT); }
      editor_del_char();
      break;

    // Escape
    case CTRL_KEY('l'):
    case ESCAPE_KEY:
      // Ignore
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
        was_vert = 1;
        
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
      was_vert = 1;
      // fall through...
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editor_move_cursor(c);
      break;

    default:
      // Insert character directly if it is a byte and not a special key
      if(c < 0x100) { editor_insert_char(c); }
      break;
  }

  // if movement wasn't vertical, reset desired rx
  if(!was_vert) {
    if(E.cy < E.num_rows) {
      erow* row = &E.row[E.cy];
      E.desired_rx = editor_row_cx_to_rx(row, E.cx);
    } else {
      E.desired_rx = 0;
    }
  }
}

// Prompt for input from the user. Returns a string from the user which should be
// free()-ed when done with. The promp should include a "%s" which is replaced with the
// user's input as they type.
char* editor_prompt(char* prompt, prompt_cb cb) {
  // allocate input buffer
  size_t buf_size = 128;
  char* buf = malloc(buf_size);

  // initialise buffer
  size_t buf_len = 0;
  buf[0] = '\0';

  while(1) {
    editor_set_status_message(prompt, buf);
    editor_refresh_screen();

    // super simple line editor
    int c = editor_read_key();
    if((c == ESCAPE_KEY) || (c == CTRL_KEY('c'))) {
      // cancel on escape / Ctrl-C
      editor_set_status_message("");
      if(cb) { cb(buf, c); }
      free(buf);
      return NULL;
    } else if(c == BACKSPACE) {
      if(buf_len == 0) { continue; }
      buf[--buf_len] = '\0';
    } else if(c == ENTER_KEY) {
      // return buffer to caller if non-empty
      if(buf_len != 0) {
        editor_set_status_message("");
        if(cb) { cb(buf, c); }
        return buf;
      }
    } else if(!iscntrl(c) && (c <= 0xff)) {
      // realloc buffer if necessary
      if(buf_len == buf_size - 1) {
        buf_size *= 2;
        buf = realloc(buf, buf_size);
      }
      buf[buf_len++] = c;
      buf[buf_len] = '\0';
    }

    // pass key to callback
    if(cb) { cb(buf, c); }
  }
}

//// MAIN LOOP

void terminal_resized(int sig) {
  if(sig != SIGWINCH) { return; }

  // Obtain terminal window size
  if(-1 == get_window_size(&E.screen_rows, &E.screen_cols)) {
    die("window size");
  }

  // Make room for status bar and message
  E.screen_rows -= 2;

  if(E.screen_rows < 1) {
    die("terminal too small");
  }

  // Set flag
  E.term_resized = 1;
}

void init_editor(void) {
  // Get initial size of terminal and register terminal size change handler
  terminal_resized(SIGWINCH);
  E.term_resized = 0;
  signal(SIGWINCH, terminal_resized);
  
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

  // Not dirty
  E.dirty = 0;

  // No syntax
  E.syntax = NULL;
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
  editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  // Input loop.
  while(1) {
    editor_refresh_screen();
    editor_process_key();
  }

  return EXIT_SUCCESS;
}
