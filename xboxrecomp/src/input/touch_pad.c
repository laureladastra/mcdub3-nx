/*
 * touch_pad.c -- an on-screen Xbox pad for touch screens (Android, ADR 0010
 * of mcdub3-nx), player 1 like the keyboard (sdl_keyboard.c).
 *
 * The renderer hands SDL's finger events here (xbox_touchpad_handle), asks
 * for the overlay's picture (xbox_touchpad_image, drawn on the CPU and only
 * redrawn when something changes) and blends it over the frame;
 * xbox_InputGetState adds the pad's state on top of port 0's
 * (xbox_touchpad_state).
 *
 *   stick + d-pad    BACK START    A B X Y  WHITE   (as on the original
 *                                                BLACK  Xbox controller)
 *   bottom left, bottom middle, bottom right: WHITE and BLACK (the
 *   Switch's L / R) two small pills, white and black, tilted along the
 *   diamond's Y-B edge just outside it
 *
 * The stick and the d-pad are one control: a finger landing inside the ring
 * is the stick, one landing on the ring the d-pad -- the arrow under it, or
 * two at a diagonal, following the finger round. The stick's up and down are
 * also gas and brake (right / left trigger, as far as pushed), as the
 * keyboard's arrows are; while driving (xbox_keyboard_driving, the title
 * says) they are the pedals only, so a race camera on the stick stays put.
 * A finger may slide from one button to another.
 *
 * All one translucent grey; the labels in a stroke font of our own (smooth,
 * sans serif: the few capitals the labels use).
 *
 * Shown while there is a touch screen, no controller on port 0, and the
 * last input was not the keyboard; a touch brings it back.
 * RECOMP_TOUCH_PAD=0 off, =1 always (with a mouse acting as a finger: the
 * desktop, to try it); RECOMP_TOUCH_PAD_ALPHA (0.1-1, default 0.55) and
 * RECOMP_TOUCH_PAD_SCALE (0.6-1.6, default 1) for opacity and size.
 */
#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)

#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "touch_pad.h"
#include "xinput_xbox.h"

extern int (*xbox_keyboard_driving)(void);      /* sdl_keyboard.c */

enum { C_NONE = -1, C_STICK, C_DPAD, C_A, C_B, C_X, C_Y, C_WHITE, C_BLACK,
       C_BACK, C_START, C_COUNT };

enum { D_UP = 1, D_DOWN = 2, D_LEFT = 4, D_RIGHT = 8 };

typedef struct {
    float x, y, w, h;       /* pixels, y down; circles: centre +- w/2 */
    int round;              /* 1 circle, 0 rounded box */
    const char *label;
    float pill_len, pill_th;    /* > 0: a pill tilted along Y-B, this long and thick */
    unsigned char tint;         /* the pill's colour (grey level): WHITE 255, BLACK 0 */
} Ctl;

#define MAX_FINGERS 10
static struct { SDL_FingerID id; int ctl, dir; } s_fing[MAX_FINGERS];
static int s_nfing;

static Ctl s_ctl[C_COUNT];
static int s_sw, s_sh;                  /* the layout's screen size */
static float s_stick_r, s_ring_in, s_ring_out;  /* stick base; the d-pad ring */
static float s_stick_x, s_stick_y;      /* -1..1, y down */
static int s_down[C_COUNT], s_dirs;     /* s_dirs: d-pad arrows held (D_*) */
static volatile int s_shown;            /* 1: visible */
static int s_mode = -2;                 /* RECOMP_TOUCH_PAD: -1 auto, 0, 1 */
static float s_alpha = 0.55f, s_scale = 1.0f;
static int s_keyboard_last, s_controller, s_touched;
static int s_dirty = 1;

/* Pad state, read by the USB thread: written whole under this lock. */
static SDL_SpinLock s_lock;
static struct { WORD buttons; BYTE analog[8]; SHORT lx, ly; } s_pad;

