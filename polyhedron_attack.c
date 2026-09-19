#include "raylib.h"
#include "raymath.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

static const char *SOLID_VS =
    "#version 330\n"
    "in vec3 vertexPosition;\n"
    "in vec3 vertexNormal;\n"
    "out vec3 fragNormal;\n"
    "out vec3 fragWorldPos;\n"
    "uniform mat4 mvp;\n"
    "uniform mat4 matModel;\n"
    "void main()\n"
    "{\n"
    "    vec4 worldPos = matModel * vec4(vertexPosition, 1.0);\n"
    "    fragWorldPos = worldPos.xyz;\n"
    "    fragNormal = mat3(matModel) * vertexNormal;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

static const char *SOLID_FS =
    "#version 330\n"
    "in vec3 fragNormal;\n"
    "in vec3 fragWorldPos;\n"
    "out vec4 finalColor;\n"
    "uniform vec4 colDiffuse;\n"
    "uniform vec3 lightDir;\n"
    "uniform vec3 viewPos;\n"
    "void main()\n"
    "{\n"
    "    vec3 n = normalize(fragNormal);\n"
    "    vec3 l = normalize(lightDir);\n"
    "    float ndl = max(dot(n, l), 0.0);\n"
    "    vec3 v = normalize(viewPos - fragWorldPos);\n"
    "    vec3 h = normalize(l + v);\n"
    "    float spec = pow(max(dot(n, h), 0.0), 26.0) * step(0.0001, ndl);\n"
    "    vec3 base = colDiffuse.rgb;\n"
    "    float lit = step(0.01, dot(base, vec3(1.0)));\n"
    "    vec3 color = base * (0.30 + 0.70*ndl) + vec3(0.22)*spec*lit;\n"
    "    finalColor = vec4(color, 1.0);\n"
    "}\n";

static const float TET_V[4][3] = {
    { 0.0f,       1.0f,       0.0f },
    { 0.0f,      -0.333333f,  0.942809f },
    { 0.816497f, -0.333333f, -0.471405f },
    { -0.816497f,-0.333333f, -0.471405f },
};
static const int TET_F[4][3] = {
    { 0,1,2 }, { 0,3,1 }, { 0,2,3 }, { 1,3,2 },
};

static const float OCT_V[6][3] = {
    { 1,0,0 }, { 0,1,0 }, { 0,0,1 }, { -1,0,0 }, { 0,-1,0 }, { 0,0,-1 },
};
static const int OCT_F[8][3] = {
    { 2,0,1 }, { 2,1,3 }, { 2,3,4 }, { 2,4,0 },
    { 5,1,0 }, { 5,3,1 }, { 5,4,3 }, { 5,0,4 },
};

static const float ICO_V[12][3] = {
    { -1, 1.61803398875f, 0 }, {  1, 1.61803398875f, 0 },
    { -1, -1.61803398875f, 0 }, {  1, -1.61803398875f, 0 },
    { 0, -1,  1.61803398875f }, { 0, 1,  1.61803398875f },
    { 0, -1, -1.61803398875f }, { 0, 1, -1.61803398875f },
    {  1.61803398875f, 0, -1 }, {  1.61803398875f, 0, 1 },
    { -1.61803398875f, 0, -1 }, { -1.61803398875f, 0, 1 },
};
static const int ICO_F[20][3] = {
    { 0,11,5 }, { 0,5,1 }, { 0,1,7 }, { 0,7,10 }, { 0,10,11 },
    { 1,5,9 }, { 5,11,4 }, { 11,10,2 }, { 10,7,6 }, { 7,1,8 },
    { 3,9,4 }, { 3,4,2 }, { 3,2,6 }, { 3,6,8 }, { 3,8,9 },
    { 4,9,5 }, { 2,4,11 }, { 6,2,10 }, { 8,6,7 }, { 9,8,1 },
};

