/* CPU reference renderer for mandelbrot.c, used to check the GPU's precision.
 *
 * Same orbit and same colouring as the shader, but in a type you choose:
 * -mode 0 float32, 1 double, 2 __float128 (113-bit, the trustworthy one).
 * -counts 1 writes raw escape counts as int16 instead of colour, which is the
 * only honest way to compare two orbits at depth. Writes to stdout; see
 * mandelbrot_check.py.
 *
 *   gcc -O2 mandelbrot_ref.c -o mandelbrot_ref -lquadmath -lm
 *   ./mandelbrot_ref -cx -0.74 -cy 0.13 -span 1e-8 -iter 40000 \
 *                    -x 740 -y 450 -cw 200 -ch 150 -w 1680 -h 1050 -mode 2 > ref.rgb
 *
 * Two things that will silently ruin a comparison if you change them:
 *   - Rows are emitted bottom-up (row 0 is the bottom of the image), which is
 *     what the GPU side's rlReadScreenPixels gives too, so raw-vs-raw needs no
 *     flip. A PNG from TakeScreenshot is top-down and DOES need one.
 *   - The colouring must stay in step with the shader's `shade()`. Keep the
 *     smoothstep/0.04 band term below in sync or the colour error means
 *     nothing (see NOTES.md).
 * W/H are the real framebuffer size, not the 1920x1080 InitWindow asks for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <quadmath.h>

static double smoothstep(double a, double b, double x)
{
    double t = (x - a)/(b - a);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t*t*(3.0 - 2.0*t);
}

static void hsv2rgb_d(double h, double s, double v, double *out)
{
    double f = fmod(h*6.0, 6.0);
    int i = (int)f;
    double ff = f - i;
    double p = v*(1.0 - s), q = v*(1.0 - s*ff), t = v*(1.0 - s*(1.0 - ff));
    switch (i%6) {
        case 0: out[0]=v; out[1]=t; out[2]=p; break;
        case 1: out[0]=q; out[1]=v; out[2]=p; break;
        case 2: out[0]=p; out[1]=v; out[2]=t; break;
        case 3: out[0]=p; out[1]=q; out[2]=v; break;
        case 4: out[0]=t; out[1]=p; out[2]=v; break;
        default: out[0]=v; out[1]=p; out[2]=q; break;
    }
}

/* Escape-time colour, identical maths to the shader. */
static void shade(int i, double r2, int maxIter, double *col)
{
    if (i >= maxIter) { col[0] = col[1] = col[2] = 0.0; return; }
    double nu = (double)i + 1.0 - log2(log(r2)/log(262144.0));
    double t = sqrt(nu/(double)maxIter);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    hsv2rgb_d(fmod(t*4.0 + 0.55 + smoothstep(400.0, 1500.0, nu)*nu*0.04, 1.0),
              0.78 - 0.38*t, 1.0, col);
}

#define ORBIT(ty, bail)                                                          \
    ty re = (ty)cx + ((ty)ux - 0.5)*(ty)span*(ty)aspect;                         \
    ty im = (ty)cy + ((ty)uy - 0.5)*(ty)span;                                    \
    ty zr = 0, zi = 0, r2 = 0;                                                   \
    for (i = 0; i < maxIter; i++) {                                              \
        ty nzr = zr*zr - zi*zi + re, nzi = 2*zr*zi + im;                          \
        zr = nzr; zi = nzi;                                                      \
        r2 = zr*zr + zi*zi;                                                      \
        if (r2 > (ty)bail) break;                                                \
    }

int main(int argc, char **argv)
{
    double cx = 0.0, cy = 0.0, span = 1.0;
    int maxIter = 1000, mode = 2, counts = 0;
    int X0 = 0, Y0 = 0, CW = 200, CH = 150, W = 1680, H = 1050;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) { fprintf(stderr, "dangling %s\n", argv[i]); return 2; }
        double d = atof(argv[i+1]);
        int n = atoi(argv[i+1]);
        const char *k = argv[i];
        if      (!strcmp(k, "-cx"))   cx = d;
        else if (!strcmp(k, "-cy"))   cy = d;
        else if (!strcmp(k, "-span")) span = d;
        else if (!strcmp(k, "-iter")) maxIter = n;
        else if (!strcmp(k, "-x"))    X0 = n;
        else if (!strcmp(k, "-y"))    Y0 = n;
        else if (!strcmp(k, "-cw"))   CW = n;
        else if (!strcmp(k, "-ch"))   CH = n;
        else if (!strcmp(k, "-w"))    W = n;
        else if (!strcmp(k, "-h"))    H = n;
        else if (!strcmp(k, "-mode")) mode = n;
        else if (!strcmp(k, "-counts")) counts = n;
        else { fprintf(stderr, "unknown flag %s\n", k); return 2; }
    }

    double aspect = (double)W/(double)H;
    unsigned char *img = malloc((size_t)CW*CH*3);
    short *cnt = malloc((size_t)CW*CH*sizeof(short));
    if (!img || !cnt) { fprintf(stderr, "oom\n"); return 2; }

    for (int yy = 0; yy < CH; yy++) for (int xx = 0; xx < CW; xx++) {
        double ux = (X0 + xx + 0.5)/(double)W;   /* y grows up, as in the shader */
        double uy = (Y0 + yy + 0.5)/(double)H;
        double col[3] = { 0.0, 0.0, 0.0 };
        int i;
        if (mode == 0) {
            ORBIT(float, 262144.0f)
            if (i < maxIter) shade(i, (double)r2, maxIter, col);
        } else if (mode == 1) {
            ORBIT(double, 262144.0)
            if (i < maxIter) shade(i, r2, maxIter, col);
        } else {
            ORBIT(__float128, 262144.0Q)
            if (i < maxIter) shade(i, (double)r2, maxIter, col);
        }
        if (counts) {
            cnt[(size_t)yy*CW + xx] = (short)(i > 32000 ? 32000 : i);
        } else {
            for (int k = 0; k < 3; k++) {
                double v = col[k]*255.0 + 0.5;
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                img[((size_t)yy*CW + xx)*3 + k] = (unsigned char)v;
            }
        }
    }

    size_t n = counts ? (size_t)CW*CH*sizeof(short) : (size_t)CW*CH*3;
    fwrite(counts ? (void *)cnt : (void *)img, 1, n, stdout);
    free(img);
    free(cnt);
    return 0;
}
