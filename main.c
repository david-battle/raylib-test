#include "raylib.h"
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(void) {
    InitWindow(1920, 1080, "Network + Input Test");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetTargetFPS(60);

    InitAudioDevice();
    Sound echoSound = LoadSound("resources/coin.wav");
    Sound clickSound = LoadSound("resources/buttonfx.wav");

    // Non-blocking UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(7777);
    inet_pton(AF_INET, "34.3.109.195", &serverAddr.sin_addr);

    Rectangle clickBox = { 100, 300, 150, 80 };
    bool boxHeld = false;
    int packetsSent = 0, packetsReceived = 0;

    while (!WindowShouldClose()) {
        // Send on spacebar, no Enter needed - IsKeyPressed fires once per press
        if (IsKeyPressed(KEY_SPACE)) {
            const char* msg = "ping";
            if (sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) { perror("sendto failed"); }
            packetsSent++;
        }

        // Poll for echo reply (non-blocking, so this is cheap every frame)
        char buf[512];
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n > 0) {
            packetsReceived++;
            PlaySound(echoSound);
        }

        // Mouse square test
        Vector2 mouse = GetMousePosition();
        boxHeld = CheckCollisionPointRec(mouse, clickBox);
        if (boxHeld && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            PlaySound(clickSound);
        }

        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText("Press SPACE to send UDP ping", 100, 100, 30, DARKGRAY);
            DrawText(TextFormat("Sent: %d   Received: %d", packetsSent, packetsReceived), 100, 150, 20, GRAY);
            DrawRectangleRec(clickBox, boxHeld ? SKYBLUE : LIGHTGRAY);
            DrawText("Click me", clickBox.x + 20, clickBox.y + 30, 20, DARKGRAY);
            DrawFPS(10, 10);
        EndDrawing();
    }

    UnloadSound(echoSound);
    UnloadSound(clickSound);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
