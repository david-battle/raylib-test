#include "raylib.h"
// GLAD is needed for the fp64 uniform setters: raylib only pulls glad.h into its
// own translation units, so clients get no GL prototypes (and raylib 6 has no
// double uniform API -- SetShaderValue and rlSetUniform only do floats).
#include "external/glad.h"
// rlgl.h declares the raw GL layer raylib does not re-export: there is no
// float/ RGBA32F texture entry point or sampler binding in the public API, and
// this reference table needs both. It expects glad.h to have come first.
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// pert.c -- Mandelbrot explorer by perturbation theory.
//
// Same escape-time colouring and same one-fullscreen-fragment-shader structure
// as mandelbrot.c, but the per-pixel orbit is carried as the *difference* from a
// single shared reference orbit instead of the orbit itself:
//
//     c = c0 + delta      z_n = Z_n + w_n      Z = orbit of c0, shared
//     w_(n+1) = 2*Z_n*w_n + w_n^2 + delta      (exact identity)
//
// Why this is the only way past ~1e-13: mandelbrot.c's fp64 path dies at the
// `dCenter + offset` add, which rounds to ulp(0.74) = 1.1e-16 no matter how
// small the offset is. The perturbation path never forms an absolute
// coordinate at all -- delta is always in units of the current span -- so that
// floor is simply absent. Measured against a __float128 CPU oracle, the fp64
// variant reproduces the reference to 0 membership errors and |dc| <= 4 on
// 30000-pixel crops at spans 1e-6..1e-16, where direct fp64 cannot represent
// the coordinates at all. See TODO.md for the measurement tables.
//
// Cost: one texelFetch per iteration, and no Z^2 term. On this GPU (2080
// Super, fp64 at 1/64 rate) the fp32 variant is the interactive one and the
// fp64 variant is the settled-frame one; the P key cycles them.
//
// Reference orbit: computed on the CPU in double, split into float hi/lo pairs
// so one RGBA32F texture can feed both the fp32 path (which reads the hi halves
// as plain floats) and the fp64 path (which adds them back as doubles). GLSL
// has no double sampler, and no compute shader on raylib's GL backend, so the
// table has to be a texture the fragment shader indexes by iteration.
//
// The reference must itself have a bounded orbit, which is the real limitation:
// it has to sit *inside* the set, while the interesting view coordinates sit on
// the boundary. RefFind() walks out from the view centre for the nearest pixel
// whose own orbit survives, so the two can differ by a few pixels. When it finds
// nothing nearby, the fp64 direct path takes over and the 1e-13 wall returns.
// ---------------------------------------------------------------------------

