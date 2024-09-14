/* INCLUDES */

#include <ctype.h>
#include <stdarg.h>
#include <sys\types.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>

/* DEFINES */

#define KAYRAK_VERSION "v1.1"

#define CONFIRM_QUIT_TIMES (2)

/*
#define TAB_STOP (4)
#define LINE_NUMBER_MARGIN (5)

#define KeywordColor (128)
#define VariableColor (33)
#define CommentColor (84)
#define MultilineCommentColor (84)
#define StringColor (172)
#define NumberColor (148)
#define MatchColor (21)
#define DefaultColor (250)
*/

#define CTRL_KEY(k) ((k) & 0x1f)

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

enum editorHighlight{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/* DATA */

struct editorSyntax {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct erow {
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;  // highlighting
    int hl_open_comment;
}erow;

struct editorConfig {
    int cx, cy; // x and y (column and row) of teh cursor
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencolumns;
    int numrows;
    char *filename;
    erow *row;
    int dirty;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editorSyntax *syntax;
    struct termios orig_termios;
};

struct editorConfig E;

struct editorHLConfig{
    int LineNumberMargin;
    int TabStop;
    int ConfirmQuitTimes;
    int KeywordColor;
    int VariableColor;
    int CommentColor;
    int MultilineCommentColor;
    int StringColor;
    int NumberColor;
    int MatchColor;
    int DefaultColor; 
};

struct editorHLConfig HL_config;

/* FILETYPES */

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,    
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/* PROTOTYPES */

void editorSetStatusMessage(const char *fmt, ...);

void editorRefreshScreen();

char *editorPrompt(char *prompt, void (*callback)(char *, int));

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

/* SYNTAX HIGLIGHTING */

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if(E.syntax == NULL)    return;

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    
    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment   );
    

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }
        
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;

            if (c == '\\' && i + 1 < row->rsize) {
                row->hl[i + 1] = HL_STRING;
                i += 2;
                continue;
            }

                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||(c == '.' && prev_hl == HL_NUMBER)) {            
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                
                if (kw2) klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_KEYWORD1: return HL_config.KeywordColor;
        case HL_KEYWORD2: return HL_config.VariableColor;
        case HL_COMMENT: return HL_config.CommentColor;
        case HL_MLCOMMENT: return HL_config.MultilineCommentColor;
        case HL_STRING: return HL_config.StringColor;
        case HL_NUMBER: return HL_config.NumberColor;
        case HL_MATCH: return HL_config.MatchColor;
        default: return HL_config.DefaultColor;
    }
}

void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    
    if (E.filename == NULL) return;
    
    char *ext = strrchr(E.filename, '.');
    
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                //re highlighting
                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/* row operations */

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    int j;
        for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += 7 - (rx % 8);
            rx++;
        }
    return rx;
}

int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (HL_config.TabStop - 1) - (cur_rx % HL_config.TabStop);
        
        cur_rx++;
        
        if (cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row) {
    int tabs;
    int j;
    for (j = 0; j < row->size; j++)
        if(row->chars[j] == '\t')  tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs*(HL_config.TabStop-1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while(idx % HL_config.TabStop != 0) row->render[idx++] = ' ';
        }else{
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx = at;
    
    E.row[at].size = len;
    
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;

    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    at -= HL_config.LineNumberMargin;
    if (at < 0 || at > row->size) at = row->size;
    
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    
    row->size++;
    row->chars[at] = c;
    
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    at -= HL_config.LineNumberMargin;
    if (at < 0 || at >= row->size) return;
    
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/* EDITOR OPERATIONS */

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == HL_config.LineNumberMargin) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx - HL_config.LineNumberMargin], row->size - E.cx + HL_config.LineNumberMargin);
        row = &E.row[E.cy];
        row->size = E.cx - HL_config.LineNumberMargin;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = HL_config.LineNumberMargin;
}

void editorInsertTab(){
    int space_num;
    if ((E.cx - HL_config.LineNumberMargin) % HL_config.TabStop == 0){
        space_num = HL_config.TabStop ;
    }else{
        space_num = HL_config.TabStop  - ((E.cx - HL_config.LineNumberMargin) % HL_config.TabStop );
    }

    int i;
    for (i = 0; i < space_num; i++){
        editorInsertChar(' ');
    }
    
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == HL_config.LineNumberMargin && E.cy == 0) return;
    
    erow *row = &E.row[E.cy];
    if (E.cx > HL_config.LineNumberMargin) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }else if(E.cx == HL_config.LineNumberMargin){
        E.cx = E.row[E.cy - 1].size + HL_config.LineNumberMargin;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
  }
}

