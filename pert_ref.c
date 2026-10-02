/* pert_ref.c -- CPU oracle for pert.c, the perturbation Mandelbrot renderer.
 *
 * Renders a crop two ways so they can be diffed:
 *
 *   direct   -mode 0 float32, 1 double, 2 __float128.  Same orbit, coordinate
 *             convention (y grows up, rows bottom-up) and colouring as
 *             mandelbrot_ref.c / mandelbrot.c's shade().
 *
 *   perturb  -pert 1.  Reference tables are built in __float128, then the
 *             per-pixel loop is emulated at a chosen width:
 *               -wprec 0  fp32 w, fp32 reference        (a fp32 GPU path; fails,
 *                        see NOTES.md -- kept only as the measured counterexample)
 *               -wprec 1  quad w, quad reference, exact delta.  Algorithm check
 *                        with no rounding anywhere: must match -mode 2.
 *               -wprec 2  double w, double reference    (the candidate path)
 *               -wprec 3  double w, reference round-tripped through float
 *                        hi/lo pairs, i.e. exactly what a GLSL sampler2D can
 *                        hand back (GLSL has no double-precision samplers, so
 *                        an fp64 reference has to be stored as two floats)
 *             -rbase R   rebase every R iterations (0 = never).  Only valid
 *                        while |w| << |Z|; see the note at the rebase below.
 *
 *   gcc -O2 pert_ref.c -o pert_ref -lquadmath -lm
 *
 * Typical Phase 0 use:
 *
 *   ./pert_ref -cx -0.743643887037151 -cy 0.131825904205330 -span 1e-12 \
 *              -iter 40000 -x 700 -y 450 -cw 400 -ch 300 -mode 2 -counts 1 \
 *              > direct.raw
 *   ./pert_ref -cx -0.743643887037151 -cy 0.131825904205330 -span 1e-12 \
 *              -iter 40000 -x 700 -y 450 -cw 400 -ch 300 -pert 1 -wprec 3 \
 *              -counts 1 > pert.raw
 *   python3 mandelbrot_check.py direct.raw pert.raw 700 450 400 300 1680 1050
 *
 * Read the checker output as: membership errors are the correctness number and
 * must be 0. The count difference is the accuracy number; read the median
 * before the mean, since a few boundary pixels legitimately diverge for
 * thousands of iterations (chaos, not error). Colour output is only meaningful
 * at shallow spans: past ~1e-4 the shader's per-iteration hue bands make any
 * two renderers disagree in colour while membership stays exact (NOTES.md).
 *
 * Two things that will silently ruin a comparison if you change them:
 *   - Rows are emitted bottom-up (row 0 is the bottom), matching the GPU
 *     side's rlReadScreenPixels. A PNG from TakeScreenshot is top-down and DOES
 *     need one flip.
 *   - The colouring must stay in step with the shader's shade().
 *
 * W/H are the real framebuffer size, not whatever InitWindow asks for.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <quadmath.h>

#define BAIL2 262144.0

typedef struct { __float128 x, y; } qc;

static qc qadd(qc a, qc b) { qc r; r.x = a.x + b.x; r.y = a.y + b.y; return r; }
static qc qmul(qc a, qc b) { qc r; r.x = a.x*b.x - a.y*b.y; r.y = a.x*b.y + a.y*b.x; return r; }
static __float128 qnorm2(qc a) { return a.x*a.x + a.y*a.y; }

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

/* Escape-time colour, identical maths to the shader's shade(). */
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

/* ---- direct path (same shape as mandelbrot_ref.c) ------------------------ */

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

/* ---- reference tables ---------------------------------------------------- */

static qc *tabZ = NULL, *tabW = NULL;
static double *tabD = NULL;        /* 4 doubles per entry: Zx Zy Wx Wy       */
static float *tabHi = NULL;        /* 4 floats per entry, Z rounded to float */
static float *tabLo = NULL;        /* the residual a GLSL sampler can carry  */
static int tabN = 0;               /* entries written (stops at an escape)  */
static double gRefDX = 0.0, gRefDY = 0.0;   /* view centre minus reference */

static void free_tables(void)
{
    free(tabZ); tabZ = NULL;
    free(tabW); tabW = NULL;
    free(tabD); tabD = NULL;
    free(tabHi); tabHi = NULL;
    free(tabLo); tabLo = NULL;
}

/* Does c's own orbit survive maxIter iterations?  Used to test candidate
   reference points, and by -refauto to find one. */