static void settings(void)
{
    const char *e;
    if (s_mode != -2)
        return;
    e = getenv("RECOMP_TOUCH_PAD");
#if defined(__ANDROID__)
    s_mode = e && *e == '0' ? 0 : e && *e == '1' ? 1 : -1;
#else
    s_mode = e && *e == '1' ? 1 : 0;
    if (s_mode == 1)
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  /* the mouse as a finger */
#endif
    if ((e = getenv("RECOMP_TOUCH_PAD_ALPHA")) != NULL) {
        float a = (float)atof(e);
        s_alpha = a < 0.1f ? 0.1f : a > 1.0f ? 1.0f : a;
    }
    if ((e = getenv("RECOMP_TOUCH_PAD_SCALE")) != NULL) {
        float k = (float)atof(e);
        s_scale = k < 0.6f ? 0.6f : k > 1.6f ? 1.6f : k;
    }
}

static void set_ctl(int i, float cx, float cy, float w, float h, int round, const char *label)
{
    Ctl *c = &s_ctl[i];
    c->x = cx - w / 2; c->y = cy - h / 2; c->w = w; c->h = h; c->round = round;
    c->label = label;
    c->pill_len = c->pill_th = 0;
}

/* Sizes from the screen's height (u), so a phone and a tablet look alike. */
static void layout(int sw, int sh)
{
    float u = (float)sh * s_scale, W = (float)sw, H = (float)sh;
    float m = 0.05f * u;                                /* margin */
    float br = 0.13f * u, d = 0.115f * u;               /* A B X Y */
    /* WHITE and BLACK: two pills tilted along Y-B, side by side just
     * outside that edge of the diamond, the same gap (g) from Y and B and
     * from each other as between the round buttons; colours, no text.
     * Each as long as a circle is wide: together they span the Y-B row
     * from end to end, WHITE beside Y, BLACK beside B. */
    const float r2 = 0.70710678f;
    float g = 1.41421356f * d - br, pl = br, pt = 0.058f * u;
    float off = br / 2 + g + pt / 2, half = pl / 2 + g / 2;
    float mx = 0.5f * d + off * r2, my = -0.5f * d - off * r2;
    float wx = mx - half * r2, wy = my - half * r2, kx = mx + half * r2, ky = my + half * r2;
    float ext = (pl - pt) / 2 * r2 + pt / 2;            /* a tilted pill's half width */
    float ax = W - m - kx - ext, ay = H - m - d - br / 2;
    float pw = 0.19f * u, ph = 0.075f * u, by = H - m - ph / 2;
    float sx, sy, band, reach;
    s_stick_r = 0.15f * u;
    s_ring_in = s_stick_r * 1.06f;                      /* the d-pad ring */
    s_ring_out = s_stick_r + 0.12f * u;
    /* The same margin m to what can be seen on both sides: the d-pad's
     * arrow tips (draw: chevron, its stroke) as the buttons' edges -- the
     * ring's faint band reaches a little further and may go under it. */
    band = s_ring_out - s_ring_in;
    reach = (s_ring_in + s_ring_out) / 2 + 0.35f * band * 0.42f + band * 0.065f;
    sx = m + reach;
    sy = H - m - reach;
    s_sw = sw; s_sh = sh;
    set_ctl(C_STICK, sx, sy, 2 * s_stick_r, 2 * s_stick_r, 1, NULL);
    set_ctl(C_DPAD, sx, sy, 2 * s_ring_out, 2 * s_ring_out, 1, NULL);
    set_ctl(C_A, ax, ay + d, br, br, 1, "A");
    set_ctl(C_B, ax + d, ay, br, br, 1, "B");
    set_ctl(C_X, ax - d, ay, br, br, 1, "X");
    set_ctl(C_Y, ax, ay - d, br, br, 1, "Y");
    set_ctl(C_WHITE, ax + wx, ay + wy, pl, pl, 1, NULL);  /* hit: a circle round it */
    set_ctl(C_BLACK, ax + kx, ay + ky, pl, pl, 1, NULL);
    s_ctl[C_WHITE].pill_len = s_ctl[C_BLACK].pill_len = pl;
    s_ctl[C_WHITE].pill_th = s_ctl[C_BLACK].pill_th = pt;
    s_ctl[C_WHITE].tint = 255;
    s_ctl[C_BLACK].tint = 0;
    /* BACK and START at the bottom, between the stick and the buttons */
    {
        float mid = (sx + s_ring_out + ax - d - br / 2) / 2;
        set_ctl(C_BACK, mid - 0.02f * u - pw / 2, by, pw, ph, 0, "BACK");
        set_ctl(C_START, mid + 0.02f * u + pw / 2, by, pw, ph, 0, "START");
    }
    s_dirty = 1;
}