static Mesh MeshPolyhedron(const float (*v)[3], int nv, const int (*f)[3], int nt, float radius)
{
    float verts[64][3];
    for (int i = 0; i < nv && i < 64; i++) {
        float l = sqrtf(v[i][0]*v[i][0] + v[i][1]*v[i][1] + v[i][2]*v[i][2]);
        verts[i][0] = v[i][0]/l*radius;
        verts[i][1] = v[i][1]/l*radius;
        verts[i][2] = v[i][2]/l*radius;
    }

    Mesh mesh = { 0 };
    mesh.vertexCount = nt * 3;
    mesh.vertices  = (float *)malloc(mesh.vertexCount * 3 * sizeof(float));
    mesh.normals   = (float *)malloc(mesh.vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float *)malloc(mesh.vertexCount * 2 * sizeof(float));

    int vc = 0;
    for (int t = 0; t < nt; t++) {
        float *p0 = verts[f[t][0]], *p1 = verts[f[t][1]], *p2 = verts[f[t][2]];
        float ux = p1[0]-p0[0], uy = p1[1]-p0[1], uz = p1[2]-p0[2];
        float wx = p2[0]-p0[0], wy = p2[1]-p0[1], wz = p2[2]-p0[2];
        float nx = uy*wz - uz*wy, ny = uz*wx - ux*wz, nz = ux*wy - uy*wx;
        float nl = sqrtf(nx*nx + ny*ny + nz*nz);
        nx /= nl; ny /= nl; nz /= nl;
        float gx = (p0[0]+p1[0]+p2[0])/3.0f;
        float gy = (p0[1]+p1[1]+p2[1])/3.0f;
        float gz = (p0[2]+p1[2]+p2[2])/3.0f;
        if (nx*gx + ny*gy + nz*gz < 0.0f) {
            float *tp = p1; p1 = p2; p2 = tp;
            nx = -nx; ny = -ny; nz = -nz;
        }

        for (int k = 0; k < 3; k++) {
            float *p = (k == 0) ? p0 : (k == 1) ? p1 : p2;
            mesh.vertices[vc*3+0] = p[0];
            mesh.vertices[vc*3+1] = p[1];
            mesh.vertices[vc*3+2] = p[2];
            mesh.normals[vc*3+0] = nx;
            mesh.normals[vc*3+1] = ny;
            mesh.normals[vc*3+2] = nz;
            mesh.texcoords[vc*2+0] = 0.0f;
            mesh.texcoords[vc*2+1] = 0.0f;
            vc++;
        }
    }
    return mesh;
}

static Mesh MeshTetrahedron(float radius)  { return MeshPolyhedron(TET_V, 4, TET_F, 4, radius); }
static Mesh MeshOctahedron(float radius)   { return MeshPolyhedron(OCT_V, 6, OCT_F, 8, radius); }

static Mesh MeshDodecahedron(float radius)
{
    float u[12][3];
    float D[20][3];
    for (int i = 0; i < 12; i++) {
        float l = sqrtf(ICO_V[i][0]*ICO_V[i][0] + ICO_V[i][1]*ICO_V[i][1] + ICO_V[i][2]*ICO_V[i][2]);
        u[i][0] = ICO_V[i][0]/l; u[i][1] = ICO_V[i][1]/l; u[i][2] = ICO_V[i][2]/l;
    }
    for (int fi = 0; fi < 20; fi++) {
        float c[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) {
            c[0] += u[ICO_F[fi][k]][0];
            c[1] += u[ICO_F[fi][k]][1];
            c[2] += u[ICO_F[fi][k]][2];
        }
        c[0] /= 3.0f; c[1] /= 3.0f; c[2] /= 3.0f;
        float l = sqrtf(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]);
        D[fi][0] = c[0]/l; D[fi][1] = c[1]/l; D[fi][2] = c[2]/l;
    }

    int tris[36][3];
    int t = 0;
    for (int vi = 0; vi < 12; vi++) {
        int face[5], n = 0;
        for (int fi = 0; fi < 20 && n < 5; fi++)
            if (ICO_F[fi][0] == vi || ICO_F[fi][1] == vi || ICO_F[fi][2] == vi)
                face[n++] = fi;

        float q[5][3], ref[3], ref2[3], ang[5];
        for (int k = 0; k < 5; k++) {
            float dc = D[face[k]][0]*u[vi][0] + D[face[k]][1]*u[vi][1] + D[face[k]][2]*u[vi][2];
            q[k][0] = D[face[k]][0] - dc*u[vi][0];
            q[k][1] = D[face[k]][1] - dc*u[vi][1];
            q[k][2] = D[face[k]][2] - dc*u[vi][2];
        }
        float rl = sqrtf(q[0][0]*q[0][0] + q[0][1]*q[0][1] + q[0][2]*q[0][2]);
        ref[0] = q[0][0]/rl; ref[1] = q[0][1]/rl; ref[2] = q[0][2]/rl;
        ref2[0] = u[vi][1]*ref[2] - u[vi][2]*ref[1];
        ref2[1] = u[vi][2]*ref[0] - u[vi][0]*ref[2];
        ref2[2] = u[vi][0]*ref[1] - u[vi][1]*ref[0];
        for (int k = 0; k < 5; k++)
            ang[k] = atan2f(q[k][0]*ref2[0] + q[k][1]*ref2[1] + q[k][2]*ref2[2],
                            q[k][0]*ref[0]  + q[k][1]*ref[1]  + q[k][2]*ref[2]);
        for (int k = 1; k < 5; k++) {
            int fv = face[k]; float av = ang[k]; int j = k;
            while (j > 0 && ang[j-1] > av) { face[j] = face[j-1]; ang[j] = ang[j-1]; j--; }
            face[j] = fv; ang[j] = av;
        }

        tris[t][0] = face[0]; tris[t][1] = face[1]; tris[t][2] = face[2]; t++;
        tris[t][0] = face[0]; tris[t][1] = face[2]; tris[t][2] = face[3]; t++;
        tris[t][0] = face[0]; tris[t][1] = face[3]; tris[t][2] = face[4]; t++;
    }
    return MeshPolyhedron(D, 20, tris, 36, radius);
}

