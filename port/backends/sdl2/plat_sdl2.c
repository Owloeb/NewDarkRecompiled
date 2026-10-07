/* plat_sdl2.c: the reference desktop backend (window, input, audio) on SDL2. Rendering is in gl_render.c.
 *
 * The game draws into an offscreen picture of exactly the size it asked for; every presented frame is scaled into the
 * window with black bars, so the window can have any size and the game's resolution stays what it chose. Mouse
 * positions are reported in that picture's pixels. Environment: SS2PORT_FULLSCREEN=0/1 overrides the game's choice,
 * SS2PORT_GLES=1 asks for OpenGL ES 2.0 instead of desktop OpenGL 2.1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "plat.h"
#include "gl_render.h"

static SDL_Window *win; static SDL_GLContext ctx;
static int surf_w = 640, surf_h = 480, want_fullscreen, rel_mode;

/* ---------------------------------------------------------------- window */
static void letterbox(int *x, int *y, int *w, int *h) {
    int ww, wh; SDL_GL_GetDrawableSize(win, &ww, &wh);
    double s = (double)ww / surf_w < (double)wh / surf_h ? (double)ww / surf_w : (double)wh / surf_h;
    *w = (int)(surf_w * s + 0.5); *h = (int)(surf_h * s + 0.5); *x = (ww - *w) / 2; *y = (wh - *h) / 2;
}
void gl_present_rect(int *x, int *y, int *w, int *h, int *ww, int *wh) { letterbox(x, y, w, h); SDL_GL_GetDrawableSize(win, ww, wh); }
void gl_swap(void) { SDL_GL_SwapWindow(win); }
int plat_video_open(int w, int h, int fullscreen, const char *title) {
    const char *fs = getenv("SS2PORT_FULLSCREEN"); if (fs) fullscreen = atoi(fs);
    surf_w = w > 0 ? w : 640; surf_h = h > 0 ? h : 480; want_fullscreen = fullscreen;
    if (!win) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL video: %s\n", SDL_GetError()); return -1; }
        int gles = getenv("SS2PORT_GLES") && atoi(getenv("SS2PORT_GLES"));
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gles ? SDL_GL_CONTEXT_PROFILE_ES : 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles ? 0 : 1);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        win = SDL_CreateWindow(title ? title : "System Shock 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, surf_w, surf_h,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!win) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return -1; }
        ctx = SDL_GL_CreateContext(win);
        if (!ctx) { fprintf(stderr, "OpenGL context: %s\n", SDL_GetError()); return -1; }
        SDL_GL_SetSwapInterval(getenv("SS2PORT_NOVSYNC") ? 0 : 1);
        if (gl_init(SDL_GL_GetProcAddress, gles)) return -1;
        SDL_StartTextInput();
    } else {
        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (!fullscreen) { int cw, ch; SDL_GetWindowSize(win, &cw, &ch); if (cw < surf_w || ch < surf_h) SDL_SetWindowSize(win, surf_w, surf_h); }
        if (title) SDL_SetWindowTitle(win, title);
    }
    gl_resize_backbuffer(surf_w, surf_h);
    return 0;
}
void plat_video_close(void) { if (ctx) { gl_shutdown(); SDL_GL_DeleteContext(ctx); ctx = NULL; } if (win) { SDL_DestroyWindow(win); win = NULL; } }
void plat_video_caps(PlatVideoCaps *c) { gl_caps(c); }
void plat_video_window_size(int *w, int *h) { *w = surf_w; *h = surf_h; }
void plat_video_display_size(int *w, int *h) {
    SDL_DisplayMode m; if (SDL_WasInit(SDL_INIT_VIDEO) || !SDL_InitSubSystem(SDL_INIT_VIDEO)) { if (!SDL_GetDesktopDisplayMode(0, &m)) { *w = m.w; *h = m.h; return; } }
    *w = 1920; *h = 1080;
}
void plat_video_set_title(const char *t) { if (win) SDL_SetWindowTitle(win, t); }
#include <stdarg.h>
static void port_log_plat(const char *fmt, ...) { char b[300]; int n = snprintf(b, sizeof b, "[sdl] "); va_list ap; va_start(ap, fmt); vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap); va_end(ap); plat_log_write(PLAT_LOG_INFO, b); }
static int warp_pending, warp_x, warp_y;      /* the motion event our own warp produces is not player movement */
static float mouse_scale = 1.0f, frac_x, frac_y;
void plat_video_mouse_mode(int relative, int visible) {
    static int last = -1; rel_mode = relative;
    if (!win) return;
    int r = SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE); SDL_ShowCursor(visible && !relative ? SDL_ENABLE : SDL_DISABLE);
    if (relative != last) { last = relative; port_log_plat("mouse: %s%s%s", relative ? "captured (relative motion)" : "free", r ? " - SDL refused: " : "", r ? SDL_GetError() : ""); }
}
void plat_video_warp_mouse(int x, int y) {
    if (!win || rel_mode) return; int bx, by, bw, bh; letterbox(&bx, &by, &bw, &bh);
    int ww, wh, dw, dh; SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetDrawableSize(win, &dw, &dh);
    warp_x = (int)((bx + (double)x * bw / surf_w) * ww / dw); warp_y = (int)((by + (double)y * bh / surf_h) * wh / dh); warp_pending = 1;
    SDL_WarpMouseInWindow(win, warp_x, warp_y);
}