/* The d-pad arrows at (x, y) on the ring: eight sectors, the diagonals
 * pressing two arrows. */
static int ring_dirs(float x, float y)
{
    float cx = s_ctl[C_STICK].x + s_stick_r, cy = s_ctl[C_STICK].y + s_stick_r;
    float a = atan2f(y - cy, x - cx) * 180.0f / 3.14159265f;   /* 0 right, 90 down */
    int sector = (int)floorf((a + 22.5f + 360.0f) / 45.0f) % 8;
    static const int dirs[8] = { D_RIGHT, D_RIGHT | D_DOWN, D_DOWN, D_DOWN | D_LEFT,
                                 D_LEFT, D_LEFT | D_UP, D_UP, D_UP | D_RIGHT };
    return dirs[sector];
}

/* The control under (x, y), pixels; generous edges. */
static int hit(float x, float y)
{
    int i, best = C_NONE;
    float bestd = 1e30f;
    {   /* the stick and its d-pad ring first */
        float cx = s_ctl[C_STICK].x + s_stick_r, cy = s_ctl[C_STICK].y + s_stick_r;
        float dd = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (dd <= s_ring_in)
            return C_STICK;
        if (dd <= s_ring_out * 1.12f)
            return C_DPAD;
    }
    for (i = C_A; i < C_COUNT; i++) {
        const Ctl *c = &s_ctl[i];
        float cx = c->x + c->w / 2, cy = c->y + c->h / 2;
        float dx = fabsf(x - cx) / (c->w / 2 * 1.18f), dy = fabsf(y - cy) / (c->h / 2 * 1.18f);
        float dd = c->round ? dx * dx + dy * dy : (dx > dy ? dx : dy);
        if (dd <= 1.0f && dd < bestd) {
            bestd = dd;
            best = i;
        }
    }
    return best;
}

