#include "raylib.h"
#include <X11/cursorfont.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdint.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void HideCursorX11Shape(void *window, unsigned int shape);

// ---------------------------------------------------------------------------
// Sparse grid. Cells live at integer world coordinates, origin at (0,0),
// unbounded in all directions. Only non-empty cells are stored.
//
// A cell's state is its RGB color: 2 bits per channel (0=00, 1=80, 2=FF)
// packed into 8 bits (2 spare, for future flags). One 64-bit word holds 8
// cells; a word at (wx, wy) covers cells x in [wx*8, wx*8+8) along the row
// y = wy. Words are kept in a hash map keyed by (wx, wy), so an entry only
// exists if it has at least one non-empty cell. floor(x/8) for negative x
// matches C's >>, so the (wx, x&7) split is correct on both sides of 0.
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t *keys;  // stored as key+1; 0 marks an empty slot
    uint8_t  *occ;   // 0 empty, 1 occupied, 2 tombstone
    uint64_t *vals;
    size_t count;
    size_t tombs;
    size_t cap;
} Grid;

static uint64_t GridHash(uint64_t k) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static size_t GridSlot(const Grid *g, uint64_t key) {
    size_t i = (size_t)GridHash(key) & (g->cap - 1);
    while (g->occ[i]) {
        if (g->occ[i] == 1 && g->keys[i] == key + 1) return i;
        i = (i + 1) & (g->cap - 1);
    }
    return i;
}

static void GridResize(Grid *g, size_t newCap) {
    uint64_t *ok = g->keys, *ov = g->vals;
    uint8_t  *oo = g->occ;
    g->keys = calloc(newCap, sizeof(uint64_t));
    g->vals = calloc(newCap, sizeof(uint64_t));
    g->occ  = calloc(newCap, sizeof(uint8_t));
    size_t oldCap = g->cap;
    g->cap = newCap;
    g->count = 0;
    g->tombs = 0;
    for (size_t i = 0; i < oldCap; i++) {
        if (oo[i] != 1) continue;
        uint64_t key = ok[i] - 1;
        size_t j = (size_t)GridHash(key) & (newCap - 1);
        while (g->occ[j]) j = (j + 1) & (newCap - 1);
        g->keys[j] = key + 1;
        g->vals[j] = ov[i];
        g->occ[j] = 1;
        g->count++;
    }
    free(ok); free(ov); free(oo);
}

static void GridInit(Grid *g) {
    g->cap = 1 << 16;
    g->count = 0;
    g->tombs = 0;
    g->keys = calloc(g->cap, sizeof(uint64_t));
    g->vals = calloc(g->cap, sizeof(uint64_t));
    g->occ  = calloc(g->cap, sizeof(uint8_t));
}

static void GridFree(Grid *g) {
    free(g->keys); free(g->vals); free(g->occ);
    g->keys = g->vals = NULL; g->occ = NULL;
    g->count = g->tombs = g->cap = 0;
}

// Returns pointer to the word for (wx, wy), or NULL if absent.
static uint64_t *GridFind(Grid *g, uint64_t key) {
    size_t i = (size_t)GridHash(key) & (g->cap - 1);
    while (g->occ[i]) {
        if (g->occ[i] == 1 && g->keys[i] == key + 1) return &g->vals[i];
        i = (i + 1) & (g->cap - 1);
    }
    return NULL;
}

static void GridSet(Grid *g, uint64_t key, uint64_t word) {
    if (word == 0) {
        uint64_t *p = GridFind(g, key);
        if (!p) return;
        *p = 0;
        size_t i = (size_t)GridHash(key) & (g->cap - 1);
        while (!(g->occ[i] == 1 && g->keys[i] == key + 1)) i = (i + 1) & (g->cap - 1);
        g->occ[i] = 2;
        g->count--;
        g->tombs++;
        if (g->tombs > g->cap / 3) GridResize(g, g->cap);
        return;
    }
    if ((g->count + g->tombs + 1) * 10 > g->cap * 7) GridResize(g, g->cap * 2);
    size_t i = GridSlot(g, key);
    if (g->occ[i] == 1) { g->vals[i] = word; return; }
    g->keys[i] = key + 1;
    g->vals[i] = word;
    g->occ[i] = 1;
    g->count++;
}

