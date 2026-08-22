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

void HideCursorX11(void *window);

// ---------------------------------------------------------------------------
// Post-processing shaders (GLSL 330, matching the static raylib build).
// Rendered pipeline: scene -> brightpass (half res) -> separable gaussian
// blur ping-pong x2 -> composite (scene + bloom + grade + vignette).
// With vs == NULL, rlgl supplies its default vertex shader, which exports
// fragTexCoord/fragColor; sampler units texture0/texture1 are auto-bound by
// name, so no explicit SetShaderValueTexture is needed for the second slot.
// ---------------------------------------------------------------------------
static const char *BRIGHTPASS_FS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform float threshold;\n"
    "void main()\n"
    "{\n"
    "    vec3 c = texture(texture0, fragTexCoord).rgb;\n"
    "    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float k = max(luma - threshold, 0.0) / (luma + 0.0001);\n"
    "    finalColor = vec4(c*k, 1.0);\n"
    "}\n";

static const char *BLUR_FS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform vec2 texelSize;\n"
    "uniform vec2 direction;\n"
    "void main()\n"
    "{\n"
    "    vec2 off = direction * texelSize;\n"
    "    vec4 sum = texture(texture0, fragTexCoord) * 0.227027;\n"
    "    sum += texture(texture0, fragTexCoord + off*1.3846154) * 0.3162162;\n"
    "    sum += texture(texture0, fragTexCoord - off*1.3846154) * 0.3162162;\n"
    "    sum += texture(texture0, fragTexCoord + off*3.2307692) * 0.0702703;\n"
    "    sum += texture(texture0, fragTexCoord - off*3.2307692) * 0.0702703;\n"
    "    finalColor = sum;\n"
    "}\n";

static const char *COMPOSITE_FS =
    "#version 330\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "out vec4 finalColor;\n"
    "uniform sampler2D texture0;\n"
    "uniform sampler2D texture1;\n"
    "uniform float bloomStrength;\n"
    "uniform float vignette;\n"
    "uniform float redFlash;\n"
    "void main()\n"
    "{\n"
    "    // WSLg/D3D12 presents RT chains Y-mirrored vs vanilla GL; un-flip here\n"
    "    vec2 uv = vec2(fragTexCoord.x, 1.0 - fragTexCoord.y);\n"
    "    vec3 col = clamp(texture(texture0, uv).rgb, 0.0, 1.0);\n"
    "    vec3 bloom = texture(texture1, uv).rgb;\n"
    "    col = mix(col, col*col*(3.0 - 2.0*col), 0.35);\n"
    "    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));\n"
    "    col = mix(vec3(luma), col, 1.12);\n"
    "    col += bloom * bloomStrength;\n"
    "    float d = length(fragTexCoord - 0.5) * 1.414;\n"
    "    col *= mix(1.0, 1.0 - smoothstep(0.5, 1.3, d), vignette);\n"
    // Rounded-rect SDF: flash hugs every edge at equal depth instead of
    // following the elliptical center-distance (which over-reached corners)
    "    vec2 bq = abs(fragTexCoord - 0.5) - vec2(0.44);\n"
    "    float bsdf = length(max(bq, vec2(0.0))) + min(max(bq.x, bq.y), 0.0) - 0.06;\n"
    "    col += vec3(0.9, 0.07, 0.05) * smoothstep(-0.12, 0.006, bsdf) * redFlash;\n"
    "    finalColor = vec4(col, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Juice systems: particles, impact rings, score popups
// ---------------------------------------------------------------------------
#define MAX_PARTICLES 512
typedef struct Particle {
    Vector2 pos, vel;
    float life, maxLife;
    float size;
    float grav;
    Color col;
} Particle;
static Particle particles[MAX_PARTICLES];
static int particleHead = 0;

#define MAX_RINGS 32
typedef struct Ring {
    Vector2 pos;
    float maxRadius;
    float life, maxLife;
    float width;
    Color col;
} Ring;
static Ring rings[MAX_RINGS];
static int ringHead = 0;

#define MAX_POPUPS 16
typedef struct Popup {
    Vector2 pos;
    float life;
    Color col;
} Popup;
static Popup popups[MAX_POPUPS];
static int popupHead = 0;

static void SpawnParticle(Vector2 pos, Vector2 vel, float life, float size,
                          float grav, Color col)
{
    Particle *p = &particles[particleHead];
    particleHead = (particleHead + 1) % MAX_PARTICLES;
    p->pos = pos; p->vel = vel;
    p->life = p->maxLife = life;
    p->size = size; p->grav = grav; p->col = col;
}

static void SpawnRing(Vector2 pos, float maxRadius, float life, float width,
                      Color col)
{
    Ring *r = &rings[ringHead];
    ringHead = (ringHead + 1) % MAX_RINGS;
    r->pos = pos; r->maxRadius = maxRadius;
    r->life = r->maxLife = life;
    r->width = width; r->col = col;
}

static void SpawnBurst(Vector2 pos, int n, float speedMin, float speedMax,
                       Color base, float grav)
{
    for (int i = 0; i < n; i++) {
        float a = GetRandomValue(0, 359)*DEG2RAD;
        float sp = GetRandomValue((int)(speedMin*100), (int)(speedMax*100))/100.0f;
        Vector2 vel = { cosf(a)*sp, sinf(a)*sp };
        Color c = {
            (unsigned char)fminf(255, fmaxf(0, base.r + GetRandomValue(-30, 30))),
            (unsigned char)fminf(255, fmaxf(0, base.g + GetRandomValue(-30, 30))),
            (unsigned char)fminf(255, fmaxf(0, base.b + GetRandomValue(-30, 30))),
            255
        };
        SpawnParticle(pos, vel, GetRandomValue(25, 60)/100.0f,
                      GetRandomValue(15, 40)/10.0f, grav, c);
    }
}