static void publish(void)
{
    WORD b = 0;
    BYTE an[8];
    int i;
    float lx = s_stick_x, ly = s_stick_y;
    memset(an, 0, sizeof an);
    for (i = 0; i < C_COUNT; i++)
        s_down[i] = 0;
    s_dirs = 0;
    for (i = 0; i < s_nfing; i++)
        if (s_fing[i].ctl >= 0) {
            s_down[s_fing[i].ctl] = 1;
            if (s_fing[i].ctl == C_DPAD)
                s_dirs |= s_fing[i].dir;
        }
    /* A and B swapped: the button lettered A presses the Xbox's B and the
     * one lettered B its A (the layout asked for, 2026-11-04) */
    if (s_down[C_A]) an[XBOX_BUTTON_B] = 255;
    if (s_down[C_B]) an[XBOX_BUTTON_A] = 255;
    if (s_down[C_X]) an[XBOX_BUTTON_X] = 255;
    if (s_down[C_Y]) an[XBOX_BUTTON_Y] = 255;
    if (s_down[C_WHITE]) an[XBOX_BUTTON_WHITE] = 255;
    if (s_down[C_BLACK]) an[XBOX_BUTTON_BLACK] = 255;
    if (s_down[C_START]) b |= XBOX_GAMEPAD_START;
    if (s_down[C_BACK]) b |= XBOX_GAMEPAD_BACK;
    if (s_dirs & D_UP) b |= XBOX_GAMEPAD_DPAD_UP;
    if (s_dirs & D_DOWN) b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (s_dirs & D_LEFT) b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (s_dirs & D_RIGHT) b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (!s_down[C_STICK])
        lx = ly = 0;
    /* up / down: gas / brake, as far as pushed (a little dead zone) */
    if (ly < -0.15f)
        an[XBOX_BUTTON_RTRIGGER] = (BYTE)(ly <= -0.85f ? 255 : (-ly - 0.15f) / 0.7f * 255.0f);
    if (ly > 0.15f)
        an[XBOX_BUTTON_LTRIGGER] = (BYTE)(ly >= 0.85f ? 255 : (ly - 0.15f) / 0.7f * 255.0f);
    SDL_AtomicLock(&s_lock);
    s_pad.buttons = b;
    memcpy(s_pad.analog, an, sizeof an);
    s_pad.lx = (SHORT)(lx * 32767.0f);
    s_pad.ly = (SHORT)(-ly * 32767.0f);                /* Xbox: up is + */
    SDL_AtomicUnlock(&s_lock);
    s_dirty = 1;
}

static void stick_to(float x, float y)
{
    float r = s_stick_r;
    float cx = s_ctl[C_STICK].x + r, cy = s_ctl[C_STICK].y + r;
    float dx = (x - cx) / r, dy = (y - cy) / r, len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) {
        dx /= len;
        dy /= len;
    }
    s_stick_x = dx;
    s_stick_y = dy;
}

static int finger(SDL_FingerID id)
{
    int i;
    for (i = 0; i < s_nfing; i++)
        if (s_fing[i].id == id)
            return i;
    return -1;
}

static void update_shown(void)
{
    int on = s_mode == 1 || (s_mode == -1 && !s_controller && !s_keyboard_last
                             && (s_touched || SDL_GetNumTouchDevices() > 0));
    if (on != s_shown) {
        s_shown = on;
        s_dirty = 1;
        if (!on) {                      /* nothing stays held when it goes */
            s_nfing = 0;
            publish();
        }
    }
}

void xbox_touchpad_handle(const SDL_Event *e)
{
    int f;
    float x, y;
    settings();
    if (!s_mode)
        return;
    switch (e->type) {
    case SDL_KEYDOWN:
        if (!s_keyboard_last) {
            s_keyboard_last = 1;
            update_shown();
        }
        return;
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        break;
    default:
        return;
    }
    if (s_keyboard_last || !s_touched) {
        s_keyboard_last = 0;
        s_touched = 1;
        update_shown();
    }
    if (!s_shown || s_sw <= 0)
        return;
    x = e->tfinger.x * (float)s_sw;
    y = e->tfinger.y * (float)s_sh;
    f = finger(e->tfinger.fingerId);
    if (e->type == SDL_FINGERDOWN) {
        if (f < 0) {
            if (s_nfing == MAX_FINGERS)
                return;
            f = s_nfing++;
            s_fing[f].id = e->tfinger.fingerId;
        }
        s_fing[f].ctl = hit(x, y);
        s_fing[f].dir = s_fing[f].ctl == C_DPAD ? ring_dirs(x, y) : 0;
        if (s_fing[f].ctl == C_STICK)
            stick_to(x, y);
        publish();
    } else if (e->type == SDL_FINGERMOTION && f >= 0) {
        if (s_fing[f].ctl == C_STICK) {
            stick_to(x, y);
            publish();
        } else if (s_fing[f].ctl == C_DPAD) {
            int dir = ring_dirs(x, y);              /* round the ring */
            if (dir != s_fing[f].dir) {
                s_fing[f].dir = dir;
                publish();
            }
        } else {
            /* slide between buttons (not onto the stick or its ring) */
            int c = hit(x, y);
            if (c != C_STICK && c != C_DPAD && c != s_fing[f].ctl) {
                s_fing[f].ctl = c;
                publish();
            }
        }
    } else if (e->type == SDL_FINGERUP && f >= 0) {
        s_fing[f] = s_fing[--s_nfing];
        publish();
    }
}