#define CELL_BITS 8
#define CELLS_PER_WORD 8

static uint64_t CellKey(int x, int y) {
    return ((uint64_t)(uint32_t)(x >> 3) << 32) | (uint32_t)y;
}

static int CellGet(Grid *g, int x, int y) {
    uint64_t *w = GridFind(g, CellKey(x, y));
    if (!w) return 0;
    return (int)((*w >> ((x & 7) * CELL_BITS)) & 0xFF);
}

static void CellSet(Grid *g, int x, int y, int state) {
    uint64_t key = CellKey(x, y);
    uint64_t *w = GridFind(g, key);
    int shift = (x & 7) * CELL_BITS;
    if (state == 0) {
        if (!w) return;
        *w &= ~(0xFFULL << shift);
        if (*w == 0) GridSet(g, key, 0);
    } else if (w) {
        *w = (*w & ~(0xFFULL << shift)) | ((uint64_t)state << shift);
    } else {
        GridSet(g, key, (uint64_t)state << shift);
    }
}

// ---------------------------------------------------------------------------
// Color model. Each channel is one of 3 levels, cycling 00 -> 80 -> FF:
//   LEVELS[0] = 00, LEVELS[1] = 80, LEVELS[2] = FF.
// A state packs red in bits 0-1, green in bits 2-3, blue in bits 4-5
// (bits 6-7 spare). State 0 = all channels off = dead/black (not drawn).
// ---------------------------------------------------------------------------
static const unsigned char LEVELS[4] = { 0, 128, 255, 255 };

static Color StateColor(int state) {
    return (Color){ LEVELS[state & 3], LEVELS[(state >> 2) & 3],
                    LEVELS[(state >> 4) & 3], 255 };
}

// Advance one channel of the cell under the cursor: left=red, middle=green,
// right=blue. screen is in pixels, (camX, camY) + zoom map to world coords.
static void CellAtScreen(Vector2 s, double camX, double camY, float zoom,
                         int *cx, int *cy) {
    *cx = (int)floor(camX + (s.x - GetScreenWidth() / 2.0f) / zoom);
    *cy = (int)floor(camY + (s.y - GetScreenHeight() / 2.0f) / zoom);
}

static void CycleChannel(Grid *g, Vector2 screen, int channel,
                         double camX, double camY, float zoom) {
    int cx, cy;
    CellAtScreen(screen, camX, camY, zoom, &cx, &cy);
    int state = CellGet(g, cx, cy);
    int shift = channel * 2;
    int lvl = ((state >> shift) & 3) + 1;
    if (lvl > 2) lvl = 0;
    state = (state & ~(3 << shift)) | (lvl << shift);
    CellSet(g, cx, cy, state);
}

// ---------------------------------------------------------------------------
// Conway's Life step. Alive = non-black, dead = black. Color is a passive
// layer: survivors keep theirs, births inherit a random birthing neighbor's.
// Sparse: tally live-neighbor counts only for live cells and their neighbors.
// ---------------------------------------------------------------------------
static uint64_t CellTallyKey(int x, int y) {   // unique per cell, unlike CellKey
    return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y;
}

typedef struct {
    uint64_t *keys;  // key+1
    uint8_t  *occ;   // 0 empty, 1 occupied
    int      *count; // live-neighbor count
    size_t count_, cap;
} Tally;

static int *TallyFindPtr(Tally *t, uint64_t key) {
    size_t i = (size_t)GridHash(key) & (t->cap - 1);
    while (t->occ[i]) {
        if (t->occ[i] == 1 && t->keys[i] == key + 1) return &t->count[i];
        i = (i + 1) & (t->cap - 1);
    }
    return NULL;
}

