#include "raylib.h"
// GLAD is needed for the fp64 uniform setters: raylib only pulls glad.h into
// its own translation units, so clients get no GL prototypes (and raylib 6 has
// no double uniform API -- SetShaderValue and rlSetUniform only do floats).
#include "external/glad.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Mandelbrot explorer.
//
// The whole set is computed by one fullscreen fragment shader straight into
// the backbuffer -- no RenderTexture, so the WSLg/D3D12 RT Y-mirror does not
// apply here (only RT-presented chains flip; see NOTES.md). raylib's OpenGL
// backend has no compute shader support, so a fragment pass over one fullscreen
// quad is the GPU path: every pixel runs its own escape-time loop in parallel.
//
// Color is smooth (continuous) escape time: the hue comes from the fractional
// iteration count, so bands flow into each other instead of stepping. Points
// that never escape are the interior, drawn black.
//
// View state lives in double (panning accumulates over many frames and zoom
// goes deep). Shallow views are computed in plain float32, which stays exact
// enough to ~2e4 zoom and renders at 60 fps; past that the shader switches to
// real fp64 (dvec2, 15-16 digits), which removes the horizontal streaking. P
// forces either path. See NOTES.md.
// ---------------------------------------------------------------------------
static const char *MANDEL_FS =
    "#version 400 core\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform vec2 center;\n"      // complex center (re, im)
    "uniform vec2 res;\n"         // framebuffer size in pixels
    "uniform float spanY;\n"      // imaginary span across the screen height
    "uniform float aspect;\n"     // width / height
    "uniform int maxIter;\n"
    // "precise" is a reserved word from GLSL 400 on, hence useDouble.
    "uniform int useDouble;\n"   // 1 = fp64 path (see below)
    // fp64 copies of what the float32 path receives as floats, so the deep path
    // never inherits a center or span that was already rounded to float32.
    "uniform dvec2 dCenter;\n"   // complex center (re, im)
    "uniform dvec2 dSpan;\n"     // (x span, y span)
    "uniform dvec2 dInvRes;\n"   // (1/width, 1/height)
    // Bailout radius in |z|^2 (i.e. |z| > 512); large so the smooth-count
    // logarithm stays well conditioned right up to the cut.
    "const float BAIL2 = 262144.0;\n"
    "vec3 hsv2rgb(vec3 c)\n"
    "{\n"
    "    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0/3.0, 1.0/3.0))*6.0 - 3.0);\n"
    "    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
    "}\n"
    // ---- colouring shared by both paths -------------------------------------
    "vec3 shade(int i, int maxIter, float r2, vec2 fc)\n"
    "{\n"
    "    if (i >= maxIter) return vec3(0.0);\n"
    // Continuous escape time: how far past the bailout this pixel got, folded
    // back into the integer step count.
    "    float nu = float(i) + 1.0 - log2(log(r2)/log(BAIL2));\n"
    // sqrt() spreads the low-iteration bands out; without it the first few
    // steps eat most of the color wheel.
    "    float t = sqrt(clamp(nu/float(maxIter), 0.0, 1.0));\n"
    // t is normalized by maxIter, but the escape counts actually on screen sit
    // far below it: measured percentiles of nu against the 40000 cap are
    // p50/p95 = 97/315 at span 1e-3, 844/- at 1e-6, 1804/4231 at 1e-10, so t
    // never passes ~0.24 and one hue cycle spans thousands of iterations --
    // a flat wash. Fade in a per-iteration band term as nu grows, off below
    // ~400 (shallow views keep the smooth ramp: only 2.2% of pixels move at
    // span 1e-3) and full by ~1500 (span 1e-6 up), where 1/0.04 = 25
    // iterations per hue cycle makes neighbouring pixels separable. Measured
    // 4-pixel-scale contrast at span 1e-10: 11.6 -> 40.3.
    "    float amp = smoothstep(400.0, 1500.0, nu);\n"
    "    vec3 col = hsv2rgb(vec3(fract(t*4.0 + 0.55 + amp*nu*0.04), 0.78 - 0.38*t, 1.0));\n"
    // Break up the wide flat gradients that 8-bit output bands badly.
    "    float n = fract(sin(dot(fc, vec2(12.9898, 78.233)))*43758.5453);\n"
    "    return col + (n - 0.5)/255.0;\n"
    "}\n"
    "void main()\n"
    "{\n"
    // gl_FragCoord, not fragTexCoord: raylib's DrawRectangle emits the
    // shapes-atlas texcoords (SUPPORT_QUADS_DRAW_MODE), not 0..1 across the
    // quad, so the varying cannot be used as a screen position.
    "    vec3 col = vec3(0.0);\n"
    "    int i = 0;\n"
    "    float r2 = 0.0;\n"
    "    if (useDouble == 0) {\n"
    // Fast float32 path: exact enough while the view is shallow (the chaotic
    // derivative stays inside float32's ~7 digits), and ~5x cheaper.
    "        vec2 uv = vec2(gl_FragCoord.x/res.x, 1.0 - gl_FragCoord.y/res.y);\n"
    // Screen y grows downward, so the imaginary offset is (0.5 - y) * span.
    "        vec2 p = center + vec2((uv.x - 0.5)*aspect, 0.5 - uv.y)*spanY;\n"
    "        vec2 z = vec2(0.0);\n"
    "        for (i = 0; i < maxIter; i++) {\n"
    "            z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + p;\n"
    "            r2 = dot(z, z);\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "        col = shade(i, maxIter, r2, gl_FragCoord.xy);\n"
    "    } else {\n"
    // Deep path. Everything stays in fp64, including the per-pixel offset:
    // computing that offset in float32 would inject exactly the round-off this
    // path exists to remove. gl_FragCoord.y is measured downward, so the
    // imaginary offset is (uv.y - 0.5), matching the float path's 0.5 - uv.y
    // after its own flip.
    "        dvec2 uv = dvec2(gl_FragCoord.x*dInvRes.x, gl_FragCoord.y*dInvRes.y);\n"
    "        dvec2 p = dCenter + dvec2((uv.x - 0.5)*dSpan.x,\n"
    "                                 (uv.y - 0.5)*dSpan.y);\n"
    "        dvec2 z = dvec2(0.0);\n"
    "        for (i = 0; i < maxIter; i++) {\n"
    "            z = dvec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + p;\n"
    "            r2 = float(dot(z, z));\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "        col = shade(i, maxIter, r2, gl_FragCoord.xy);\n"
    "    }\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";