static int quad_survives(double cx, double cy, int maxIter)
{
    __float128 zr = 0, zi = 0, cr = cx, ci = cy;
    for (int i = 0; i < maxIter; i++) {
        __float128 t = zr*zr - zi*zi + cr;
        zi = 2*zr*zi + ci; zr = t;
        if (zr*zr + zi*zi > 4) return 0;
    }
    return 1;
}

/* Spiral out from the view centre in whole pixels for the closest point whose
   own orbit survives.  The view can be centred on the boundary, where the
   interesting structure is, while the perturbation reference sits a few pixels
   away inside the set -- that separation is what makes perturbation usable at
   all, since the reference itself must live in the basin. */
static int refauto(double cx, double cy, double span, int maxIter,
                   double aspect, int W, int H, double *ox, double *oy)
{
    for (int r = 0; r <= 64; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (r > 0 && abs(dx) != r && abs(dy) != r) continue;
                double px = cx + dx*span/W*aspect;
                double py = cy + dy*span/H;
                if (quad_survives(px, py, maxIter)) { *ox = px; *oy = py; return 1; }
            }
        }
    }
    return 0;
}

static int build_tables(double cx, double cy, double d0, int n, int *refEscaped)
{
    qc c0;
    c0.x = (__float128)cx;
    c0.y = (__float128)cy;

    free_tables();
    tabZ = calloc((size_t)n + 2, sizeof *tabZ);
    tabW = calloc((size_t)n + 2, sizeof *tabW);
    tabD = calloc(((size_t)n + 2)*4, sizeof *tabD);
    tabHi = calloc(((size_t)n + 2)*4, sizeof *tabHi);
    tabLo = calloc(((size_t)n + 2)*4, sizeof *tabLo);
    if (!tabZ || !tabW || !tabD || !tabHi || !tabLo) return 0;

    tabZ[0].x = tabZ[0].y = tabW[0].x = tabW[0].y = 0;
    *refEscaped = 0;
    int last = n;
    for (int k = 0; k < n; k++) {
        tabZ[k+1] = qadd(qmul(tabZ[k], tabZ[k]), c0);
        if (qnorm2(tabZ[k+1]) > (__float128)BAIL2) { *refEscaped = 1; last = k+1; break; }
        tabW[k+1] = qadd(qadd(qmul(tabZ[k], tabW[k]), qmul(tabW[k], tabW[k])),
                         (qc){ (__float128)d0, 0 });
    }
    tabN = last;
    for (int k = 0; k <= tabN; k++) {
        tabD[k*4+0] = (double)tabZ[k].x;
        tabD[k*4+1] = (double)tabZ[k].y;
        tabD[k*4+2] = (double)tabW[k].x;
        tabD[k*4+3] = (double)tabW[k].y;
        for (int c = 0; c < 4; c++) {
            double v = tabD[k*4+c];
            tabHi[k*4+c] = (float)v;
            tabLo[k*4+c] = (float)(v - (double)(float)v);
        }
    }
    return last;
}

/* The rebase value is a first-order scaling of the exact nominal perturbation
 * W_n by delta/d0, as a COMPLEX multiply. That is only valid while |w| << |Z|:
 * the neglected term is (delta-d0)/2 * (zddot/zdot) relative, and zddot/zdot
 * grows like the orbit's own sensitivity, so by n ~ log2(1/d0) -- about 80
 * steps at span 1e-25 -- it is worthless and actively corrupts the orbit. It is
 * here for the fp32 path and for the early orbit only. */
#define REBASE(Wx, Wy, s0, s1) do { \
        float _x = (Wx), _y = (Wy); \
        wx = _x*(s0) - _y*(s1); \
        wy = _x*(s1) + _y*(s0); \
        last = i; \
    } while (0)