static void TallyResize(Tally *t, size_t newCap) {
    uint64_t *ok = t->keys;
    uint8_t  *oo = t->occ;
    int      *oc = t->count;
    t->keys = calloc(newCap, sizeof(uint64_t));
    t->occ  = calloc(newCap, sizeof(uint8_t));
    t->count = calloc(newCap, sizeof(int));
    size_t oldCap = t->cap;
    t->cap = newCap;
    t->count_ = 0;
    for (size_t i = 0; i < oldCap; i++) {
        if (oo[i] != 1) continue;
        uint64_t key = ok[i] - 1;
        size_t j = (size_t)GridHash(key) & (newCap - 1);
        while (t->occ[j]) j = (j + 1) & (newCap - 1);
        t->keys[j] = key + 1;
        t->count[j] = oc[i];
        t->occ[j] = 1;
        t->count_++;
    }
    free(ok); free(oo); free(oc);
}

static void TallyInit(Tally *t) {
    t->cap = 1 << 14;
    t->count_ = 0;
    t->keys = calloc(t->cap, sizeof(uint64_t));
    t->occ  = calloc(t->cap, sizeof(uint8_t));
    t->count = calloc(t->cap, sizeof(int));
}

static void TallyFree(Tally *t) {
    free(t->keys); free(t->occ); free(t->count);
    t->keys = NULL; t->occ = NULL; t->count = NULL;
    t->count_ = t->cap = 0;
}

static void TallyEnsure(Tally *t, uint64_t key) {
    if (TallyFindPtr(t, key)) return;
    if ((t->count_ + 1) * 10 > t->cap * 7) TallyResize(t, t->cap * 2);
    size_t i = (size_t)GridHash(key) & (t->cap - 1);
    while (t->occ[i]) i = (i + 1) & (t->cap - 1);
    t->keys[i] = key + 1;
    t->count[i] = 0;
    t->occ[i] = 1;
    t->count_++;
}

static void TallyInc(Tally *t, uint64_t key) {
    if (TallyFindPtr(t, key)) { (*TallyFindPtr(t, key))++; return; }
    TallyEnsure(t, key);
    *TallyFindPtr(t, key) = 1;
}

static void StepLife(Grid *g, Grid *next) {
    GridInit(next);
    Tally t;
    TallyInit(&t);
    for (size_t i = 0; i < g->cap; i++) {
        if (g->occ[i] != 1) continue;
        int wx = (int)((g->keys[i] - 1) >> 32);
        int wy = (int)((g->keys[i] - 1) & 0xFFFFFFFFULL);
        for (int j = 0; j < CELLS_PER_WORD; j++) {
            int state = (int)((g->vals[i] >> (j * CELL_BITS)) & 0xFF);
            if (!state) continue;
            int x = wx * CELLS_PER_WORD + j, y = wy;
            TallyEnsure(&t, CellTallyKey(x, y));
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx != 0 || dy != 0)
                        TallyInc(&t, CellTallyKey(x + dx, y + dy));
        }
    }
    for (size_t i = 0; i < t.cap; i++) {
        if (t.occ[i] != 1) continue;
        uint64_t key = t.keys[i] - 1;
        int x = (int)(key >> 32);
        int y = (int)(key & 0xFFFFFFFFULL);
        int neighbors = t.count[i];
        int state = CellGet(g, x, y);
        if (state) {
            if (neighbors == 2 || neighbors == 3) CellSet(next, x, y, state);
        } else if (neighbors == 3) {
            int colors[3], n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (dx != 0 || dy != 0) {
                        int s = CellGet(g, x + dx, y + dy);
                        if (s) colors[n++] = s;
                    }
            CellSet(next, x, y, colors[rand() % 3]);
        }
    }
    TallyFree(&t);
}

