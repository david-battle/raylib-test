#include "raylib.h"
#include "raymath.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Lighting shader (GLSL 330, matching the static raylib build). Attribute and
// uniform names ("vertexPosition", "vertexNormal", "mvp", "matModel",
// "colDiffuse") are auto-located by raylib. Flat per-face normals give the
// polyhedra a faceted gem look; a white specular highlight adds a sheen.
// The sphere mesh uses smooth per-vertex normals, so it shades as a polished
// ball with the same shader. A black base color (unused now) stays black.
// ---------------------------------------------------------------------------
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
    // Model transform is a pure rotation (no scaling or translation), so the
    // upper-3x3 is an orthonormal rotation: normals transform with it directly.
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
    // Specular only on lit surfaces; a black base (unused) stays pure black.
    "    float lit = step(0.01, dot(base, vec3(1.0)));\n"
    "    vec3 color = base * (0.30 + 0.70*ndl) + vec3(0.22)*spec*lit;\n"
    "    finalColor = vec4(color, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------------------
// Canonical vertex/face tables for the platonic solids. Vertices are given at
// unit circumradius ("raw", not normalized; MeshPolyhedron normalizes) and
// face windings need not be correct — MeshPolyhedron orients each face
// outward automatically. Tetrahedron (4 faces), cube (6), octahedron (8),
// icosahedron (20). The dodecahedron (12) is built as the icosahedron's dual,
// so it gets no table of its own.
// ---------------------------------------------------------------------------
static const float TET_V[4][3] = {
    { 1,  1,  1 }, {  1, -1, -1 }, { -1,  1, -1 }, { -1, -1,  1 },
};
static const int TET_F[4][3] = {
    { 0,1,2 }, { 0,3,1 }, { 0,2,3 }, { 1,3,2 },
};

static const float CUB_V[8][3] = {
    { -1,-1,-1 }, { 1,-1,-1 }, { 1,1,-1 }, { -1,1,-1 },
    { -1,-1, 1 }, { 1,-1, 1 }, { 1,1, 1 }, { -1,1, 1 },
};
static const int CUB_F[12][3] = {
    { 0,1,2 }, { 0,2,3 },                    // -Z face
    { 4,5,6 }, { 4,6,7 },                    // +Z face
    { 0,3,7 }, { 0,7,4 },                    // -X face
    { 1,5,6 }, { 1,6,2 },                    // +X face
    { 0,4,5 }, { 0,5,1 },                    // -Y face
    { 3,2,6 }, { 3,6,7 },                    // +Y face
};