void xbox_touchpad_controller(int connected)
{
    settings();
    if (!s_mode || connected == s_controller)
        return;
    s_controller = connected;
    update_shown();
}

int xbox_touchpad_shown(void)
{
    settings();
    if (s_mode && !s_controller && !s_keyboard_last && !s_shown)
        update_shown();
    return s_shown;
}

void xbox_touchpad_state(XBOX_INPUT_STATE *st)
{
    int i, v;
    if (!s_shown)
        return;
    SDL_AtomicLock(&s_lock);
    st->Gamepad.wButtons |= s_pad.buttons;
    for (i = 0; i < 8; i++)
        if (s_pad.analog[i] > st->Gamepad.bAnalogButtons[i])
            st->Gamepad.bAnalogButtons[i] = s_pad.analog[i];
    v = st->Gamepad.sThumbLX + s_pad.lx;
    st->Gamepad.sThumbLX = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = st->Gamepad.sThumbLY
        + (xbox_keyboard_driving && xbox_keyboard_driving() ? 0 : s_pad.ly);
    st->Gamepad.sThumbLY = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    SDL_AtomicUnlock(&s_lock);
}

/* ── The picture ─────────────────────────────────────────────────────── */

static uint32_t *s_img;
static int s_iw, s_ih;
static float s_k;                       /* image pixels per screen pixel */

/* Source-over in straight alpha: grey level g at coverage a (0..1). */
static void put(int x, int y, unsigned char g, float a)
{
    unsigned char *p;
    float da, oa, v;
    if (x < 0 || y < 0 || x >= s_iw || y >= s_ih || a <= 0.0f)
        return;
    p = (unsigned char *)&s_img[y * s_iw + x];
    da = p[3] / 255.0f;
    oa = a + da * (1.0f - a);
    if (oa <= 0.0f)
        return;
    v = (g * a + p[0] * da * (1.0f - a)) / oa;
    p[0] = p[1] = p[2] = (unsigned char)v;
    p[3] = (unsigned char)(oa * 255.0f);
}

/* A filled circle or rounded box (corner radius rr), anti-aliased; ring > 0
 * draws only an outline that thick. Image pixels. */
static void shape(float x, float y, float w, float h, float rr, float ring,
                  unsigned char g, float a)
{
    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
    int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1, px, py;
    float cx = x + w / 2, cy = y + h / 2, hx = w / 2 - rr, hy = h / 2 - rr;
    for (py = y0; py <= y1; py++)
        for (px = x0; px <= x1; px++) {
            float qx = fabsf(px + 0.5f - cx) - hx, qy = fabsf(py + 0.5f - cy) - hy;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float in = (qx > qy ? qx : qy);
            float dist = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rr;  /* <0 inside */
            float cov = 0.5f - dist;
            if (ring > 0) {
                float cin = 0.5f - (-dist - ring);
                cov = (cov < 1 ? cov : 1) * (1 - (cin < 0 ? 0 : cin > 1 ? 1 : cin));
            }
            if (cov > 0)
                put(px, py, g, a * (cov > 1 ? 1 : cov));
        }
}

/* ── Strokes: the labels' letters and the d-pad's arrows ──
 * Line segments with round ends, anti-aliased by distance. */
#define MAX_SEG 160
static float s_seg[MAX_SEG][4];
static int s_nseg;

static void seg(float x0, float y0, float x1, float y1)
{
    if (s_nseg < MAX_SEG) {
        s_seg[s_nseg][0] = x0; s_seg[s_nseg][1] = y0;
        s_seg[s_nseg][2] = x1; s_seg[s_nseg][3] = y1;
        s_nseg++;
    }
}