// Bounding box of the set (re in [-2.05, 0.65], im in [-1.16, 1.16]) padded a
// little, used to frame the whole set on the reset view.
static const double BOX_X0 = -2.20, BOX_X1 = 0.80;
static const double BOX_Y0 = -1.35, BOX_Y1 = 1.35;

#define ZOOM_STEP   0.80     // wheel notch scales the span by this^wheel
#define MIN_SPAN    1e-9     // stops the view collapsing to a single point
#define MAX_SPAN    8.0
#define GLIDE       20.0f    // exponential approach rate for zoom/pan, per second
#define ITER_STEP   1.25     // manual iteration nudge per +/- press (multiplier)
#define PRECISE_ZOOM 2e4     // auto-switch to the fp64 path past this
#define ITER_SLOPE  900.0    // iterations to add per doubling of zoom
#define FRAME_BUDGET_MS 600.0  // governor target; ~3x headroom to the ~2 s
                               // watchdog. Measured: 99%-interior fp64 view at
                               // 12682 iterations = 1.1 s at 1680x1050, so the
                               // budget is what keeps deep views off the cliff.
#define ITER_START  4000      // first frame's count; climbs to the target in
                               // well under a second, and avoids an unaffordable
                               // opening frame
#define ITER_HARD_MAX 400000  // runaway guard on auto/+ only. Unreachable in
                               // practice: MIN_SPAN bounds zoom, so auto tops out
                               // near 30000 and the governor trims before this.