static int SelectExternalMonitor(void)
{
    int count = GetMonitorCount();
    int best = GetCurrentMonitor();
    int bestArea = -1;
    for (int i = 0; i < count; i++) {
        const char *name = GetMonitorName(i);
        TraceLog(LOG_INFO, "Monitor %d: \"%s\" %dx%d pos(%d,%d)",
                 i, name, GetMonitorWidth(i), GetMonitorHeight(i),
                 (int)GetMonitorPosition(i).x, (int)GetMonitorPosition(i).y);
        int builtin = name &&
            (strstr(name, "eDP") || strstr(name, "LVDS") ||
             strstr(name, "Built") || strstr(name, "Laptop") ||
             strstr(name, "LCD") || strstr(name, "Internal"));
        if (builtin) continue;
        int area = GetMonitorWidth(i) * GetMonitorHeight(i);
        if (area > bestArea) { bestArea = area; best = i; }
    }
    TraceLog(LOG_INFO, "Using monitor %d: %s", best, GetMonitorName(best));
    return best;
}

#define MAX_ENEMY 42
#define MAX_EBULLETS 8
#define MAX_COVER 8

enum { TYPE_OCT = 0, TYPE_DODE = 1 };
enum { ST_PLAY = 0, ST_OVER = 1 };

typedef struct Enemy { float x, y, rx, ry, srx, sry; int type, alive; } Enemy;
typedef struct Bullet { float x, y; int active; } Bullet;
typedef struct Cover { float x, y, scale, ry, sry; int hp; } Cover;

static Enemy enemies[MAX_ENEMY];
static Bullet pbullets[1];
static Bullet ebullets[MAX_EBULLETS];
static Cover covers[MAX_COVER];
static int coverCount;

static float formX, formY, formDir;
static float playerX;
static float respawnT;
static float waveT;
static float saucerX, saucerDir, saucerT;
static int saucerActive;
static int score, lives, state;
static int wave, activeCols, activeRows;

static Sound sndShoot;
static Sound sndHit;
static Sound sndCover;
static Sound sndPlayerHit;
static Sound sndEnemyFire;
static Sound sndSaucer;

static const float COL_SP = 2.6f;
static const float ROW_SP = 1.6f;
static const float PLAYFIELD_EDGE = 15.5f;
static const float TOP_Y = 7.0f;
static const float DESC_STEP = 0.8f;
static const float LETHAL_BOT = -6.0f;
static const float PLAYER_Y = -8.0f;
static const float PLAYER_X_LIMIT = 14.5f;
static const float SAUCER_Y = 9.2f;