static const char *PERT_FS =
    "#version 400 core\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform vec2 res;\n"          // framebuffer size in pixels
    "uniform vec2 center;\n"       // fp32 copies, for the fallback path only
    "uniform float spanY;\n"
    "uniform float aspect;\n"
    "uniform int maxIter;\n"
    "uniform int path;\n"          // PATH_* below
    "uniform int refN;\n"          // entries valid in the reference texture
    "uniform sampler2D refOrb;\n"
    "uniform vec2 refOff;\n"       // (centre - reference) in pixels, fp32
    "uniform vec2 stepF;\n"        // per-pixel complex step, fp32
    "uniform dvec2 dCenter;\n"
    "uniform dvec2 dSpan;\n"
    "uniform dvec2 dInvRes;\n"
    "uniform dvec2 dRefOff;\n"     // same offset, fp64
    "uniform dvec2 dStep;\n"       // per-pixel complex step, fp64
    "const float BAIL2 = 262144.0;\n"
    // Must match REF_TEX_W. A power of two so the compiler turns i%TW into a
    // mask instead of an integer division in the inner loop.
    "#define TW 1024\n"
    "vec3 hsv2rgb(vec3 c)\n"
    "{\n"
    "    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0/3.0, 1.0/3.0))*6.0 - 3.0);\n"
    "    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);\n"
    "}\n"
    // ---- colouring, identical to mandelbrot.c so the two are comparable ------
    "vec3 shade(int i, int maxIter, float r2, vec2 fc)\n"
    "{\n"
    "    if (i >= maxIter) return vec3(0.0);\n"
    "    float nu = float(i) + 1.0 - log2(log(r2)/log(BAIL2));\n"
    "    float t = sqrt(clamp(nu/float(maxIter), 0.0, 1.0));\n"
    "    float amp = smoothstep(400.0, 1500.0, nu);\n"
    "    vec3 col = hsv2rgb(vec3(fract(t*4.0 + 0.55 + amp*nu*0.04), 0.78 - 0.38*t, 1.0));\n"
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
    "    if (path == 0) {\n"
    // ---- fp32 direct: the fallback when no reference orbit exists ------------
    "        vec2 uv = vec2(gl_FragCoord.x/res.x, 1.0 - gl_FragCoord.y/res.y);\n"
    "        vec2 p = center + vec2((uv.x - 0.5)*aspect, 0.5 - uv.y)*spanY;\n"
    "        vec2 z = vec2(0.0);\n"
    "        for (i = 0; i < maxIter; i++) {\n"
    "            z = vec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + p;\n"
    "            r2 = dot(z, z);\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "    } else if (path == 1) {\n"
    // ---- fp64 direct: same floor as mandelbrot.c, kept as an A/B oracle -------
    "        dvec2 uv = dvec2(gl_FragCoord.x*dInvRes.x, gl_FragCoord.y*dInvRes.y);\n"
    "        dvec2 p = dCenter + dvec2((uv.x - 0.5)*dSpan.x, (uv.y - 0.5)*dSpan.y);\n"
    "        dvec2 z = dvec2(0.0);\n"
    "        for (i = 0; i < maxIter; i++) {\n"
    "            z = dvec2(z.x*z.x - z.y*z.y, 2.0*z.x*z.y) + p;\n"
    "            r2 = float(dot(z, z));\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "    } else if (path == 2) {\n"
    // ---- fp32 perturbation: the interactive path -----------------------------
    // The offset is the exact integer pixel index, never uv-0.5 (that cancels
    // catastrophically once the span is small), and delta is formed as one
    // product so it never underflows early. Both scalings are fp32, which is
    // fine: their error is *relative*, so it shifts the image by a constant
    // fraction of a pixel at every depth.
    "        vec2 pixOff = vec2(gl_FragCoord.x - res.x*0.5, gl_FragCoord.y - res.y*0.5);\n"
    "        vec2 delta = (pixOff + refOff)*stepF;\n"
    "        vec2 w = vec2(0.0);\n"
    "        vec4 t = texelFetch(refOrb, ivec2(0, 0), 0);\n"
    "        for (i = 0; i < maxIter && i < refN; i++) {\n"
    "            vec2 Z = vec2(t.x, t.z);\n"
    // 2*Z*w is the exact linear part; the w*w term is what perturbation does
    // not linearise away, and it is small only for as long as |w| << |Z|.
    "            w = vec2(2.0*(Z.x*w.x - Z.y*w.y) + (w.x*w.x - w.y*w.y),\n"
    "                     2.0*(Z.x*w.y + Z.y*w.x) + (2.0*w.x*w.y)) + delta;\n"
    "            t = texelFetch(refOrb, ivec2((i + 1) % TW, (i + 1)/TW), 0);\n"
    "            vec2 z1 = vec2(t.x, t.z) + w;\n"
    "            r2 = dot(z1, z1);\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "    } else {\n"
    // ---- fp64 perturbation: the deep-zoom path -------------------------------
    // Same arithmetic as path 2, but every intermediate is fp64, including the
    // reference (a float hi/lo pair summed back into a double, because GLSL has
    // no double-precision sampler) and the pixel offset.
    "        dvec2 pixOff = dvec2(gl_FragCoord.x - res.x*0.5, gl_FragCoord.y - res.y*0.5);\n"
    "        dvec2 delta = (pixOff + dRefOff)*dStep;\n"
    "        dvec2 w = dvec2(0.0);\n"
    "        vec4 t = texelFetch(refOrb, ivec2(0, 0), 0);\n"
    "        for (i = 0; i < maxIter && i < refN; i++) {\n"
    "            dvec2 Z = dvec2(double(t.x) + double(t.y), double(t.z) + double(t.w));\n"
    "            w = dvec2(2.0*(Z.x*w.x - Z.y*w.y) + (w.x*w.x - w.y*w.y),\n"
    "                     2.0*(Z.x*w.y + Z.y*w.x) + (2.0*w.x*w.y)) + delta;\n"
    "            t = texelFetch(refOrb, ivec2((i + 1) % TW, (i + 1)/TW), 0);\n"
    "            dvec2 z1 = dvec2(double(t.x) + double(t.y), double(t.z) + double(t.w)) + w;\n"
    "            r2 = float(dot(z1, z1));\n"
    "            if (r2 > BAIL2) break;\n"
    "        }\n"
    "    }\n"
    "    if (path == 4) {\n"
    "        float fi = float(i);\n"
    "        finalColor = vec4(floor(fi/256.0)/255.0, mod(fi, 256.0)/255.0,\n"
    "                         i >= maxIter ? 1.0 : 0.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    finalColor = vec4(shade(i, maxIter, r2, gl_FragCoord.xy), 1.0);\n"
    "}";

// Bounding box of the set, as mandelbrot.c, for the reset view.
static const double BOX_X0 = -2.20, BOX_X1 = 0.80;
static const double BOX_Y0 = -1.35, BOX_Y1 = 1.35;

// Shader path values. These *must* match the branches in PERT_FS; a mismatch is
// silent, because the shader will happily run the branch you did not mean.
#define PATH_FP32_DIRECT 0
#define PATH_FP64_DIRECT 1
#define PATH_PERT32      2
#define PATH_PERT64      3

// What the P key cycles through. MODE_AUTO is resolved to one of the four above
// each frame, so it never reaches the shader.
#define MODE_AUTO     0
#define MODE_PERT32   1
#define MODE_PERT64   2
#define MODE_DIRECT64 3
#define MODE_COUNT    4
static const char *MODE_NAME[MODE_COUNT] = { "auto", "pert fp32", "pert fp64", "direct fp64" };

#define ZOOM_STEP   0.80     // wheel notch scales the span by this^wheel
// 1e-30 is where the fp32 path's delta (~1e-33) is still a normal float;
// the fp64 path has far more room, but one MIN_SPAN has to cover both and the
// iteration governor bites long before this.
#define MIN_SPAN    1e-30
#define MAX_SPAN    8.0
#define GLIDE       20.0f    // exponential approach rate for zoom/pan, per second
#define ITER_STEP   1.25     // manual iteration nudge per +/- press (multiplier)
#define ITER_SLOPE  900.0    // iterations to add per doubling of zoom
// Higher than mandelbrot.c's 600 because the fp64 perturbation frame is worth
// waiting for. Measured on this GPU: the watchdog kills the context somewhere
// between 1.0 and 1.2 s per frame at 1920x1080 fp64, and the loss is silent --
// the window just goes permanently black, which reads like a shader bug. Deep
// views are cheap (a repelling reference lets nearly every pixel bail early, so
// 1e-50 at 150926 iterations still renders), but a view whose reference is
// *attracting* runs every pixel to the cap and is the case that overruns.
// 700 ms keeps a clear margin. StepGovernor only reacts to the frame it just
// measured, so the first frame of a new view can still overrun; that is the
// remaining exposure.
#define FRAME_BUDGET_MS 700.0
#define ITER_START  4000     // first frame's count; climbs in well under a second
#define ITER_HARD_MAX 400000 // runaway guard on auto/+ only

// Reference texture layout. 1024x1024 entries is 4 MB and far more than the
// governor ever asks for; only the rows in use are uploaded each frame.
#define REF_TEX_W 1024
#define REF_TEX_H 1024
// How far the reference may drift from the view centre before it is re-chosen,
// and how far the search will walk looking for one. The search cost is
// maxIter double iterations per candidate, so a large radius is a visible stall
// -- but a reference further away than a few pixels makes w large, which is the
// thing perturbation is trying to avoid.
#define REF_KEEP_PX   6.0
#define REF_SEARCH_PX 32

static float *refData = NULL;       // RGBA32F, REF_TEX_W*REF_TEX_H*4
static unsigned int refTexId = 0;
static double refX = 0.0, refY = 0.0;
static double refZr = 0.0, refZi = 0.0;   // orbit value at entry refFilled-1
static int refFilled = 0;           // entries computed for (refX, refY)
static int refValid = 0;

static void RefUpload(void)
{
    int rows = (refFilled + REF_TEX_W - 1)/REF_TEX_W;
    rlUpdateTexture(refTexId, 0, 0, REF_TEX_W, rows,
                    PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, refData);
}

// Does c's own orbit stay bounded for n iterations? This is the criterion for a
// usable reference, and it is exactly the orbit the table then stores, so a
// candidate accepted here can never fail while its table is being extended.
static int RefSurvives(double cx, double cy, int n)
{
    double zr = 0.0, zi = 0.0;
    for (int i = 0; i < n; i++) {
        double t = zr*zr - zi*zi + cx;
        zi = 2.0*zr*zi + cy;
        zr = t;
        if (zr*zr + zi*zi > 4.0) return 0;
    }
    return 1;
}

// Extend the table to `need` entries. Entry i holds z_i split as two floats per
// component: the rounded value and the residual, so the pair reconstructs to
// ~48 bits. The fp32 path reads the high halves as ordinary floats; the fp64
// path adds each pair back as a double, which is all a GLSL sampler can carry.
// Entries below refFilled already describe this c0, so a growing iteration
// budget only costs the new tail. Returns 0 if the orbit escapes first.
static int RefExtend(int need)
{
    if (need > REF_TEX_W*REF_TEX_H) need = REF_TEX_W*REF_TEX_H;
    for (int i = refFilled; i < need; i++) {
        double zr = refZr*refZr - refZi*refZi + refX;
        double zi = 2.0*refZr*refZi + refY;
        refZr = zr; refZi = zi;
        if (zr*zr + zi*zi > 4.0) { refFilled = i; refValid = 0; return 0; }
        size_t o = ((size_t)(i/REF_TEX_W)*REF_TEX_W + (i%REF_TEX_W))*4;
        float hx = (float)zr, hy = (float)zi;
        refData[o + 0] = hx;
        refData[o + 1] = (float)(zr - (double)hx);
        refData[o + 2] = hy;
        refData[o + 3] = (float)(zi - (double)hy);
        refFilled = i + 1;
    }
    return 1;
}

// Adopt a fresh reference at (px, py): entry 0 is z_0 = 0, then fill the tail.
static void RefAdopt(double px, double py, int need)
{
    refX = px; refY = py;
    refZr = refZi = 0.0;
    refFilled = 1;
    refData[0] = refData[1] = refData[2] = refData[3] = 0.0f;
    refValid = RefExtend(need);
}

// Walks out from the view centre in whole pixels for the closest point whose
// orbit survives `need` iterations. The reference does not have to *be* the view
// centre -- it only has to be in the set, while the interesting coordinates are
// on the boundary -- which is what makes perturbation usable at depth.
static int RefFind(double cx, double cy, double span, double aspect,
                   int sw, int sh, int need, int radiusPx)
{
    double pxStep = span*aspect/sw, pyStep = span/sh;
    for (int r = 0; r <= radiusPx; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (r > 0 && abs(dx) != r && abs(dy) != r) continue;
                double px = cx + dx*pxStep, py = cy + dy*pyStep;
                if (!RefSurvives(px, py, need)) continue;
                RefAdopt(px, py, need);
                if (refValid) return 1;
            }
        }
    }
    refValid = 0;
    return 0;
}

