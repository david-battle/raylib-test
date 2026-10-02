#!/usr/bin/env python3
"""Build a headless one-shot renderer out of mandelbrot.c, for verification.

There is no way to get a deterministic frame out of the app itself: it wants a
window, glides toward the target view, and its real size is whatever the
fullscreen monitor gives it. So patch the frame loop into "render N frames,
dump, quit" by string surgery on a *copy* of the source, and compile that.

    python3 mandelbrot_harness.py mandelbrot.c /tmp/h.c
    gcc -O2 -I ~/raylib/src /tmp/h.c -o /tmp/h ~/raylib/src/libraylib.a \\
        -lm -lpthread -ldl -lX11
    /tmp/h <cx> <cy> <span> <precMode> <iterScale> <iterations|0> out.png [frames]

    precMode: 0 auto, 1 force fp64, 2 force float32
    iterations 0 keeps the app's AutoIterations; else overrides the cap
    frames: how many frames to render before dumping (default 12; the view has
        to have finished gliding, and it is timed from frame 3)

The framebuffer it gets is whatever WSLg hands the (non-fullscreen) window,
which is not the app's fullscreen monitor - pass the real size to
mandelbrot_ref's -w/-h, or the comparison is silently misaligned.

Writes out.png for eyeballing and out.raw for mandelbrot_check.py. The raw dump
is the untouched rlReadScreenPixels buffer: RGBA, 8 bits per channel, and
already flipped so row 0 is the TOP of the image (raylib flips it for you in
rlgl.h). The reference renders bottom-up, so the checker does the one flip.

Caveat: the patches below are literal string matches on mandelbrot.c, so they
break loudly (compile error) if that file's frame loop or variable names
change. Fix the pattern, don't start rewriting the renderer for this.
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()


def sub(old, new):
    global s
    if s.count(old) != 1:
        sys.exit("pattern not found exactly once in %s:\n%s" % (src, old))
    s = s.replace(old, new, 1)


sub('#include <math.h>', '#include <math.h>\n#include <stdlib.h>\n#include <stdio.h>\n#include "rlgl.h"')
sub('    SetTargetFPS(60);', '    SetTargetFPS(0);')
sub('int main(void)\n{', 'int main(int argc, char **argv)\n{')
sub('    int precMode = 0;', '''    int precMode = 0;
    /* harness args */
    int iterFix = (argc >= 7) ? atoi(argv[6]) : 0;
    if (argc >= 4) { cx = tcx = atof(argv[1]); cy = tcy = atof(argv[2]); span = tspan = atof(argv[3]); }
    if (argc >= 5) precMode = atoi(argv[4]);
    if (argc >= 6) iterScale = atof(argv[5]);
    const char *outName = (argc >= 8) ? argv[7] : "shot.png";
    int shotFrame = (argc >= 9) ? atoi(argv[8]) : 12;''')
sub('    while (!WindowShouldClose()) {', '''    int fc = 0;
    double t0 = 0.0;
    double tPrev = 0.0, worstFrame = 0.0;
    while (!WindowShouldClose()) {
        fc++;
        if (fc > 1) { double dt_ = GetTime() - tPrev; if (dt_ > worstFrame) worstFrame = dt_; }
        tPrev = GetTime();''')
sub('        int iter = (int)(AutoIterations(span, homeSpan, precise)*iterScale + 0.5);',
    '        int iter = iterFix ? iterFix : (int)(AutoIterations(span, homeSpan, precise)*iterScale + 0.5);')
sub('''        EndDrawing();
    }

    UnloadShader''', '''        EndDrawing();
        if (fc == 3) t0 = GetTime();
        if (fc == shotFrame) {
            TraceLog(LOG_INFO, "prec %d iter %d -> %.2f ms/frame", precise, iter,
                     1000.0*(GetTime() - t0)/(shotFrame - 3));
            TakeScreenshot(outName);
            {
                /* raw RGB for the checker; rows come back bottom-up */
                char rawName[256];
                snprintf(rawName, sizeof(rawName), "%s", outName);
                for (char *q = rawName; *q; q++) if (*q == '.' && q[1] == 'p' && q[2] == 'n' && q[3] == 'g' && !q[4]) *q = 0;
                strcat(rawName, ".raw");
                unsigned char *px = rlReadScreenPixels(sw, sh);
                FILE *f = fopen(rawName, "wb");
                if (!px || !f) TraceLog(LOG_WARNING, "raw dump failed");
                else { fwrite(px, 1, (size_t)sw*sh*4, f); fclose(f); }  /* RGBA */
                free(px);
            }
            /* A deep fp64 view can outrun the GPU watchdog: one frame takes
               seconds, the context resets, and every later frame renders
               nothing. The result is a black dump that reads exactly like a
               broken shader, so refuse to pass it off as a measurement. */
            if (worstFrame > 1.5) {
                fprintf(stderr, "WARN: slowest frame %.2f s - likely GPU watchdog reset; "
                        "lower the iteration count and re-run (see NOTES.md)\\n", worstFrame);
                CloseWindow();
                return 3;
            }
            break;
        }
    }

    UnloadShader''')
open(dst, 'w').write(s)
