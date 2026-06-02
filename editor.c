/*
 * editor.c — Terminal Text Editor (kilo-style, raw mode)
 * Corg-Labs Educational Implementation
 *
 * Features: raw mode, line numbers, navigation, search, edit, save,
 *           C syntax highlighting, status bar
 *
 * Build:  gcc editor.c -o editor
 * Usage:  ./editor filename.c
 */

#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <time.h>

/* ── ANSI escape codes ───────────────────────────────────────────────────── */
#define ESC_CLEAR      "\x1b[2J"
#define ESC_HOME       "\x1b[H"
#define ESC_HIDECURSOR "\x1b[?25l"
#define ESC_SHOWCURSOR "\x1b[?25h"
#define ESC_BOLD       "\x1b[1m"
#define ESC_RESET      "\x1b[0m"
/* colours */
#define COL_KEYWORD    "\x1b[1;34m"   /* bold blue   */
#define COL_STRING     "\x1b[32m"     /* green       */
#define COL_COMMENT    "\x1b[90m"     /* grey        */
#define COL_NUMBER     "\x1b[36m"     /* cyan        */
#define COL_SEARCH     "\x1b[1;33m"   /* bold yellow */

/* ── Key definitions ─────────────────────────────────────────────────────── */
#ifndef CTRL
#define CTRL(k)       ((k) & 0x1f)
#endif
#define KEY_UP        1000
#define KEY_DOWN      1001
#define KEY_LEFT      1002
#define KEY_RIGHT     1003
#define KEY_PGUP      1004
#define KEY_PGDN      1005
#define KEY_HOME      1006
#define KEY_END       1007
#define KEY_DEL       1008
#define KEY_BACKSPACE 127

/* ── Editor row ──────────────────────────────────────────────────────────── */
#define TABSTOP 4
typedef struct {
    int    len;
    char  *chars;
    int    rlen;
    char  *render;   /* tab-expanded, for display */
} Row;

/* ── Editor state ────────────────────────────────────────────────────────── */
#define STATUS_MSG_LEN 128
typedef struct {
    int cx, cy;           /* cursor position in file coords */
    int rx;               /* render x (after tab expansion) */
    int rowoff, coloff;   /* scroll offsets */
    int screen_rows;
    int screen_cols;
    int num_rows;
    Row *rows;
    int dirty;
    char filename[256];
    char status_msg[STATUS_MSG_LEN];
    time_t status_time;
    struct termios orig_termios;
} Editor;

static Editor E;

