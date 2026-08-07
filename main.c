#include "raylib.h"
#include <stdio.h>
#include <math.h>
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

    Image spriteImg = LoadImage("resources/sprite.png");
    for (int y = 0; y < spriteImg.height; y++) {
        for (int x = 0; x < spriteImg.width; x++) {
            Color px = GetImageColor(spriteImg, x, y);
            if (px.r > 200 && px.g < 80 && px.b > 200)
                ImageDrawPixel(&spriteImg, x, y, (Color){ px.r, px.g, px.b, 0 });
        }
    }
    Texture2D sprite = LoadTextureFromImage(spriteImg);
    UnloadImage(spriteImg);

    // Non-blocking UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(7777);
    inet_pton(AF_INET, "34.3.109.195", &serverAddr.sin_addr);

    Rectangle clickBox = { 100, 300, 150, 80 };
    bool boxHeld = false;
    Vector2 spritePos = { 180, 340 };
    Vector2 spriteVel = { 1.5f, 0.8f };
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

        float vx = spriteVel.x + GetRandomValue(-10, 10)*0.01f;
        float vy = spriteVel.y + GetRandomValue(-10, 10)*0.01f;
        float dx = mouse.x - (spritePos.x + 32);
        float dy = mouse.y - (spritePos.y + 32);
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist > 1.0f) {
            vx += (dx/dist)*0.04f;
            vy += (dy/dist)*0.04f;
        }
        spriteVel.x = vx < -3.0f ? -3.0f : (vx > 3.0f ? 3.0f : vx);
        spriteVel.y = vy < -3.0f ? -3.0f : (vy > 3.0f ? 3.0f : vy);
        spritePos.x += spriteVel.x;
        spritePos.y += spriteVel.y;
        if (spritePos.x < -64) spritePos.x = GetScreenWidth();
        else if (spritePos.x > GetScreenWidth()) spritePos.x = -64;
        if (spritePos.y < -64) spritePos.y = GetScreenHeight();
        else if (spritePos.y > GetScreenHeight()) spritePos.y = -64;

        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText("Press SPACE to send UDP ping", 100, 100, 30, DARKGRAY);
            DrawText(TextFormat("Sent: %d   Received: %d", packetsSent, packetsReceived), 100, 150, 20, GRAY);
            DrawRectangleRec(clickBox, boxHeld ? SKYBLUE : LIGHTGRAY);
            DrawText("Click me", clickBox.x + 20, clickBox.y + 30, 20, DARKGRAY);
            DrawTexture(sprite, spritePos.x, spritePos.y, WHITE);
            DrawFPS(10, 10);
        EndDrawing();
    }

    UnloadSound(echoSound);
    UnloadSound(clickSound);
    UnloadTexture(sprite);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
