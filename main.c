#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <string.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#define BUFFER_DEFAULT_SIZE 512
#define REALLOC_INCREMENT 64
#define GAP_SIZE 4 // TODO: Remove because only used at start

//
// Split Buffer
//
struct buffer {
    char* data;
    int headLen;
    int tailLen;
    int totalLen;
};

struct editorState {
    int height;
    int width;
    struct buffer buffer; 
    int viewTopIdx; // Points to the start of the first line visible in the viewport
    int viewBottomIdx; // Points to the end of the last line visible in the viewport
    int cursorX;
    int cursorY;
};

struct editorState state;

int tailStart() {
    return state.buffer.totalLen - state.buffer.tailLen;
}

int gapSize() {
     return state.buffer.totalLen - state.buffer.headLen - state.buffer.tailLen; 
}


void setBottomIdx() {
    //state.viewBottomIdx = 600;
    int newlines = 0;
    for (int i=state.viewTopIdx; i<state.buffer.totalLen; i++) {
        if (state.buffer.data[i] == '\n') {
            newlines++;
            if (newlines == state.height - 4) {
                state.viewBottomIdx = i;
                return;
            }
        }
    }
    state.viewBottomIdx = state.buffer.totalLen;
}
// TODO: anchor view top
void moveCursor(int idx) {
    // TODO: error handling (idx > length)
    // Move the gap buffer to the cursors current position
    if (idx > state.buffer.headLen) {
        if (idx < tailStart()) {
            return; // no need to move inside of the gap
        }
        // Copy start of tail to end of head
        memmove(
            state.buffer.data + state.buffer.headLen,
            state.buffer.data + tailStart(),
            idx - tailStart()
        );
        state.buffer.headLen += idx - tailStart();
        state.buffer.tailLen -= idx - tailStart();
    } else if (idx < state.buffer.headLen) {
        // Copy end of head to start of tail
        memmove(
            state.buffer.data + tailStart() - (state.buffer.headLen - idx),
            state.buffer.data + idx,
            state.buffer.headLen - idx
        );
        state.buffer.tailLen += state.buffer.headLen - idx;
        state.buffer.headLen -= state.buffer.headLen - idx; 
    }
    memset(state.buffer.data + state.buffer.headLen, 0, gapSize());

    // Detect if the screen should move up or down
    setBottomIdx();
    // TODO: clean below
    if (state.buffer.headLen > state.viewBottomIdx) {
        for (int i=state.viewTopIdx; i<state.buffer.totalLen; i++) {
            if (state.buffer.data[i] == '\n') {
                state.viewTopIdx = i + 1;
                setBottomIdx();
                if (state.buffer.headLen <= state.viewBottomIdx) break;
            }
        }
    }
    if (state.buffer.headLen < state.viewTopIdx) {
        for (int i=state.viewBottomIdx; i>0; i--) {
            if (state.buffer.data[i] == '\n') {
                state.viewTopIdx = i + 1;
                setBottomIdx();
                if (state.buffer.headLen >=  state.viewTopIdx) break;
            }
        }
    }
}

void insertChar(char c) {
    if (gapSize() == 0) {
        state.buffer.totalLen += REALLOC_INCREMENT;
        state.buffer.data = realloc(state.buffer.data, state.buffer.totalLen);
        // Move tail end to create gap
        memmove(state.buffer.data + state.buffer.headLen + REALLOC_INCREMENT,
                state.buffer.data + state.buffer.headLen,
                state.buffer.tailLen
        );
    }
    state.buffer.data[state.buffer.headLen] = c;
    state.buffer.headLen++;
}

//
// Terminal Initialization 
//

struct termios oldTermios;

void resetTermMode() {
   tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldTermios); 
}

// Set up raw mode and enable byte by byte reads
void setTermMode() {
    tcgetattr(STDIN_FILENO, &oldTermios);
    atexit(resetTermMode);

    struct termios newTermios = oldTermios;

    // TODO: explain all flags
    newTermios.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    newTermios.c_iflag &= ~(IXON); // Unbind ctrl-q, ctrl-s, ctrl-m

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &newTermios);
}

// TODO: detect window resizing
void updateWindowSize() {
    struct winsize ws;
    
    // TODO: error handling
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    state.width = ws.ws_col;
    state.height = ws.ws_row;
}

//
// VT Code Helpers
//

void vtClearScreen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
}

void vtMoveCursor(int x, int y) {
    printf("\x1b[%d;%dH", y, x);
}

