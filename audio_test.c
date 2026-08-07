#include "raylib.h"

int main(void) {
    InitAudioDevice();
    if (!IsAudioDeviceReady()) {
        TraceLog(LOG_ERROR, "Audio device did NOT initialize");
    } else {
        TraceLog(LOG_INFO, "Audio device initialized OK");
    }
    Sound s = LoadSound("resources/coin.wav");
    PlaySound(s);
    WaitTime(2.0);
    UnloadSound(s);
    CloseAudioDevice();
    return 0;
}