// Text over a fractal is unreadable whenever the fractal happens to be as light
// as the glyphs, so stamp the string at the 8 neighbouring offsets in black and
// then draw the fill on top. A backing bar would work too, but it hides the
// image underneath and reads as a sticker; this keeps the fractal visible
// through the gaps between and inside glyphs. The offset tracks the glyph size,
// otherwise a 1px outline on 22px text reads as a smudge.
static void DrawTextOutlined(const char *text, int x, int y, int size, Color fill)
{
    int o = (size >= 20) ? 2 : 1;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dy == 0) continue;
        DrawText(text, x + dx*o, y + dy*o, size, BLACK);
    }
    DrawText(text, x, y, size, fill);
}

// OS key auto-repeat delivers extra KEY events while a key stays down, so
// IsKeyPressed() can fire several times per physical press. Latch on the rising
// edge of IsKeyDown() instead: one press, one action.
static bool KeyStroke(int *held, int down)
{
    int rising = down && !*held;
    *held = down;
    return rising;
}

// How many steps to ask for. Empirically the count needed grows far faster than
// the zoom itself (steep in log2(zoom)), and past what a pixel gets it just
// reads as interior, i.e. a black blob -- so under-asking costs visible detail.
// There is deliberately no ceiling here: frame cost is governed by measurement
// in StepGovernor() instead, which trims only what the machine cannot afford.
// That also means asking high is nearly free: pixels that escape early cost the
// same at 25000 iterations as at 5000.
static int AutoIterations(double span, double homeSpan)
{
    double zoom = homeSpan/span;
    int n = 150 + (int)(ITER_SLOPE*log2(zoom < 1.0 ? 1.0 : zoom));
    if (n < 150) n = 150;
    if (n > ITER_HARD_MAX) n = ITER_HARD_MAX;
    return n;
}

// Hold the frame inside a budget by trimming the iteration count, rather than
// capping it by a constant that some views exceed. This matters because the cost
// of one iteration is not a property of the shader: pixels that escape early are
// nearly free, and pixels deep in the interior pay the whole cap. A view that is
// 99% interior at 12000 iterations costs ~1.1 s on this GPU while a view full of
// filaments costs ~135 ms at the same count, so no static cap can keep both
// safe -- and overrunning the ~2 s watchdog loses the GL context and leaves a
// permanently black window, which looks exactly like a shader bug.
//
// So: `want` is what auto/+/- ask for, `iter` is what the frame can afford.
// Dropping below `want` (a `-` press, or zooming out) takes effect at once;
// climbing back up is gradual, which is also what keeps a zoom step from
// producing one unaffordable frame before the governor can react.
static void StepGovernor(double frameMs, int want, int *iter)
{
    static double ms = 0.0;
    static int cur = 0;
    if (ms <= 0.0) ms = frameMs;
    else ms += 0.25*(frameMs - ms);
    if (cur <= 0) cur = ITER_START;
    if (cur > want) cur = want;
    else if (ms > FRAME_BUDGET_MS) cur = (int)(cur*0.75);
    else if (ms < FRAME_BUDGET_MS*0.5) cur += cur/16 + 1;
    if (cur > want) cur = want;
    if (cur < 50) cur = 50;
    *iter = cur;
}