/* ── Terminal ────────────────────────────────────────────────────────────── */
static void die(const char *s) {
    write(STDOUT_FILENO, ESC_CLEAR ESC_HOME, 7);
    perror(s);
    exit(1);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disable_raw_mode);
    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT|ICRNL|INPCK|ISTRIP|IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_lflag &= ~(ECHO|ICANON|IEXTEN|ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        /* query cursor position */
        if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
        char buf[32]; int i = 0;
        while (i < 31) {
            if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
            if (buf[i] == 'R') break;
            i++;
        }
        buf[i] = '\0';
        if (buf[0] != '\x1b' || buf[1] != '[') return -1;
        if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
        return 0;
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* ── Input ───────────────────────────────────────────────────────────────── */
static int read_key(void) {
    char c;
    int  n;
    while ((n = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (n == -1 && errno != EAGAIN) die("read");
    }
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1) return '\x1b';
                if (seq2 == '~') {
                    switch (seq[1]) {
                        case '1': return KEY_HOME;
                        case '3': return KEY_DEL;
                        case '4': return KEY_END;
                        case '5': return KEY_PGUP;
                        case '6': return KEY_PGDN;
                        case '7': return KEY_HOME;
                        case '8': return KEY_END;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return KEY_UP;
                    case 'B': return KEY_DOWN;
                    case 'C': return KEY_RIGHT;
                    case 'D': return KEY_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
        return '\x1b';
    }
    return (int)(unsigned char)c;
}

/* ── Row operations ──────────────────────────────────────────────────────── */
static void row_update_render(Row *row) {
    free(row->render);
    row->render = malloc(row->len * TABSTOP + 1);
    int j = 0;
    for (int i = 0; i < row->len; i++) {
        if (row->chars[i] == '\t') {
            do { row->render[j++] = ' '; } while (j % TABSTOP);
        } else {
            row->render[j++] = row->chars[i];
        }
    }
    row->render[j] = '\0';
    row->rlen = j;
}

static void insert_row(int at, const char *s, int len) {
    E.rows = realloc(E.rows, (E.num_rows + 1) * sizeof(Row));
    if (at < E.num_rows) memmove(&E.rows[at+1], &E.rows[at], (E.num_rows-at)*sizeof(Row));
    E.rows[at].len   = len;
    E.rows[at].chars = malloc(len + 1);
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';
    E.rows[at].render = NULL;
    E.rows[at].rlen   = 0;
    row_update_render(&E.rows[at]);
    E.num_rows++;
    E.dirty++;
}

static void delete_row(int at) {
    if (at < 0 || at >= E.num_rows) return;
    free(E.rows[at].chars);
    free(E.rows[at].render);
    memmove(&E.rows[at], &E.rows[at+1], (E.num_rows-at-1)*sizeof(Row));
    E.num_rows--;
    E.dirty++;
}

static void row_insert_char(Row *row, int at, int c) {
    if (at < 0 || at > row->len) at = row->len;
    row->chars = realloc(row->chars, row->len + 2);
    memmove(&row->chars[at+1], &row->chars[at], row->len - at + 1);
    row->chars[at] = (char)c;
    row->len++;
    row_update_render(row);
    E.dirty++;
}

static void row_delete_char(Row *row, int at) {
    if (at < 0 || at >= row->len) return;
    memmove(&row->chars[at], &row->chars[at+1], row->len - at);
    row->len--;
    row_update_render(row);
    E.dirty++;
}

static void row_append_string(Row *row, const char *s, int len) {
    row->chars = realloc(row->chars, row->len + len + 1);
    memcpy(&row->chars[row->len], s, len);
    row->len += len;
    row->chars[row->len] = '\0';
    row_update_render(row);
    E.dirty++;
}

/* ── Editor operations ───────────────────────────────────────────────────── */
static void insert_char(int c) {
    if (E.cy == E.num_rows) insert_row(E.num_rows, "", 0);
    row_insert_char(&E.rows[E.cy], E.cx, c);
    E.cx++;
}

static void insert_newline(void) {
    if (E.cx == 0) {
        insert_row(E.cy, "", 0);
    } else {
        Row *row = &E.rows[E.cy];
        insert_row(E.cy + 1, &row->chars[E.cx], row->len - E.cx);
        row->len = E.cx;
        row->chars[row->len] = '\0';
        row_update_render(row);
    }
    E.cy++;
    E.cx = 0;
}

static void delete_char(void) {
    if (E.cy == E.num_rows) return;
    if (E.cx == 0 && E.cy == 0) return;
    if (E.cx > 0) {
        row_delete_char(&E.rows[E.cy], E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.rows[E.cy-1].len;
        row_append_string(&E.rows[E.cy-1], E.rows[E.cy].chars, E.rows[E.cy].len);
        delete_row(E.cy);
        E.cy--;
    }
}

/* ── File I/O ────────────────────────────────────────────────────────────── */
static void open_file(const char *filename) {
    strncpy(E.filename, filename, 255);
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        /* New file */
        return;
    }
    char  *line = NULL;
    size_t cap  = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) != -1) {
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        insert_row(E.num_rows, line, (int)n);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

static void save_file(void) {
    if (!E.filename[0]) {
        snprintf(E.status_msg, STATUS_MSG_LEN, "No filename.");
        E.status_time = time(NULL);
        return;
    }
    FILE *fp = fopen(E.filename, "w");
    if (!fp) {
        snprintf(E.status_msg, STATUS_MSG_LEN, "Can't save: %s", strerror(errno));
        E.status_time = time(NULL);
        return;
    }
    for (int i = 0; i < E.num_rows; i++)
        fprintf(fp, "%s\n", E.rows[i].chars);
    fclose(fp);
    E.dirty = 0;
    snprintf(E.status_msg, STATUS_MSG_LEN, "Saved to %s", E.filename);
    E.status_time = time(NULL);
}

/* ── Syntax highlighting ─────────────────────────────────────────────────── */
static const char *C_KEYWORDS[] = {
    "int","long","short","char","void","float","double","unsigned","signed",
    "struct","union","typedef","enum","static","extern","auto","register",
    "const","volatile","inline","return","if","else","while","do","for",
    "break","continue","switch","case","default","goto","sizeof","NULL",
    "#include","#define","#ifdef","#ifndef","#endif","#else","#pragma",NULL
};

/* Append a highlighted render of `row` into a dynamic buffer */
static void append_highlighted(char **buf, int *blen, const char *render, int rlen) {
    int i = 0;
    int in_string = 0; char str_char = 0;
    int in_comment = 0;
    while (i < rlen) {
        char c = render[i];
        /* Line comment */
        if (!in_string && !in_comment && i+1 < rlen && render[i]=='/' && render[i+1]=='/') {
            in_comment = 1;
            char tmp[16]; int tl = snprintf(tmp,16,"%s",COL_COMMENT);
            *buf = realloc(*buf, *blen + tl + 1);
            memcpy(*buf + *blen, tmp, tl); *blen += tl;
        }
        if (in_comment) {
            *buf = realloc(*buf, *blen + 2);
            (*buf)[(*blen)++] = c; (*buf)[*blen] = '\0';
            i++; continue;
        }
        /* Strings */
        if (!in_string && (c == '"' || c == '\'')) {
            in_string = 1; str_char = c;
            char tmp[16]; int tl = snprintf(tmp,16,"%s",COL_STRING);
            *buf = realloc(*buf, *blen + tl + 1);
            memcpy(*buf + *blen, tmp, tl); *blen += tl;
        }
        if (in_string) {
            *buf = realloc(*buf, *blen + 2);
            (*buf)[(*blen)++] = c; (*buf)[*blen] = '\0';
            if (c == str_char && (i == 0 || render[i-1] != '\\')) {
                in_string = 0;
                char tmp[16]; int tl = snprintf(tmp,16,"%s",ESC_RESET);
                *buf = realloc(*buf, *blen + tl + 1);
                memcpy(*buf + *blen, tmp, tl); *blen += tl;
            }
            i++; continue;
        }
        /* Numbers */
        if (isdigit((unsigned char)c) && (i==0 || !isalnum((unsigned char)render[i-1]))) {
            char tmp[32]; int tl = snprintf(tmp,32,"%s%c%s",COL_NUMBER,c,ESC_RESET);
            *buf = realloc(*buf, *blen + tl + 1);
            memcpy(*buf + *blen, tmp, tl); *blen += tl;
            i++; continue;
        }
        /* Keywords */
        int matched = 0;
        for (int k = 0; C_KEYWORDS[k]; k++) {
            int klen = (int)strlen(C_KEYWORDS[k]);
            if (strncmp(&render[i], C_KEYWORDS[k], klen) == 0) {
                char after = render[i+klen];
                if (!isalnum((unsigned char)after) && after != '_') {
                    char tmp[64];
                    int tl = snprintf(tmp,64,"%s%.*s%s", COL_KEYWORD, klen, &render[i], ESC_RESET);
                    *buf = realloc(*buf, *blen + tl + 1);
                    memcpy(*buf + *blen, tmp, tl); *blen += tl;
                    i += klen; matched = 1; break;
                }
            }
        }
        if (matched) continue;
        /* Plain char */
        *buf = realloc(*buf, *blen + 2);
        (*buf)[(*blen)++] = c; (*buf)[*blen] = '\0';
        i++;
    }
    if (in_comment || in_string) {
        char tmp[16]; int tl = snprintf(tmp,16,"%s",ESC_RESET);
        *buf = realloc(*buf, *blen + tl + 1);
        memcpy(*buf + *blen, tmp, tl); *blen += tl;
    }
}

/* ── Rendering ───────────────────────────────────────────────────────────── */
#define BUF_APPEND(b,bl,s,l) do { \
    *(b) = realloc(*(b), *(bl)+(l)+1); \
    memcpy(*(b)+*(bl),(s),(l)); *(bl)+=(l); (*(b))[*(bl)]='\0'; \
} while(0)

static void refresh_screen(void) {
    /* Scroll */
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screen_rows) E.rowoff = E.cy - E.screen_rows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + (E.screen_cols - 6)) E.coloff = E.rx - (E.screen_cols - 6) + 1;

    char *buf = NULL; int blen = 0;
    BUF_APPEND(&buf, &blen, ESC_HIDECURSOR ESC_HOME, 9);

    int linenum_width = 5;  /* "  42 |" style */

    for (int y = 0; y < E.screen_rows; y++) {
        int filerow = y + E.rowoff;
        /* Line number */
        char lnum[16];
        if (filerow < E.num_rows) {
            snprintf(lnum, 16, "\x1b[90m%4d \x1b[0m", filerow + 1);
        } else {
            snprintf(lnum, 16, "\x1b[90m   ~ \x1b[0m");
        }
        BUF_APPEND(&buf, &blen, lnum, strlen(lnum));

        if (filerow < E.num_rows) {
            Row *row = &E.rows[filerow];
            /* Highlight and clip to coloff */
            char *hl = NULL; int hl_len = 0;
            int is_c = strstr(E.filename, ".c") || strstr(E.filename, ".h");
            if (is_c) {
                append_highlighted(&hl, &hl_len, row->render, row->rlen);
            } else {
                hl = strdup(row->render);
                hl_len = row->rlen;
            }
            /* Naive: just print from coloff (ignoring escape codes for now) */
            int skip = E.coloff;
            int show = E.screen_cols - linenum_width;
            /* For simplicity write raw render chars clipped */
            int start = skip < row->rlen ? skip : row->rlen;
            int end   = start + show;
            if (end > row->rlen) end = row->rlen;
            /* Use highlighted version */
            if (is_c) {
                free(hl); hl = NULL; hl_len = 0;
                append_highlighted(&hl, &hl_len,
                    row->render + (start < row->rlen ? start : row->rlen),
                    end - start);
                BUF_APPEND(&buf, &blen, hl, hl_len);
            } else {
                BUF_APPEND(&buf, &blen, row->render + start, end - start);
            }
            free(hl);
        }
        BUF_APPEND(&buf, &blen, ESC_RESET "\x1b[K\r\n", 9);
    }

    /* Status bar */
    char status[256], rstatus[64];
    int slen = snprintf(status, 256,
        "\x1b[7m %.20s%s - %d lines ",
        E.filename[0] ? E.filename : "[New File]",
        E.dirty ? " [+]" : "",
        E.num_rows);
    int rslen = snprintf(rstatus, 64, " Ln %d, Col %d \x1b[m",
        E.cy + 1, E.rx + 1);
    BUF_APPEND(&buf, &blen, status, slen);
    int pad = E.screen_cols - (slen - /* escape bytes ~5 */ 5) - (rslen - 5);
    while (pad-- > 0) BUF_APPEND(&buf, &blen, " ", 1);
    BUF_APPEND(&buf, &blen, rstatus, rslen);
    BUF_APPEND(&buf, &blen, "\r\n", 2);

    /* Message bar */
    BUF_APPEND(&buf, &blen, "\x1b[K", 3);
    if (time(NULL) - E.status_time < 5 && E.status_msg[0])
        BUF_APPEND(&buf, &blen, E.status_msg, strlen(E.status_msg));
    else
        BUF_APPEND(&buf, &blen, "^S Save  ^F Find  ^Q Quit", 25);

    /* Move cursor */
    char cur[32];
    int cur_x = (E.rx - E.coloff) + linenum_width + 1;
    int cur_y = (E.cy - E.rowoff) + 1;
    snprintf(cur, 32, "\x1b[%d;%dH", cur_y, cur_x);
    BUF_APPEND(&buf, &blen, cur, strlen(cur));
    BUF_APPEND(&buf, &blen, ESC_SHOWCURSOR, 6);

    write(STDOUT_FILENO, buf, blen);
    free(buf);
}