static void SetDifficulty(void)
{
    activeCols = 6 + (wave > 2 ? 1 : 0);
    if (activeCols > 7) activeCols = 7;
    activeRows = 4 + (wave > 1 ? 1 : 0) + (wave > 3 ? 1 : 0);
    if (activeRows > 6) activeRows = 6;
}

static void ResetFormation(void)
{
    formX = 0.0f; formY = TOP_Y; formDir = 1.0f;
    int count = activeCols * activeRows;
    for (int i = 0; i < count; i++) {
        int col = i % activeCols, row = i / activeCols;
        enemies[i].x = (col - (activeCols-1)/2.0f) * COL_SP;
        enemies[i].y = -(float)row * ROW_SP;
        enemies[i].type = row < 2 ? TYPE_DODE : TYPE_OCT;
        enemies[i].alive = 1;
        enemies[i].rx = 0.0f; enemies[i].ry = 0.0f;
        enemies[i].srx = 40.0f * ((i % 2) ? 1.0f : -1.0f);
        enemies[i].sry = 55.0f * ((i % 3) ? 1.0f : -1.0f);
    }
    for (int i = count; i < MAX_ENEMY; i++) enemies[i].alive = 0;
}

static void ClearBullets(void)
{
    pbullets[0].active = 0;
    for (int i = 0; i < MAX_EBULLETS; i++) ebullets[i].active = 0;
}

static void InitGame(void)
{
    score = 0; lives = 3; state = ST_PLAY;
    respawnT = 0.0f; playerX = 0.0f;
    waveT = 0.0f; wave = 1;
    saucerActive = 0; saucerT = 20.0f + (float)(rand() % 15);
    SetDifficulty();
    ResetFormation();
    ClearBullets();

    coverCount = 0;
    const float bx[4] = { -8.0f, -3.0f, 3.0f, 8.0f };
    const float by[2] = { -5.5f, -3.3f };
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 2; j++) {
            covers[coverCount].x = bx[i];
            covers[coverCount].y = by[j];
            covers[coverCount].scale = 1.0f;
            covers[coverCount].ry = 0.0f;
            covers[coverCount].sry = 22.0f * (((i + j) % 2) ? 1.0f : -1.0f);
            covers[coverCount].hp = 2;
            coverCount++;
        }
}

static int AliveCount(void)
{
    int n = 0;
    for (int i = 0; i < activeCols*activeRows; i++) if (enemies[i].alive) n++;
    return n;
}

static int BulletHitsCover(float bx, float by)
{
    for (int i = 0; i < coverCount; i++) {
        if (covers[i].hp <= 0) continue;
        float s = covers[i].scale;
        if (fabsf(bx - covers[i].x) < 1.0f*s && fabsf(by - covers[i].y) < 1.0f*s) {
            covers[i].hp--;
            if (covers[i].hp == 1) covers[i].scale *= 0.55f;
            PlaySound(sndCover);
            return 1;
        }
    }
    return 0;
}

static void LoseLife(void)
{
    lives--;
    if (lives < 0) { state = ST_OVER; return; }
    respawnT = 2.0f;
    playerX = 0.0f;
    ResetFormation();
    ClearBullets();
}

static void EnemyFire(void)
{
    int count = activeCols * activeRows;
    int alive = AliveCount();
    if (!alive) return;
    float frac = alive / (float)count;
    if (((float)(rand() % 1000)) / 1000.0f < (0.003f + (1.0f - frac) * 0.016f)) {
        int pick;
        do { pick = rand() % count; } while (!enemies[pick].alive);
        for (int k = 0; k < MAX_EBULLETS; k++) {
            if (!ebullets[k].active) {
                ebullets[k].x = enemies[pick].x + formX;
                ebullets[k].y = enemies[pick].y + formY - 1.1f;
                ebullets[k].active = 1;
                PlaySound(sndEnemyFire);
                break;
            }
        }
    }
}