static void SpawnPopup(Vector2 pos, Color col)
{
    Popup *p = &popups[popupHead];
    popupHead = (popupHead + 1) % MAX_POPUPS;
    p->pos = pos;
    p->life = 0.9f;
    p->col = col;
}

static void AddTrauma(float *trauma, float amt)
{
    *trauma = (*trauma > 1.0f - amt) ? 1.0f : *trauma + amt;
}

// Scene ambient at a world point: same falloff as GenBackground, mapped
// to a grayscale tint (70 = moody edge, 255 = full center light)
static Color SceneLightTint(Vector2 p, int w, int h)
{
    float cx = w/2.0f, cy = h/2.0f;
    float maxD = sqrtf(cx*cx + cy*cy);
    float s = 1.0f - sqrtf((p.x-cx)*(p.x-cx) + (p.y-cy)*(p.y-cy))/maxD;
    if (s < 0) s = 0;
    float v = s*s*s*s*sqrtf(s);
    unsigned char c = (unsigned char)(70 + 185*v);
    return (Color){ c, c, c, 255 };
}

// Radial background: pitch-black edges rising along a slow ^3.5 curve to a
// dim center. The usable range is only ~17 levels corner-to-center, which
// bands badly at 8-bit depth (~70px rings), so the bake applies Bayer
// ordered dithering to dissolve the steps into sub-visible grain.
static Image GenBackground(int w, int h)
{
    Image img = GenImageColor(w, h, BLACK);
    Color *px = (Color *)img.data;
    float cx = w/2.0f, cy = h/2.0f;
    float maxD = sqrtf(cx*cx + cy*cy);
    static const float bayer[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float s = 1.0f - sqrtf(dx*dx + dy*dy)/maxD;
            float v = s*s*s*s*sqrtf(s);
            float d = bayer[(x & 3) + ((y & 3) << 2)]*(1.1f/16.0f) - 0.55f;
            float rr = 11*v + d;
            float gg = 10*v + d;
            float bb = 17*v + d;
            px[y*w + x].r = (unsigned char)(rr < 0 ? 0 : rr);
            px[y*w + x].g = (unsigned char)(gg < 0 ? 0 : gg);
            px[y*w + x].b = (unsigned char)(bb < 0 ? 0 : (bb > 255 ? 255 : bb));
        }
    return img;
}

// White radial glow with quadratic alpha falloff; tint at draw time
static Texture2D GenGlowTexture(int size)
{
    Image img = GenImageColor(size, size, BLANK);
    Color *px = (Color *)img.data;
    float c = size/2.0f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float dx = x - c, dy = y - c;
            float t = sqrtf(dx*dx + dy*dy)/c;
            unsigned char a = (t < 1.0f) ? (unsigned char)(255*(1.0f - t)*(1.0f - t)) : 0;
            px[y*size + x] = (Color){ 255, 255, 255, a };
        }
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

static void DrawGlow(Texture2D glow, Vector2 pos, float radius, Color tint)
{
    Rectangle src = { 0, 0, (float)glow.width, (float)glow.height };
    Rectangle dst = { pos.x - radius, pos.y - radius, radius*2.0f, radius*2.0f };
    DrawTexturePro(glow, src, dst, (Vector2){ 0, 0 }, 0, tint);
}