/* ── Search ──────────────────────────────────────────────────────────────── */
static void find(void) {
    char query[128] = "";
    snprintf(E.status_msg, STATUS_MSG_LEN, "Search (ESC to cancel): ");
    E.status_time = time(NULL);
    refresh_screen();

    /* Read query string */
    int qlen = 0;
    while (1) {
        int c = read_key();
        if (c == '\x1b' || c == CTRL('q')) return;
        if (c == '\r') break;
        if (c == KEY_BACKSPACE && qlen > 0) { query[--qlen] = '\0'; }
        else if (!iscntrl(c) && qlen < 127) { query[qlen++] = (char)c; query[qlen] = '\0'; }
        snprintf(E.status_msg, STATUS_MSG_LEN, "Search: %s", query);
        E.status_time = time(NULL);
        refresh_screen();
    }
    if (!qlen) return;

    for (int i = 0; i < E.num_rows; i++) {
        int r = (E.cy + i + 1) % E.num_rows;
        char *match = strstr(E.rows[r].render, query);
        if (match) {
            E.cy = r;
            E.cx = (int)(match - E.rows[r].render);
            E.rowoff = r;
            snprintf(E.status_msg, STATUS_MSG_LEN, "Found: %s", query);
            E.status_time = time(NULL);
            return;
        }
    }
    snprintf(E.status_msg, STATUS_MSG_LEN, "Not found: %s", query);
    E.status_time = time(NULL);
}

