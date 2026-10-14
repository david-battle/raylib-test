#include "raylib.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define COLS 10
#define ROWS 20

// Each piece lives in its own square grid (size in SIZES) centered on the grid
// midpoint, so a 90-degree rotation about that midpoint leaves the piece where
// it is. 3x3 for T/L/J/S/Z, 2x2 for O, 4x4 for I (centered on its middle row).
static const int SHAPES[7][4][4] = {
    { {0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0} }, // I
    { {1,1,0,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // O
    { {0,1,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // T
    { {1,0,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // L
    { {0,0,1,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // J
    { {0,1,1,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // S
    { {1,1,0,0}, {0,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // Z
};

static const int SIZES[7] = { 4, 2, 3, 3, 3, 3, 3 };

static const Color COLORS[8] = {
    {0,0,0,0},          // empty
    {0,255,255,255},    // I cyan
    {255,255,0,255},    // O yellow
    {160,0,255,255},    // T purple
    {255,150,0,255},    // L orange
    {0,0,255,255},      // J blue
    {0,255,0,255},      // S green
    {255,0,0,255},      // Z red
};

static int board[ROWS][COLS];

typedef struct { int s[4][4]; int x, y; int type; } Piece;

static int collide(int b[ROWS][COLS], int s[4][4], int px, int py) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!s[y][x]) continue;
            int bx = px + x, by = py + y;
            if (bx < 0 || bx >= COLS || by >= ROWS) return 1;
            if (by >= 0 && b[by][bx]) return 1;
        }
    return 0;
}

static int drop_row(int b[ROWS][COLS], int s[4][4], int px, int py) {
    int dy = 0;
    while (!collide(b, s, px, py + dy + 1)) dy++;
    return py + dy;
}




// 90-degree rotation of the n x n sub-grid about the grid midpoint. Because
// every piece is centered in its own grid, the occupied cells stay centered.
static void rotate(int s[4][4], int n) {
    int t[4][4] = {{0}};
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            t[x][n-1-y] = s[y][x];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            s[y][x] = t[y][x];
}

static int SelectExternalMonitor(void) {
    int count = GetMonitorCount();
    if (count <= 1) return 0;
    // Prefer highest resolution non-builtin-like; also prefer rdp-2 over rdp-0 if present
    int best = 0;
    int bestArea = -1;
    for (int i = 0; i < count; i++) {
        const char *name = GetMonitorName(i);
        int area = GetMonitorWidth(i) * GetMonitorHeight(i);
        int isBuiltin = 0;
        if (name) {
            if (strstr(name, "eDP") || strstr(name, "LVDS") || strstr(name, "Built") ||
                strstr(name, "Laptop") || strstr(name, "LCD") || strstr(name, "Internal") ||
                strcmp(name, "rdp-0") == 0) {
                isBuiltin = 1;
            }
        }
        if (!isBuiltin && area > bestArea) {
            bestArea = area;
            best = i;
        }
    }
    if (bestArea < 0) {
        // fallback: pick index 1 if exists
        best = (count > 1) ? 1 : 0;
    }
    return best;
}

int main(void) {
    int ext = SelectExternalMonitor();
    SetConfigFlags(FLAG_FULLSCREEN_MODE | FLAG_VSYNC_HINT);
    InitWindow(GetMonitorWidth(ext), GetMonitorHeight(ext), "Tetris");
    SetWindowMonitor(ext);
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetTargetFPS(60);
    InitAudioDevice();

    Sound sMove   = LoadSound("resources/buttonfx.wav");
    Sound sRot    = LoadSound("resources/ping_send.wav");
    Sound sLand   = LoadSound("resources/coin.wav");
    Sound sClear  = LoadSound("resources/hit_splat.wav");
    Sound sOver   = LoadSound("resources/boom.wav");
    #if defined(PLATFORM_DESKTOP)
    Music bgm = LoadMusicStream("resources/country.mp3");
    #endif

    Piece cur = {0};
    int nextType = 0;
    int score = 0, lines = 0, level = 1, gameOver = 0, paused = 0;
    float timer = 0.0f, gravity = 0.8f;

    #define SPAWN() do { \
        memcpy(cur.s, SHAPES[nextType], sizeof(cur.s)); cur.type = nextType; \
        cur.x = 3; cur.y = 0; \
        nextType = GetRandomValue(0, 6); \
        if (collide(board, cur.s, cur.x, cur.y)) gameOver = 1; \
    } while (0)

    nextType = GetRandomValue(0, 6);
    SPAWN();

    #if defined(PLATFORM_DESKTOP)
    if (bgm.frameCount > 0) PlayMusicStream(bgm);
    #endif

    while (!WindowShouldClose()) {
        #if defined(PLATFORM_DESKTOP)
        if (bgm.frameCount > 0) UpdateMusicStream(bgm);
        #endif

        if (gameOver) {
            if (IsKeyPressed(KEY_ENTER)) {
                memset(board, 0, sizeof(board));
                score = lines = 0; level = 1; gameOver = 0;
                gravity = 0.8f;
                paused = 0;
                SPAWN();
            }
        } else if (IsKeyPressed(KEY_P) || IsKeyPressed(KEY_PAUSE)) {
            paused = !paused;
            #if defined(PLATFORM_DESKTOP)
            if (paused && bgm.frameCount > 0) PauseMusicStream(bgm);
            else if (!paused && bgm.frameCount > 0) ResumeMusicStream(bgm);
            #endif
        } else if (paused) {
            #if defined(PLATFORM_DESKTOP)
            if (bgm.frameCount > 0) UpdateMusicStream(bgm);
            #endif
        } else {
            if (IsKeyPressed(KEY_LEFT)  && !collide(board, cur.s, cur.x-1, cur.y)) { cur.x--; PlaySound(sMove); }
            if (IsKeyPressed(KEY_RIGHT) && !collide(board, cur.s, cur.x+1, cur.y)) { cur.x++; PlaySound(sMove); }
            if (IsKeyPressed(KEY_DOWN)  && !collide(board, cur.s, cur.x, cur.y+1)) { cur.y++; score += 1; }

            if (IsKeyPressed(KEY_UP)) {
                int tmp[4][4]; memcpy(tmp, cur.s, sizeof(tmp));
                rotate(tmp, SIZES[cur.type]);
                for (int kick = 0; kick >= -2; kick--)
                    if (!collide(board, tmp, cur.x+kick, cur.y)) {
                        memcpy(cur.s, tmp, sizeof(tmp)); cur.x += kick;
                        PlaySound(sRot); break;
                    }
            }
            if (IsKeyPressed(KEY_SPACE)) {
                while (!collide(board, cur.s, cur.x, cur.y+1)) { cur.y++; score += 2; }
                timer = 999.0f;
            }

            float interval = 1.0f / gravity;
            timer += GetFrameTime();
            if (timer >= interval) {
                timer = 0.0f;
                if (!collide(board, cur.s, cur.x, cur.y+1)) {
                    cur.y++;
                } else {
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++)
                            if (cur.s[y][x] && cur.y+y >= 0)
                                board[cur.y+y][cur.x+x] = cur.type + 1;

                    int cleared = 0;
                    for (int y = ROWS-1; y >= 0; y--) {
                        int full = 1;
                        for (int x = 0; x < COLS; x++) if (!board[y][x]) { full = 0; break; }
                        if (full) {
                            cleared++;
                            for (int yy = y; yy > 0; yy--)
                                memcpy(board[yy], board[yy-1], sizeof(board[0]));
                            memset(board[0], 0, sizeof(board[0]));
                            y++;
                        }
                    }
                    if (cleared) {
                        int pts[5] = {0, 100, 300, 500, 800};
                        score += pts[cleared] * level;
                        lines += cleared;
                        level = 1 + lines / 10;
                        gravity = 0.8f + (level-1) * 0.25f;
                        timer = 0.0f;
                        PlaySound(sClear);
                    } else {
                        PlaySound(sLand);
                    }
                    SPAWN();
                    if (gameOver) PlaySound(sOver);
                }
            }
        }

        BeginDrawing();
            ClearBackground((Color){18,18,24,255});

            int sw = GetRenderWidth(), sh = GetRenderHeight();
            int cell = sh / (ROWS + 1);
            if (cell > sw / (COLS + 7)) cell = sw / (COLS + 7);
            int boardW = COLS*cell, boardH = ROWS*cell;
            int ox = (sw - boardW - 6*cell) / 2;
            if (ox < cell) ox = cell;
            int oy = (sh - boardH) / 2;
            int px = ox + boardW + cell;
            int fs = cell * 3 / 4;
            if (fs < 10) fs = 10;

            DrawRectangleLinesEx((Rectangle){ox-2, oy-2, boardW+4, boardH+4}, 2, (Color){90,90,110,255});

            for (int y = 0; y < ROWS; y++)
                for (int x = 0; x < COLS; x++) {
                    if (!board[y][x]) continue;
                    DrawRectangle(ox+x*cell, oy+y*cell, cell-1, cell-1, COLORS[board[y][x]]);
                    DrawRectangleLines(ox+x*cell, oy+y*cell, cell-1, cell-1, (Color){0,0,0,120});
                }

            if (!gameOver) {
                int grounded = collide(board, cur.s, cur.x, cur.y+1);
                Color pc = COLORS[cur.type+1];
                float pulse = 0.0f;
                if (grounded) {
                    // Resting on the stack but not locked yet (heavy gravity
                    // still has to tick): pulse the fill toward white and blink
                    // a bounding-box halo so it reads as "about to lock".
                    pulse = 0.5f + 0.5f * sinf((float)GetTime() * 9.0f);
                    float mix = 0.30f + 0.45f * pulse;
                    pc.r = (unsigned char)(pc.r + (255 - pc.r) * mix);
                    pc.g = (unsigned char)(pc.g + (255 - pc.g) * mix);
                    pc.b = (unsigned char)(pc.b + (255 - pc.b) * mix);

                    int minx = 4, miny = 4, maxx = -1, maxy = -1;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++)
                            if (cur.s[y][x]) {
                                if (x < minx) minx = x;
                                if (x > maxx) maxx = x;
                                if (y < miny) miny = y;
                                if (y > maxy) maxy = y;
                            }
                    Rectangle box = {
                        ox + (cur.x+minx)*cell, oy + (cur.y+miny)*cell,
                        (maxx-minx+1)*cell, (maxy-miny+1)*cell
                    };
                    Color halo = pulse > 0.5f ? (Color){255,255,255,255}
                                              : (Color){255,210,90,255};
                    DrawRectangleLinesEx(box, 3.0f, halo);
                }
                // Ghost drop profile: 1-pixel strip just below bottom frame
                if (!gameOver && !paused) {
                    int gy = drop_row(board, cur.s, cur.x, cur.y);
                    int stripY = oy + ROWS * cell + 1; // just below bottom frame
                    for (int x = 0; x < COLS; x++) {
                        // check if any part of ghost covers this column at or below bottom?
                        for (int y = 0; y < 4; y++) {
                            if (!cur.s[y][x - cur.x >= 0 && (x - cur.x) < 4 ? (x - cur.x) : -1]) continue; // easier loop
                        }
                    }
                    // simpler: for each cell in ghost, if gy+y >= ROWS draw a 1-px wide bar at stripY under that column
                    for (int y = 0; y < 4; y++) {
                        for (int x = 0; x < 4; x++) {
                            if (!cur.s[y][x]) continue;
                            int by = gy + y;
                            if (by >= ROWS) {
                                int cx = cur.x + x;
                                if (cx >= 0 && cx < COLS) {
                                    DrawRectangle(ox + cx*cell, stripY, cell-1, 2, (Color){255,255,255,160});
                                }
                            }
                        }
                    }
                }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++)
                        if (cur.s[y][x] && cur.y+y >= 0) {
                            DrawRectangle(ox+(cur.x+x)*cell, oy+(cur.y+y)*cell, cell-1, cell-1, pc);
                            DrawRectangleLines(ox+(cur.x+x)*cell, oy+(cur.y+y)*cell, cell-1, cell-1, (Color){0,0,0,120});
                        }
            }

            DrawText("NEXT", px, oy, fs, RAYWHITE);
            int ns = SIZES[nextType];
            for (int y = 0; y < ns; y++)
                for (int x = 0; x < ns; x++)
                    if (SHAPES[nextType][y][x])
                        DrawRectangle(px + x*cell, oy + fs + cell + y*cell, cell-1, cell-1, COLORS[nextType+1]);

            DrawText(TextFormat("SCORE %d", score), px, oy + boardH - fs*5, fs, RAYWHITE);
            DrawText(TextFormat("LINES %d", lines), px, oy + boardH - fs*4, fs, RAYWHITE);
            DrawText(TextFormat("LEVEL %d", level), px, oy + boardH - fs*3, fs, RAYWHITE);

            int cy = oy + 7*cell;
            DrawText("CONTROLS", px, cy, fs, (Color){180,180,200,255});
            static const char *ctl[][2] = {
                { "LEFT / RIGHT", "move" },
                { "DOWN",        "soft drop" },
                { "UP",          "rotate" },
                { "SPACE",       "hard drop" },
                { "P / PAUSE",   "pause" },
                { "ENTER",       "restart" },
                { "ESC",         "quit" },
            };
            int cfs = fs * 2 / 3;
            if (cfs < 10) cfs = 10;
            int colX = 0;
            for (int i = 0; i < 7; i++) {
                int w = MeasureText(ctl[i][0], cfs);
                if (w > colX) colX = w;
            }
            colX += px + cfs;
            for (int i = 0; i < 7; i++) {
                int ly = cy + fs + i * (cfs + cfs/3);
                DrawText(ctl[i][0], px, ly, cfs, RAYWHITE);
                DrawText(ctl[i][1], colX, ly, cfs, (Color){160,160,180,255});
            }

            if (gameOver) {
                DrawRectangle(0, 0, sw, sh, (Color){0,0,0,180});
                int big = fs * 2;
                DrawText("GAME OVER", (sw - MeasureText("GAME OVER", big))/2, sh/2 - big, big, RAYWHITE);
                DrawText("ENTER to restart", (sw - MeasureText("ENTER to restart", fs))/2, sh/2 + fs, fs, RAYWHITE);
            }
        EndDrawing();
    }

    UnloadSound(sMove); UnloadSound(sRot); UnloadSound(sLand);
    UnloadSound(sClear); UnloadSound(sOver);
    #if defined(PLATFORM_DESKTOP)
    UnloadMusicStream(bgm);
    #endif
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
