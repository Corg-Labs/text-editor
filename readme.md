# text-editor in C

A **kilo-style terminal text editor** implemented in a single C file. It puts the terminal into **raw mode**, handles arrow keys and escape sequences, supports multi-row editing with scrolling, and includes **C syntax highlighting** (keywords, strings, comments, numbers). Files are opened from the command line, saved with Ctrl-S, and searched with Ctrl-F.

Part of the [Corg-Labs](https://github.com/Corg-Labs) collection of single-file C programs.

---

# Features
- `termios` raw mode: disables echo, canonical mode, and signal key processing
- Line number gutter, horizontal and vertical scrolling
- C syntax highlighting: keywords (bold blue), strings (green), comments (grey), numbers (cyan)
- Ctrl-S save, Ctrl-F incremental search, Ctrl-Q quit
- Unsaved-changes guard: Ctrl-Q requires a second press when `dirty > 0`
- Entire screen written in one `write()` call per frame to prevent flicker

---

# Tutorial

## 1. Enabling Terminal Raw Mode

```c
struct termios raw = E.orig_termios;
raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
raw.c_oflag &= ~(OPOST);
raw.c_cflag |=  (CS8);
raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
raw.c_cc[VMIN]  = 0;
raw.c_cc[VTIME] = 1;
tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
```

`atexit(disable_raw_mode)` restores the terminal even on abnormal exit.

## 2. Decoding Escape Sequences

Arrow keys arrive as multi-byte escape sequences starting with `\x1b[`:

```c
if (seq[0] == '[') {
    switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
    }
}
```

## 3. The Row and Render Buffer

Each line is stored as a `Row` with a raw `chars` buffer and a tab-expanded `render` buffer:

```c
typedef struct {
    int   len;
    char *chars;   /* raw content */
    int   rlen;
    char *render;  /* tab-expanded for display */
} Row;
```

Edits always call `row_update_render` to keep `render` in sync.

## 4. C Syntax Highlighting

`append_highlighted()` scans the render buffer injecting ANSI colour codes, tracking state for strings and line comments:

```c
if (!in_string && render[i]=='/' && render[i+1]=='/') {
    in_comment = 1;
    append_escape(COL_COMMENT);
}
if (!in_string && (c == '"' || c == '\'')) {
    in_string = 1;
    append_escape(COL_STRING);
}
```

Keywords are matched with `strncmp` followed by a word-boundary check.

## 5. Single-Write Screen Refresh

Every frame builds the entire screen content into a heap buffer, then writes it all at once to prevent flickering:

```c
BUF_APPEND(&buf, &blen, ESC_HIDECURSOR ESC_HOME, 9);
/* ... all rows, status bar, message bar ... */
write(STDOUT_FILENO, buf, blen);
free(buf);
```

---

# Build

```
gcc editor.c -o editor
```

# Run

```
./editor myfile.c
./editor newfile.txt
```

# Controls

| Key        | Action                        |
|------------|-------------------------------|
| Arrow keys | Move cursor                   |
| Ctrl-S     | Save file                     |
| Ctrl-F     | Search                        |
| Ctrl-Q     | Quit (twice if unsaved)       |

---

# Concepts Practiced
- `termios` raw mode and terminal restore
- ANSI escape sequence decoding for special keys
- Dynamic row model for text editing
- Syntax highlighting with multi-state token scanning
- Single-write double-buffered screen rendering

# Dependencies
Standard C library + POSIX (`termios.h`, `sys/ioctl.h`). No external libraries.