static const float OCT_V[6][3] = {
    { 1,0,0 }, { 0,1,0 }, { 0,0,1 }, { -1,0,0 }, { 0,-1,0 }, { 0,0,-1 },
};
static const int OCT_F[8][3] = {
    { 2,0,1 }, { 2,1,3 }, { 2,3,4 }, { 2,4,0 },       // upper half (v2)
    { 5,1,0 }, { 5,3,1 }, { 5,4,3 }, { 5,0,4 },       // lower half (v5)
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

// ---------------------------------------------------------------------------
// Build a flat-shaded polyhedron mesh from a vertex set and triangle face
// table. Vertices are normalized out to `radius`; each face's vertices are
// duplicated so the face carries a constant (outward, hence flat) normal.
// Face winding is auto-corrected: if the cross-product normal points toward
// the polyhedron's centroid, the indices are swapped.
// ---------------------------------------------------------------------------
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
        // Auto-orient outward: the normal must point away from the centroid
        // of the triangle (the solid always encloses the origin). Swap the
        // face's winding too, so the corrected normal matches the vertex
        // order the rasterizer uses for back-face culling.
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
static Mesh MeshCube(float radius)         { return MeshPolyhedron(CUB_V, 8, CUB_F, 12, radius); }
static Mesh MeshOctahedron(float radius)   { return MeshPolyhedron(OCT_V, 6, OCT_F, 8, radius); }
static Mesh MeshIcosahedron(float radius)  { return MeshPolyhedron(ICO_V, 12, ICO_F, 20, radius); }

// ---------------------------------------------------------------------------
// Dodecahedron = dual of the icosahedron. Its 20 vertices are the normalized
// centroids of the icosahedron's 20 faces; its 12 pentagonal faces are the
// icosahedron's 12 vertices, each spanned by the 5 face-centroids touching
// it, ordered by angle around that vertex. Built at runtime so the face
// topology can never drift out of sync with Iconic_F.
// ---------------------------------------------------------------------------
static Mesh MeshDodecahedron(float radius)
{
    float u[12][3];          // unit icosahedron vertices
    float D[20][3];          // unit dodecahedron vertices (face centroids)
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

    // For each icosa vertex, order the 5 faces touching it around it, then
    // fan the resulting pentagon into 3 triangles.
    int tris[36][3];
    int t = 0;
    for (int vi = 0; vi < 12; vi++) {
        int face[5], n = 0;
        for (int fi = 0; fi < 20 && n < 5; fi++)
            if (ICO_F[fi][0] == vi || ICO_F[fi][1] == vi || ICO_F[fi][2] == vi)
                face[n++] = fi;

        // Project the 5 centroids onto the plane through the origin
        // perpendicular to u[vi], then sort by angle around u[vi].
        float q[5][3], ref[3], ref2[3], ang[5];
        for (int k = 0; k < 5; k++) {
            float dc = D[face[k]][0]*u[vi][0] + D[face[k]][1]*u[vi][1] + D[face[k]][2]*u[vi][2];
            q[k][0] = D[face[k]][0] - dc*u[vi][0];
            q[k][1] = D[face[k]][1] - dc*u[vi][1];
            q[k][2] = D[face[k]][2] - dc*u[vi][2];
        }
        float rl = sqrtf(q[0][0]*q[0][0] + q[0][1]*q[0][1] + q[0][2]*q[0][2]);
        ref[0] = q[0][0]/rl; ref[1] = q[0][1]/rl; ref[2] = q[0][2]/rl;
        // ref2 = cross(u[vi], ref)
        ref2[0] = u[vi][1]*ref[2] - u[vi][2]*ref[1];
        ref2[1] = u[vi][2]*ref[0] - u[vi][0]*ref[2];
        ref2[2] = u[vi][0]*ref[1] - u[vi][1]*ref[0];
        for (int k = 0; k < 5; k++)
            ang[k] = atan2f(q[k][0]*ref2[0] + q[k][1]*ref2[1] + q[k][2]*ref2[2],
                            q[k][0]*ref[0]  + q[k][1]*ref[1]  + q[k][2]*ref[2]);
        // insertion sort (of 5) by angle, in place
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

// Pick the external (non-built-in laptop) monitor. Built-in panels are
// excluded by name; if several remain the largest wins, else fall back to
// the current monitor. Fullscreen then goes there via SetWindowMonitor().
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

#define NUM_SOLIDS 6

// Left to right, top to bottom (3 cols x 2 rows), ordered by increasing face
// count: tetrahedron 4, cube 6, octahedron 8, dodecahedron 12, icosahedron
// 20, sphere (infinite). Each gets a jewel tone and its own on-axis camera
// distance (per-solid worst-case silhouette fitting, measured offline).
static Color JewelColor[NUM_SOLIDS] = {
    { 158,  22,  46, 255 },   // ruby       (tetrahedron)
    {  18, 130,  72, 255 },   // emerald    (cube)
    {  22,  66, 158, 255 },   // sapphire   (octahedron)
    { 124,  38, 152, 255 },   // amethyst   (dodecahedron)
    { 214, 128,  26, 255 },   // topaz      (icosahedron)
    { 230, 182,  44, 255 },   // citrine    (sphere)
};

static float SolidDist[NUM_SOLIDS] = { 8.69f, 9.99f, 9.99f, 9.99f, 9.99f, 9.21f };

int main(void)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT);   // 4x MSAA on the main framebuffer

    InitWindow(1920, 1080, "Platonic Solids");
    SetWindowState(FLAG_FULLSCREEN_MODE);
    SetWindowMonitor(SelectExternalMonitor());
    SetTargetFPS(60);

    Shader shader = LoadShaderFromMemory(SOLID_VS, SOLID_FS);
    int locLight = GetShaderLocation(shader, "lightDir");
    int locView = GetShaderLocation(shader, "viewPos");
    float lightDir[3] = { 0.44f, 0.66f, 0.61f };   // normalized, toward the light

    Model model[NUM_SOLIDS];
    float rotX[NUM_SOLIDS], rotY[NUM_SOLIDS];

    Mesh custom[NUM_SOLIDS-1];        // polyhedra (sphere is GenMesh*, self-uploading)
    custom[0] = MeshTetrahedron(1.0f);
    custom[1] = MeshCube(1.0f);
    custom[2] = MeshOctahedron(1.0f);
    custom[3] = MeshDodecahedron(1.0f);
    custom[4] = MeshIcosahedron(1.0f);
    for (int i = 0; i < NUM_SOLIDS-1; i++) { UploadMesh(&custom[i], false); model[i] = LoadModelFromMesh(custom[i]); }
    // NOTE: GenMesh* uploads to the GPU itself, but meshes built by
    // MeshPolyhedron() must be uploaded explicitly — LoadModelFromMesh() only
    // copies CPU data, and DrawMesh() silently draws nothing for a mesh whose
    // vaoId/vboId are still unset.
    model[5] = LoadModelFromMesh(GenMeshSphere(1.0f, 24, 16));

    for (int i = 0; i < NUM_SOLIDS; i++) {
        model[i].materials[0].shader = shader;
        model[i].materials[0].maps[MATERIAL_MAP_DIFFUSE].color = JewelColor[i];
        rotX[i] = -14.0f;          // same starting tilt for every solid
        rotY[i] = 22.0f;
    }

    int sw = GetScreenWidth(), sh = GetScreenHeight();
    float fovy = 40.0f * DEG2RAD;
    float tanHalf = tanf(fovy / 2.0f);
    float cellW = sw / 3.0f, cellH = sh / 2.0f;      // one 1/6 cell

    Camera3D camera = { 0 };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 40.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Tilt state (degrees about the world X and Y axes), matching the
    // original static tilt. Arrow keys rotate only the active solid:
    // Left/Right spin it around the vertical (Y) axis — right = clockwise as
    // seen from the front; Up/Down tip it around the horizontal (X) axis.
    const float ROT_SPEED = 60.0f;   // degrees per second
    int active = 0;

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE)) break;
        if (IsKeyPressed(KEY_TAB)) active = (active + 1) % NUM_SOLIDS;

        float dt = GetFrameTime();
        if (IsKeyDown(KEY_UP))    rotX[active] -= ROT_SPEED * dt;
        if (IsKeyDown(KEY_DOWN))  rotX[active] += ROT_SPEED * dt;
        if (IsKeyDown(KEY_RIGHT)) rotY[active] += ROT_SPEED * dt;
        if (IsKeyDown(KEY_LEFT))  rotY[active] -= ROT_SPEED * dt;

        for (int i = 0; i < NUM_SOLIDS; i++)
            model[i].transform = MatrixMultiply(MatrixRotateX(rotX[i] * DEG2RAD),
                                                MatrixRotateY(rotY[i] * DEG2RAD));

        BeginDrawing();
            ClearBackground(WHITE);

            for (int i = 0; i < NUM_SOLIDS; i++) {
                // Aim the camera along a ray through this cell's center.
                int col = i % 3, row = i / 3;
                float cx = (col + 0.5f) * cellW;
                float cy = (row + 0.5f) * cellH;
                float pps = (sh / 2.0f) / (SolidDist[i] * tanHalf);   // px per world unit
                float camX = (sw / 2.0f - cx) / pps;
                float camY = (cy - sh / 2.0f) / pps;
                camera.position = (Vector3){ camX, camY, SolidDist[i] };
                camera.target = (Vector3){ camX, camY, 0.0f };
                float viewPos[3] = { camX, camY, SolidDist[i] };

                BeginMode3D(camera);
                    BeginShaderMode(shader);
                        SetShaderValue(shader, locLight, lightDir, SHADER_UNIFORM_VEC3);
                        SetShaderValue(shader, locView, viewPos, SHADER_UNIFORM_VEC3);
                        DrawModel(model[i], (Vector3){ 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
                    EndShaderMode();
                EndMode3D();
            }

            // Box around the active cell.
            int col = active % 3, row = active / 3;
            DrawRectangleLinesEx((Rectangle){ col*cellW, row*cellH, cellW, cellH },
                                 6.0f, (Color){ 28, 28, 28, 255 });
        EndDrawing();
    }

    for (int i = 0; i < NUM_SOLIDS; i++) UnloadModel(model[i]);
    UnloadShader(shader);
    CloseWindow();
    return 0;
}