// Pick the external (non-built-in laptop) monitor, as ico.c/cell.c do.
static int SelectExternalMonitor(void)
{
    int count = GetMonitorCount();
    int best = GetCurrentMonitor();
    int bestArea = -1;
    for (int i = 0; i < count; i++) {
        const char *name = GetMonitorName(i);
        TraceLog(LOG_INFO, "Monitor %d: \"%s\" %dx%d", i, name,
                 GetMonitorWidth(i), GetMonitorHeight(i));
        int builtin = name &&
            (strstr(name, "eDP") || strstr(name, "LVDS") ||
             strstr(name, "Built") || strstr(name, "Laptop") ||
             strstr(name, "LCD") || strstr(name, "Internal"));
        if (builtin) continue;
        int area = GetMonitorWidth(i)*GetMonitorHeight(i);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    TraceLog(LOG_INFO, "Using monitor %d: %s", best, GetMonitorName(best));
    return best;
}

int main(void)
{
    InitWindow(1920, 1080, "Mandelbrot");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetWindowMonitor(SelectExternalMonitor());
    SetTargetFPS(60);

    Shader shader = LoadShaderFromMemory(NULL, MANDEL_FS);

    int locCenter  = GetShaderLocation(shader, "center");
    int locRes     = GetShaderLocation(shader, "res");
    int locSpan    = GetShaderLocation(shader, "spanY");
    int locAspect  = GetShaderLocation(shader, "aspect");
    int locIter    = GetShaderLocation(shader, "maxIter");
    int locDouble  = GetShaderLocation(shader, "useDouble");
    int locDCenter = GetShaderLocation(shader, "dCenter");
    int locDSpan   = GetShaderLocation(shader, "dSpan");
    int locDInvRes = GetShaderLocation(shader, "dInvRes");

    // Render size, not screen size: gl_FragCoord spans the framebuffer, and on
    // WSLg the two can disagree after a fullscreen monitor switch. Refreshed
    // each frame so a resize cannot leave the mapping stretched.
    int sw = GetRenderWidth(), sh = GetRenderHeight();
    double aspect = (double)sw/(double)sh;

    // Home view: center on the set's middle, span just wide enough to hold it.
    double homeSpan = (BOX_Y1 - BOX_Y0);
    if ((BOX_X1 - BOX_X0)/aspect > homeSpan) homeSpan = (BOX_X1 - BOX_X0)/aspect;
    double homeX = (BOX_X0 + BOX_X1)*0.5, homeY = (BOX_Y0 + BOX_Y1)*0.5;

    // Drawn view and goal view glide toward each other, so wheel zooms read as
    // a continuous dive instead of a jump cut.
    double cx = homeX, cy = homeY, span = homeSpan;
    double tcx = cx, tcy = cy, tspan = span;
    // Iteration budget: auto-scaled from the zoom, nudged by +/- as a
    // multiplier on the auto value (not a replacement, so deep zoom keeps
    // auto-scaling while you tune it), reset to pure auto with A.
    double iterScale = 1.0;
    // Precision path: 0 = auto (switch past PRECISE_ZOOM), 1 = always
    // fp64, 2 = never. P cycles it.
    int precMode = 0;
    int heldPlus = 0, heldMinus = 0, heldAuto = 0, heldHud = 0, heldPrec = 0;

    bool showHud = true;
    while (!WindowShouldClose()) {
        if (GetRenderWidth() != sw || GetRenderHeight() != sh) {
            sw = GetRenderWidth(); sh = GetRenderHeight();
            aspect = (double)sw/(double)sh;
            homeSpan = (BOX_Y1 - BOX_Y0);
            if ((BOX_X1 - BOX_X0)/aspect > homeSpan) homeSpan = (BOX_X1 - BOX_X0)/aspect;
        }

        if (IsKeyPressed(KEY_ESCAPE)) break;
        if (IsKeyPressed(KEY_SPACE)) { tcx = homeX; tcy = homeY; tspan = homeSpan; }
        if (KeyStroke(&heldHud, IsKeyDown(KEY_F1))) showHud = !showHud;
        if (KeyStroke(&heldAuto, IsKeyDown(KEY_A))) iterScale = 1.0;
        if (KeyStroke(&heldPrec, IsKeyDown(KEY_P))) precMode = (precMode + 1) % 3;
        if (KeyStroke(&heldPlus, IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))) {
            iterScale *= ITER_STEP;
            if (iterScale > 64.0) iterScale = 64.0;
        }
        if (KeyStroke(&heldMinus, IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)))
            iterScale /= ITER_STEP;

        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            // Grab-and-pull: the feature under the cursor tracks the cursor.
            // Screen x and imaginary y grow in opposite directions, so the y
            // term carries the opposite sign from the x term.
            double dx = -d.x/sw*span*aspect;
            double dy = d.y/sh*span;
            cx += dx; cy += dy; tcx += dx; tcy += dy;
        }

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            double f = pow((double)ZOOM_STEP, wheel);
            Vector2 m = GetMousePosition();
            // Complex point under the cursor in the current target view; keep
            // it under the cursor after the scale change.
            double mx = tcx + ((double)m.x/sw - 0.5)*tspan*aspect;
            double my = tcy + (0.5 - (double)m.y/sh)*tspan;
            double ax = mx - tcx, ay = my - tcy;
            tcx = mx - ax*f;
            tcy = my - ay*f;
            tspan *= f;
            if (tspan < MIN_SPAN) tspan = MIN_SPAN;
            if (tspan > MAX_SPAN) tspan = MAX_SPAN;
        }

        float dt = GetFrameTime();
        float k = 1.0f - expf(-GLIDE*dt);
        cx += (tcx - cx)*k;
        cy += (tcy - cy)*k;
        span += (tspan - span)*k;
        // Settle exactly on the target: exponential decay alone would creep for
        // seconds and never quite land.
        if (fabs(tcx - cx) < span*1e-13) cx = tcx;
        if (fabs(tcy - cy) < span*1e-13) cy = tcy;
        if (fabs(tspan - span) < span*1e-13) span = tspan;

        double zoom = homeSpan/span;
        int precise = (precMode == 1) ? 1 : (precMode == 2) ? 0 : (zoom > PRECISE_ZOOM);
        int want = (int)(AutoIterations(span, homeSpan)*iterScale + 0.5);
        if (want < 50) want = 50;
        if (want > 4000000) want = 4000000;
        int iter = want;
        StepGovernor(1000.0*(double)GetFrameTime(), want, &iter);

        BeginDrawing();
            ClearBackground(BLACK);
            BeginShaderMode(shader);
                float c[2] = { (float)cx, (float)cy };
                float res[2] = { (float)sw, (float)sh };
                float sp = (float)span;
                float ar = (float)aspect;
                SetShaderValue(shader, locCenter, c, SHADER_UNIFORM_VEC2);
                SetShaderValue(shader, locRes, res, SHADER_UNIFORM_VEC2);
                SetShaderValue(shader, locSpan, &sp, SHADER_UNIFORM_FLOAT);
                SetShaderValue(shader, locAspect, &ar, SHADER_UNIFORM_FLOAT);
                SetShaderValue(shader, locIter, &iter, SHADER_UNIFORM_INT);
                SetShaderValue(shader, locDouble, &precise, SHADER_UNIFORM_INT);

                // fp64 uniforms. raylib has no double uniform API (neither
                // SetShaderValue nor rlSetUniform knows about doubles), so set
                // these straight through GL while the program is bound.
                if (precise) {
                    glUniform2d(locDCenter, cx, cy);
                    glUniform2d(locDSpan, span*aspect, span);
                    glUniform2d(locDInvRes, 1.0/sw, 1.0/sh);
                }
                DrawRectangle(0, 0, sw, sh, WHITE);
            EndShaderMode();

            if (showHud) {
                char l0[128], l1[160], l2[128], l3[128];
                snprintf(l0, sizeof l0, "center  %.8f %+.8fi   (%.3gx home)", cx, cy, zoom);
                snprintf(l1, sizeof l1, "span    %.3e   prec %s", span,
                         precMode == 1 ? "double (forced)" : precMode == 2 ? "float (forced)" :
                         precise ? "double (auto)" : "float (auto)");
                if (iter < want) snprintf(l2, sizeof l2, "iter    %d budget (want %d)", iter, want);
                else if (iterScale == 1.0) snprintf(l2, sizeof l2, "iter    %d auto", iter);
                else snprintf(l2, sizeof l2, "iter    %d auto x%.2f", iter, iterScale);
                snprintf(l3, sizeof l3, "%d fps   |   wheel zoom  drag pan  +/- iter  A auto"
                                        "  P precision  SPACE reset  F1 hud  ESC quit", GetFPS());
                DrawTextOutlined(l0, 24, 24, 22, (Color){ 235, 235, 235, 255 });
                DrawTextOutlined(l1, 24, 52, 22, (Color){ 235, 235, 235, 255 });
                DrawTextOutlined(l2, 24, 80, 22, (Color){ 235, 235, 235, 255 });
                DrawTextOutlined(l3, 24, sh - 34, 18, (Color){ 200, 200, 200, 255 });
            }
        EndDrawing();
    }

    UnloadShader(shader);
    CloseWindow();
    return 0;
}