// Make sure a reference exists that can serve `wantIter` iterations for this
// view, and upload it. Returns the number of iterations available (0 if none),
// which the caller clamps maxIter to: the shader indexes entry i+1 on step i, so
// refFilled entries support refFilled-1 iterations.
static int RefPrepare(double cx, double cy, double span, double aspect,
                      int sw, int sh, int wantIter)
{
    int need = wantIter + 1;
    if (need > REF_TEX_W*REF_TEX_H) need = REF_TEX_W*REF_TEX_H;

    if (refValid &&
        fabs(refX - cx) <= REF_KEEP_PX*span*aspect &&
        fabs(refY - cy) <= REF_KEEP_PX*span) {
        if (RefExtend(need)) { RefUpload(); return refFilled - 1; }
        // It escaped under the larger budget; fall through and look for another.
    }
    refValid = 0;
    if (!RefFind(cx, cy, span, aspect, sw, sh, need, REF_SEARCH_PX)) {
        // Nothing survives the full budget nearby. A reference that survives a
        // fraction of it still beats direct fp64's 1e-13 wall, since the caller
        // simply clamps maxIter to whatever it got.
        for (int f = 4; f >= 1 && !refValid; f *= 2) {
            int n = need/f;
            if (n < 64) break;
            RefFind(cx, cy, span, aspect, sw, sh, n, REF_SEARCH_PX);
        }
        if (!refValid) return 0;
    }
    RefUpload();
    return refFilled - 1;
}

