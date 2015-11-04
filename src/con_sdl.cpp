/*    con_sdl.cpp
 *
 *    Copyright (c) 2015, Ryan C. Gordon
 *
 *    You may distribute under the terms of either the GNU General Public
 *    License or the Artistic License, as specified in the README file.
 */

#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "SDL.h"

#include "console.h"
#include "gui.h"

#include "con_i18n.h"
#include "s_files.h"

#ifdef CAST_FD_SET_INT
#define FD_SET_CAST() (int *)
#else
#define FD_SET_CAST()
#endif

#define MIN_SCRWIDTH 20
#define MIN_SCRHEIGHT 6

#define MAX_PIPES 40
//#define PIPE_BUFLEN 4096

#include "sdl_font.h"

static inline void dbg(const char *fmt, ...)
{
#if 1
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#endif
} // dbg

#if 1
#define FIXME(what)
#else
#define FIXME(what) { \
    static int seen = 0; \
    if (!seen) { \
        seen = 1; \
        dbg("FIXME: %s\n", what); \
    } \
}
#endif

typedef struct {
    int used;
    int id;
    int fd;
    int pid;
    int stopped;
    EModel *notify;
} GPipe;

static GPipe Pipes[MAX_PIPES] = {
    { 0 },
};

static int initX = 0, initY = 0;
static unsigned int ScreenCols = 80;
static unsigned int ScreenRows = 40;
static unsigned int CursorX = 0;
static unsigned int CursorY = 0;
static int CursorVisible = 1;
static unsigned char *ScreenBuffer = NULL;
static int Refresh = 0;

static int FontCX, FontCY;
static int rc;
static char winTitle[256] = "FTE";
static char winSTitle[256] = "FTE";

static SDL_Window *win = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *font = NULL;
static SDL_Texture *backbuffer = NULL;
static bool bWindowDirty = false;

static int AllocBuffer() {
    unsigned char *p;
    unsigned int i;

    ScreenBuffer = (unsigned char *)malloc(2 * ScreenCols * ScreenRows);
    if (ScreenBuffer == NULL) return -1;
    for (i = 0, p = ScreenBuffer; i < ScreenCols * ScreenRows; i++) {
        *p++ = 32;
        *p++ = 0x07;
    }
    return 0;
}

struct rgb {
    Uint8 r, g, b;
};
static const rgb dcolors[] =
{
    {   0,   0,   0 },  //     black
    {   0,   0, 160 },  // darkBlue
    {   0, 160,   0 },  // darkGreen
    {   0, 160, 160 },  // darkCyan
    { 160,   0,   0 },  // darkRed
    { 160,   0, 160 },  // darkMagenta
    { 160, 160,   0 },  // darkYellow
    { 204, 204, 204 },  // paleGray
    { 160, 160, 160 },  // darkGray
    {   0,   0, 255 },  //     blue
    {   0, 255,   0 },  //     green
    {   0, 255, 255 },  //     cyan
    { 255,   0,   0 },  //     red
    { 255,   0, 255 },  //     magenta
    { 255, 255,   0 },  //     yellow
    { 255, 255, 255 },  //     white
};

static SDL_Surface *LoadFontToSurface()
{
    FIXME("Load external TTF files, build a bitmap on the fly.");

    // use the default, built-in bitmap.
    SDL_RWops *rwops = SDL_RWFromConstMem(default_sdl_font, sizeof (default_sdl_font));
    if (!rwops)
        return NULL;

    SDL_Surface *surface = SDL_LoadBMP_RW(rwops, 1);
    SDL_SetColorKey(surface, 1, SDL_MapRGB(surface->format, 0, 0, 0));
    return surface;
}

static int SetupSDLWindow(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO) == -1) {
        char buf[512];
        SDL_snprintf(buf, sizeof (buf), "Could not initialize SDL! (%s)", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, NULL);
        DieError(1, "XFTE Fatal: %s", buf);
    }

    SDL_Surface *surface = LoadFontToSurface();
    if (!surface) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", "Could not load font!", NULL);
        SDL_Quit();
        DieError(1, "XFTE Fatal: could not load font!");
    }

    FontCX = surface->w / 256;
    FontCY = surface->h;

#if 1  // FIXME
    FIXME("support -geometry command line");
    initX = initY = SDL_WINDOWPOS_UNDEFINED;
#else
    // this is correct behavior
    if (initX < 0)
        initX = DisplayWidth(display, DefaultScreen(display)) + initX;
    if (initY < 0)
        initY = DisplayHeight(display, DefaultScreen(display)) + initY;
