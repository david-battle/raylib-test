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
    "    vec3 col = hsv2rgb(vec3(fract(t*4.0 + 0.55), 0.78 - 0.38*t, 1.0));\n"
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
#define PRECISE_ITER 40000  // iteration cap in fp64 mode. The cap is nearly
                            // free: frame cost tracks the average escape count,
                            // not maxIter, so only pixels that never escape pay.

// OS key auto-repeat delivers extra KEY events while a key stays down, so
// IsKeyPressed() can fire several times per physical press. Latch on the rising
// edge of IsKeyDown() instead: one press, one action.
static bool KeyStroke(int *held, int down)
{
    int rising = down && !*held;
    *held = down;
    return rising;
}

// How many steps to allow. Empirically the count needed grows far faster than
// the zoom itself (steep in log2(zoom)), so this is a guess that errs low: past
// the cap the unsolved pixels just read as interior, i.e. black blobs. Use +/-
// when the picture looks over-filled. The cap differs per path because the
// fp64 path costs ~5x per iteration.
static int AutoIterations(double span, double homeSpan, int precise)
{
    double zoom = homeSpan/span;
    int n = 150 + (int)(400.0*log2(zoom < 1.0 ? 1.0 : zoom));
    int cap = precise ? PRECISE_ITER : 12000;
    if (n < 150) n = 150;
    if (n > cap) n = cap;
    return n;
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
        if (KeyStroke(&heldPlus, IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)))
            iterScale *= ITER_STEP;
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
        int iter = (int)(AutoIterations(span, homeSpan, precise)*iterScale + 0.5);
        if (iter < 50) iter = 50;
        if (iter > 4000000) iter = 4000000;

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
                if (iterScale == 1.0) snprintf(l2, sizeof l2, "iter    %d auto", iter);
                else snprintf(l2, sizeof l2, "iter    %d auto x%.2f", iter, iterScale);
                snprintf(l3, sizeof l3, "%d fps   |   wheel zoom  drag pan  +/- iter  A auto"
                                        "  P precision  SPACE reset  F1 hud  ESC quit", GetFPS());
                DrawText(l0, 24, 24, 22, (Color){ 235, 235, 235, 255 });
                DrawText(l1, 24, 52, 22, (Color){ 235, 235, 235, 255 });
                DrawText(l2, 24, 80, 22, (Color){ 235, 235, 235, 255 });
                DrawText(l3, 24, sh - 34, 18, (Color){ 150, 150, 150, 255 });
            }
        EndDrawing();
    }

    UnloadShader(shader);
    CloseWindow();
    return 0;
}