// The HUD must not be drawn for --shot: the checker diffs the raw framebuffer
// against the oracle, and glyph pixels inside the crop read as render errors.
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

// How many steps to ask for. Same policy and rationale as mandelbrot.c: no
// constant cap, because the cost of one iteration is a property of the view
// (pixels that escape early are nearly free), so only measured frame time can
// decide. See NOTES.md on losing the GL context to the watchdog.
static int AutoIterations(double span, double homeSpan)
{
    double zoom = homeSpan/span;
    int n = 150 + (int)(ITER_SLOPE*log2(zoom < 1.0 ? 1.0 : zoom));
    if (n < 150) n = 150;
    if (n > ITER_HARD_MAX) n = ITER_HARD_MAX;
    return n;
}

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

int main(int argc, char **argv)
{
    // Headless render for the verification loop, replacing the string surgery in
    // mandelbrot_harness.py: --shot cx cy span iter path out.png writes out.png
    // and out.raw (RGBA, top-down, whole framebuffer -- mandelbrot_check.py's
    // expected GPU-side format).
    int shot = 0, shotPath = PATH_PERT64;
    double shotCx = 0.0, shotCy = 0.0, shotSpan = 1.0;
    int shotIter = 0, shotW = 1920, shotH = 1080;
    const char *shotOut = "pert.png";
    for (int i = 1; i < argc; i++) {
        // --size wins over --shot's own default, so it can appear either side.
        if (!strcmp(argv[i], "--size") && i+2 < argc) {
            shotW = atoi(argv[i+1]); shotH = atoi(argv[i+2]);
            i += 2;
        } else if (!strcmp(argv[i], "--shot") && i+5 < argc) {
            shot = 1;
            shotCx = atof(argv[i+1]); shotCy = atof(argv[i+2]);
            shotSpan = atof(argv[i+3]); shotIter = atoi(argv[i+4]);
            shotPath = atoi(argv[i+5]); shotOut = argv[i+6];
            i += 6;
        }
    }

    InitWindow(shot ? shotW : 1920, shot ? shotH : 1080, "Mandelbrot (perturbation)");
    if (!shot) {
        SetWindowState(FLAG_FULLSCREEN_MODE);
        SetWindowMonitor(SelectExternalMonitor());
    }
    SetTargetFPS(shot ? 0 : 60);

    Shader shader = LoadShaderFromMemory(NULL, PERT_FS);
    int locRes     = GetShaderLocation(shader, "res");
    int locCenter  = GetShaderLocation(shader, "center");
    int locSpan    = GetShaderLocation(shader, "spanY");
    int locAspect  = GetShaderLocation(shader, "aspect");
    int locIter    = GetShaderLocation(shader, "maxIter");
    int locPath    = GetShaderLocation(shader, "path");
    int locRefN    = GetShaderLocation(shader, "refN");
    int locRefSamp = GetShaderLocation(shader, "refOrb");
    int locRefOff  = GetShaderLocation(shader, "refOff");
    int locStepF   = GetShaderLocation(shader, "stepF");
    int locDCenter = GetShaderLocation(shader, "dCenter");
    int locDSpan   = GetShaderLocation(shader, "dSpan");
    int locDInvRes = GetShaderLocation(shader, "dInvRes");
    int locDRefOff = GetShaderLocation(shader, "dRefOff");
    int locDStep   = GetShaderLocation(shader, "dStep");

    // Reference orbit texture: zeroed, so entries the CPU has not written yet
    // read as Z=0 rather than as garbage.
    refData = (float *)calloc((size_t)REF_TEX_W*REF_TEX_H*4, sizeof(float));
    if (!refData) { TraceLog(LOG_ERROR, "reference table allocation failed"); return 1; }
    refTexId = rlLoadTexture(refData, REF_TEX_W, REF_TEX_H,
                             PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1);
    rlTextureParameters(refTexId, RL_TEXTURE_FILTER_NEAREST, RL_TEXTURE_FILTER_NEAREST);

    TraceLog(LOG_WARNING, "locs: dCenter %d dSpan %d dInvRes %d dRefOff %d dStep %d refOrb %d refN %d refOff %d stepF %d",
             locDCenter, locDSpan, locDInvRes, locDRefOff, locDStep, locRefSamp, locRefN, locRefOff, locStepF);
    int sw = GetRenderWidth(), sh = GetRenderHeight();
    double aspect = (double)sw/(double)sh;
    double homeSpan = (BOX_Y1 - BOX_Y0);
    if ((BOX_X1 - BOX_X0)/aspect > homeSpan) homeSpan = (BOX_X1 - BOX_X0)/aspect;
    double homeX = (BOX_X0 + BOX_X1)*0.5, homeY = (BOX_Y0 + BOX_Y1)*0.5;

    double cx = homeX, cy = homeY, span = homeSpan;
    double tcx = cx, tcy = cy, tspan = span;
    if (shot) {
        tcx = cx = shotCx; tcy = cy = shotCy; tspan = span = shotSpan;
    }
    double iterScale = 1.0;
    // --shot takes a shader path value (see the PATH_* block) rather than a UI
    // mode, so the verification loop can name the branch it wants exactly.
    int pathMode = MODE_AUTO;
    // --shot path 4 is the debug count renderer: it is path 3's arithmetic with
    // a different output encoding, so it has to resolve to MODE_PERT64 and then
    // get its shader path overridden below.
    int debugCounts = (shot && shotPath >= 4);
    if (shot) pathMode = (shotPath == PATH_PERT32) ? MODE_PERT32
                        : (shotPath == PATH_PERT64 || shotPath >= 4) ? MODE_PERT64
                                                    : MODE_DIRECT64;
    int heldPlus = 0, heldMinus = 0, heldAuto = 0, heldHud = 0, heldPath = 0;
    bool showHud = true;
    int frames = 0;

    while (!WindowShouldClose()) {
        if (!shot && (GetRenderWidth() != sw || GetRenderHeight() != sh)) {
            sw = GetRenderWidth(); sh = GetRenderHeight();
            aspect = (double)sw/(double)sh;
            homeSpan = (BOX_Y1 - BOX_Y0);
            if ((BOX_X1 - BOX_X0)/aspect > homeSpan) homeSpan = (BOX_X1 - BOX_X0)/aspect;
        }

        if (!shot) {
            if (IsKeyPressed(KEY_ESCAPE)) break;
            if (IsKeyPressed(KEY_SPACE)) { tcx = homeX; tcy = homeY; tspan = homeSpan; }
            if (KeyStroke(&heldHud, IsKeyDown(KEY_F1))) showHud = !showHud;
            if (KeyStroke(&heldAuto, IsKeyDown(KEY_A))) iterScale = 1.0;
            if (KeyStroke(&heldPath, IsKeyDown(KEY_P))) pathMode = (pathMode + 1)%MODE_COUNT;
            if (KeyStroke(&heldPlus, IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD))) {
                iterScale *= ITER_STEP;
                if (iterScale > 64.0) iterScale = 64.0;
            }
            if (KeyStroke(&heldMinus, IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)))
                iterScale /= ITER_STEP;

            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vector2 d = GetMouseDelta();
                double dx = -d.x/sw*span*aspect;
                double dy = d.y/sh*span;
                cx += dx; cy += dy; tcx += dx; tcy += dy;
            }

            float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) {
                double f = pow((double)ZOOM_STEP, wheel);
                Vector2 m = GetMousePosition();
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
            if (fabs(tcx - cx) < span*1e-13) cx = tcx;
            if (fabs(tcy - cy) < span*1e-13) cy = tcy;
            if (fabs(tspan - span) < span*1e-13) span = tspan;
        }

        double zoom = homeSpan/span;
        bool moving = (fabs(tcx - cx) > span*1e-9) || (fabs(tcy - cy) > span*1e-9) ||
                      (fabs(tspan - span) > span*1e-9);
        int want = shot ? shotIter
                        : (int)(AutoIterations(span, homeSpan)*iterScale + 0.5);
        if (want < 50) want = 50;
        if (want > ITER_HARD_MAX) want = ITER_HARD_MAX;
        int iter = want;
        if (!shot) StepGovernor(1000.0*(double)GetFrameTime(), want, &iter);

        // The reference has to be in hand before the path is chosen, since its
        // existence decides whether perturbation is available at all.
        int refMax = RefPrepare(cx, cy, span, aspect, sw, sh, want);
        if (refMax > 0 && iter > refMax) iter = refMax;

        int path;
        if (pathMode == MODE_AUTO) {
            // fp32 while anything is moving (it is ~60x cheaper at 1/64 fp64
            // rate on this GPU), fp64 once the view has settled.
            if (refMax <= 0) path = PATH_FP64_DIRECT;
            else path = moving ? PATH_PERT32 : PATH_PERT64;
        } else if (pathMode == MODE_DIRECT64) {
            path = PATH_FP64_DIRECT;
        } else {
            path = (pathMode == MODE_PERT32) ? PATH_PERT32 : PATH_PERT64;
            if (refMax <= 0) path = PATH_FP64_DIRECT;
        }

        if (debugCounts) path = shotPath;

        double stepX = span*aspect/sw, stepY = span/sh;
        double refOffX = (cx - refX)/stepX, refOffY = (cy - refY)/stepY;

        BeginDrawing();
            ClearBackground(BLACK);
            BeginShaderMode(shader);
                float fres[2] = { (float)sw, (float)sh };
                float fc[2] = { (float)cx, (float)cy };
                float fsp = (float)span, far = (float)aspect;
                // (centre - reference) expressed in whole pixels, so it can be
                // added to the exact integer pixel offset before the single
                // scaling multiply. Computing it in c units and adding it after
                // would lose it to the same cancellation the direct path suffers.
                float fRefOff[2] = { (float)refOffX, (float)refOffY };
                float fStep[2] = { (float)stepX, (float)stepY };
                int nIter = iter, nPath = path, nRef = refMax;
                SetShaderValue(shader, locRes, fres, SHADER_UNIFORM_VEC2);
                SetShaderValue(shader, locCenter, fc, SHADER_UNIFORM_VEC2);
                SetShaderValue(shader, locSpan, &fsp, SHADER_UNIFORM_FLOAT);
                SetShaderValue(shader, locAspect, &far, SHADER_UNIFORM_FLOAT);
                SetShaderValue(shader, locIter, &nIter, SHADER_UNIFORM_INT);
                SetShaderValue(shader, locPath, &nPath, SHADER_UNIFORM_INT);
                SetShaderValue(shader, locRefN, &nRef, SHADER_UNIFORM_INT);
                SetShaderValue(shader, locRefOff, fRefOff, SHADER_UNIFORM_VEC2);
                SetShaderValue(shader, locStepF, fStep, SHADER_UNIFORM_VEC2);
                // fp64 uniforms. raylib has no double uniform API, so set these
                // straight through GL while the program is bound.
                glUniform2d(locDCenter, cx, cy);
                glUniform2d(locDSpan, span*aspect, span);
                glUniform2d(locDInvRes, 1.0/sw, 1.0/sh);
                glUniform2d(locDRefOff, refOffX, refOffY);
                glUniform2d(locDStep, stepX, stepY);
                // Path 4 is the count encoder: it runs path 3's arithmetic, so
                // it needs the sampler bound too.
                if (path == PATH_PERT32 || path == PATH_PERT64 || debugCounts)
                    rlSetUniformSampler(locRefSamp, refTexId);
                DrawRectangle(0, 0, sw, sh, WHITE);
            EndShaderMode();

            if (showHud) {
                char l0[128], l1[160], l2[160], l3[128];
                snprintf(l0, sizeof l0, "center  %.8f %+.8fi   (%.4gx home)", cx, cy, zoom);
                snprintf(l1, sizeof l1, "span    %.3e   path %s%s", span, MODE_NAME[pathMode],
                         pathMode == MODE_AUTO ? (path == PATH_PERT32 ? " (moving)"
                                                                  : path == PATH_PERT64 ? " (settled)"
                                                                                          : " (no reference)") : "");
                if (refMax > 0) {
                    double offx = (cx - refX)/span*aspect, offy = (cy - refY)/span;
                    snprintf(l2, sizeof l2, "iter    %d of %d   ref %+.3f %+.3f px, %d entries",
                             iter, want, offx, offy, refMax);
                } else {
                    snprintf(l2, sizeof l2, "iter    %d of %d   no reference in %d px",
                             iter, want, REF_SEARCH_PX);
                }
                snprintf(l3, sizeof l3, "%d fps   |   wheel zoom  drag pan  +/- iter  A auto"
                                        "  P path  SPACE reset  F1 hud  ESC quit", GetFPS());
                if (!shot) DrawTextOutlined(l0, 24, 24, 22, (Color){ 235, 235, 235, 255 });
                if (!shot) DrawTextOutlined(l1, 24, 52, 22, (Color){ 235, 235, 235, 255 });
                if (!shot) DrawTextOutlined(l2, 24, 80, 22, (Color){ 235, 235, 235, 255 });
                if (!shot) DrawTextOutlined(l3, 24, sh - 34, 18, (Color){ 200, 200, 200, 255 });
            }
        EndDrawing();

        if (shot && frames == 0) {
            double mx = 0.0, my = 0.0;
            for (int k = 0; k <= refMax; k++) {
                mx = fmax(mx, fabs(refData[(size_t)k*4 + 0]) + fabs(refData[(size_t)k*4 + 1]));
                my = fmax(my, fabs(refData[(size_t)k*4 + 2]) + fabs(refData[(size_t)k*4 + 3]));
            }
            TraceLog(LOG_WARNING, "table: max|Zx|=%.17g max|Zy|=%.17g refX=%.17g refY=%.17g",
                     mx, my, refX, refY);
        }
        if (shot) {
            TraceLog(LOG_WARNING, "shot: path %d iter %d refN %d refValid %d "
                   "refOff %.10g %.10g step %.10g %.10g",
                   path, iter, refMax, refValid, refOffX, refOffY, stepX, stepY);
        }
        if (shot && ++frames >= 3) {
            unsigned char *px = rlReadScreenPixels(sw, sh);
            Image img = { 0 };
            img.data = px; img.width = sw; img.height = sh;
            img.mipmaps = 1; img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            char raw[512];
            snprintf(raw, sizeof raw, "%s.raw", shotOut);
            FILE *f = fopen(raw, "wb");
            if (f) { fwrite(px, 1, (size_t)sw*sh*4, f); fclose(f); }
            ExportImage(img, shotOut);
            TraceLog(LOG_INFO, "wrote %s and %s  (%dx%d, path %d, iter %d)",
                     shotOut, raw, sw, sh, path, iter);
            break;
        }
    }

    free(refData);
    rlUnloadTexture(refTexId);
    UnloadShader(shader);
    CloseWindow();
    return 0;
}