/* An elliptic arc as segments, angles in degrees (0 right, 90 down). */
static void arc(float cx, float cy, float rx, float ry, float a0, float a1, float ox, float oy,
                float k)
{
    int i, n = (int)(fabsf(a1 - a0) / 15.0f) + 2;     /* ~15 degrees a segment */
    float px = 0, py = 0;
    for (i = 0; i <= n; i++) {
        float t = (a0 + (a1 - a0) * i / n) * 3.14159265f / 180.0f;
        float x = ox + (cx + rx * cosf(t)) * k, y = oy + (cy + ry * sinf(t)) * k;
        if (i)
            seg(px, py, x, y);
        px = x;
        py = y;
    }
}

/* Draw the queued segments, stroke width sw, grey g at alpha a. */
static void strokes(float sw, unsigned char g, float a)
{
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, r = sw / 2;
    int i, px, py;
    for (i = 0; i < s_nseg; i++) {
        x0 = fminf(x0, fminf(s_seg[i][0], s_seg[i][2]));
        x1 = fmaxf(x1, fmaxf(s_seg[i][0], s_seg[i][2]));
        y0 = fminf(y0, fminf(s_seg[i][1], s_seg[i][3]));
        y1 = fmaxf(y1, fmaxf(s_seg[i][1], s_seg[i][3]));
    }
    for (py = (int)floorf(y0 - r) - 1; py <= (int)ceilf(y1 + r) + 1; py++)
        for (px = (int)floorf(x0 - r) - 1; px <= (int)ceilf(x1 + r) + 1; px++) {
            float qx = px + 0.5f, qy = py + 0.5f, best = 1e30f;
            for (i = 0; i < s_nseg; i++) {
                float ax = s_seg[i][0], ay = s_seg[i][1];
                float bx = s_seg[i][2] - ax, by = s_seg[i][3] - ay;
                float l2 = bx * bx + by * by;
                float t = l2 > 0 ? ((qx - ax) * bx + (qy - ay) * by) / l2 : 0;
                float dx, dy;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                dx = qx - (ax + t * bx);
                dy = qy - (ay + t * by);
                best = fminf(best, dx * dx + dy * dy);
            }
            {
                float cov = r + 0.5f - sqrtf(best);
                if (cov > 0)
                    put(px, py, g, a * (cov > 1 ? 1 : cov));
            }
        }
    s_nseg = 0;
}

/* One capital at (ox, oy), height k; its advance (width) in units of k. A
 * geometric sans serif: straight strokes, round bowls. */
