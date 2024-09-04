/* INCLUDES */

#include <ctype.h>
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

/* DATA */

struct editorConfig {
    int screenrows;
    int screencolumns;
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

char editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
  
    return c;
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

void editorDrawRows(struct abuf *abuf){
    int y;
    for (y = 0; y < E.screenrows; y++) {
        if (y == E.screenrows / 2) {
        char welcome[80];
        
        int welcomelen = snprintf(welcome, sizeof(welcome),
        ">\t\tKAYRAK editor -- version %s", KAYRAK_VERSION);
        if (welcomelen > E.screencolumns) welcomelen = E.screencolumns;
        
        abufAppend(abuf, welcome, welcomelen);
    } else {
        abufAppend(abuf, ">", 1);
    }
        abufAppend(abuf, "\x1b[K", 3);
        if(y < E.screenrows - 1) abufAppend(abuf, "\r\n", 2);
    }
}

void editorRefreshScreen() {  
    struct abuf ab = ABUF_INIT;

    abufAppend(&ab, "\x1b[?25l", 6);  //hide cursor 
    //abAppend(&ab, "\x1b[2J", 4);    //erase everything on screen
    abufAppend(&ab, "\x1b[H", 3);
    
    editorDrawRows(&ab);
    
    abufAppend(&ab, "\x1b[H", 3);
    abufAppend(&ab, "\x1b[?25h", 6);  //show cursor
    
    write(STDOUT_FILENO, ab.b, ab.len);
    
    abufFree(&ab);
}

/* INPUT */

void editorProcessKeypress() {
    char c = editorReadKey();

    switch (c) {
        case CTRL_KEY('q'):
            editorRefreshScreen();
            exit(0);
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

int main() {
    enableRawMode();

    if (getTermianlSize(&E.screenrows, &E.screencolumns) == -1) die("getTerminalSize");   
    

    while (1) {
        write(STDOUT_FILENO, "\x1b[2J", 4); //Clears the screen
        write(STDOUT_FILENO, "\x1b[H", 3);  //Poisitons the cursor to the top left

        editorProcessKeypress();
    }
    
    return 0;
}