/* ---------------------------------------------------------------- input: SDL scan codes (USB HID) to DirectInput scan codes */
static int dik_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) { static const uint8_t l[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C }; return l[s - SDL_SCANCODE_A]; }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return 0x02 + (s - SDL_SCANCODE_1);
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return 0x3B + (s - SDL_SCANCODE_F1);
    switch (s) {
    case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58; case SDL_SCANCODE_PRINTSCREEN: return 0xB7;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46; case SDL_SCANCODE_PAUSE: return 0xC5; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_HOME: return 0xC7; case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_DELETE: return 0xD3; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_KP_DIVIDE: return 0xB5; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_KP_1: return 0x4F; case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C; case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49; case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_NONUSBACKSLASH: return 0x56;
    case SDL_SCANCODE_APPLICATION: return 0xDD; case SDL_SCANCODE_LCTRL: return 0x1D; case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_LGUI: return 0xDB;
    case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_RGUI: return 0xDC;
    default: return 0;
    }
}
static void to_surface(int wx, int wy, int *x, int *y) {
    int bx, by, bw, bh; letterbox(&bx, &by, &bw, &bh);
    int ww, wh, dw, dh; SDL_GetWindowSize(win, &ww, &wh); SDL_GL_GetDrawableSize(win, &dw, &dh);
    double px = (double)wx * dw / (ww ? ww : 1), py = (double)wy * dh / (wh ? wh : 1);
    *x = bw ? (int)((px - bx) * surf_w / bw) : 0; *y = bh ? (int)((py - by) * surf_h / bh) : 0;
    if (*x < 0) *x = 0; if (*y < 0) *y = 0; if (*x >= surf_w) *x = surf_w - 1; if (*y >= surf_h) *y = surf_h - 1;
}
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p; uint32_t c = *s++; int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n) c &= 0x3F >> n; for (int i = 0; i < n && (*s & 0xC0) == 0x80; i++) c = (c << 6) | (*s++ & 0x3F);
    *p = (const char *)s; return c;
}
void plat_video_poll(void (*sink)(const PlatEvent *, void *), void *user) {
    if (!win) return;
    SDL_Event e; PlatEvent p;
    while (SDL_PollEvent(&e)) {
        memset(&p, 0, sizeof p);
        switch (e.type) {
        case SDL_QUIT: p.type = PLAT_EV_QUIT; sink(&p, user); break;
        case SDL_KEYDOWN: case SDL_KEYUP:
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT)) { want_fullscreen = !want_fullscreen; SDL_SetWindowFullscreen(win, want_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); break; }
            p.type = PLAT_EV_KEY; p.down = e.type == SDL_KEYDOWN; p.repeat = e.key.repeat; p.key = dik_of(e.key.keysym.scancode); if (p.key) sink(&p, user); break;
        case SDL_TEXTINPUT: { const char *t = e.text.text; while (*t) { p.type = PLAT_EV_TEXT; p.text = utf8_next(&t); sink(&p, user); } break; }
        case SDL_MOUSEMOTION: {
            p.type = PLAT_EV_MOUSE_MOVE; to_surface(e.motion.x, e.motion.y, &p.x, &p.y);
            if (warp_pending && e.motion.x == warp_x && e.motion.y == warp_y) { warp_pending = 0; p.dx = p.dy = 0; sink(&p, user); break; }
            float fx = e.motion.xrel * mouse_scale + frac_x, fy = e.motion.yrel * mouse_scale + frac_y;
            p.dx = (int)fx; p.dy = (int)fy; frac_x = fx - (float)p.dx; frac_y = fy - (float)p.dy;
            sink(&p, user); break; }
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            static const int map[6] = { -1, 0, 2, 1, 3, 4 }; int b = e.button.button < 6 ? map[e.button.button] : -1; if (b < 0) break;
            p.type = PLAT_EV_MOUSE_BUTTON; p.down = e.type == SDL_MOUSEBUTTONDOWN; p.button = b; sink(&p, user); break; }
        case SDL_MOUSEWHEEL: p.type = PLAT_EV_MOUSE_WHEEL; p.dy = (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y) * 120; if (p.dy) sink(&p, user); break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) { p.type = PLAT_EV_FOCUS; p.down = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED; sink(&p, user); if (p.down && rel_mode) SDL_SetRelativeMouseMode(SDL_TRUE); }
            else if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) { p.type = PLAT_EV_RESIZE; p.x = e.window.data1; p.y = e.window.data2; sink(&p, user); }
            break;
        }
    }
}