static float glyph(char c, float ox, float oy, float k)
{
#define L(a, b, c2, d) seg(ox + (a) * k, oy + (b) * k, ox + (c2) * k, oy + (d) * k)
    switch (c) {
    case 'A': L(0, 1, 0.31f, 0); L(0.31f, 0, 0.62f, 1); L(0.12f, 0.64f, 0.50f, 0.64f); return 0.62f;
    case 'B':
        L(0, 0, 0, 1); L(0, 0, 0.30f, 0); L(0, 0.48f, 0.32f, 0.48f); L(0, 1, 0.33f, 1);
        arc(0.30f, 0.24f, 0.24f, 0.24f, -90, 90, ox, oy, k);
        arc(0.33f, 0.74f, 0.26f, 0.26f, -90, 90, ox, oy, k);
        return 0.59f;
    case 'C': arc(0.36f, 0.5f, 0.36f, 0.5f, -50, -310, ox, oy, k); return 0.66f;
    case 'E': L(0.52f, 0, 0, 0); L(0, 0, 0, 1); L(0, 1, 0.52f, 1); L(0, 0.5f, 0.44f, 0.5f); return 0.52f;
    case 'H': L(0, 0, 0, 1); L(0.56f, 0, 0.56f, 1); L(0, 0.5f, 0.56f, 0.5f); return 0.56f;
    case 'I': L(0, 0, 0, 1); return 0.0f;
    case 'K': L(0, 0, 0, 1); L(0.54f, 0, 0, 0.62f); L(0.19f, 0.42f, 0.58f, 1); return 0.58f;
    case 'L': L(0, 0, 0, 1); L(0, 1, 0.48f, 1); return 0.48f;
    case 'R':
        L(0, 1, 0, 0); L(0, 0, 0.32f, 0); L(0, 0.5f, 0.32f, 0.5f); L(0.26f, 0.5f, 0.56f, 1);
        arc(0.32f, 0.25f, 0.25f, 0.25f, -90, 90, ox, oy, k);
        return 0.57f;
    case 'S':
        arc(0.29f, 0.25f, 0.27f, 0.25f, -30, -270, ox, oy, k);
        arc(0.29f, 0.75f, 0.29f, 0.25f, -90, 150, ox, oy, k);
        return 0.58f;
    case 'T': L(0, 0, 0.6f, 0); L(0.3f, 0, 0.3f, 1); return 0.6f;
    case 'W': L(0, 0, 0.19f, 1); L(0.19f, 1, 0.39f, 0.2f); L(0.39f, 0.2f, 0.59f, 1); L(0.59f, 1, 0.78f, 0); return 0.78f;
    case 'X': L(0, 0, 0.58f, 1); L(0.58f, 0, 0, 1); return 0.58f;
    case 'Y': L(0, 0, 0.31f, 0.52f); L(0.62f, 0, 0.31f, 0.52f); L(0.31f, 0.52f, 0.31f, 1); return 0.62f;
    default: return 0.5f;
    }
#undef L
}

/* A label centred on (cx, cy), capitals h tall -- smaller if it would be
 * wider than maxw. */
static void text(const char *s, float cx, float cy, float h, float maxw, unsigned char g, float a)
{
    float gap = 0.30f, w = 0, x;
    const char *p;
    for (p = s; *p; p++) {                  /* measure: draw nothing */
        int keep = s_nseg;
        w += glyph(*p, 0, 0, 1) + (p[1] ? gap : 0);
        s_nseg = keep;
    }
    if (w * h > maxw)
        h = maxw / w;
    x = cx - w * h / 2;
    for (p = s; *p; p++)
        x += (glyph(*p, x, cy - h / 2, h) + gap) * h;
    strokes(h * 0.13f, g, a);
}

/* A d-pad arrow: a chevron pointing (dx, dy) from (cx, cy), size s. */
static void chevron(float cx, float cy, float dx, float dy, float s)
{
    float px = -dy, py = dx;                /* across */
    seg(cx - px * s * 0.6f - dx * s * 0.25f, cy - py * s * 0.6f - dy * s * 0.25f,
        cx + dx * s * 0.35f, cy + dy * s * 0.35f);
    seg(cx + dx * s * 0.35f, cy + dy * s * 0.35f,
        cx + px * s * 0.6f - dx * s * 0.25f, cy + py * s * 0.6f - dy * s * 0.25f);
}

