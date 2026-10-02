#!/bin/bash
# Verify pert.c against the __float128 CPU oracle in pert_ref.c.
#
#   ./pert_verify.sh [W H]
#
# Renders the same view both ways and prints the membership / colour error. The
# oracle is exact at any depth, so this is the only honest check past span ~1e-13
# -- direct fp64 cannot even represent the coordinates there. It is slow
# (quad, one orbit per pixel), so keep the crop small.
#
# Note the resolution: the fp64 perturbation path costs ~2.3 s per frame at
# 1920x1080 and 6000 iterations, which overruns the ~2 s GPU watchdog and loses
# the GL context -- silently, as a black frame, with no driver message. A black
# render here is that, not a numerical failure, so the default is small. Run
# with "./pert_verify.sh 1920 1080" once you know the frame is affordable.
set -u
# Do not run this while the interactive ./pert is on the GPU. A second fullscreen
# instance contends for the device: shots then either segfault or silently dump a
# partially rendered frame, which reads as a numerical regression when nothing
# changed. Observed as span 1e-20 banding drifting 8.17 -> 15.98 and 1e-50
# segfaulting, both purely from running the verifier next to a live viewer.
W=${1:-480}
H=${2:-270}
CX=${CX:--1.7497}
CY=${CY:-0.0}
CROP=${CROP:-160}          # square-ish crop at the centre, in framebuffer pixels
X=$(( (W - CROP) / 2 ))
Y=$(( (H - CROP) / 2 ))

gcc -O2 -Wall -Wextra -I ~/raylib/src pert.c -o pert ~/raylib/src/libraylib.a \
    -lm -lpthread -ldl -lX11 || exit 1
[ -x ./pert_ref ] || gcc -O2 -Wall -Wextra pert_ref.c -o pert_ref -lquadmath -lm || exit 1

printf "%dx%d, crop %d,%d %dx%d, centre (%.12g, %.12g)\n" $W $H $X $Y $CROP $CROP $CX $CY
printf "%-8s %-7s %-9s %s\n" span iter path result
for S in ${SPANS:-1e-3 1e-6 1e-9 1e-12 1e-16 1e-20}; do
    N=$(python3 -c "import math;print(min(200000,int(150+900*math.log2(2.7/$S))))")
    ./pert_ref -cx "$CX" -cy "$CY" -span "$S" -iter "$N" -x "$X" -y "$Y" \
               -cw "$CROP" -ch "$CROP" -w "$W" -h "$H" -mode 2 > /tmp/pv_ref.rgb 2>/dev/null || {
        printf "%-8s %-7s oracle failed\n" "$S" "$N"; continue; }
    for P in 2 3; do
        NAME=$([ $P = 2 ] && echo pert32 || echo pert64)
        timeout 600 ./pert --size "$W" "$H" --shot "$CX" "$CY" "$S" "$N" "$P" \
            "/tmp/pv_$NAME.png" >/tmp/pv.log 2>&1
        if [ $? -ne 0 ]; then
            printf "%-8s %-7s %-9s shot failed\n" "$S" "$N" "$NAME"
            continue
        fi
        printf "%-8s %-7s %-9s " "$S" "$N" "$NAME"
        python3 mandelbrot_check.py /tmp/pv_ref.rgb "/tmp/pv_$NAME.png.raw" \
            "$X" "$Y" "$CROP" "$CROP" "$W" "$H" | head -1 | sed 's|.*raw ||'
    done
done