//
// Rendering
//

void updateCursorPos() {
    int newlines = 0; // TODO: maybe some better way to find newlines because we do it so often?
    int lastNewline = state.viewTopIdx; // The start of the screen is guaranteed to be a newline or start of file
    for (int i=state.viewTopIdx; i<state.buffer.headLen; i++) {
        if (state.buffer.data[i] == '\n') {
            newlines++;
            lastNewline = i+1;
        }
    }
    state.cursorY = newlines + 1;
    state.cursorX = state.buffer.headLen - lastNewline + 1;
}

void render() {
    vtClearScreen();
    vtMoveCursor(0,0);
    setBottomIdx();
    fwrite(state.buffer.data + state.viewTopIdx, 1, state.buffer.headLen - state.viewTopIdx, stdout);
    //fflush(stdout);
    //printf("!");
    int size = fwrite(
            state.buffer.data + state.buffer.totalLen - state.buffer.tailLen,
            1,
            state.viewBottomIdx - state.buffer.headLen - gapSize(),
            stdout
    );
    
    updateCursorPos();
    vtMoveCursor(state.cursorX, state.cursorY);
    fflush(stdout);
}

//
// Input Handling
//

char readByte() {
    char c;
    // TODO: error handling
    read(STDIN_FILENO, &c, 1);
    return c;
}

// All keybinds must contain the ctrl key, and there are 31 possible keys you can detect with ctrl
void (*keybinds[31]) ();

void parseInput() {
    char c = readByte();
    // TODO: prevent insertion of special characters
    // Detect keybinds
    if (c < 32 && keybinds[c] != 0)
        keybinds[c]();
    else if (c == '\x1b') {
        // TODO: will break on other escape codes
        readByte(); // Skip bracket
        switch(readByte()) {
        case 'C': // Right
            moveCursor(state.buffer.headLen + gapSize()+1);
            break;
        case 'D': // Left
            moveCursor(state.buffer.headLen - 1);
            break;
        case 'B': // Down 
            // TODO: implement normal up and down arrow behavior
            for (int i=tailStart(); i<state.buffer.totalLen; i++) {
                if (state.buffer.data[i] == '\n') {
                    moveCursor(i+1);
                    break;
                }
            }
            break;
        case 'A': // Up
            for (int i=state.buffer.headLen; i>0; i--) {
                if (state.buffer.data[i] == '\n') {
                    moveCursor(i-1);
                    break;
                }
            }
            break;
        }
    }
    else if (c > 32) {
        insertChar(c);
    }
    render();
}

void quit() {
    exit(0);
}

void debug() {
    vtClearScreen();
    vtMoveCursor(0,0);
    printf(
        "headLen: %d tailStart: %d, tailLen: %d, totalLen %d, viewTopIdx %d, viewBottomIdx %d, misc: %d\n",
        state.buffer.headLen,
        tailStart(),
        state.buffer.tailLen,
        state.buffer.totalLen,
        state.viewTopIdx,
        state.viewBottomIdx,
        state.viewBottomIdx - state.buffer.headLen - gapSize()
    );
    for (int i=state.buffer.headLen-1; i<tailStart()+30; i++) {
        if (i<tailStart() && i >= state.buffer.headLen) printf("gap ");
        printf("%d%c\n", i, state.buffer.data[i]);
    }
    readByte();
    render();
}
    

#define CTRL_KEY(k) ((k) - 0x60)
void setUpKeybinds() {
    keybinds[CTRL_KEY('q')] = quit;
    keybinds[CTRL_KEY('w')] = debug;
}

//
// Main
//

int main() {
    setTermMode();
    updateWindowSize();
    setUpKeybinds();
    
    vtClearScreen();
    vtMoveCursor(0,0);    
    
    // read file into buffer
    FILE *fptr;
    fptr = fopen("main.c","rb");
    fseek(fptr, 0L, SEEK_END);
    long bufsize = ftell(fptr);
    fseek(fptr, 0L, SEEK_SET);
    // TODO: might need +1 for null at end
    state.buffer.data = malloc(bufsize + GAP_SIZE);
    state.buffer.tailLen = bufsize;
    state.buffer.totalLen = bufsize + GAP_SIZE;
    size_t nread = fread(state.buffer.data + GAP_SIZE, 1, bufsize, fptr);
    
    setBottomIdx(); 
    render();
    for (;;) {
        parseInput();
    }    

    return 0;
}