void editorMoveCursorCtrl(int i){
}

void editorMoveCursorShift(int i){
}

/*  FILE I/O */

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    int j;
    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    
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

void editorOpen(char *filename){
    free(E.filename);
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void editorSave() {
    if (E.filename == NULL){
            E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
            
            if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        
        editorSelectSyntaxHighlight();
    }
    
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);  
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

void editorSetConfig(){
    FILE *fp = fopen("config.txt", "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    size_t linelen;
    size_t linesep;
    size_t linenum = 0;// matchcolor=123
    
    while ((linelen = getline(&line, &linecap, fp)) != -1){
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        int i;
        for (i = 0; i < linelen; i++){
            if(line[i] == '='){ 
                linesep = i;
                break;  
            }
        }
        
        linenum++;
        char *sep = strchr(line, '=');
        if (sep == NULL) continue;

        *sep = '\0';
        //char *key = line;
        char *strvalue = sep + 1;

        int value = atoi(strvalue);

        switch (linenum){
            case 1:
                HL_config.LineNumberMargin = value;
                break;
            case 2:
                HL_config.TabStop = value;
                break;
            case 3:
                HL_config.KeywordColor = value;
                break;
            case 4:
                HL_config.VariableColor = value;
                break;
            case 5:
                HL_config.CommentColor = value;
                break;
            case 6:
                HL_config.MultilineCommentColor = value;
                break;
            case 7:
                HL_config.StringColor = value;
                break;
            case 8:
                HL_config.NumberColor = value;
                break;
            case 9:
                HL_config.MatchColor = value;
                break;
            case 10:
                HL_config.DefaultColor = value;
                break;
        }
    }
    
    free(line);
    fclose(fp);
}

/* SEARCH */

void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    
    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    
    if (last_match == -1) direction = 1;
    
    int current = last_match;
    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;
        
        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render) + HL_config.LineNumberMargin;
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind() {  
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    
    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
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

void editorScroll() {
    E.rx = E.cx;
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
    if (E.rx >= E.coloff + E.screencolumns) {
        E.coloff = E.rx - E.screencolumns + 1;
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
                    abufAppend(abuf, "\x1b[38;5;242m", 11);
                    abufAppend(abuf, ">", 1);
                    abufAppend(abuf, "\x1b[39m", 5);
                    centerPadding--;
                }
            
                while (centerPadding--) abufAppend(abuf, " ", 1);
                abufAppend(abuf, welcome, welcomelen);
            } else {
                abufAppend(abuf, "\x1b[38;5;242m", 11);
                abufAppend(abuf, ">", 1);
                abufAppend(abuf, "\x1b[39m", 5);

            }   
        }else{
            int len = E.row[filerow].rsize - E.coloff;
            if(len < 0) len = 0;
            if (len > E.screencolumns) len = E.screencolumns;   
            char *c = &E.row[filerow].render[E.coloff];

            char linenum[50];
            sprintf(linenum, "%d", filerow + 1);
            int i;
            for(i = strlen(linenum); i < HL_config.LineNumberMargin; i++)    linenum[i] = ' ';
            linenum[i] = '\0';
            abufAppend(abuf, "\x1b[38;5;242m", 11);
            abufAppend(abuf, linenum, HL_config.LineNumberMargin);
            abufAppend(abuf, "\x1b[39m", 5);

            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {          
                    if (current_color != -1) {
                        abufAppend(abuf, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abufAppend(abuf, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if(current_color != color){
                        current_color = color;
                        char buf[16];
                        int clen;
                        if(hl[j] == HL_MATCH){
                            clen = snprintf(buf, sizeof(buf), "\x1b[48;5;%dm", color);
                        } else{
                            clen = snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", color);
                        }
                        abufAppend(abuf, buf, clen);
                    }
                    abufAppend(abuf, &c[j], 1);
                }
            }
            abufAppend(abuf, "\x1b[39m", 5);
            abufAppend(abuf, "\x1b[48;5;m", 8);
        }
        
        
        abufAppend(abuf, "\x1b[K", 3);
        abufAppend(abuf, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *abuf) {  
    abufAppend(abuf, "\x1b[7m", 4);
    
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[Unnamed]", E.numrows,
    E.dirty ? "[Modified]" : "");

    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d:%d",
    E.syntax ? E.syntax->filetype : "no ft", E.cx - HL_config.LineNumberMargin, E.cy + 1);
    
    if (len > E.screencolumns) len = E.screencolumns;
    
    abufAppend(abuf, status, len);
    
    while (len < E.screencolumns) {
        if (E.screencolumns - len == rlen) {
            abufAppend(abuf, rstatus, rlen);
            break;
        } else {
            abufAppend(abuf, " ", 1);
            len++;
        }
    }
    
    abufAppend(abuf, "\x1b[m", 3);
    abufAppend(abuf, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *abuf) {
    abufAppend(abuf, "\x1b[K", 3);
    
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencolumns) msglen = E.screencolumns;
    
    if (msglen && time(NULL) - E.statusmsg_time < 10)
        abufAppend(abuf, E.statusmsg, msglen);
}

void editorRefreshScreen() {  
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abufAppend(&ab, "\x1b[?25l", 6);  //hide cursor 
    //abufAppend(&ab, "\x1b[2J", 4);    //erase everything on screen
    abufAppend(&ab, "\x1b[H", 3);
    
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1, E.rx - E.coloff + 1);
    abufAppend(&ab, buf, strlen(buf));

    abufAppend(&ab, "\x1b[?25h", 6);  //show cursor
    
    write(STDOUT_FILENO, ab.b, ab.len);
    
    abufFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/* INPUT */

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    
    buf[0] = '\0';
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();    
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
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
        
        if (callback) callback(buf, c);
    }
}

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key){
    case ARROW_UP:
        if (E.cy != 0) E.cy--;
        break;
    case ARROW_LEFT:
        if(E.cx > HL_config.LineNumberMargin) E.cx--;
        else if(E.cy > 0){
            E.cy--;
            E.cx = E.row[E.cy].size + HL_config.LineNumberMargin;
        }
        break;
    case ARROW_DOWN:
        if(E.cy < E.numrows) E.cy++;
        break;
    case ARROW_RIGHT:
        if(row && E.cx < row->size + HL_config.LineNumberMargin) E.cx++;
        else if(row && E.cx == row->size + HL_config.LineNumberMargin){
            E.cy++;
            E.cx = HL_config.LineNumberMargin;
        }
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = (row ? row->size : 0) + HL_config.LineNumberMargin;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
    
}