// ---------------------------------------------------------------------------
// Selection + internal clipboard (not the OS clipboard). Selection is a cell
// rectangle stored in gSelX0..gSelY1 (inclusive); the clipboard is a w*h
// array of cell states. Copy/cut replace the clipboard (freeing the old).
// ---------------------------------------------------------------------------
typedef struct {
    int w, h;
    int *cells;   // w*h states, row-major
} Clip;
static Clip gClip = { 0, 0, NULL };
static bool gHasSel = false;   // finalized selection present
static bool gSelActive = false;// left-drag in progress (live)
static int gSelAx, gSelAy;     // drag anchor cell
static int gSelX0, gSelY0, gSelX1, gSelY1;  // inclusive cell bounds

static void ClipFree(Clip *c) {
    free(c->cells);
    c->cells = NULL;
    c->w = c->h = 0;
}

static void CopySelection(Grid *g) {
    if (!gHasSel) return;
    int w = gSelX1 - gSelX0 + 1;
    int h = gSelY1 - gSelY0 + 1;
    ClipFree(&gClip);
    gClip.w = w;
    gClip.h = h;
    gClip.cells = malloc((size_t)w * h * sizeof(int));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            gClip.cells[y * w + x] = CellGet(g, gSelX0 + x, gSelY0 + y);
}

static void CutSelection(Grid *g) {
    if (!gHasSel) return;
    CopySelection(g);
    for (int y = gSelY0; y <= gSelY1; y++)
        for (int x = gSelX0; x <= gSelX1; x++)
            CellSet(g, x, y, 0);
}

static void PasteClipboard(Grid *g, Vector2 screen, double camX, double camY, float zoom) {
    if (!gClip.cells) return;
    int tx, ty;
    CellAtScreen(screen, camX, camY, zoom, &tx, &ty);
    for (int y = 0; y < gClip.h; y++)
        for (int x = 0; x < gClip.w; x++) {
            int st = gClip.cells[y * gClip.w + x];
            // Paste only live cells; leave the target cell untouched where
            // the buffer is dead.
            if (st) CellSet(g, tx + x, ty + y, st);
        }
}

// Dashed line from a to b; `offset` shifts the dash phase so the pattern
// marches along the line (the animated selection edges).
static void DrawDashedLine(Vector2 a, Vector2 b, float dash, float gap,
                           float offset, float th, Color c) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    Vector2 d = { dx / len, dy / len };
    for (float pos = -offset; pos < len; pos += dash + gap) {
        float e0 = fmaxf(pos, 0.0f);
        float e1 = fminf(pos + dash, len);
        if (e1 <= e0) continue;
        DrawLineEx((Vector2){ a.x + d.x * e0, a.y + d.y * e0 },
                   (Vector2){ a.x + d.x * e1, a.y + d.y * e1 }, th, c);
    }
}

static void DrawSelectionRect(double camX, double camY, float zoom) {
    if (!gHasSel && !gSelActive) return;
    float left = (float)((gSelX0 - camX) * zoom + GetScreenWidth() / 2.0);
    float top = (float)((gSelY0 - camY) * zoom + GetScreenHeight() / 2.0);
    float wpx = (float)((gSelX1 - gSelX0 + 1) * zoom);
    float hpx = (float)((gSelY1 - gSelY0 + 1) * zoom);
    Color sc = (Color){ 0, 220, 255, 255 };
    float offset = (float)GetTime() * 30.0f;   // px/s dash march
    float dash = 12.0f, gap = 8.0f, th = 2.0f;
    DrawDashedLine((Vector2){ left, top }, (Vector2){ left + wpx, top }, dash, gap, offset, th, sc);
    DrawDashedLine((Vector2){ left + wpx, top }, (Vector2){ left + wpx, top + hpx }, dash, gap, offset, th, sc);
    DrawDashedLine((Vector2){ left + wpx, top + hpx }, (Vector2){ left, top + hpx }, dash, gap, offset, th, sc);
    DrawDashedLine((Vector2){ left, top + hpx }, (Vector2){ left, top }, dash, gap, offset, th, sc);
}

