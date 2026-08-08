#include "raylib.h"
#include <stdio.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

void HideCursorX11(unsigned long window);

int main(void) {
    InitWindow(1920, 1080, "Network + Input Test");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetTargetFPS(60);
    HideCursor();
    HideCursorX11((unsigned long)(uintptr_t)GetWindowHandle());

    InitAudioDevice();
    Sound echoSound = LoadSound("resources/coin.wav");
    Sound clickSound = LoadSound("resources/buttonfx.wav");
    Sound shootSound = LoadSound("resources/weird.wav");
    Sound hitCursorSound = LoadSound("resources/target.ogg");
    Sound selfHitSound = LoadSound("resources/spring.wav");
    Sound spriteClickSound = LoadSound("resources/sound.wav");

    Image baseImg = LoadImage("resources/sprite.png");
    for (int y = 0; y < baseImg.height; y++) {
        for (int x = 0; x < baseImg.width; x++) {
            Color px = GetImageColor(baseImg, x, y);
            if (px.r > 200 && px.g < 80 && px.b > 200)
                ImageDrawPixel(&baseImg, x, y, (Color){ px.r, px.g, px.b, 0 });
        }
    }
    // Glasses bridge connecting the two eye ovals (cols 30-35 x rows 44-45)
    ImageDrawRectangle(&baseImg, 30, 44, 6, 2, WHITE);
    // 4-frame sheet: center, look right, look left, blink.
    // Eyes are solid white ovals (left ~cols 22-30, right ~cols 34-42,
    // rows 40-49); draw 2x2 black pupils at (25,43) and (37,43) and shift.
    Image sheet = GenImageColor(64*4, 64, BLANK);
    for (int f = 0; f < 4; f++) ImageDrawImage(&sheet, baseImg, f*64, 0, WHITE);
    for (int f = 0; f < 4; f++) {
        int off = f*64;
        if (f < 3) {
            int dx = (f == 1) ? 2 : (f == 2) ? -2 : 0;
            for (int y = 43; y <= 44; y++) {
                ImageDrawPixel(&sheet, off + 25 + dx, y, BLACK);
                ImageDrawPixel(&sheet, off + 26 + dx, y, BLACK);
                ImageDrawPixel(&sheet, off + 37 + dx, y, BLACK);
                ImageDrawPixel(&sheet, off + 38 + dx, y, BLACK);
            }
        } else {
            ImageDrawRectangle(&sheet, off + 22, 44, 21, 2, WHITE);
        }
    }
    UnloadImage(baseImg);
    Texture2D sprite = LoadTextureFromImage(sheet);
    UnloadImage(sheet);

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
    int playerScore = 0, spriteScore = 0;

    #define MAX_DOTS 16
    Vector2 dotPos[MAX_DOTS] = { 0 }, dotVel[MAX_DOTS] = { 0 };
    bool dotActive[MAX_DOTS] = { false };
    int dotAge[MAX_DOTS] = { 0 };
    int fireCooldown = 60;
    int hitFlash = 0;
    int animTimer = 0, animFrame = 0;

    #define SCORE_TARGET 20
    #define MAX_CONFETTI 80
    Vector2 confettiPos[MAX_CONFETTI] = { 0 };
    float confettiVy[MAX_CONFETTI] = { 0 };
    Color confettiColor[MAX_CONFETTI] = { 0 };
    bool gameOver = false;
    int winner = 0;
    int gameOverTimer = 0;

    while (!WindowShouldClose()) {
        HideCursor();

        if (gameOver) {
            gameOverTimer--;
            BeginDrawing();
                if (winner == 1) {
                    ClearBackground(RAYWHITE);
                    for (int i = 0; i < MAX_CONFETTI; i++) {
                        confettiPos[i].y += confettiVy[i];
                        if (confettiPos[i].y > GetScreenHeight()) {
                            confettiPos[i].y = -10;
                            confettiPos[i].x = GetRandomValue(0, GetScreenWidth());
                        }
                        DrawCircleV(confettiPos[i], 6, confettiColor[i]);
                    }
                    DrawText("YOU WIN!", GetScreenWidth()/2 - MeasureText("YOU WIN!", 80)/2,
                             GetScreenHeight()/2 - 40, 80, DARKGRAY);
                } else {
                    ClearBackground(BLACK);
                    DrawText("YOU LOSE", GetScreenWidth()/2 - MeasureText("YOU LOSE", 80)/2,
                             GetScreenHeight()/2 - 40, 80, RED);
                }
                DrawFPS(10, 10);
            EndDrawing();
            if (gameOverTimer <= 0) {
                gameOver = false;
                playerScore = 0;
                spriteScore = 0;
                for (int i = 0; i < MAX_DOTS; i++) dotActive[i] = false;
            }
            continue;
        }

        if (playerScore >= SCORE_TARGET || spriteScore >= SCORE_TARGET) {
            gameOver = true;
            gameOverTimer = 180;
            winner = (playerScore >= SCORE_TARGET) ? 1 : 2;
            if (winner == 1) {
                for (int i = 0; i < MAX_CONFETTI; i++) {
                    confettiPos[i] = (Vector2){ GetRandomValue(0, GetScreenWidth()), GetRandomValue(-GetScreenHeight(), 0) };
                    confettiVy[i] = (float)GetRandomValue(2, 8);
                    confettiColor[i] = (Color){ GetRandomValue(0, 255), GetRandomValue(0, 255), GetRandomValue(0, 255), 255 };
                }
            }
        }

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

        // Click the sprite to teleport it somewhere random
        Rectangle spriteRect = { spritePos.x, spritePos.y, 64, 64 };
        if (CheckCollisionPointRec(mouse, spriteRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            playerScore++;
            PlaySound(spriteClickSound);
            spritePos.x = GetRandomValue(0, GetScreenWidth() - 64);
            spritePos.y = GetRandomValue(0, GetScreenHeight() - 64);
        }

        float vx = spriteVel.x + GetRandomValue(-10, 10)*0.01f;
        float vy = spriteVel.y + GetRandomValue(-10, 10)*0.01f;
        float dx = mouse.x - (spritePos.x + 32);
        float dy = mouse.y - (spritePos.y + 32);
        float dist = sqrtf(dx*dx + dy*dy);
        if (dist > 1.0f && dist < 200.0f) {
            vx -= (dx/dist)*0.04f;
            vy -= (dy/dist)*0.04f;
        }
        spriteVel.x = vx < -3.0f ? -3.0f : (vx > 3.0f ? 3.0f : vx);
        spriteVel.y = vy < -3.0f ? -3.0f : (vy > 3.0f ? 3.0f : vy);
        spritePos.x += spriteVel.x;
        spritePos.y += spriteVel.y;
        if (spritePos.x < -64) spritePos.x = GetScreenWidth();
        else if (spritePos.x > GetScreenWidth()) spritePos.x = -64;
        if (spritePos.y < -64) spritePos.y = GetScreenHeight();
        else if (spritePos.y > GetScreenHeight()) spritePos.y = -64;

        // Occasionally shoot a dot at the mouse
        Vector2 spriteCenter = { spritePos.x + 32, spritePos.y + 32 };
        if (fireCooldown <= 0) {
            for (int i = 0; i < MAX_DOTS; i++) {
                if (!dotActive[i]) {
                    dotPos[i] = spriteCenter;
                    float dx = mouse.x - dotPos[i].x;
                    float dy = mouse.y - dotPos[i].y;
                    float len = sqrtf(dx*dx + dy*dy);
                    if (len > 0.1f) {
                        dotVel[i] = (Vector2){ dx/len*8.0f, dy/len*8.0f };
                        dotActive[i] = true;
                        dotAge[i] = 0;
                        PlaySound(shootSound);
                        fireCooldown = GetRandomValue(40, 150);
                    }
                    break;
                }
            }
        } else {
            fireCooldown--;
        }
        for (int i = 0; i < MAX_DOTS; i++) {
            if (!dotActive[i]) continue;
            dotPos[i].x += dotVel[i].x;
            dotPos[i].y += dotVel[i].y;
            dotAge[i]++;
            if (dotPos[i].x < -10 || dotPos[i].x > GetScreenWidth() + 10 ||
                dotPos[i].y < -10 || dotPos[i].y > GetScreenHeight() + 10)
                dotActive[i] = false;
            float hitDx = dotPos[i].x - mouse.x;
            float hitDy = dotPos[i].y - mouse.y;
            if (hitDx*hitDx + hitDy*hitDy < 144.0f) {
                if (hitFlash == 0) {
                    spriteScore++;
                    PlaySound(hitCursorSound);
                }
                hitFlash = 20;
            }
            if (dotAge[i] > 10 && CheckCollisionPointRec(dotPos[i],
                    (Rectangle){ spritePos.x, spritePos.y, 64, 64 })) {
                playerScore++;
                PlaySound(selfHitSound);
                dotActive[i] = false;
            }
        }

        if (hitFlash > 0) hitFlash--;

        animTimer++;
        if (animTimer >= 8) { animTimer = 0; animFrame = (animFrame + 1) % 4; }

        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText("Press SPACE to send UDP ping", 100, 100, 30, DARKGRAY);
            DrawText(TextFormat("Sent: %d   Received: %d", packetsSent, packetsReceived), 100, 150, 20, GRAY);
            const char *score = TextFormat("You: %d   Sprite: %d", playerScore, spriteScore);
            DrawText(score, GetScreenWidth() - MeasureText(score, 40) - 20, 20, 40, DARKGRAY);
            DrawRectangleRec(clickBox, boxHeld ? SKYBLUE : LIGHTGRAY);
            DrawText("Click me", clickBox.x + 20, clickBox.y + 30, 20, DARKGRAY);
            DrawTextureRec(sprite, (Rectangle){ animFrame*64, 0, 64, 64 }, spritePos, WHITE);
            for (int i = 0; i < MAX_DOTS; i++) {
                if (dotActive[i]) DrawCircleV(dotPos[i], 5, RED);
            }
            DrawTexturePro(sprite, (Rectangle){ 0, 0, 64, 64 },
                           (Rectangle){ mouse.x - 16, mouse.y - 16, 32, 32 },
                           (Vector2){ 0, 0 }, 0.0f, hitFlash > 0 ? RED : WHITE);
            DrawFPS(10, 10);
        EndDrawing();
    }

    UnloadSound(echoSound);
    UnloadSound(clickSound);
    UnloadSound(shootSound);
    UnloadSound(hitCursorSound);
    UnloadSound(selfHitSound);
    UnloadSound(spriteClickSound);
    UnloadTexture(sprite);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