void editorProcessKeypress() {
    static int quit_times = CONFIRM_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case '\t':
            editorInsertTab();
            break;
        case CTRL_KEY('q'):      
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
            break;
        case CTRL_KEY('f'):
            editorFind();
            break;
        case CTRL_KEY('r'):
            E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();            
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
                }
                int times = E.screenrows;
                while (times--)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            char seq[5];
            int i = 0;

            if (read(STDIN_FILENO, &seq[i], 1) == 0) break;
            i++;

            // Read characters until we have the full escape sequence
            while (i < sizeof(seq) - 1) {
                if (read(STDIN_FILENO, &seq[i], 1) == 0) break;
                // Check if we have reached the end of the sequence
                i++;
                if (seq[i] >= 'A' && seq[i] <= 'D') {
                    break;
                }
            }
            seq[i] = '\0';  

            if (seq[0] == ';') {
                char mod = seq[1];
                char dir = seq[2];

                // Shift + Arrow
                if (mod == '2') {
                    switch (dir) {
                        case 'A': editorMoveCursorShift(ARROW_UP); break;
                        case 'B': editorMoveCursorShift(ARROW_DOWN); break;
                        case 'C': editorMoveCursorShift(ARROW_RIGHT); break;
                        case 'D': editorMoveCursorShift(ARROW_LEFT); break;
                    }
                }
                // Ctrl + Arrow
                else if (mod == '5') {
                    switch (dir) {
                        case 'A': editorMoveCursorCtrl(ARROW_UP); break;
                        case 'B': editorMoveCursorCtrl(ARROW_DOWN); break;
                        case 'C': editorMoveCursorCtrl(ARROW_RIGHT); break;
                        case 'D': editorMoveCursorCtrl(ARROW_LEFT); break;
                    }
                }
            }
            break;
        default:
            editorInsertChar(c);
            break;
  }

  quit_times = CONFIRM_QUIT_TIMES;
}

/* INIT */

void initEditor(){
    editorSetConfig();

    E.cx = HL_config.LineNumberMargin;
    E.cy = 0;
    E.rx = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    
    if (getTermianlSize(&E.screenrows, &E.screencolumns) == -1) die("getTerminalSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if(argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctrl-R = rename");
    
    while (1) {
        editorRefreshScreen();

        editorProcessKeypress();
    }
    
    return 0;
}