#endif
    const int winw = ScreenCols * FontCX;
    const int winh = ScreenRows * FontCY;
    win = SDL_CreateWindow(winTitle, initX, initY, winw, winh, SDL_WINDOW_RESIZABLE);
    if (!win)
    {
        char buf[512];
        SDL_snprintf(buf, sizeof (buf), "Could not create window! (%s)", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, NULL);
        SDL_FreeSurface(surface);
        SDL_Quit();
        DieError(1, "XFTE Fatal: %s", buf);
    }

    SDL_SetWindowMinimumSize(win, MIN_SCRWIDTH * FontCX, MIN_SCRHEIGHT * FontCY);

    renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_TARGETTEXTURE);
    if (!renderer)
    {
        char buf[512];
        SDL_snprintf(buf, sizeof (buf), "Could not create renderer! (%s)", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, win);
        SDL_DestroyWindow(win);
        SDL_FreeSurface(surface);
        SDL_Quit();
        DieError(1, "XFTE Fatal: %s", buf);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    font = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (!font)
    {
        char buf[512];
        SDL_snprintf(buf, sizeof (buf), "Could not create font texture! (%s)", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, win);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(win);
        SDL_Quit();
        DieError(1, "XFTE Fatal: %s", buf);
    }

    Uint32 fmt;  // presumably SDL picked the best format for the font texture, so use that.
    SDL_QueryTexture(font, &fmt, NULL, NULL, NULL);
    backbuffer = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_TARGET, winw, winh);
    if (!backbuffer)
    {
        char buf[512];
        SDL_snprintf(buf, sizeof (buf), "Could not create backbuffer! (%s)", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, win);
        SDL_DestroyTexture(font);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(win);
        SDL_Quit();
        DieError(1, "XFTE Fatal: %s", buf);
    }

    SDL_SetTextureBlendMode(font, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(renderer, backbuffer);
    SDL_RenderClear(renderer);

    return 0;
}