static void UpdatePlayer(float dt)
{
    if (respawnT > 0.0f) respawnT -= dt;
    float dx = 0.0f;
    if (IsKeyDown(KEY_LEFT)) dx -= 1.0f;
    if (IsKeyDown(KEY_RIGHT)) dx += 1.0f;
    playerX += dx * 8.0f * dt;
    if (playerX > PLAYER_X_LIMIT) playerX = PLAYER_X_LIMIT;
    if (playerX < -PLAYER_X_LIMIT) playerX = -PLAYER_X_LIMIT;

    if (IsKeyPressed(KEY_SPACE) && !pbullets[0].active && respawnT <= 0.0f) {
        pbullets[0].x = playerX;
        pbullets[0].y = PLAYER_Y + 0.7f;
        pbullets[0].active = 1;
        PlaySound(sndShoot);
    }
}

static void UpdateFormation(float dt)
{
    if (waveT > 0.0f) {
        waveT -= dt;
        if (waveT <= 0.0f) { waveT = 0.0f; ResetFormation(); }
        return;
    }

    int alive = AliveCount();
    if (alive == 0) { wave++; SetDifficulty(); waveT = 1.5f; score += 500; return; }
    int count = activeCols * activeRows;
    float speed = 2.0f + (1.0f - alive / (float)count) * 1.8f;
    float halfW = (activeCols-1) * COL_SP / 2.0f;
    float limit = PLAYFIELD_EDGE - halfW;

    formX += formDir * speed * dt;
    if (formX > limit) { formX = limit; formDir = -1.0f; formY -= DESC_STEP; }
    if (formX < -limit) { formX = -limit; formDir = 1.0f; formY -= DESC_STEP; }

    if (formY - (activeRows-1)*ROW_SP < LETHAL_BOT) LoseLife();
}

static void UpdateBullets(float dt)
{
    if (pbullets[0].active) {
        pbullets[0].y += 16.0f * dt;
        if (pbullets[0].y > 9.6f) {
            pbullets[0].active = 0;
        } else {
            float pbx = pbullets[0].x, pby = pbullets[0].y;
            if (BulletHitsCover(pbx, pby)) {
                pbullets[0].active = 0;
            } else {
                int hit = 0;
                if (saucerActive &&
                    fabsf(pbx - saucerX) < 1.05f && pby > 8.2f && pby < 10.2f) {
                    score += 200;
                    saucerActive = 0;
                    saucerT = 18.0f + rand() % 16;
                    pbullets[0].active = 0;
                    PlaySound(sndHit);
                    hit = 1;
                }
                if (!hit) {
                    for (int i = 0; i < activeCols*activeRows; i++) {
                        if (!enemies[i].alive) continue;
                        float ey = enemies[i].y + formY;
                        if (fabsf(pbx - (enemies[i].x + formX)) < 0.85f &&
                            pby > (ey - 0.9f) && pby < (ey + 0.9f)) {
                            enemies[i].alive = 0;
                            score += enemies[i].type == TYPE_DODE ? 50 : 30;
                            pbullets[0].active = 0;
                            PlaySound(sndHit);
                            hit = 1;
                            break;
                        }
                    }
                }
            }
        }
    }

    for (int k = 0; k < MAX_EBULLETS; k++) {
        if (!ebullets[k].active) continue;
        ebullets[k].y -= 9.0f * dt;
        if (ebullets[k].y < -9.6f) {
            ebullets[k].active = 0;
            continue;
        }
        if (BulletHitsCover(ebullets[k].x, ebullets[k].y)) {
            ebullets[k].active = 0;
            continue;
        }
        if (respawnT <= 0.0f &&
            fabsf(ebullets[k].x - playerX) < 0.7f &&
            ebullets[k].y < (PLAYER_Y + 0.7f) && ebullets[k].y > (PLAYER_Y - 0.4f)) {
            ebullets[k].active = 0;
            PlaySound(sndPlayerHit);
            LoseLife();
            if (state == ST_OVER) return;
        }
    }
}

static void UpdateSaucer(float dt)
{
    if (!saucerActive) {
        saucerT -= dt;
        if (saucerT <= 0.0f) {
            saucerActive = 1;
            saucerX = -18.0f;
            PlaySound(sndSaucer);
        }
    } else {
        saucerX += 3.0f * dt;
        if (saucerX > 18.0f) {
            saucerActive = 0;
            saucerT = 15.0f + rand() % 18;
        }
    }
}