/* ── Cursor movement ─────────────────────────────────────────────────────── */
static void move_cursor(int key) {
    Row *row = (E.cy < E.num_rows) ? &E.rows[E.cy] : NULL;
    switch (key) {
        case KEY_LEFT:
            if (E.cx > 0) E.cx--;
            else if (E.cy > 0) { E.cy--; E.cx = E.rows[E.cy].len; }
            break;
        case KEY_RIGHT:
            if (row && E.cx < row->len) E.cx++;
            else if (row && E.cx == row->len && E.cy < E.num_rows-1) { E.cy++; E.cx=0; }
            break;
        case KEY_UP:   if (E.cy > 0) E.cy--; break;
        case KEY_DOWN: if (E.cy < E.num_rows) E.cy++; break;
    }
    row = (E.cy < E.num_rows) ? &E.rows[E.cy] : NULL;
    if (E.cx > (row ? row->len : 0)) E.cx = row ? row->len : 0;
}

/* ── Process key ─────────────────────────────────────────────────────────── */
static int quit_warned = 0;
static void process_keypress(void) {
    int c = read_key();
    switch (c) {
        case CTRL('q'):
            if (E.dirty && !quit_warned) {
                snprintf(E.status_msg, STATUS_MSG_LEN,
                    "WARNING: Unsaved changes! Press ^Q again to quit.");
                E.status_time = time(NULL);
                quit_warned = 1;
                return;
            }
            write(STDOUT_FILENO, ESC_CLEAR ESC_HOME, 7);
            exit(0);
        case CTRL('s'): save_file(); break;
        case CTRL('f'): find(); break;
        case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
            move_cursor(c); quit_warned = 0; break;
        case KEY_PGUP:
            E.cy = E.rowoff;
            for (int i = 0; i < E.screen_rows; i++) move_cursor(KEY_UP);
            break;
        case KEY_PGDN:
            E.cy = E.rowoff + E.screen_rows - 1;
            if (E.cy > E.num_rows) E.cy = E.num_rows;
            for (int i = 0; i < E.screen_rows; i++) move_cursor(KEY_DOWN);
            break;
        case KEY_HOME: E.cx = 0; break;
        case KEY_END:
            if (E.cy < E.num_rows) E.cx = E.rows[E.cy].len;
            break;
        case KEY_BACKSPACE: delete_char(); break;
        case KEY_DEL:
            move_cursor(KEY_RIGHT);
            delete_char();
            break;
        case '\r': insert_newline(); break;
        case CTRL('l'): case '\x1b': break;
        default:
            if (!iscntrl(c)) insert_char(c);
            quit_warned = 0;
    }
    /* Update render x */
    if (E.cy < E.num_rows) {
        Row *row = &E.rows[E.cy];
        int rx = 0;
        for (int i = 0; i < E.cx && i < row->len; i++) {
            if (row->chars[i] == '\t') rx += TABSTOP - (rx % TABSTOP);
            else rx++;
        }
        E.rx = rx;
    } else {
        E.rx = E.cx;
    }
}

/* ── Init & main ─────────────────────────────────────────────────────────── */
static void init_editor(void) {
    memset(&E, 0, sizeof(E));
    if (get_window_size(&E.screen_rows, &E.screen_cols) == -1) die("get_window_size");
    E.screen_rows -= 2;  /* status bar + message bar */
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <filename>\n", argv[0]);
        return 1;
    }
    enable_raw_mode();
    init_editor();
    open_file(argv[1]);
    snprintf(E.status_msg, STATUS_MSG_LEN, "Help: ^S Save  ^F Find  ^Q Quit");
    E.status_time = time(NULL);
    while (1) {
        refresh_screen();
        process_keypress();
    }
    return 0;
}