int main(int argc, char **argv) {
    InitWindow(1920, 1080, "Network + Input Test");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetTargetFPS(60);
    // NOTE: Do NOT call raylib HideCursor(): on X11 it installs GLFW's own
    // invisible cursor, overriding the X11 cursor set below every frame.
    HideCursorX11(GetWindowHandle());

    InitAudioDevice();
    Sound echoSound = LoadSound("resources/coin.wav");
    Sound sendSound = LoadSound("resources/ping_send.wav");
    Sound clickSound = LoadSound("resources/buttonfx.wav");
    Sound shootSound = LoadSound("resources/weird.wav");
    Sound hitCursorSound = LoadSound("resources/hit_splat.wav");
    Sound selfHitSound = LoadSound("resources/spring.wav");
    Sound spriteClickSound = LoadSound("resources/sound.wav");
    Music countryMusic = LoadMusicStream("resources/country.mp3");

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

    // Fake overhead light, baked per-pixel: darker toward each column's
    // bottom, rim highlight on its top-most pixel. Recolor-only pass --
    // no geometry moves (eye/mouth coordinates stay valid). Near-white
    // pixels (eye ovals) keep >=0.92 factor so pupils keep their contrast.
    for (int x = 0; x < baseImg.width; x++) {
        int ymin = -1, ymax = -1;
        for (int y = 0; y < baseImg.height; y++) {
            if (GetImageColor(baseImg, x, y).a > 0) {
                if (ymin < 0) ymin = y;
                ymax = y;
            }
        }
        if (ymin < 0) continue;
        for (int y = ymin; y <= ymax; y++) {
            Color px = GetImageColor(baseImg, x, y);
            if (px.a == 0) continue;
            float rel = (float)(y - ymin)/(ymax - ymin);
            float f = 1.10f - 0.34f*rel;
            bool white = px.r > 200 && px.g > 200 && px.b > 200;
            if (white && f < 0.92f) f = 0.92f;
            if (y == ymin) f += 0.08f;
            ImageDrawPixel(&baseImg, x, y,
                (Color){ (unsigned char)fminf(255, px.r*f),
                         (unsigned char)fminf(255, px.g*f),
                         (unsigned char)fminf(255, px.b*f), px.a });
        }
    }
    // 4-frame sheet: center, look right, look left, blink.
    // Eyes are solid white ovals (left ~cols 22-30, right ~cols 34-42,
    // rows 40-49); draw 2x2 black pupils at (25,43) and (37,43) and shift.
    Image sheet = GenImageColor(64*4, 64, BLANK);
    for (int f = 0; f < 4; f++) ImageDrawImage(&sheet, baseImg, f*64, 0, WHITE);
    // Mouth animates with the gaze frames: open fanged chomp while looking
    // around (1-2), closed grin at rest (0) and on the blink frame (3).
    Color mouthCol = { 40, 18, 75, 255 };
    Color fangCol = { 245, 245, 255, 255 };
    for (int f = 0; f < 4; f++) {
        int off = f*64;
        if (f == 1 || f == 2) {
            for (int y = 49; y <= 59; y++)
                for (int x = 23; x <= 41; x++) {
                    float ex = (x - 32)/9.0f, ey = (y - 54)/5.0f;
                    if (ex*ex + ey*ey <= 1.0f)
                        ImageDrawPixel(&sheet, off + x, y, mouthCol);
                }
            for (int i = 0; i < 3; i++) {
                ImageDrawPixel(&sheet, off + 26 + i, 50, fangCol);
                ImageDrawPixel(&sheet, off + 36 + i, 50, fangCol);
            }
            for (int y = 51; y <= 53; y++) {
                ImageDrawPixel(&sheet, off + 27, y, fangCol);
                ImageDrawPixel(&sheet, off + 36, y, fangCol);
            }
        } else {
            for (int y = 51; y <= 55; y++)
                for (int x = 24; x <= 40; x++) {
                    float ex = (x - 32)/8.0f, ey = (y - 50)/4.0f;
                    if (ex*ex + ey*ey <= 1.0f && y > 50)
                        ImageDrawPixel(&sheet, off + x, y, mouthCol);
                }
            for (int i = 0; i < 2; i++) {
                ImageDrawPixel(&sheet, off + 27 + i, 51, fangCol);
                ImageDrawPixel(&sheet, off + 27 + i, 52, fangCol);
                ImageDrawPixel(&sheet, off + 35 + i, 51, fangCol);
                ImageDrawPixel(&sheet, off + 35 + i, 52, fangCol);
            }
            ImageDrawPixel(&sheet, off + 27, 53, fangCol);
            ImageDrawPixel(&sheet, off + 36, 53, fangCol);
        }
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

    // White silhouette of the same sheet, used as the hit/pop flash overlay
    // (a plain tint can only darken a texture, never whiten it).
    Image sheetWhite = ImageCopy(sheet);
    for (int y = 0; y < sheetWhite.height; y++)
        for (int x = 0; x < sheetWhite.width; x++) {
            Color px = GetImageColor(sheetWhite, x, y);
            if (px.a > 0) ImageDrawPixel(&sheetWhite, x, y,
                                         (Color){ 255, 255, 255, px.a });
        }

    Texture2D sprite = LoadTextureFromImage(sheet);
    Texture2D spriteFlash = LoadTextureFromImage(sheetWhite);
    UnloadImage(sheet);
    UnloadImage(sheetWhite);

    // Background: radial gradient with a faint techy dot grid, baked once.
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    Image bgImg = GenBackground(sw, sh);
    Texture2D bgTex = LoadTextureFromImage(bgImg);
    UnloadImage(bgImg);
    Texture2D glowTex = GenGlowTexture(128);

    Shader brightpassShader = LoadShaderFromMemory(NULL, BRIGHTPASS_FS);
    Shader blurShader = LoadShaderFromMemory(NULL, BLUR_FS);
    Shader compositeShader = LoadShaderFromMemory(NULL, COMPOSITE_FS);
    int locThreshold = GetShaderLocation(brightpassShader, "threshold");
    int locTexel = GetShaderLocation(blurShader, "texelSize");
    int locDir = GetShaderLocation(blurShader, "direction");
    int locBloomStrength = GetShaderLocation(compositeShader, "bloomStrength");
    int locVignette = GetShaderLocation(compositeShader, "vignette");
    int locRedFlash = GetShaderLocation(compositeShader, "redFlash");

    RenderTexture2D sceneRT = { 0 }, bloomA = { 0 }, bloomB = { 0 };
    float thresholdVal = 0.62f;
    float bloomStrengthVal = 0.8f;
    float vignetteVal = 0.75f;
    float redFlashVal = 0.0f;

    #define RECREATE_TARGETS() do { \
        if (sceneRT.id) UnloadRenderTexture(sceneRT); \
        if (bloomA.id) UnloadRenderTexture(bloomA); \
        if (bloomB.id) UnloadRenderTexture(bloomB); \
        sceneRT = LoadRenderTexture(sw, sh); \
        bloomA = LoadRenderTexture((sw + 1)/2, (sh + 1)/2); \
        bloomB = LoadRenderTexture((sw + 1)/2, (sh + 1)/2); \
    } while (0)
    RECREATE_TARGETS();

    // Ambient dust motes drifting upward for depth
    #define MAX_MOTES 40
    Vector2 motePos[MAX_MOTES] = { 0 };
    float moteSpd[MAX_MOTES] = { 0 };
    float moteSize[MAX_MOTES] = { 0 };
    for (int i = 0; i < MAX_MOTES; i++) {
        motePos[i] = (Vector2){ GetRandomValue(0, sw), GetRandomValue(0, sh) };
        moteSpd[i] = GetRandomValue(4, 18)/10.0f;
        moteSize[i] = GetRandomValue(10, 28)/10.0f;
    }

    // Non-blocking UDP socket
    const char *serverIp = (argc > 1) ? argv[1] : "34.3.109.195";
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sock, F_SETFL, O_NONBLOCK);
    struct sockaddr_in serverAddr = {0};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(7777);
    if (inet_pton(AF_INET, serverIp, &serverAddr.sin_addr) != 1) {
        TraceLog(LOG_ERROR, "Invalid IP address: %s", serverIp);
        CloseWindow();
        return 1;
    }

    Rectangle clickBox = { 100, 300, 150, 80 };
    bool boxHeld = false;
    Vector2 spritePos = { 180, 340 };
    Vector2 spriteVel = { 1.5f, 0.8f };
    int packetsSent = 0, packetsReceived = 0;
    int playerScore = 0, spriteScore = 0;

    #define MAX_DOTS 24
    #define MAX_DOT_AGE 300
    #define TRAIL_LEN 10
    Vector2 dotPos[MAX_DOTS] = { 0 }, dotVel[MAX_DOTS] = { 0 };
    Vector2 dotTrail[MAX_DOTS][TRAIL_LEN] = { 0 };
    int dotTrailLen[MAX_DOTS] = { 0 };
    bool dotActive[MAX_DOTS] = { false };
    int dotAge[MAX_DOTS] = { 0 };
    int fireCooldown = 60;
    int hitFlash = 0;
    int animTimer = 0, animFrame = 0;
    int homingLevel = 0;
    float popT = 0.0f;
    float shakeTrauma = 0.0f;

    #define SCORE_TARGET 12
    #define MAX_CONFETTI 160
    Vector2 confettiPos[MAX_CONFETTI] = { 0 };
    float confettiVy[MAX_CONFETTI] = { 0 };
    float confettiRot[MAX_CONFETTI] = { 0 };
    float confettiRotV[MAX_CONFETTI] = { 0 };
    Color confettiColor[MAX_CONFETTI] = { 0 };
    int confettiCount = MAX_CONFETTI/2;
    bool gameOver = false;
    bool winSongStarted = false;
    int winner = 0;
    int gameOverTimer = 0;
    double worldTime = 0.0;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        worldTime += dt;

        if (IsWindowResized()) {
            sw = GetScreenWidth(); sh = GetScreenHeight();
            Image bgResize = GenBackground(sw, sh);
            UpdateTexture(bgTex, bgResize.data);
            UnloadImage(bgResize);
            RECREATE_TARGETS();
        }

        // Decay juice timers
        popT = popT > dt*2.8f ? popT - dt*2.8f : 0.0f;
        shakeTrauma = shakeTrauma > dt*1.6f ? shakeTrauma - dt*1.6f : 0.0f;
        float s2 = shakeTrauma*shakeTrauma;
        Vector2 shakeOff = {
            GetRandomValue(-1000, 1000)/1000.0f * 22.0f*s2,
            GetRandomValue(-1000, 1000)/1000.0f * 14.0f*s2
        };
        Camera2D shakeCam = { 0 };
        shakeCam.target = shakeOff;
        shakeCam.zoom = 1.0f;

        for (int i = 0; i < MAX_PARTICLES; i++) {
            Particle *p = &particles[i];
            if (p->life <= 0) continue;
            p->life -= dt;
            p->vel.y += p->grav*dt;
            float drag = expf(-3.5f*dt);
            p->vel.x *= drag; p->vel.y *= drag;
            p->pos.x += p->vel.x; p->pos.y += p->vel.y;
        }
        for (int i = 0; i < MAX_RINGS; i++)
            if (rings[i].life > 0) rings[i].life -= dt;
        for (int i = 0; i < MAX_POPUPS; i++)
            if (popups[i].life > 0) popups[i].life -= dt;
        for (int i = 0; i < MAX_MOTES; i++) {
            motePos[i].y -= moteSpd[i]*dt*10.0f;
            motePos[i].x += sinf(worldTime*0.7f + i)*0.08f;
            if (motePos[i].y < -8) {
                motePos[i].y = sh + 8;
                motePos[i].x = GetRandomValue(0, sw);
            }
        }

        if (gameOver) {
            if (winner == 1) {
                if (!winSongStarted) {
                    PlayMusicStream(countryMusic);
                    winSongStarted = true;
                }
                UpdateMusicStream(countryMusic);
                if (GetKeyPressed() != 0)
                    gameOverTimer = 0;
            } else {
                gameOverTimer--;
            }
            for (int i = 0; i < confettiCount; i++) {
                confettiPos[i].y += confettiVy[i];
                confettiRot[i] += confettiRotV[i]*dt;
                confettiPos[i].x += sinf(confettiPos[i].y*0.02f + i)*0.8f;
                if (confettiPos[i].y > GetScreenHeight()) {
                    confettiPos[i].y = -10;
                    confettiPos[i].x = GetRandomValue(0, GetScreenWidth());
                }
            }
            shakeCam.target = shakeOff;

            BeginDrawing();
                BeginTextureMode(sceneRT);
                    ClearBackground(BLACK);
                    DrawTexturePro(bgTex,
                        (Rectangle){ 0, 0, bgTex.width, bgTex.height },
                        (Rectangle){ 0, 0, sw, sh },
                        (Vector2){ 0, 0 }, 0, WHITE);
                    BeginMode2D(shakeCam);
                        for (int i = 0; i < confettiCount; i++) {
                            Vector2 origin = { 5, 3 };
                            Rectangle rc = { confettiPos[i].x, confettiPos[i].y, 10, 6 };
                            DrawRectanglePro(rc, origin, confettiRot[i], confettiColor[i]);
                        }
                        if (winner == 1) {
                            // Bright copy blooms around the crisp HUD title
                            DrawText("YOU WIN!",
                                     GetScreenWidth()/2 - MeasureText("YOU WIN!", 80)/2,
                                     GetScreenHeight()/2 - 40, 80, GOLD);
                        }
                    EndMode2D();
                EndTextureMode();

                SetShaderValue(brightpassShader, locThreshold, &thresholdVal, SHADER_UNIFORM_FLOAT);
                BeginTextureMode(bloomA);
                    BeginShaderMode(brightpassShader);
                        DrawTexturePro(sceneRT.texture,
                            (Rectangle){ 0, 0, sw, sh },
                            (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                            (Vector2){ 0, 0 }, 0, WHITE);
                    EndShaderMode();
                EndTextureMode();
                Vector2 bw = { 1.0f/bloomA.texture.width, 1.0f/bloomA.texture.height };
                SetShaderValue(blurShader, locTexel, &bw, SHADER_UNIFORM_VEC2);
                for (int pass = 0; pass < 2; pass++) {
                    Vector2 dirH = { 1, 0 }, dirV = { 0, 1 };
                    SetShaderValue(blurShader, locDir, &dirH, SHADER_UNIFORM_VEC2);
                    BeginTextureMode(bloomB);
                        BeginShaderMode(blurShader);
                            DrawTexturePro(bloomA.texture,
                                (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                                (Rectangle){ 0, 0, bloomB.texture.width, bloomB.texture.height },
                                (Vector2){ 0, 0 }, 0, WHITE);
                        EndShaderMode();
                    EndTextureMode();
                    SetShaderValue(blurShader, locDir, &dirV, SHADER_UNIFORM_VEC2);
                    BeginTextureMode(bloomA);
                        BeginShaderMode(blurShader);
                            DrawTexturePro(bloomB.texture,
                                (Rectangle){ 0, 0, bloomB.texture.width, bloomB.texture.height },
                                (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                                (Vector2){ 0, 0 }, 0, WHITE);
                        EndShaderMode();
                    EndTextureMode();
                }

                SetShaderValue(compositeShader, locBloomStrength, &bloomStrengthVal, SHADER_UNIFORM_FLOAT);
                SetShaderValue(compositeShader, locVignette, &vignetteVal, SHADER_UNIFORM_FLOAT);
                SetShaderValue(compositeShader, locRedFlash, &redFlashVal, SHADER_UNIFORM_FLOAT);
                BeginShaderMode(compositeShader);
                    DrawTexturePro(sceneRT.texture,
                        (Rectangle){ 0, 0, sw, sh },
                        (Rectangle){ 0, 0, sw, sh },
                        (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();

                if (winner == 1) {
                    DrawText("YOU WIN!", GetScreenWidth()/2 - MeasureText("YOU WIN!", 80)/2,
                             GetScreenHeight()/2 - 40, 80,
                             (Color){ 255, 240, 190, 255 });
                    if (spriteScore == 0) {
                        DrawText("SHUTOUT!", GetScreenWidth()/2 - MeasureText("SHUTOUT!", 40)/2,
                                 GetScreenHeight()/2 + 45, 40, RED);
                        const char *winInfo = TextFormat("Level: %d    You: %d   Sprite: %d", homingLevel, playerScore, spriteScore);
                        DrawText(winInfo, GetScreenWidth()/2 - MeasureText(winInfo, 30)/2,
                                 GetScreenHeight()/2 + 95, 30, RED);
                        DrawText("Press any key to continue", GetScreenWidth()/2 - MeasureText("Press any key to continue", 30)/2,
                                 GetScreenHeight()/2 + 135, 30, GRAY);
                    } else {
                        const char *winInfo = TextFormat("Level: %d    You: %d   Sprite: %d", homingLevel, playerScore, spriteScore);
                        DrawText(winInfo, GetScreenWidth()/2 - MeasureText(winInfo, 30)/2,
                                 GetScreenHeight()/2 + 55, 30, LIGHTGRAY);
                        DrawText("Press any key to continue", GetScreenWidth()/2 - MeasureText("Press any key to continue", 30)/2,
                                 GetScreenHeight()/2 + 95, 30, GRAY);
                    }
                } else {
                    DrawText("YOU LOSE", GetScreenWidth()/2 - MeasureText("YOU LOSE", 80)/2,
                             GetScreenHeight()/2 - 40, 80, RED);
                    const char *loseInfo = TextFormat("Level: %d    You: %d   Sprite: %d", homingLevel, playerScore, spriteScore);
                    DrawText(loseInfo, GetScreenWidth()/2 - MeasureText(loseInfo, 30)/2,
                             GetScreenHeight()/2 + 55, 30, GRAY);
                }
                DrawFPS(10, 10);
            EndDrawing();
            if (gameOverTimer <= 0) {
                gameOver = false;
                winSongStarted = false;
                if (winner == 2 || spriteScore == 0) homingLevel = 0;
                playerScore = 0;
                spriteScore = 0;
                for (int i = 0; i < MAX_DOTS; i++) dotActive[i] = false;
                StopMusicStream(countryMusic);
            }
            continue;
        }

        if (playerScore >= SCORE_TARGET || spriteScore >= SCORE_TARGET) {
            gameOver = true;
            gameOverTimer = 180;
            hitFlash = 0;
            winner = (playerScore >= SCORE_TARGET) ? 1 : 2;
            if (winner == 1) {
                homingLevel++;
                AddTrauma(&shakeTrauma, 0.7f);
                // Vivid fixed-hue palette: random RGB averages to beige,
                // which bloom then smears into an all-gold wash
                static const Color confPalette[] = {
                    { 255, 60, 60, 255 }, { 255, 140, 0, 255 },
                    { 255, 225, 40, 255 }, { 80, 220, 80, 255 },
                    { 60, 180, 255, 255 }, { 170, 80, 255, 255 },
                    { 255, 80, 190, 255 }
                };
                confettiCount = (spriteScore == 0) ? MAX_CONFETTI : MAX_CONFETTI/2;
                for (int i = 0; i < confettiCount; i++) {
                    confettiPos[i] = (Vector2){ GetRandomValue(0, GetScreenWidth()), GetRandomValue(-GetScreenHeight(), 0) };
                    confettiVy[i] = (float)GetRandomValue(2, 8);
                    confettiRot[i] = GetRandomValue(0, 359);
                    confettiRotV[i] = GetRandomValue(-240, 240);
                    confettiColor[i] = confPalette[GetRandomValue(0, 6)];
                }
            }
        }

        // Send on spacebar, no Enter needed - IsKeyPressed fires once per press
        if (IsKeyPressed(KEY_SPACE)) {
            const char* msg = "ping";
            if (sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) { perror("sendto failed"); }
            PlaySound(sendSound);
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
            Vector2 oldCenter = { spritePos.x + 32, spritePos.y + 32 };
            SpawnBurst(oldCenter, 16, 60, 260, (Color){ 170, 110, 255, 255 }, 320);
            SpawnRing(oldCenter, 70, 0.4f, 3, (Color){ 190, 130, 255, 200 });
            SpawnPopup(oldCenter, GOLD);
            AddTrauma(&shakeTrauma, 0.30f);
            popT = 1.0f;
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
        } else {
            float cx = GetScreenWidth()/2.0f - (spritePos.x + 32);
            float cy = GetScreenHeight()/2.0f - (spritePos.y + 32);
            float cdist = sqrtf(cx*cx + cy*cy);
            if (cdist > 1.0f) {
                vx += (cx/cdist)*0.02f;
                vy += (cy/cdist)*0.02f;
            }
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
                        dotTrailLen[i] = 0;
                        PlaySound(shootSound);
                        SpawnRing(dotPos[i], 34, 0.25f, 2, (Color){ 255, 160, 60, 180 });
                        popT = popT < 0.25f ? 0.25f : popT;
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
            if (homingLevel > 0) {
                float hx = mouse.x - dotPos[i].x;
                float hy = mouse.y - dotPos[i].y;
                float hlen = sqrtf(hx*hx + hy*hy);
                if (hlen > 1.0f) {
                    float vlen = sqrtf(dotVel[i].x*dotVel[i].x + dotVel[i].y*dotVel[i].y);
                    float t = 0.05f*homingLevel;
                    if (t > 0.9f) t = 0.9f;
                    dotVel[i].x += (hx/hlen*vlen - dotVel[i].x)*t;
                    dotVel[i].y += (hy/hlen*vlen - dotVel[i].y)*t;
                }
            }
            dotPos[i].x += dotVel[i].x;
            dotPos[i].y += dotVel[i].y;
            dotAge[i]++;
            for (int j = TRAIL_LEN - 1; j > 0; j--) dotTrail[i][j] = dotTrail[i][j-1];
            dotTrail[i][0] = dotPos[i];
            if (dotTrailLen[i] < TRAIL_LEN) dotTrailLen[i]++;
            if (dotAge[i] > MAX_DOT_AGE) dotActive[i] = false;
            if (dotPos[i].x < -10 || dotPos[i].x > GetScreenWidth() + 10 ||
                dotPos[i].y < -10 || dotPos[i].y > GetScreenHeight() + 10)
                dotActive[i] = false;
            float hitDx = dotPos[i].x - mouse.x;
            float hitDy = dotPos[i].y - mouse.y;
            if (hitDx*hitDx + hitDy*hitDy < 144.0f) {
                if (hitFlash == 0) {
                    spriteScore++;
                    PlaySound(hitCursorSound);
                    SpawnBurst(dotPos[i], 18, 80, 300, (Color){ 255, 140, 50, 255 }, 380);
                    SpawnRing(dotPos[i], 90, 0.35f, 4, (Color){ 255, 120, 40, 220 });
                    SpawnPopup(dotPos[i], (Color){ 255, 120, 80, 255 });
                    AddTrauma(&shakeTrauma, 0.35f);
                }
                hitFlash = 12;
                dotActive[i] = false;
                continue;
            }
            if (dotAge[i] > 10 && CheckCollisionPointRec(dotPos[i],
                    (Rectangle){ spritePos.x, spritePos.y, 64, 64 })) {
                playerScore++;
                PlaySound(selfHitSound);
                SpawnBurst(dotPos[i], 12, 50, 220, (Color){ 160, 100, 255, 255 }, 300);
                SpawnRing(dotPos[i], 60, 0.35f, 3, (Color){ 180, 120, 255, 200 });
                SpawnPopup(dotPos[i], (Color){ 200, 160, 255, 255 });
                AddTrauma(&shakeTrauma, 0.18f);
                popT = 1.0f;
                dotActive[i] = false;
            }
        }

        if (hitFlash > 0) hitFlash--;

        animTimer++;
        if (animTimer >= 8) { animTimer = 0; animFrame = (animFrame + 1) % 4; }

        // Sprite squash/stretch: click pop + firing flinch + idle breathing
        float squash = sinf(popT*PI);
        float breathe = sinf(worldTime*2.6f)*0.02f;
        float scaleX = 1.0f + 0.28f*squash - breathe;
        float scaleY = 1.0f - 0.20f*squash + breathe;
        float lean = spriteVel.x*1.5f;
        Vector2 spriteC = { spritePos.x + 32, spritePos.y + 32 };
        Rectangle spriteDst = {
            spriteC.x - 32*scaleX, spriteC.y - 32*scaleY,
            64*scaleX, 64*scaleY
        };
        float flashAlpha = powf(popT, 1.5f)*0.9f;

        BeginDrawing();
            // ---- World pass -> sceneRT ----
            BeginTextureMode(sceneRT);
                ClearBackground(BLACK);
                DrawTexturePro(bgTex,
                    (Rectangle){ 0, 0, bgTex.width, bgTex.height },
                    (Rectangle){ 0, 0, sw, sh },
                    (Vector2){ 0, 0 }, 0, WHITE);
                BeginMode2D(shakeCam);
                    BeginBlendMode(BLEND_ADDITIVE);
                        for (int i = 0; i < MAX_MOTES; i++) {
                            unsigned char ma = (unsigned char)(20 +
                                10*sinf(worldTime*1.3f + i*1.7f));
                            DrawCircleV(motePos[i], moteSize[i],
                                        (Color){ 150, 160, 210, ma });
                        }
                    EndBlendMode();
                    Color amb = SceneLightTint(spriteC, sw, sh);
                    DrawEllipse(spriteC.x, spritePos.y + 61,
                                26*scaleX, 7*scaleY,
                                (Color){ 0, 0, 0,
                                         (unsigned char)(30 + 50*amb.r/255) });
                    BeginBlendMode(BLEND_ADDITIVE);
                        DrawGlow(glowTex, spriteC, 62,
                                 (Color){ 120, 80, 220,
                                          (unsigned char)(8 + 18*amb.r/255) });
                    EndBlendMode();
                    DrawTexturePro(sprite,
                        (Rectangle){ animFrame*64, 0, 64, 64 },
                        spriteDst, (Vector2){ 0, 0 }, lean, amb);
                    if (flashAlpha > 0.01f)
                        DrawTexturePro(spriteFlash,
                            (Rectangle){ animFrame*64, 0, 64, 64 },
                            spriteDst, (Vector2){ 0, 0 }, lean,
                            (Color){ 255*amb.r/255, 255*amb.r/255,
                                     255*amb.r/255,
                                     (unsigned char)(255*flashAlpha) });
                    BeginBlendMode(BLEND_ADDITIVE);
                        // Dot trails, oldest first so heads layer brightest
                        for (int i = 0; i < MAX_DOTS; i++) {
                            if (!dotActive[i]) continue;
                            for (int j = dotTrailLen[i] - 1; j >= 0; j--) {
                                float t = 1.0f - (float)j/TRAIL_LEN;
                                unsigned char ta =
                                    (unsigned char)(90*t*t);
                                DrawCircleV(dotTrail[i][j], 1.5f + 3.5f*t,
                                            (Color){ 255, 90, 40, ta });
                            }
                        }
                        // Emissive dots: soft glow halo + white-hot core
                        for (int i = 0; i < MAX_DOTS; i++) {
                            if (!dotActive[i]) continue;
                            DrawGlow(glowTex, dotPos[i], 14, (Color){ 255, 110, 50, 70 });
                            DrawCircleV(dotPos[i], 5.5f, (Color){ 255, 170, 70, 170 });
                            DrawCircleV(dotPos[i], 3, (Color){ 255, 245, 200, 255 });
                        }
                        for (int i = 0; i < MAX_RINGS; i++) {
                            if (rings[i].life <= 0) continue;
                            float t = 1.0f - rings[i].life/rings[i].maxLife;
                            float rr = rings[i].maxRadius*(1.0f - (1.0f - t)*(1.0f - t));
                            unsigned char ra = (unsigned char)(rings[i].col.a *
                                                (rings[i].life/rings[i].maxLife));
                            DrawRing(rings[i].pos, rr - rings[i].width, rr,
                                     0, 360, 32,
                                     (Color){ rings[i].col.r, rings[i].col.g,
                                              rings[i].col.b, ra });
                        }
                        for (int i = 0; i < MAX_PARTICLES; i++) {
                            Particle *p = &particles[i];
                            if (p->life <= 0) continue;
                            float t = p->life/p->maxLife;
                            unsigned char pa = (unsigned char)(255*powf(t, 1.5f));
                            DrawCircleV(p->pos, p->size*t,
                                        (Color){ p->col.r, p->col.g,
                                                 p->col.b, pa });
                        }
                    EndBlendMode();
                EndMode2D();
            EndTextureMode();

            // ---- Bloom chain: brightpass -> blur x2 ----
            SetShaderValue(brightpassShader, locThreshold, &thresholdVal, SHADER_UNIFORM_FLOAT);
            BeginTextureMode(bloomA);
                BeginShaderMode(brightpassShader);
                    DrawTexturePro(sceneRT.texture,
                        (Rectangle){ 0, 0, sw, sh },
                        (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                        (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();
            EndTextureMode();
            Vector2 bw = { 1.0f/bloomA.texture.width, 1.0f/bloomA.texture.height };
            SetShaderValue(blurShader, locTexel, &bw, SHADER_UNIFORM_VEC2);
            for (int pass = 0; pass < 2; pass++) {
                Vector2 dirH = { 1, 0 }, dirV = { 0, 1 };
                SetShaderValue(blurShader, locDir, &dirH, SHADER_UNIFORM_VEC2);
                BeginTextureMode(bloomB);
                    BeginShaderMode(blurShader);
                        DrawTexturePro(bloomA.texture,
                            (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                            (Rectangle){ 0, 0, bloomB.texture.width, bloomB.texture.height },
                            (Vector2){ 0, 0 }, 0, WHITE);
                    EndShaderMode();
                EndTextureMode();
                SetShaderValue(blurShader, locDir, &dirV, SHADER_UNIFORM_VEC2);
                BeginTextureMode(bloomA);
                    BeginShaderMode(blurShader);
                        DrawTexturePro(bloomB.texture,
                            (Rectangle){ 0, 0, bloomB.texture.width, bloomB.texture.height },
                            (Rectangle){ 0, 0, bloomA.texture.width, bloomA.texture.height },
                            (Vector2){ 0, 0 }, 0, WHITE);
                    EndShaderMode();
                EndTextureMode();
            }

            // ---- Composite: scene + bloom + grade + vignette (+ red edge flash) ----
            redFlashVal = 0.55f*hitFlash/12.0f;
            SetShaderValue(compositeShader, locBloomStrength, &bloomStrengthVal, SHADER_UNIFORM_FLOAT);
            SetShaderValue(compositeShader, locVignette, &vignetteVal, SHADER_UNIFORM_FLOAT);
            SetShaderValue(compositeShader, locRedFlash, &redFlashVal, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(compositeShader);
                DrawTexturePro(sceneRT.texture,
                    (Rectangle){ 0, 0, sw, sh },
                    (Rectangle){ 0, 0, sw, sh },
                    (Vector2){ 0, 0 }, 0, WHITE);
            EndShaderMode();

            // ---- HUD pass: crisp, unshaken, unbloomed ----
            DrawText(TextFormat("Level: %d", homingLevel), 20, 40, 20, LIGHTGRAY);
#ifdef SHOW_UI
            DrawText("Press SPACE to send UDP ping", 100, 100, 30, LIGHTGRAY);
            DrawText(TextFormat("Sent: %d   Received: %d", packetsSent, packetsReceived), 100, 150, 20, GRAY);
#endif
            const char *score = TextFormat("You: %d   Sprite: %d", playerScore, spriteScore);
            DrawText(score, GetScreenWidth() - MeasureText(score, 40) - 20, 20, 40, LIGHTGRAY);
#ifdef SHOW_UI
            DrawRectangleRec(clickBox, boxHeld ? SKYBLUE : (Color){ 60, 60, 70, 255 });
            DrawText("Click me", clickBox.x + 20, clickBox.y + 30, 20, LIGHTGRAY);
#endif
            for (int i = 0; i < MAX_POPUPS; i++) {
                if (popups[i].life <= 0) continue;
                float t = popups[i].life/0.9f;
                unsigned char pa = (unsigned char)(255*(t < 0.5f ? t*2.0f : 1.0f));
                const char *txt = "+1";
                DrawText(txt,
                         (int)(popups[i].pos.x - MeasureText(txt, 24)/2),
                         (int)(popups[i].pos.y - 20 - 40*(1.0f - t)),
                         24, (Color){ popups[i].col.r, popups[i].col.g,
                                      popups[i].col.b, pa });
            }
            // The X11 reticle cursor (hide_cursor_x11.c) IS the crosshair;
            // don't draw one at the mouse or there'd be two objects.
            DrawFPS(10, 10);
        EndDrawing();
    }

    UnloadSound(echoSound);
    UnloadSound(sendSound);
    UnloadSound(clickSound);
    UnloadSound(shootSound);
    UnloadSound(hitCursorSound);
    UnloadSound(selfHitSound);
    UnloadSound(spriteClickSound);
    UnloadMusicStream(countryMusic);
    UnloadTexture(sprite);
    UnloadTexture(spriteFlash);
    UnloadTexture(bgTex);
    UnloadTexture(glowTex);
    UnloadRenderTexture(sceneRT);
    UnloadRenderTexture(bloomA);
    UnloadRenderTexture(bloomB);
    UnloadShader(brightpassShader);
    UnloadShader(blurShader);
    UnloadShader(compositeShader);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