int main(void)
{
    srand((unsigned)time(NULL));

    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1920, 1080, "Polyhedron Attack");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetWindowMonitor(SelectExternalMonitor());
    SetTargetFPS(60);

    InitAudioDevice();
    sndShoot = LoadSound("resources/weird.wav");
    sndHit = LoadSound("resources/hit_splat.wav");
    sndCover = LoadSound("resources/buttonfx.wav");
    sndPlayerHit = LoadSound("resources/boom.wav");
    sndEnemyFire = LoadSound("resources/ping_send.wav");
    sndSaucer = LoadSound("resources/coin.wav");
    SetSoundVolume(sndEnemyFire, 0.35f);

    Shader shader = LoadShaderFromMemory(SOLID_VS, SOLID_FS);
    int locLight = GetShaderLocation(shader, "lightDir");
    int locView = GetShaderLocation(shader, "viewPos");
    float lightDir[3] = { 0.0f, 0.0f, 1.0f };

    Mesh mtet = MeshTetrahedron(1.0f);
    Mesh moct = MeshOctahedron(1.0f);
    Mesh mdod = MeshDodecahedron(1.0f);
    UploadMesh(&mtet, false);
    UploadMesh(&moct, false);
    UploadMesh(&mdod, false);
    Model modelTet = LoadModelFromMesh(mtet);
    Model modelOct = LoadModelFromMesh(moct);
    Model modelDod = LoadModelFromMesh(mdod);
    Model modelCov = LoadModelFromMesh(GenMeshCube(2.0f, 2.0f, 2.0f));
    Model modelSph = LoadModelFromMesh(GenMeshSphere(1.0f, 24, 16));
    Model modelPB  = LoadModelFromMesh(GenMeshCylinder(0.09f, 1.1f, 10));
    Model modelEB  = LoadModelFromMesh(GenMeshCylinder(0.11f, 1.2f, 10));
    Model modelPanel = LoadModelFromMesh(GenMeshCube(33.4f, 20.4f, 0.6f));

    modelTet.materials[0].shader = shader;
    modelOct.materials[0].shader = shader;
    modelDod.materials[0].shader = shader;
    modelCov.materials[0].shader = shader;
    modelSph.materials[0].shader = shader;
    modelPB.materials[0].shader = shader;
    modelEB.materials[0].shader = shader;

    modelTet.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){  80, 190, 255, 255 };
    modelOct.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){  70, 200,  90, 255 };
    modelDod.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 170,  90, 230, 255 };
    modelCov.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 140, 155, 185, 255 };
    modelSph.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 245, 210,  70, 255 };
    modelPB.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 255, 240, 150, 255 };
    modelEB.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 250,  90,  90, 255 };
    modelPanel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = (Color){ 20, 28, 48, 255 };

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 0.0f, 28.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 40.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    InitGame();

    float saucerRy = 0.0f;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE)) break;

        float dt = GetFrameTime();

        for (int i = 0; i < coverCount; i++)
            if (covers[i].hp > 0) covers[i].ry += covers[i].sry * dt;

        if (state == ST_PLAY) {
            UpdatePlayer(dt);
            UpdateFormation(dt);
            if (state == ST_PLAY && waveT <= 0.0f) EnemyFire();
            if (state == ST_PLAY) UpdateBullets(dt);
            if (state == ST_PLAY) UpdateSaucer(dt);

            for (int i = 0; i < MAX_ENEMY; i++) {
                if (enemies[i].alive) {
                    enemies[i].rx += enemies[i].srx * dt;
                    enemies[i].ry += enemies[i].sry * dt;
                }
            }
            saucerRy += 70.0f * dt;
        }

        BeginDrawing();
            ClearBackground((Color){ 8, 12, 22, 255 });

            BeginMode3D(camera);
                DrawModel(modelPanel, (Vector3){ 0.0f, 0.0f, -0.4f }, 1.0f, WHITE);

                BeginShaderMode(shader);
                    SetShaderValue(shader, locLight, lightDir, SHADER_UNIFORM_VEC3);
                    SetShaderValue(shader, locView, (float[3]){ camera.position.x, camera.position.y, camera.position.z }, SHADER_UNIFORM_VEC3);

                    for (int i = 0; i < MAX_ENEMY; i++) {
                        if (!enemies[i].alive) continue;
                        Model *m = enemies[i].type == TYPE_DODE ? &modelDod : &modelOct;
                        m->transform = MatrixMultiply(
                            MatrixRotateX(enemies[i].rx * DEG2RAD),
                            MatrixRotateY(enemies[i].ry * DEG2RAD));
                        Vector3 p = { enemies[i].x + formX, enemies[i].y + formY, 0.0f };
                        DrawModel(*m, p, 0.7f, WHITE);
                    }

                    if (respawnT <= 0.0f || ((int)(GetTime() * 10.0f) % 2) == 0)
                        DrawModel(modelTet, (Vector3){ playerX, PLAYER_Y, 0.0f }, 0.7f, WHITE);

                    if (saucerActive) {
                        modelSph.transform = MatrixMultiply(
                            MatrixRotateX(saucerRy * 0.3f * DEG2RAD),
                            MatrixRotateY(saucerRy * DEG2RAD));
                        DrawModel(modelSph, (Vector3){ saucerX, SAUCER_Y, 0.0f }, 0.8f, WHITE);
                    }

                    for (int i = 0; i < coverCount; i++) {
                        if (covers[i].hp > 0) {
                            modelCov.transform = MatrixRotateY(covers[i].ry * DEG2RAD);
                            DrawModel(modelCov, (Vector3){ covers[i].x, covers[i].y, 0.0f }, covers[i].scale, WHITE);
                        }
                    }

                    if (pbullets[0].active)
                        DrawModel(modelPB, (Vector3){ pbullets[0].x, pbullets[0].y, 0.0f }, 1.0f, WHITE);
                    for (int k = 0; k < MAX_EBULLETS; k++)
                        if (ebullets[k].active)
                            DrawModel(modelEB, (Vector3){ ebullets[k].x, ebullets[k].y, 0.0f }, 1.0f, WHITE);
                EndShaderMode();
            EndMode3D();

            int sw = GetScreenWidth(), sh = GetScreenHeight();
            DrawText("POLYHEDRON ATTACK", sw/2 - MeasureText("POLYHEDRON ATTACK", 30)/2, 16, 30, WHITE);
            DrawText("SCORE", 24, 16, 22, (Color){ 140, 155, 185, 255 });
            DrawText(TextFormat("%d", score), 24, 44, 28, WHITE);
            DrawText("LIVES", sw - MeasureText("LIVES", 22) - 24, 16, 22, (Color){ 140, 155, 185, 255 });
            DrawText(TextFormat("%d", lives), sw - MeasureText(TextFormat("%d", lives), 28) - 24, 44, 28, WHITE);

            DrawText("<- -> MOVE     SPACE FIRE     ESC QUIT", sw/2 - MeasureText("<- -> MOVE     SPACE FIRE     ESC QUIT", 18)/2, sh - 34, 18, (Color){ 120, 135, 160, 255 });

            if (state == ST_OVER) {
                DrawText("GAME OVER", sw/2 - MeasureText("GAME OVER", 64)/2, sh/2 - 60, 64, (Color){ 255, 90, 90, 255 });
                DrawText(TextFormat("FINAL SCORE  %d", score), sw/2 - MeasureText(TextFormat("FINAL SCORE  %d", score), 30)/2, sh/2 + 10, 30, WHITE);
                DrawText("PRESS ENTER FOR ANOTHER WAVE", sw/2 - MeasureText("PRESS ENTER FOR ANOTHER WAVE", 22)/2, sh/2 + 60, 22, (Color){ 140, 155, 185, 255 });
            }
        EndDrawing();
    }

    UnloadSound(sndShoot);
    UnloadSound(sndHit);
    UnloadSound(sndCover);
    UnloadSound(sndPlayerHit);
    UnloadSound(sndEnemyFire);
    UnloadSound(sndSaucer);
    CloseAudioDevice();
    UnloadModel(modelTet);
    UnloadModel(modelOct);
    UnloadModel(modelDod);
    UnloadModel(modelCov);
    UnloadModel(modelSph);
    UnloadModel(modelPB);
    UnloadModel(modelEB);
    UnloadModel(modelPanel);
    UnloadShader(shader);
    CloseWindow();
    return 0;
}