int ConInit(int XSize, int YSize) {
    if (XSize != -1)
        ScreenCols = XSize;
    if (YSize != -1)
        ScreenRows = YSize;
    if (AllocBuffer() == -1) return -1;
#ifndef NO_SIGNALS
    signal(SIGALRM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
#endif
    return 0;
}

int ConDone(void) {
    SDL_SetRenderTarget(renderer, NULL);
    SDL_DestroyTexture(backbuffer);
    backbuffer = NULL;
    SDL_DestroyTexture(font);
    font = NULL;
    SDL_DestroyRenderer(renderer);
    renderer = NULL;
    SDL_DestroyWindow(win);
    win = NULL;
    SDL_Quit();
    return 0;
}

int ConSuspend(void) {
    return 0;
}

int ConContinue(void) {
    return 0;
}

int ConClear(void) {
    TDrawBuffer B;
    MoveCh(B, ' ', 0x07, ScreenCols);
    return ConPutLine(0, 0, ScreenCols, ScreenRows, B);
}

int ConSetTitle(char *Title, char *STitle) {
    char buf[sizeof(winTitle)] = {0};
    JustFileName(Title, buf);
    if (buf[0] == '\0') // if there is no filename, try the directory name.
        JustLastDirectory(Title, buf);

    strncpy(winTitle, "FTE - ", sizeof(winTitle) - 1);
    if (buf[0] != 0) // if there is a file/dir name, stick it in here.
    {
        strncat(winTitle, buf, sizeof(winTitle) - 1 - strlen(winTitle));
        strncat(winTitle, " - ", sizeof(winTitle) - 1 - strlen(winTitle));
    }
    strncat(winTitle, Title, sizeof(winTitle) - 1 - strlen(winTitle));
    winTitle[sizeof(winTitle) - 1] = 0;
    strncpy(winSTitle, STitle, sizeof(winSTitle) - 1);
    winSTitle[sizeof(winSTitle) - 1] = 0;
    SDL_SetWindowTitle(win, winTitle);

    return 0;
}

int ConGetTitle(char *Title, int MaxLen, char *STitle, int SMaxLen) {
    strncpy(Title, winTitle, MaxLen);
    Title[MaxLen - 1] = 0;
    strncpy(STitle, winSTitle, SMaxLen);
    STitle[SMaxLen - 1] = 0;
    return 0;
}

#define InRange(x,a,y) (((x) <= (a)) && ((a) < (y)))
#define CursorXYPos(x,y) (ScreenBuffer + ((x) + ((y) * ScreenCols)) * 2)

static void DrawString(const unsigned char *temp, const unsigned int l,
                       unsigned int x, unsigned int y, const unsigned char attr)
{
    const rgb *fgcolor = &dcolors[attr % 16];
    const rgb *bgcolor = &dcolors[attr / 16];
    SDL_SetTextureColorMod(font, fgcolor->r, fgcolor->g, fgcolor->b);
    SDL_SetRenderDrawColor(renderer, bgcolor->r, bgcolor->g, bgcolor->b, 0xFF);

    SDL_Rect srcrect = { 0, 0, FontCX, FontCY };
    SDL_Rect dstrect = { (int) x, (int) y, FontCX, FontCY };
    for (unsigned int i = 0; i < l; i++) {
        const Uint8 ch = (Uint8) temp[i];
        srcrect.x = ch * FontCX;
        SDL_RenderFillRect(renderer, &dstrect);
        SDL_RenderCopy(renderer, font, &srcrect, &dstrect);
        dstrect.x += FontCX;
    }
    bWindowDirty = true;
}

void DrawCursor(int Show) {
    if (CursorVisible) {
        unsigned char *p = CursorXYPos(CursorX, CursorY), attr;
        attr = p[1];
        /*if (Show) attr = ((((attr << 4) & 0xF0)) | (attr >> 4)) ^ 0x77;*/
        if (Show)
            attr = (attr ^ 0x77);

        DrawString(p, 1, CursorX * FontCX, CursorY * FontCY, attr);
    }
}

int ConPutBox(int X, int Y, int W, int H, PCell Cell) {
    unsigned int i;
    unsigned char temp[1024], attr;
    unsigned char *p, *ps, *c, *ops;
    unsigned int len, x, l, ox, olen, skip;


    if (X >= (int) ScreenCols || Y >= (int) ScreenRows ||
        X + W > (int) ScreenCols || Y + H > (int) ScreenRows) {
        //fprintf(stderr, "%d %d  %d %d %d %d\n", ScreenCols, ScreenRows, X, Y, W, H);
        return -1;
    }
    //XClearArea(display, win, X, Y, W * FontCX, H * FontCY, False);

    //fprintf(stderr, "%d %d  %d %d %d %d\n", ScreenCols, ScreenRows, X, Y, W, H);
    for (i = 0; i < (unsigned int)H; i++) {
        len = W;
        p = CursorXYPos(X, Y + i);
        ps = (unsigned char *) Cell;
        x = X;
        while (len > 0) {
            if (!Refresh) {
                c = CursorXYPos(x, Y + i);
                skip = 0;
                ops = ps;
                ox = x;
                olen = len;
                while ((len > 0) && c[0] == ps[0] && c[1] == ps[1] )
                {
                    ps+=2;
                    c+=2;
                    x++;
                    len--;
                    skip++;
                }
                if (len <= 0) break;
                if (skip <= 4) {
                    ps = ops;
                    x = ox;
                    len = olen;
                }
            }
            p = ps;
            l = 1;
            temp[0] = *ps++; attr = *ps++;
            while ((l < len) && ((unsigned char) (ps[1]) == attr)) {
                temp[l++] = *ps++;
                ps++;
            }

            DrawString(temp, l, x * FontCX, (Y + i) * FontCY, attr);
            //temp[l] = 0; printf("%s\n", temp);
            len -= l;
            x += l;
        }

/*	if (x < ScreenCols - 1) {
	    printf("XX %d   %d   %d\n", X, x, W);
	    XFillRectangle(display, win, GCs[15 * 16 + 7],
			   x * FontCX, (Y + i) * FontCY,
			   (ScreenCols - x - 1) * FontCX, FontCY);
	}
*/

        p = CursorXYPos(X, Y + i);
        if ( ((void *) p) != ((void *) Cell) )
            memcpy(p, Cell, W * 2);
        if (i + Y == CursorY)
            DrawCursor(1);
        Cell += W;
    }
    return 0;
}

int ConGetBox(int X, int Y, int W, int H, PCell Cell) {
    int i;

    for (i = 0; i < H; i++) {
        memcpy(Cell, CursorXYPos(X, Y + i), 2 * W);
        Cell += W;
    }
    return 0;
}

int ConPutLine(int X, int Y, int W, int H, PCell Cell) {
    int i;
    for (i = 0; i < H; i++) {
        if (ConPutBox(X, Y + i, W, 1, Cell) != 0) return -1;
    }
    return 0;
}

int ConSetBox(int X, int Y, int W, int H, TCell Cell) {
    TDrawBuffer B;
    int i;

    for (i = 0; i < W; i++)
	B[i] = Cell;
    ConPutLine(X, Y, W, H, B);
    return 0;
}

static void UpdateWindow(int xx, int yy, int ww, int hh);

int ConScroll(int Way, int X, int Y, int W, int H, TAttr Fill, int Count) {
    TCell Cell;
    int l;

    MoveCh(&Cell, ' ', Fill, 1);
    DrawCursor(0);
    if (Way == csUp) {
        /*XCopyArea(display, win, win, GCs[0],
                  X * FontCX,
                  (Y + Count) * FontCY,
                  W * FontCX,
                  (H - Count) * FontCY,
                  X * FontCX,
                  Y * FontCY);*/
        for (l = 0; l < H - Count; l++)
            memcpy(CursorXYPos(X, Y + l), CursorXYPos(X, Y + l + Count), 2 * W);

        if (ConSetBox(X, Y + l, W, Count, Cell) == -1)
            return -1;
    } else if (Way == csDown) {
        /*XCopyArea(display, win, win, GCs[0],
                  X * FontCX,
                  Y * FontCY,
                  W * FontCX,
                  (H - Count) * FontCY,
                  X * FontCX,
                  (Y + Count) * FontCY);*/
        for (l = H - 1; l >= Count; l--)
            memcpy(CursorXYPos(X, Y + l), CursorXYPos(X, Y + l - Count), 2 * W);

        if (ConSetBox(X, Y, W, Count, Cell) == -1)
            return -1;
    }
    int w, h;
    SDL_GetWindowSize(win, &w, &h);
    UpdateWindow(0, 0, w, h);
    DrawCursor(1);
    return 0;
}

int ConSetSize(int X, int Y) {
    unsigned char *NewBuffer;
    unsigned char *p;
    int i;
    int MX, MY;

    p = NewBuffer = (unsigned char *) malloc(X * Y * 2);
    if (NewBuffer == NULL) return -1;
    for (i = 0; i < X * Y; i++) {
        *p++ = ' ';
        *p++ = 0x07;
    }
    MX = ScreenCols;
    if (X < MX)
	MX = X;
    MY = ScreenRows;
    if (Y < MY)
	MY = Y;
    p = NewBuffer;
    for (i = 0; i < MY; i++) {
        memcpy(p, CursorXYPos(0, i), MX * 2);
        p += X * 2;
    }
    free(ScreenBuffer);
    ScreenBuffer = NewBuffer;
    ScreenCols = X;
    ScreenRows = Y;
    //ConPutBox(0, 0, ScreenCols, ScreenRows, (PCell) ScreenBuffer);
    //if (Refresh == 0)
    //    XResizeWindow(display, win, ScreenCols * FontCX, ScreenRows * FontCY);
    return 0;
}

int ConQuerySize(int *X, int *Y) {
    *X = ScreenCols;
    *Y = ScreenRows;
    return 0;
}

int ConSetCursorPos(int X, int Y) {
    DrawCursor(0);
    CursorX = X;
    CursorY = Y;
    DrawCursor(1);
    return 0;
}

int ConQueryCursorPos(int *X, int *Y) {
    *X = CursorX;
    *Y = CursorY;
    return 0;
}

int ConShowCursor(void) {
    CursorVisible = 1;
    DrawCursor(1);
    return 0;
}

int ConHideCursor(void) {
    DrawCursor(0);
    CursorVisible = 0;
    return 0;
}

int ConCursorVisible(void) {
    return 1;
}
int ConSetCursorSize(int Start, int End) {
    return 1;
}

int ConSetMousePos(int X, int Y) {
    return 0;
}

static int LastMouseX = -1, LastMouseY = -1;

int ConQueryMousePos(int *X, int *Y) {
    if (X) *X = LastMouseX;
    if (Y) *Y = LastMouseY;
    return 0;
}

bool bCursorShowing = true;
int ConShowMouse(void) {
    if (!bCursorShowing)
    {
        SDL_ShowCursor(1);
        bCursorShowing = true;
    }
    return 0;
}

int ConHideMouse(void) {
    if (bCursorShowing)
    {
        SDL_ShowCursor(0);
        bCursorShowing = false;
    }
    return 0;
}

int ConMouseVisible(void) {
    return bCursorShowing ? 1 : 0;
}

int ConQueryMouseButtons(int *ButtonCount) {
    *ButtonCount = 3;
    return 0;
}

static void UpdateWindow(int xx, int yy, int ww, int hh) {
    PCell p;
    int i;

    /* show redrawn area */
    /*
    XFillRectangle(display, win, GCs[14], xx, yy, ww, hh);
    XFlush(display);
    i = XEventsQueued(display, QueuedAfterReading);
    while (i-- > 0) {
	XEvent e;
	XNextEvent(display, &e);
    }
    // sleep(1);*/

    ww /= FontCX; ww += 2;
    hh /= FontCY;
    xx /= FontCX;
    yy /= FontCY;

    /*
     * OK for this moment I suggest this method - it works somehow
     * But I suppose the correct solution would meant general rewrite
     * of some basic behavior of FTE editor
     * THIS IS TEMPORAL FIX AND SHOULD BE SOLVED IN GENERAL WAY !
     */
    hh *= 3; yy -= hh; hh += hh + 2;
    if (yy < 0)
	yy = 0;
    if (xx + ww > (int)ScreenCols) ww = ScreenCols - xx;
    if (yy + hh > (int)ScreenRows) hh = ScreenRows - yy;
    Refresh = 1;
    //frames->Repaint();
    //frames->Update();
    p = (PCell) CursorXYPos(xx, yy);
    for (i = 0; i < hh; i++) {
        ConPutBox(xx, yy + i, ww, 1, p);
        p += ScreenCols;
    }
    //fprintf(stderr, "UPDATE\tx:%3d  y:%3d  w:%3d  h:%3d\n", xx, yy, ww, hh);
    //XFlush(display);
    Refresh = 0;
}

static void ResizeWindow(int ww, int hh) {
    Uint32 texfmt;
    int texw, texh, texaccess;
    SDL_QueryTexture(backbuffer, &texfmt, &texaccess, &texw, &texh);

    SDL_SetRenderTarget(renderer, NULL);
    const SDL_Rect r = { 0, 0, texw, texh };
    SDL_RenderCopy(renderer, backbuffer, &r, &r);
    SDL_RenderPresent(renderer);
    SDL_SetRenderTarget(renderer, backbuffer);

    if ((ww > texw) || (hh > texh)) {
        SDL_DestroyTexture(backbuffer);
        backbuffer = SDL_CreateTexture(renderer, texfmt, texaccess, ww, hh);
        if (!backbuffer)
        {
            char buf[512];
            SDL_snprintf(buf, sizeof (buf), "Could not create backbuffer! (%s)", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "FTE-SDL Fatal", buf, win);
            SDL_DestroyTexture(font);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(win);
            SDL_Quit();
            DieError(1, "XFTE Fatal: %s", buf);
        }
        SDL_SetRenderTarget(renderer, backbuffer);
        SDL_RenderClear(renderer);
        bWindowDirty = true;
    }

    int ox = ScreenCols;
    int oy = ScreenRows;
    ww /= FontCX; if (ww <= 4) ww = 4;
    hh /= FontCY; if (hh <= 2) hh = 2;
    if ((int)ScreenCols != ww || (int)ScreenRows != hh) {
        Refresh = 0;
        ConSetSize(ww, hh);
        Refresh = 1;
        if (ox < (int)ScreenCols)
            UpdateWindow(ox * FontCX, 0,
                         (ScreenCols - ox) * FontCX, ScreenRows * FontCY);
        if (oy < (int)ScreenRows)
            UpdateWindow(0, oy * FontCY,
                         ScreenCols * FontCX, (ScreenRows - oy) * FontCY);
        Refresh = 0;
    }
}

static struct {
    SDL_Keycode keysym;
    long keycode;
} key_table[] = {
    { SDLK_KP_5,           kbPgUp | kfGray | kfCtrl },  // This is XK_KP_Begin in the X11 code and I live and die by it.  --ryan.
    { SDLK_RALT,           kbPgUp | kfGray | kfCtrl },  // hack to match my MacBook keyboard. This is XK_KP_Begin in the X11 code and I live and die by it.  --ryan.

    { SDLK_ESCAPE,         kbEsc },
    { SDLK_TAB,            kbTab },
    { SDLK_RETURN,         kbEnter },
    { SDLK_PAUSE,          kbPause },
    { SDLK_BACKSPACE,      kbBackSp },
    { SDLK_HOME,           kbHome },
    { SDLK_UP,             kbUp },
    { SDLK_PAGEUP,         kbPgUp },
    { SDLK_LEFT,           kbLeft },
    { SDLK_RIGHT,          kbRight },
    { SDLK_END,            kbEnd },
    { SDLK_DOWN,           kbDown },
    { SDLK_PAGEDOWN,       kbPgDn },
    { SDLK_SELECT,         kbEnd },
    { SDLK_KP_ENTER,       kbEnter | kfGray },
    { SDLK_INSERT,         kbIns | kfGray },
    { SDLK_DELETE,         kbDel | kfGray },
    { SDLK_KP_MEMADD,      '+' | kfGray },
    { SDLK_KP_MEMSUBTRACT, '-' | kfGray },
    { SDLK_KP_MEMMULTIPLY, '*' | kfGray },
    { SDLK_KP_MEMDIVIDE,   '/' | kfGray },
// !!! FIXME
    //{ SDLK_KP_HOME,        kbHome | kfGray },
    //{ SDLK_KP_UP,          kbUp | kfGray },
    //{ SDLK_KP_PAGEUP,      kbPgUp | kfGray },
    //{ SDLK_KP_LEFT,        kbLeft | kfGray },
    //{ SDLK_KP_RIGHT,       kbRight | kfGray },
    //{ SDLK_KP_END,         kbEnd | kfGray },
    //{ SDLK_KP_DOWN,        kbDown | kfGray },
    //{ SDLK_KP_NEXT,        kbPgDn| kfGray },
    { SDLK_NUMLOCKCLEAR,   kbNumLock },
    { SDLK_CAPSLOCK,       kbCapsLock },
    { SDLK_PRINTSCREEN,    kbPrtScr },
    { SDLK_LSHIFT,         kbShift },
    { SDLK_RSHIFT,         kbShift | kfGray },
    { SDLK_LCTRL,          kbCtrl },
    { SDLK_RCTRL,          kbCtrl | kfGray },
    { SDLK_LALT,           kbAlt },
    { SDLK_RALT,           kbAlt | kfGray },
    { SDLK_LGUI,           kbAlt },
    { SDLK_RGUI,           kbAlt | kfGray },
    { SDLK_F1,             kbF1 },
    { SDLK_F2,             kbF2 },
    { SDLK_F3,             kbF3 },
    { SDLK_F4,             kbF4 },
    { SDLK_F5,             kbF5 },
    { SDLK_F6,             kbF6 },
    { SDLK_F7,             kbF7 },
    { SDLK_F8,             kbF8 },
    { SDLK_F9,             kbF9 },
    { SDLK_F10,            kbF10 },
    { SDLK_F11,            kbF11 },
    { SDLK_F12,            kbF12 },
    { SDLK_KP_0,           '0' | kfGray },
    { SDLK_KP_1,           '1' | kfGray },
    { SDLK_KP_2,           '2' | kfGray },
    { SDLK_KP_3,           '3' | kfGray },
    { SDLK_KP_4,           '4' | kfGray },
    { SDLK_KP_5,           '5' | kfGray },
    { SDLK_KP_6,           '6' | kfGray },
    { SDLK_KP_7,           '7' | kfGray },
    { SDLK_KP_8,           '8' | kfGray },
    { SDLK_KP_9,           '9' | kfGray },
    { SDLK_KP_DECIMAL,     '.' | kfGray },

    // !!! FIXME: ???
    //{ 0x1000FF6F,        kbDel | kfShift | kfGray },
    //{ 0x1000FF70,        kbIns | kfCtrl | kfGray },
    //{ 0x1000FF71,        kbIns | kfShift | kfGray },
    //{ 0x1000FF72,        kbIns | kfGray },
    //{ 0x1000FF73,        kbDel | kfGray },
    //{ 0x1000FF74,        kbTab | kfShift },
    //{ 0x1000FF75,        kbTab | kfShift },

    { 0,                 0 }
};

static void ProcessSDLEvent(const SDL_Event &sdlevent, TEvent *Event) {
    switch (sdlevent.type) {
        case SDL_WINDOWEVENT:
            switch (sdlevent.window.event) {
                case SDL_WINDOWEVENT_EXPOSED: {
                    int w, h;
                    SDL_GetWindowSize(win, &w, &h);
                    UpdateWindow(0, 0, w, h);
                    break;
                }
                case SDL_WINDOWEVENT_SIZE_CHANGED: {
                    ResizeWindow(sdlevent.window.data1, sdlevent.window.data2);
                    Event->What = evCommand;
                    Event->Msg.Command = cmResize;
                    break;
                }
                case SDL_WINDOWEVENT_CLOSE: {
                    Event->What = evCommand;
                    Event->Msg.Command = cmClose;
                    break;
                }
            }
            break;

        case SDL_QUIT:
            Event->What = evCommand;
            Event->Msg.Command = cmClose;
            break;

        case SDL_MOUSEMOTION:
            Event->Mouse.X = sdlevent.motion.x / FontCX;
            Event->Mouse.Y = sdlevent.motion.y / FontCY;
            if (LastMouseX != Event->Mouse.X || LastMouseY != Event->Mouse.Y) {
                Event->What = evMouseMove;
                Event->Mouse.Count = 1;
                Event->Mouse.Buttons = 0;
                if (sdlevent.motion.state & SDL_BUTTON_LMASK) Event->Mouse.Buttons |= 1;
                if (sdlevent.motion.state & SDL_BUTTON_RMASK) Event->Mouse.Buttons |= 2;
                if (sdlevent.motion.state & SDL_BUTTON_MMASK) Event->Mouse.Buttons |= 4;
                LastMouseX = (int) Event->Mouse.X;
                LastMouseY = (int) Event->Mouse.Y;
            }
            break;

        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEBUTTONDOWN:
            Event->Mouse.X = sdlevent.button.x / FontCX;
            Event->Mouse.Y = sdlevent.button.y / FontCY;
            Event->What = (sdlevent.button.state == SDL_PRESSED) ? evMouseDown : evMouseUp;
            Event->Mouse.Count = sdlevent.button.clicks;
            Event->Mouse.Buttons = 0;
            if (sdlevent.button.button == SDL_BUTTON_LEFT) Event->Mouse.Buttons |= 1;
            if (sdlevent.button.button == SDL_BUTTON_RIGHT) Event->Mouse.Buttons |= 2;
            if (sdlevent.button.button == SDL_BUTTON_MIDDLE) Event->Mouse.Buttons |= 4;
            break;

        case SDL_MOUSEWHEEL:
            if (sdlevent.wheel.x || sdlevent.wheel.y) {
                Event->What = evMouseDown;
                Event->Mouse.X = LastMouseX;
                Event->Mouse.Y = LastMouseY;
                Event->Mouse.Count = 1;
                Event->Mouse.Buttons = 0;

                if (sdlevent.wheel.y > 0) {
                    Event->Msg.Command = cmVScrollPgUp;
                } else if (sdlevent.wheel.y < 0) {
                    Event->Msg.Command = cmVScrollPgDn;
                } else if (sdlevent.wheel.x > 0) {
                    Event->Msg.Command = cmHScrollPgRt;
                } else if (sdlevent.wheel.x < 0) {
                    Event->Msg.Command = cmHScrollPgLt; // fix core to use count
                }
            }
            break;

        case SDL_KEYDOWN:
        //case SDL_KEYUP:  // con_x11.cpp has this commented out too. --ryan.
        {
            unsigned int myState = 0;
            Event->What = (sdlevent.key.state == SDL_PRESSED) ? evKeyDown : evKeyUp;

            if (sdlevent.key.keysym.mod & KMOD_SHIFT) myState |= kfShift;
            if (sdlevent.key.keysym.mod & KMOD_CTRL) myState |= kfCtrl;
            if (sdlevent.key.keysym.mod & KMOD_LALT) myState |= kfAlt;  // Hack: I use RALT to fake XK_KP_Begin on my MacBook. Ignore it here. --ryan.

            unsigned int i;
            for (i = 0; i < SDL_arraysize(key_table); i++) {
                long k;
                if (sdlevent.key.keysym.sym == key_table[i].keysym) {
                    k = key_table[i].keycode;
                    if ((k < 256) && (myState == kfShift)) {
                        myState = 0;
                    }
                    Event->Key.Code = k | myState;
                    break;
                }
            }
            if (i == SDL_arraysize(key_table)) {
                long key = (long) sdlevent.key.keysym.sym;
                if ((key < 256) && (myState & (kfCtrl|kfAlt))) {
                    if ((key >= 'a') && (key < 'z' + 32)) {
                        key &= ~0x20;
                    }
                    if ((myState & kfCtrl) && key < 32)
                        key += 64;

                    Event->Key.Code = key | myState;
                } else {
                    Event->What = evNone;
                }
            }
        }
        break;

        case SDL_TEXTINPUT: {
            // !!! FIXME: This is cheap, but disregard codepoints > 255.
            FIXME("...unicode?");
            // !!! FIXME: This is also cheap, but disregard multi-char strings.
            FIXME("multichar strings?");
            const Uint8 ch = (Uint8) sdlevent.text.text[0];
            if ((ch & 128) == 0) { // high bit set? Not low ascii.
                unsigned int myState = 0;
                const SDL_Keymod mod = SDL_GetModState();
                Event->What = evKeyDown;

                if (mod & KMOD_SHIFT) myState |= kfShift;
                if (mod & KMOD_CTRL) myState |= kfCtrl;
                if (mod & KMOD_LALT) myState |= kfAlt;  // Hack: I use RALT to fake XK_KP_Begin on my MacBook. Ignore it here. --ryan.

                int key = (int) ch;
                if (myState & (kfAlt | kfCtrl)) {
                    if ((key >= 'a') && (key < 'z' + 32))
                        key &= ~0x20;
                }
                if ((myState & kfCtrl) && key < 32)
                    key += 64;
                Event->Key.Code = key | myState;
            }
        }
        break;
    }
}

static TEvent Pending = { evNone };

int ConGetEvent(TEventMask EventMask, TEvent *Event, int WaitTime, int Delete) {
    if (bWindowDirty) {
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        const SDL_Rect r = { 0, 0, w, h };
        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderCopy(renderer, backbuffer, &r, &r);
        SDL_RenderPresent(renderer);
        SDL_SetRenderTarget(renderer, backbuffer);
        bWindowDirty = false;
    }

    fd_set read_fds;
    Event->What = evNone;
    if (Pending.What != evNone) {
        *Event = Pending;
        if (Delete) Pending.What = evNone;
        if (Event->What & EventMask)
            return 0;
        else
            Pending.What = evNone;
    }

    const Uint32 timeout = (WaitTime >= 0) ? (SDL_GetTicks()+WaitTime) : 0;

    Event->What = evNone;

    while (true) {
        SDL_Event sdlevent;
        while (SDL_PollEvent(&sdlevent)) {
            ProcessSDLEvent(sdlevent, Event);
            if (Delete == 0)
                Pending = *Event;
            if (Event->What & EventMask)
                return 0;
            else
                Pending.What = evNone;
            Event->What = evNone;
        }

        bool bHavePipes = false;
        FD_ZERO(&read_fds);
        for (int p = 0; p < MAX_PIPES; p++) {
            if ((Pipes[p].used) && (Pipes[p].fd != -1)) {
                bHavePipes = true;
                FD_SET(Pipes[p].fd, &read_fds);
            }
        }

        if (bHavePipes) {
            struct timeval timeout = { 0, 0 };  // poll the pipes.
            if (select((int) (sizeof(fd_set) * 8), FD_SET_CAST() &read_fds, NULL, NULL, &timeout) > 0) {
                for (int pp = 0; pp < MAX_PIPES; pp++) {
                    if ((Pipes[pp].used) && (Pipes[pp].fd != -1)) {
                        if (FD_ISSET(Pipes[pp].fd, &read_fds)) {
                            if (Pipes[pp].notify) {
                                Event->What = evNotify;
                                Event->Msg.View = 0;
                                Event->Msg.Model = Pipes[pp].notify;
                                Event->Msg.Command = cmPipeRead;
                                Event->Msg.Param1 = pp;
                                Pipes[pp].stopped = 0;
                            }
                            //fprintf(stderr, "Pipe %d\n", Pipes[pp].fd);
                            return 0;
                        }
                    }
                }
            }
        }

        if (WaitTime >= 0) {
            if (SDL_TICKS_PASSED(SDL_GetTicks(), timeout)) {
                return -1;
            }
        }

        SDL_Delay(30);
    }

    return 0;
}

int ConPutEvent(TEvent Event) {
    Pending = Event;
    return 0;
}

int ConFlush(void) {
    //XFlush(display);
FIXME("prsent on flush?");
printf("flush\n");
    return 0;
}

int ConGrabEvents(TEventMask EventMask) {
    return 0;
}

int GetXSelection(int *len, char **data) {
    *len = 0;
    char *utf8 = SDL_GetClipboardText();
    // we don't convert this back to latin1. Deal.
    if (!utf8)
        return -1;
    *data = strdup(utf8);
    SDL_free(utf8);
    if (!*data)
        return -1;

    *len = (int) SDL_strlen(*data);
    return 0;
}

int SetXSelection(int len, char *data) {
    // SDL wants UTF-8 text, so we have to sort of pray this is latin1.
    char *utf8 = SDL_iconv_string("UTF-8", "latin1", data, len);
    if (utf8 == NULL)
        return -1;

    SDL_SetClipboardText(utf8);
    SDL_free(utf8);
    return 0;
}

GUI::GUI(int &argc, char **argv, int XSize, int YSize) {
    //int o = 1;

    FIXME("handle -font argument");
    FIXME("handle -geometry argument");
#if 0
    for (int c = 1; c < argc; c++) {
        if (strcmp(argv[c], "-font") == 0) {
            if (c + 1 < argc) {
                strcpy(WindowFont, argv[++c]);
            }
        } else if (strcmp(argv[c], "-geometry") == 0) {
            if (c + 1 < argc) {

                XParseGeometry(argv[++c], &initX, &initY,
                               &ScreenCols, &ScreenRows);
                if (ScreenCols > 255)
                    ScreenCols = 255;
                else if (ScreenCols < MIN_SCRWIDTH)
                    ScreenCols = MIN_SCRWIDTH;
                if (ScreenRows > 255)
                    ScreenRows = 255;
                else if (ScreenRows < MIN_SCRHEIGHT)
                    ScreenRows = MIN_SCRHEIGHT;
                setUserPosition = 1;
            }
        }

        else
            argv[o++] = argv[c];
    }

    argc = o;
    argv[argc] = 0;
#endif

    if (::ConInit(XSize, YSize) == 0
        && SetupSDLWindow(argc, argv) == 0)
        gui = this;
    else
        gui = NULL;
    fArgc = argc;
    fArgv = argv;
}

GUI::~GUI() {
    ::ConDone();
}

int GUI::ConSuspend(void) {
    return ::ConSuspend();
}

int GUI::ConContinue(void) {
    return ::ConContinue();
}

int GUI::ShowEntryScreen() {
    return 1;
}

int GUI::OpenPipe(char *Command, EModel *notify) {
#ifndef NO_PIPES
    int i;

    for (i = 0; i < MAX_PIPES; i++) {
        if (Pipes[i].used == 0) {
            int pfd[2];

            Pipes[i].id = i;
            Pipes[i].notify = notify;
            Pipes[i].stopped = 1;

            if (pipe((int *)pfd) == -1)
                return -1;

            switch (Pipes[i].pid = fork()) {
            case -1: /* fail */
                return -1;
            case 0: /* child */
                signal(SIGPIPE, SIG_DFL);
                close(pfd[0]);
                close(0);
                assert(open("/dev/null", O_RDONLY) == 0);
                dup2(pfd[1], 1);
                dup2(pfd[1], 2);
                close(pfd[1]);
                exit(system(Command));
            default:
                close(pfd[1]);
                fcntl(pfd[0], F_SETFL, O_NONBLOCK);
                Pipes[i].fd = pfd[0];
            }
            Pipes[i].used = 1;
            //fprintf(stderr, "Pipe Open: %d\n", i);
            return i;
        }
    }
    return -1;
#else
    return 0;
#endif
}

int GUI::SetPipeView(int id, EModel *notify) {
#ifndef NO_PIPES
    if (id < 0 || id > MAX_PIPES)
        return -1;
    if (Pipes[id].used == 0)
        return -1;
    //fprintf(stderr, "Pipe View: %d %08X\n", id, notify);
    Pipes[id].notify = notify;
#endif
    return 0;
}

int GUI::ReadPipe(int id, void *buffer, int len) {
#ifndef NO_PIPES
    int rc;

    if (id < 0 || id > MAX_PIPES)
        return -1;
    if (Pipes[id].used == 0)
        return -1;
    //fprintf(stderr, "Pipe Read: Get %d %d\n", id, len);

    rc = read(Pipes[id].fd, buffer, len);
    //fprintf(stderr, "Pipe Read: Got %d %d\n", id, len);
    if (rc == 0) {
        close(Pipes[id].fd);
        Pipes[id].fd = -1;
        return -1;
    }
    if (rc == -1) {
        Pipes[id].stopped = 1;
        return 0;
    }
    return rc;
#else
    return 0;
#endif
}

int GUI::ClosePipe(int id) {
#ifndef NO_PIPES
    int status;

    if (id < 0 || id > MAX_PIPES)
        return -1;
    if (Pipes[id].used == 0)
        return -1;
    if (Pipes[id].fd != -1)
        close(Pipes[id].fd);
    kill(Pipes[id].pid, SIGHUP);
    alarm(2);
    waitpid(Pipes[id].pid, &status, 0);
    alarm(0);
    //fprintf(stderr, "Pipe Close: %d\n", id);
    Pipes[id].used = 0;
    return WEXITSTATUS(status);
#else
    return 0;
#endif
}

int GUI::RunProgram(int mode, char *Command) {
    char Cmd[1024];

    char const* xterm = getenv("TERM");
    if (NULL == xterm || 0 == *xterm)
        xterm = "xterm";

    strcpy(Cmd, xterm);

    if (*Command == 0)  // empty string = shell
        strcat(Cmd, " -ls &");
    else {
        strcat(Cmd, " -e ");
        // buffer overflow problem: -2 for possible async.
    strncat(Cmd, Command, sizeof(Cmd) - strlen(Cmd) - 2);
        Cmd[sizeof(Cmd) - 3] = 0;
        if (mode == RUN_ASYNC)
            strcat(Cmd, " &");
    }
    rc = system(Cmd);
    return rc;
}

char ConGetDrawChar(int index) {
    static char tab[] = "\x0D\x0C\x0E\x0B\x12\x19____+>\x1F\x01\x12 ";

    assert(index >= 0 && index < (int) strlen(tab));

    return tab[index];
}