// Preview of the clipboard at the mouse cursor, grid-aligned at the same
// zoom scale. Drawn opaque on every other frame (half the monitor's native
// refresh) so it flashes as a preview rather than reading as real cells; the
// flashing distinguishes it even when the buffer matches cells underneath.
static void DrawClipPreview(double camX, double camY, float zoom, int frame) {
    if (!gClip.cells) return;
    if ((frame & 1) == 0) return;
    int tx, ty;
    CellAtScreen(GetMousePosition(), camX, camY, zoom, &tx, &ty);
    int px = (int)zoom + 1;
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    for (int y = 0; y < gClip.h; y++)
        for (int x = 0; x < gClip.w; x++) {
            int st = gClip.cells[y * gClip.w + x];
            if (!st) continue;
            double sx = (tx + x - camX) * zoom + sw / 2.0;
            double sy = (ty + y - camY) * zoom + sh / 2.0;
            DrawRectangle((int)sx, (int)sy, px, px, StateColor(st));
        }
}

// Pick the external (non-built-in laptop) monitor. Built-in panels are
// excluded by name; if several remain the largest wins, else fall back to
// the current monitor. Fullscreen then goes there via SetWindowMonitor().
static int SelectExternalMonitor(void) {
    int count = GetMonitorCount();
    int best = GetCurrentMonitor();
    int bestArea = -1;
    for (int i = 0; i < count; i++) {
        const char *name = GetMonitorName(i);
        TraceLog(LOG_INFO, "Monitor %d: \"%s\" %dx%d pos(%d,%d)",
                 i, name, GetMonitorWidth(i), GetMonitorHeight(i),
                 (int)GetMonitorPosition(i).x, (int)GetMonitorPosition(i).y);
        int builtin = name &&
            (strstr(name, "eDP") || strstr(name, "LVDS") ||
             strstr(name, "Built") || strstr(name, "Laptop") ||
             strstr(name, "LCD") || strstr(name, "Internal"));
        if (builtin) continue;
        int area = GetMonitorWidth(i) * GetMonitorHeight(i);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    TraceLog(LOG_INFO, "Using monitor %d: %s", best, GetMonitorName(best));
    return best;
}

int main(int argc, char **argv) {
    InitWindow(1920, 1080, "Cell");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetWindowMonitor(SelectExternalMonitor());
    SetTargetFPS(60);
    // Replace the arrow with a circle glyph via X11 (WSLg ignores raylib's
    // HideCursor()). Do NOT call raylib's HideCursor() here. The cursor IS
    // the mouse marker, so nothing is drawn at the mouse position.
    HideCursorX11Shape(GetWindowHandle(), XC_circle);

    InitAudioDevice();

    // Network scaffolding: a non-blocking UDP socket ready for later use.
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) fcntl(sock, F_SETFL, O_NONBLOCK);
    (void)sock;

    srand((unsigned)time(NULL));
    Grid grid;
    GridInit(&grid);

    // Camera: zoom = pixels per cell, (camX, camY) = world coord at screen
    // center (starts at origin 0,0).
    float zoom = 32.0f;
    // Camera in double: it's the accumulation point for panning, and floats
    // lose per-cell precision past ~2^24. double is integer-exact to 2^53,
    // so it stays precise well beyond the int32 cell coordinate range.
    double camX = 0.0, camY = 0.0;
    bool leftHeld = false, leftDrag = false;
    Vector2 leftPress = { 0, 0 };
    bool midHeld = false, midDrag = false;
    Vector2 midPress = { 0, 0 };
    Vector2 dragLast = { 0, 0 };
    int cursorShape = XC_circle;

    // Test fill: every cell covering the screen at 32px/cell gets a random
    // non-empty state; everything beyond stays empty.
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    int minX = (int)floorf(-sw / (2.0f * 32.0f));
    int maxX = (int)ceilf(sw / (2.0f * 32.0f)) - 1;
    int minY = (int)floorf(-sh / (2.0f * 32.0f));
    int maxY = (int)ceilf(sh / (2.0f * 32.0f)) - 1;
    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++) {
            int lr = rand() % 3, lg = rand() % 3, lb = rand() % 3;
            if (lr == 0 && lg == 0 && lb == 0) lr = 1;
            CellSet(&grid, x, y, lr | (lg << 2) | (lb << 4));
        }

    bool showHud = true;
    int frame = 0;
    bool paused = false;
    double gensPerSec = 5.0;
    double timer = 0.0;
    bool periodActive = false;
    double periodAcc = 0.0;
    while (!WindowShouldClose()) {
        frame++;
        if (IsKeyPressed(KEY_ESCAPE)) break;
        if (IsKeyPressed(KEY_F1)) showHud = !showHud;
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_Q)) {   // clear clipboard + cancel selection
            ClipFree(&gClip);
            gHasSel = false;
            gSelActive = false;
        }
        // Single-step: act immediately on press, then keep repeating while held
        // (OS key repeat is unreliable here, so repeat is emulated).
        if (IsKeyPressed(KEY_PERIOD)) {
            Grid next;
            StepLife(&grid, &next);
            GridFree(&grid);
            grid = next;
            periodAcc = 0.0;
            periodActive = true;
        } else if (IsKeyDown(KEY_PERIOD) && periodActive) {
            periodAcc += GetFrameTime();
            if (periodAcc >= 0.08) {
                Grid next;
                StepLife(&grid, &next);
                GridFree(&grid);
                grid = next;
                periodAcc = 0.0;
            }
        } else {
            periodActive = false;
            periodAcc = 0.0;
        }
        if (IsKeyPressed(KEY_KP_ADD)) { gensPerSec *= 1.5; if (gensPerSec > 240.0) gensPerSec = 240.0; }
        if (IsKeyPressed(KEY_KP_SUBTRACT)) { gensPerSec /= 1.5; if (gensPerSec < 0.05) gensPerSec = 0.05; }
        if (!paused) {
            timer += GetFrameTime();
            double interval = 1.0 / gensPerSec;
            int steps = 0;
            while (timer >= interval && steps < 240) {
                Grid next;
                StepLife(&grid, &next);
                GridFree(&grid);
                grid = next;
                timer -= interval;
                steps++;
            }
            if (steps == 240) timer = 0.0;
        }

        // Zoom toward the mouse: keep the world point under the cursor fixed.
        Vector2 mouse = GetMousePosition();
        double wx = camX + (mouse.x - sw / 2.0f) / zoom;
        double wy = camY + (mouse.y - sh / 2.0f) / zoom;
        if (GetMouseWheelMove() != 0) {
            zoom *= powf(1.25f, GetMouseWheelMove());
            if (zoom < 0.25f) zoom = 0.25f;
            if (zoom > 256.0f) zoom = 256.0f;
            camX = wx - (mouse.x - sw / 2.0f) / zoom;
            camY = wy - (mouse.y - sh / 2.0f) / zoom;
        }

        // Pan: WASD / arrows, plus middle-button drag. Left drag selects; a bare
        // click cancels the selection (or cycles red if none). Click vs drag
        // is decided by how far the pointer moved (~5px).
        float pan = 1200.0f / zoom * GetFrameTime();
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) camX += pan;
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) camX -= pan;
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) camY += pan;
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) camY -= pan;

        // ---- Left: drag to select; click cancels selection or cycles red ----
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            leftHeld = true;
            leftDrag = false;
            leftPress = mouse;
            CellAtScreen(leftPress, camX, camY, zoom, &gSelAx, &gSelAy);
        }
        if (leftHeld && !leftDrag &&
            (fabsf(mouse.x - leftPress.x) > 5 || fabsf(mouse.y - leftPress.y) > 5)) {
            leftDrag = true;
            gSelActive = true;
            gHasSel = false;   // new drag supersedes any prior selection
        }
        if (leftHeld) {        // live-update selection bounds while held
            int cx, cy;
            CellAtScreen(mouse, camX, camY, zoom, &cx, &cy);
            gSelX0 = MIN(gSelAx, cx); gSelX1 = MAX(gSelAx, cx);
            gSelY0 = MIN(gSelAy, cy); gSelY1 = MAX(gSelAy, cy);
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (leftDrag) {    // finished a selection drag
                gSelActive = false;
                gHasSel = true;
            } else if (gHasSel) {
                gHasSel = false;   // single click cancels selection
            } else {
                CycleChannel(&grid, leftPress, 0, camX, camY, zoom);
            }
            leftHeld = false;
            leftDrag = false;
        }

        // ---- Middle: drag pans; bare click cycles green ----
        if (IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE)) {
            midHeld = true;
            midDrag = false;
            midPress = mouse;
        }
        if (midHeld && !midDrag &&
            (fabsf(mouse.x - midPress.x) > 5 || fabsf(mouse.y - midPress.y) > 5)) {
            midDrag = true;
            dragLast = midPress;
        }
        if (midDrag) {
            camX -= (mouse.x - dragLast.x) / zoom;
            camY -= (mouse.y - dragLast.y) / zoom;
            dragLast = mouse;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_MIDDLE)) {
            if (!midDrag) CycleChannel(&grid, midPress, 1, camX, camY, zoom);
            midHeld = false;
            midDrag = false;
        }

        // ---- Right: click cycles blue ----
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) CycleChannel(&grid, mouse, 2, camX, camY, zoom);

        // ---- Copy / cut / paste (internal clipboard) ----
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (ctrl && IsKeyPressed(KEY_C)) CopySelection(&grid);
        if (ctrl && IsKeyPressed(KEY_X)) CutSelection(&grid);
        if (ctrl && IsKeyPressed(KEY_V)) PasteClipboard(&grid, mouse, camX, camY, zoom);

        // Cursor: crosshair while drag-selecting, move-cursor (fleur) while a
        // cut/copy buffer preview is showing, else the circle. Swap on change.
        int want = XC_circle;
        if (gClip.cells) want = XC_fleur;
        else if (gSelActive) want = XC_crosshair;
        if (want != cursorShape) {
            HideCursorX11Shape(GetWindowHandle(), (unsigned int)want);
            cursorShape = want;
        }

        double leftX = camX - sw / (2.0f * zoom);
        double rightX = camX + sw / (2.0f * zoom);
        double topY = camY - sh / (2.0f * zoom);
        double botY = camY + sh / (2.0f * zoom);
        int px = (int)zoom + 1;

        BeginDrawing();
            ClearBackground(BLACK);
            for (size_t i = 0; i < grid.cap; i++) {
                if (grid.occ[i] != 1) continue;
                int wx = (int)((grid.keys[i] - 1) >> 32);
                int wy = (int)((grid.keys[i] - 1) & 0xFFFFFFFFULL);
                double x0 = wx * 8.0;
                if (x0 + 8 < leftX || x0 > rightX) continue;
                // A row spans y in [wy, wy+1); cull only when it lies fully above or
                // below the viewport, so partially-visible rows still render.
                if (wy + 1 < topY || wy > botY) continue;
                for (int j = 0; j < 8; j++) {
                    int state = (int)((grid.vals[i] >> (j * CELL_BITS)) & 0xFF);
                    if (!state) continue;
                    double sx = (x0 + j - camX) * zoom + sw / 2.0f;
                    double sy = (wy - camY) * zoom + sh / 2.0f;
                    DrawRectangle((int)sx, (int)sy, px, px, StateColor(state));
                }
            }
            DrawClipPreview(camX, camY, zoom, frame);
            DrawSelectionRect(camX, camY, zoom);
            if (showHud) {
                DrawText(TextFormat("zoom %.2f  center (%d, %d)", zoom, (int)camX, (int)camY),
                         10, 10, 18, GRAY);
                DrawText(TextFormat("gens/s %.1f  %s", gensPerSec, paused ? "PAUSED" : "running"),
                         10, 32, 18, GRAY);
            }
        EndDrawing();
    }

    ClipFree(&gClip);
    GridFree(&grid);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}