static void draw(void)
{
    const unsigned char fill = 110, edge = 215, ink = 245;
    int i;
    memset(s_img, 0, (size_t)s_iw * s_ih * 4);
    for (i = 0; i < C_COUNT; i++) {
        const Ctl *c = &s_ctl[i];
        float x = c->x * s_k, y = c->y * s_k, w = c->w * s_k, h = c->h * s_k;
        float rr = c->round ? w / 2 : h * 0.5f;
        float a = s_alpha * (s_down[i] ? 0.9f : 0.5f);
        if (i == C_DPAD) {
            /* the ring and its four arrows */
            static const float dir[4][3] = { { 0, -1, D_UP }, { 0, 1, D_DOWN },
                                             { -1, 0, D_LEFT }, { 1, 0, D_RIGHT } };
            float cx = x + w / 2, cy = y + h / 2, rm = (s_ring_in + s_ring_out) / 2 * s_k;
            float band = (s_ring_out - s_ring_in) * s_k;
            int k;
            shape(x, y, w, h, w / 2, band, fill, s_alpha * 0.32f);
            for (k = 0; k < 4; k++) {
                int on = (s_dirs & (int)dir[k][2]) != 0;
                if (on)
                    shape(cx + dir[k][0] * rm - band * 0.42f, cy + dir[k][1] * rm - band * 0.42f,
                          band * 0.84f, band * 0.84f, band * 0.42f, 0, edge, s_alpha * 0.35f);
                chevron(cx + dir[k][0] * rm, cy + dir[k][1] * rm, dir[k][0], dir[k][1], band * 0.42f);
                strokes(band * 0.13f + 1.0f, ink, s_alpha * (on ? 1.0f : 0.7f));
            }
            continue;
        }
        if (i == C_STICK) {
            float kr = w * 0.23f;
            float kx = x + w / 2 + s_stick_x * (w / 2 - kr) * (s_down[i] ? 1 : 0);
            float ky = y + h / 2 + s_stick_y * (h / 2 - kr) * (s_down[i] ? 1 : 0);
            shape(x, y, w, h, rr, 0, fill, s_alpha * 0.4f);
            shape(x, y, w, h, rr, 1.5f * s_k + 1, edge, s_alpha * 0.6f);
            shape(kx - kr, ky - kr, 2 * kr, 2 * kr, kr, 0, edge, s_alpha * (s_down[i] ? 0.9f : 0.6f));
            continue;
        }
        if (c->pill_len > 0) {
            /* a tilted pill: a rim, then the colour inside it */
            float cx = x + w / 2, cy = y + h / 2, r2 = 0.70710678f;
            float hl = (c->pill_len - c->pill_th) / 2 * s_k * r2, th = c->pill_th * s_k;
            seg(cx - hl, cy - hl, cx + hl, cy + hl);
            strokes(th, edge, s_alpha * (s_down[i] ? 0.95f : 0.6f));
            seg(cx - hl, cy - hl, cx + hl, cy + hl);
            strokes(th - 2 * (1.5f * s_k + 1), c->tint,
                    s_alpha * (s_down[i] ? 1.0f : c->tint ? 0.6f : 0.75f));
            continue;
        }
        shape(x, y, w, h, rr, 0, s_down[i] ? edge : fill, a);
        shape(x, y, w, h, rr, 1.5f * s_k + 1, edge, s_alpha * 0.6f);
        if (c->label)
            text(c->label, x + w / 2, y + h / 2, h * 0.36f, w * 0.72f,
                 ink, s_alpha < 0.9f ? 0.95f : 1.0f);
    }
}

int xbox_touchpad_image(int sw, int sh, const uint32_t **rgba, int *w, int *h)
{
    if (!xbox_touchpad_shown() || sw <= 0 || sh <= 0)
        return -1;
    if (sw != s_sw || sh != s_sh) {
        int iw, ih;
        layout(sw, sh);
        /* At most ~1280 wide: the overlay is soft shapes, linear-filtered. */
        s_k = sw > 1280 ? 1280.0f / (float)sw : 1.0f;
        iw = (int)(sw * s_k + 0.5f);
        ih = (int)(sh * s_k + 0.5f);
        if (iw != s_iw || ih != s_ih) {
            free(s_img);
            s_img = (uint32_t *)malloc((size_t)iw * ih * 4);
            s_iw = s_img ? iw : 0;
            s_ih = s_img ? ih : 0;
        }
    }
    if (!s_img)
        return -1;
    *rgba = s_img;
    *w = s_iw;
    *h = s_ih;
    if (!s_dirty)
        return 0;
    s_dirty = 0;
    draw();
    return 1;
}

#endif