/* ---------------------------------------------------------------- audio */
static SDL_AudioDeviceID adev; static void (*afill)(int16_t *, int, void *); static void *auser;
static void audio_cb(void *u, Uint8 *stream, int len) { (void)u; afill((int16_t *)stream, len / 4, auser); }
int plat_audio_open(int rate, void (*fill)(int16_t *, int, void *), void *user) {
    if (getenv("SS2PORT_NOSOUND") || SDL_InitSubSystem(SDL_INIT_AUDIO)) return 0;
    SDL_AudioSpec want = { 0 }, have; want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    afill = fill; auser = user;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!adev) { fprintf(stderr, "SDL audio: %s\n", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(adev, 0); return have.freq;
}
void plat_audio_close(void) { if (adev) { SDL_CloseAudioDevice(adev); adev = 0; } }
void plat_audio_pause(int p) { if (adev) SDL_PauseAudioDevice(adev, p); }

void plat_alert(const char *title, const char *text) { if (!getenv("SS2PORT_NOALERT")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, win); }

int port_main(int argc, char **argv);
int main(int argc, char **argv) {
    /* SS2PORT_MOUSE_WARP=1: emulate captured mouse by re-centring the pointer (for systems without pointer capture,
       such as some remote desktops); SS2PORT_MOUSE_SCALE: multiply mouse motion (e.g. 0.5) */
    const char *mw = getenv("SS2PORT_MOUSE_WARP"); SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, mw && *mw == '1' ? "1" : "0");
    const char *ms = getenv("SS2PORT_MOUSE_SCALE"); if (ms && atof(ms) > 0) mouse_scale = (float)atof(ms);
    if (SDL_Init(0)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    int r = port_main(argc, argv);
    SDL_Quit(); return r;
}
