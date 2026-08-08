#!/bin/bash
for f in resources/*.wav resources/*.ogg resources/*.mp3 resources/*.flac resources/*.xm; do
    echo "==> Playing: $f"
    ./audio_test "$f"
done