int main(int argc, char **argv)
{
    double cx = 0.0, cy = 0.0, span = 1.0;
    int maxIter = 1000, mode = 2, counts = 0, pert = 0, wprec = 3, rbase = 0, doRefauto = 0;
    int X0 = 0, Y0 = 0, CW = 200, CH = 150, W = 1680, H = 1050;
    double d0arg = 0.0;

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
        else if (!strcmp(k, "-pert")) pert = n;
        else if (!strcmp(k, "-wprec")) wprec = n;
        else if (!strcmp(k, "-rbase")) rbase = n;
        else if (!strcmp(k, "-d0"))   d0arg = d;
        else if (!strcmp(k, "-refauto")) doRefauto = n;
        else { fprintf(stderr, "unknown flag %s\n", k); return 2; }
    }

    double aspect = (double)W/(double)H;
    unsigned char *img = malloc((size_t)CW*CH*3);
    short *cnt = malloc((size_t)CW*CH*sizeof(short));
    if (!img || !cnt) { fprintf(stderr, "oom\n"); return 2; }

    /* d0 is the nominal offset the W table is built for. Only used by -rbase. */
    double d0 = 0.0;
    int refEscaped = 0;
    if (pert) {
        static const double DF[] = { 0.25, 0.0625, 0.015625, 0.00390625, 0.0009765625 };
        double refx = cx, refy = cy;
        if (doRefauto) {
            if (!refauto(cx, cy, span, maxIter, aspect, W, H, &refx, &refy)) {
                fprintf(stderr, "FAIL: -refauto found no surviving reference within 64 pixels\n");
                return 3;
            }
            fprintf(stderr, "refauto: reference at c=(%.17g,%.17g), %.4g px from view centre\n",
                    refx, refy, (refx-cx)*W/(span*aspect));
        }
        gRefDX = cx - refx; gRefDY = cy - refy;
        double cands[5];
        int nc = 0;
        if (d0arg != 0.0) cands[nc++] = d0arg;
        for (unsigned k = 0; k < sizeof DF/sizeof DF[0]; k++) {
            if (nc < 5) cands[nc++] = span*0.5*DF[k];
        }
        for (int k = 0; k < nc; k++) {
            int esc = 0;
            build_tables(refx, refy, cands[k], maxIter, &esc);
            /* Reject if the nominal point escapes: W then runs away instead of
             * staying bounded, which is the whole point of the table. */
            int nomEsc = 0;
            if (!esc) for (int j = 0; j <= tabN; j++) {
                if (qnorm2(qadd(tabZ[j], tabW[j])) > (__float128)BAIL2) { nomEsc = 1; break; }
            }
            if (!esc && !nomEsc) { d0 = cands[k]; refEscaped = 0; break; }
            refEscaped = esc || nomEsc;
        }
        if (d0 == 0.0) { d0 = span*0.5*DF[0]; build_tables(refx, refy, d0, maxIter, &refEscaped); }
        fprintf(stderr, "pert: d0 %.3e  wprec %d  rbase %d  iter %d  tabN %d\n",
                d0, wprec, rbase, maxIter, tabN);
        /* An escaping reference makes every pixel bogus, and it fails silently:
         * the loop just stops early and every count comes out the same. That
         * reads as a precision result, so refuse instead. */
        if (refEscaped) {
            fprintf(stderr, "FAIL: the reference orbit for c=(%.17g,%.17g) escapes at n=%d;"
                            " perturbation needs a centre in the set's basin.\n",
                    refx, refy, tabN);
            return 3;
        }
    }

    /* Uniforms the shader would receive, rounded to fp32 exactly as it would.
     * delta is then formed as (exact integer pixel offset * uSpan) * uInvRes,
     * in that order, so nothing underflows before the last multiply. */
    float fSpanX = (float)(span*aspect), fSpanY = (float)span;
    float fInvW = (float)(1.0/(double)W), fInvH = (float)(1.0/(double)H);
    float fD0 = (float)d0;

    for (int yy = 0; yy < CH; yy++) for (int xx = 0; xx < CW; xx++) {
        double ux = (X0 + xx + 0.5)/(double)W;   /* y grows up, as in the shader */
        double uy = (Y0 + yy + 0.5)/(double)H;
        double col[3] = { 0.0, 0.0, 0.0 };
        int i = 0;
        double r2 = 0.0;

        if (!pert) {
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
        } else if (wprec == 1) {
            /* Quad perturbation, exact delta: no rounding at all, so whatever
             * disagrees with -mode 2 is the algorithm and nothing else. */
            __float128 dx = (((X0 + xx + 0.5) - (double)W*0.5)*(__float128)(span*aspect))
                            / (__float128)W + (__float128)gRefDX;
            __float128 dy = (((Y0 + yy + 0.5) - (double)H*0.5)*(__float128)span)
                            / (__float128)H + (__float128)gRefDY;
            qc dq = { dx, dy }, wq = { 0, 0 };
            qc Zc = tabZ[0];
            int last = 0;
            for (i = 0; i < maxIter && i < tabN; i++) {
                if (rbase > 0 && i - last >= rbase) {
                    __float128 s0 = dx/(__float128)fD0, s1 = dy/(__float128)fD0;
                    qc Wv = tabW[i];
                    wq.x = Wv.x*s0 - Wv.y*s1;
                    wq.y = Wv.x*s1 + Wv.y*s0;
                    last = i;
                }
                qc zn = tabZ[i+1];
                wq = qadd(qadd(qmul((qc){ 2*Zc.x, 2*Zc.y }, wq), qmul(wq, wq)), dq);
                __float128 zx = zn.x + wq.x, zy = zn.y + wq.y;
                r2 = (double)(zx*zx + zy*zy);
                if (r2 > BAIL2) break;
                Zc = zn;
            }
            if (i < maxIter) shade(i, r2, maxIter, col);
        } else if (wprec == 0) {
            /* fp32 perturbation with an fp32 reference. Kept as the measured
             * counterexample: N roundings of a value that reaches O(1) put
             * ~N*2^-24 of absolute error into z, which no depth can fix. */
            float dx, dy;
            {
                float px = (float)((X0 + xx + 0.5) - (double)W*0.5);
                float py = (float)((Y0 + yy + 0.5) - (double)H*0.5);
                dx = (double)((px*fSpanX)*fInvW) + gRefDX;
                dy = (double)((py*fSpanY)*fInvH) + gRefDY;
            }
            float wx = 0.0f, wy = 0.0f;
            float zx = tabHi[0], zy = tabHi[1];
            int last = 0;
            for (i = 0; i < maxIter && i < tabN; i++) {
                if (rbase > 0 && i - last >= rbase) {
                    float s0 = dx/fD0, s1 = dy/fD0;
                    REBASE(tabHi[i*4+2], tabHi[i*4+3], s0, s1);
                }
                float nzx = tabHi[(i+1)*4+0], nzy = tabHi[(i+1)*4+1];
                float nwx = 2.0f*(zx*wx - zy*wy) + (wx*wx - wy*wy) + dx;
                float nwy = 2.0f*(zx*wy + zy*wx) + (2.0f*wx*wy)          + dy;
                wx = nwx; wy = nwy;
                float zzx = nzx + wx, zzy = nzy + wy;
                r2 = (double)(zzx*zzx + zzy*zzy);
                if (r2 > BAIL2) break;
                zx = nzx; zy = nzy;
            }
            if (i < maxIter) shade(i, r2, maxIter, col);
        } else {
            /* fp64 perturbation -- the candidate. delta keeps the fp32 pixel
             * offset the shader has, so it carries ~1e-7 *relative* error,
             * which is the entire point: that is depth-independent, while the
             * coordinate's absolute error in the direct fp64 path is not.
             * -wprec 3 additionally round-trips the reference through float
             * hi/lo pairs, which is all a GLSL sampler2D can carry. */
            double dx, dy;
            {
                float px = (float)((X0 + xx + 0.5) - (double)W*0.5);
                float py = (float)((Y0 + yy + 0.5) - (double)H*0.5);
                dx = (double)((px*fSpanX)*fInvW) + gRefDX;
                dy = (double)((py*fSpanY)*fInvH) + gRefDY;
            }
            double wx = 0.0, wy = 0.0, zx, zy;
            if (wprec == 3) {
                zx = (double)tabHi[0] + (double)tabLo[0];
                zy = (double)tabHi[1] + (double)tabLo[1];
            } else {
                zx = tabD[0];
                zy = tabD[1];
            }
            int last = 0;
            for (i = 0; i < maxIter && i < tabN; i++) {
                if (rbase > 0 && i - last >= rbase) {
                    double s0 = dx/(double)fD0, s1 = dy/(double)fD0;
                    double Wx, Wy;
                    if (wprec == 3) {
                        Wx = (double)tabHi[i*4+2] + (double)tabLo[i*4+2];
                        Wy = (double)tabHi[i*4+3] + (double)tabLo[i*4+3];
                    } else {
                        Wx = tabD[i*4+2];
                        Wy = tabD[i*4+3];
                    }
                    wx = Wx*s0 - Wy*s1;
                    wy = Wx*s1 + Wy*s0;
                    last = i;
                }
                double nzx, nzy;
                if (wprec == 3) {
                    nzx = (double)tabHi[(i+1)*4+0] + (double)tabLo[(i+1)*4+0];
                    nzy = (double)tabHi[(i+1)*4+1] + (double)tabLo[(i+1)*4+1];
                } else {
                    nzx = tabD[(i+1)*4+0];
                    nzy = tabD[(i+1)*4+1];
                }
                double nwx = 2.0*(zx*wx - zy*wy) + (wx*wx - wy*wy) + dx;
                double nwy = 2.0*(zx*wy + zy*wx) + (2.0*wx*wy)          + dy;
                wx = nwx; wy = nwy;
                double zzx = nzx + wx, zzy = nzy + wy;
                r2 = zzx*zzx + zzy*zzy;
                if (r2 > BAIL2) break;
                zx = nzx; zy = nzy;
            }
            if (i < maxIter) shade(i, r2, maxIter, col);
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
    free_tables();
    return 0;
}