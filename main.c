/* INCLUDES */

#include <ctype.h>
#include <sys\types.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>

/* DEFINES */

#define KAYRAK_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN
};

/* DATA */

typedef struct erow {
    int size;
    char *chars;
}erow;

struct editorConfig {
    int cx, cy; // x and y (column and row) of teh cursor
    int rowoff;
    int coloff;
    int screenrows;
    int screencolumns;
    int numrows;
    erow *row;
    struct termios orig_termios;
};

struct editorConfig E;

/* TERMINAL */

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); //Clears the screen
    write(STDOUT_FILENO, "\x1b[H", 3);  //Poisitons the cursor to the top left

    perror(s);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, & E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, & E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    
    struct termios raw = E.orig_termios;

    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols) {  
    char buf[32];
    unsigned int i = 0;
    
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        
        if (buf[i] == 'R') break;
        i++;
    }
    
    buf[i] = '\0';  
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    
    return 0;
}

int getTermianlSize(int *rows, int *cols) {
  struct winsize ws;
  
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
        return -1;
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/* row operations */

void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    
    int at = E.numrows;
    E.row[at].size = len;
    
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    
    E.row[at].chars[len] = '\0';
    E.numrows++;
}

/*  FILE I/O */

void editorOpen(char *filename){
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    linelen = getline(&line, &linecap, fp);
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorAppendRow(line, linelen);
    }
    free(line);
    fclose(fp);

}

/* APPEND BUUFFER */

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abufAppend(struct abuf *abuf, const char *s, int len) {
    char *new = realloc(abuf->b, abuf->len + len);
    
    if (new == NULL) return;
    
    memcpy(&new[abuf->len], s, len);
    abuf->b = new;
    abuf->len += len;
}

void abufFree(struct abuf *abuf) {
  free(abuf->b);
}

/* OUTPUT */

void editorScroll() {
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencolumns) {
        E.coloff = E.cx - E.screencolumns + 1;
    }
}

void editorDrawRows(struct abuf *abuf){
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows){
            if (y == E.screenrows / 3 && E.numrows == 0) {
                char welcome[80];
                
                int welcomelen = snprintf(welcome, sizeof(welcome),
                "KAYRAK editor -- version %s", KAYRAK_VERSION);
                if (welcomelen > E.screencolumns) welcomelen = E.screencolumns;

                int centerPadding = (E.screencolumns - welcomelen) / 2;
                if(centerPadding){
                    abufAppend(abuf, ">", 1);
                    centerPadding--;
                }
            
                while (centerPadding--) abufAppend(abuf, " ", 1);
                abufAppend(abuf, welcome, welcomelen);
            } else {
                abufAppend(abuf, ">", 1);
            }   
        }else{
            int len = E.row[filerow].size - E.coloff;
            if(len < 0) len = 0;
            if (len > E.screencolumns) len = E.screencolumns;
            abufAppend(abuf, &E.row[filerow].chars[E.coloff], len);
        }
        
        
        abufAppend(abuf, "\x1b[K", 3);
        if(y < E.screenrows - 1) abufAppend(abuf, "\r\n", 2);
    }
}

void editorRefreshScreen() {  
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abufAppend(&ab, "\x1b[?25l", 6);  //hide cursor 
    //abAppend(&ab, "\x1b[2J", 4);    //erase everything on screen
    abufAppend(&ab, "\x1b[H", 3);
    
    editorDrawRows(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.cx - E.coloff + 1);
    abufAppend(&ab, buf, strlen(buf));

    abufAppend(&ab, "\x1b[?25h", 6);  //show cursor
    
    write(STDOUT_FILENO, ab.b, ab.len);
    
    abufFree(&ab);
}

/* INPUT */

void editorMoveCursor(int key){
    switch (key){
    case ARROW_UP:
        if (E.cy != 0)  E.cy--;
        break;
    case ARROW_LEFT:
        if(E.cx != 0)    E.cx--;
        break;
    case ARROW_DOWN:
        if(E.cy < E.numrows)    E.cy++;
        break;
    case ARROW_RIGHT:
        E.cx++;
        break;
    }
}

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
    }

    /*
    if (iscntrl(c)) {
        printf("%d\r\n", c);
    } else {
        printf("%d ('%c')\r\n", c, c);
    }
    */
}

/* INIT */

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;

    if (getTermianlSize(&E.screenrows, &E.screencolumns) == -1) die("getTerminalSize");   
}

int main(int argc, char *argv[  ]) {
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }
    
    while (1) {
        editorRefreshScreen();

        editorProcessKeypress();
    }
    
    return 0;
}