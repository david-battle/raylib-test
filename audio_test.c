#include "raylib.h"
#include <time.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        TraceLog(LOG_ERROR, "Usage: %s <path/to/soundfile>", argv[0]);
        return 1;
    }
    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_ERROR, "Audio device did NOT initialize");
    } else {
        TraceLog(LOG_INFO, "Audio device initialized OK");
    }
    Sound s = LoadSound(argv[1]);
    PlaySound(s);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (IsSoundPlaying(s)) {
        struct timespec req = { 0, 10000000 };
        while (nanosleep(&req, &req) == -1) continue;

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec)/1e9;
        if (elapsed >= 5.0) break;
    }

    UnloadSound(s);
    CloseAudioDevice();
    return 0;
}
