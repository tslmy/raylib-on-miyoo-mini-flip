#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include "btBulletDynamicsCommon.h"

// MMF button → raylib key constants (from evdev probe)
#define MMF_DPAD_UP     265
#define MMF_DPAD_DOWN   264
#define MMF_DPAD_LEFT   263
#define MMF_DPAD_RIGHT  262
#define MMF_A           32   // KEY_SPACE
#define MMF_B           341  // KEY_LEFT_CONTROL
#define MMF_L1          69   // KEY_E
#define MMF_R1          84   // KEY_T

static float RandF(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

// ═══════════════════════════════════════════════════════════════════
// Dice geometry — ported from tslmy/threejs-dice
// ═══════════════════════════════════════════════════════════════════

#define MAX_DIE_VERTS 20
#define MAX_DIE_FACES 20
#define MAX_FACE_VERTS 5

struct Face {
    int idx[MAX_FACE_VERTS];
    int count;
    int value;  // face number (≥0), or -1 for non-numbered edge faces (d10)
};

struct DiceDef {
    const char* name;
    int numValues;
    float scaleFactor;
    bool invertUpside; // true for d4: read bottom face, not top
    int numVerts;
    float rawVerts[MAX_DIE_VERTS][3];
    int numFaces;
    Face faces[MAX_DIE_FACES];
};

static const DiceDef DICE_DEFS[] = {
    // ─── D4 (tetrahedron) ───
    {
        "d4", 4, 1.2f, true,
        4,
        {{1,1,1}, {-1,-1,1}, {-1,1,-1}, {1,-1,-1}},
        4,
        {
            {{1,0,2},       3, 1},
            {{0,1,3},       3, 2},
            {{0,3,2},       3, 3},
            {{1,2,3},       3, 4},
        }
    },
    // ─── D6 (cube) ───
    {
        "d6", 6, 0.9f, false,
        8,
        {{-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
         {-1,-1,1},  {1,-1,1},  {1,1,1},  {-1,1,1}},
        6,
        {
            {{0,3,2,1},     4, 1},
            {{1,2,6,5},     4, 2},
            {{0,1,5,4},     4, 3},
            {{3,7,6,2},     4, 4},
            {{0,4,7,3},     4, 5},
            {{4,5,6,7},     4, 6},
        }
    },
    // ─── D8 (octahedron) ───
    {
        "d8", 8, 1.0f, false,
        6,
        {{1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}},
        8,
        {
            {{0,2,4},       3, 1},
            {{0,4,3},       3, 2},
            {{0,3,5},       3, 3},
            {{0,5,2},       3, 4},
            {{1,3,4},       3, 5},
            {{1,4,2},       3, 6},
            {{1,2,5},       3, 7},
            {{1,5,3},       3, 8},
        }
    },
    // ─── D10 (pentagonal trapezohedron) ───
    {
        "d10", 10, 0.9f, false,
        12,
        {
            { 1.000f,  0.000f, -0.105f},
            { 0.809f,  0.588f,  0.105f},
            { 0.309f,  0.951f, -0.105f},
            {-0.309f,  0.951f,  0.105f},
            {-0.809f,  0.588f, -0.105f},
            {-1.000f,  0.000f,  0.105f},
            {-0.809f, -0.588f, -0.105f},
            {-0.309f, -0.951f,  0.105f},
            { 0.309f, -0.951f, -0.105f},
            { 0.809f, -0.588f,  0.105f},
            { 0.000f,  0.000f, -1.000f},
            { 0.000f,  0.000f,  1.000f},
        },
        20,
        {
            // 10 value faces
            {{5,7,11},      3, 0},
            {{4,2,10},      3, 1},
            {{1,3,11},      3, 2},
            {{0,8,10},      3, 3},
            {{7,9,11},      3, 4},
            {{8,6,10},      3, 5},
            {{9,1,11},      3, 6},
            {{2,0,10},      3, 7},
            {{3,5,11},      3, 8},
            {{6,4,10},      3, 9},
            // 10 non-value edge faces
            {{1,0,2},       3, -1},
            {{1,2,3},       3, -1},
            {{3,2,4},       3, -1},
            {{3,4,5},       3, -1},
            {{5,4,6},       3, -1},
            {{5,6,7},       3, -1},
            {{7,6,8},       3, -1},
            {{7,8,9},       3, -1},
            {{9,8,0},       3, -1},
            {{9,0,1},       3, -1},
        }
    },
    // ─── D12 (dodecahedron) ───
    {
        "d12", 12, 0.9f, false,
        20,
        {
            { 0.000f,  0.618f,  1.618f},
            { 0.000f,  0.618f, -1.618f},
            { 0.000f, -0.618f,  1.618f},
            { 0.000f, -0.618f, -1.618f},
            { 1.618f,  0.000f,  0.618f},
            { 1.618f,  0.000f, -0.618f},
            {-1.618f,  0.000f,  0.618f},
            {-1.618f,  0.000f, -0.618f},
            { 0.618f,  1.618f,  0.000f},
            { 0.618f, -1.618f,  0.000f},
            {-0.618f,  1.618f,  0.000f},
            {-0.618f, -1.618f,  0.000f},
            { 1.000f,  1.000f,  1.000f},
            { 1.000f,  1.000f, -1.000f},
            { 1.000f, -1.000f,  1.000f},
            { 1.000f, -1.000f, -1.000f},
            {-1.000f,  1.000f,  1.000f},
            {-1.000f,  1.000f, -1.000f},
            {-1.000f, -1.000f,  1.000f},
            {-1.000f, -1.000f, -1.000f},
        },
        12,
        {
            {{2,14,4,12,0}, 5, 1},
            {{15,9,11,19,3},5, 2},
            {{16,10,17,7,6},5, 3},
            {{6,7,19,11,18},5, 4},
            {{6,18,2,0,16}, 5, 5},
            {{18,11,9,14,2},5, 6},
            {{1,17,10,8,13},5, 7},
            {{1,13,5,15,3}, 5, 8},
            {{13,8,12,4,5}, 5, 9},
            {{5,4,14,9,15}, 5, 10},
            {{0,12,8,10,16},5, 11},
            {{3,19,7,17,1}, 5, 12},
        }
    },
    // ─── D20 (icosahedron) ───
    {
        "d20", 20, 1.0f, false,
        12,
        {
            {-1.000f,  1.618f,  0.000f},
            { 1.000f,  1.618f,  0.000f},
            {-1.000f, -1.618f,  0.000f},
            { 1.000f, -1.618f,  0.000f},
            { 0.000f, -1.000f,  1.618f},
            { 0.000f,  1.000f,  1.618f},
            { 0.000f, -1.000f, -1.618f},
            { 0.000f,  1.000f, -1.618f},
            { 1.618f,  0.000f, -1.000f},
            { 1.618f,  0.000f,  1.000f},
            {-1.618f,  0.000f, -1.000f},
            {-1.618f,  0.000f,  1.000f},
        },
        20,
        {
            {{0,11,5},      3, 1},
            {{0,5,1},       3, 2},
            {{0,1,7},       3, 3},
            {{0,7,10},      3, 4},
            {{0,10,11},     3, 5},
            {{1,5,9},       3, 6},
            {{5,11,4},      3, 7},
            {{11,10,2},     3, 8},
            {{10,7,6},      3, 9},
            {{7,1,8},       3, 10},
            {{3,9,4},       3, 11},
            {{3,4,2},       3, 12},
            {{3,2,6},       3, 13},
            {{3,6,8},       3, 14},
            {{3,8,9},       3, 15},
            {{4,9,5},       3, 16},
            {{2,4,11},      3, 17},
            {{6,2,10},      3, 18},
            {{8,6,7},       3, 19},
            {{9,8,1},       3, 20},
        }
    },
};
#define NUM_DICE_TYPES 6

// ═══════════════════════════════════════════════════════════════════
// Runtime die geometry (normalized + scaled)
// ═══════════════════════════════════════════════════════════════════

static const float DIE_RADIUS = 0.7f;
static Vector3 dieVerts[MAX_DIE_VERTS];
static int dieNumVerts;
static Face dieFaces[MAX_DIE_FACES];
static int dieNumFaces;

static void SetupDieGeometry(int typeIdx) {
    const DiceDef& def = DICE_DEFS[typeIdx];
    dieNumVerts = def.numVerts;
    dieNumFaces = def.numFaces;
    float r = DIE_RADIUS * def.scaleFactor;
    for (int i = 0; i < def.numVerts; i++) {
        Vector3 v = {def.rawVerts[i][0], def.rawVerts[i][1], def.rawVerts[i][2]};
        dieVerts[i] = Vector3Scale(Vector3Normalize(v), r);
    }
    for (int i = 0; i < def.numFaces; i++)
        dieFaces[i] = def.faces[i];
}

// ═══════════════════════════════════════════════════════════════════
// Bullet3 physics
// ═══════════════════════════════════════════════════════════════════

static btDiscreteDynamicsWorld* world;
static btRigidBody* dieBody;
static btRigidBody* floorBody;
static btDefaultCollisionConfiguration* collisionConfig;
static btCollisionDispatcher* dispatcher;
static btDbvtBroadphase* broadphase;
static btSequentialImpulseConstraintSolver* solver;

static void InitPhysics() {
    collisionConfig = new btDefaultCollisionConfiguration();
    dispatcher = new btCollisionDispatcher(collisionConfig);
    broadphase = new btDbvtBroadphase();
    solver = new btSequentialImpulseConstraintSolver();
    world = new btDiscreteDynamicsWorld(dispatcher, broadphase, solver, collisionConfig);
    world->setGravity(btVector3(0, -9.81f, 0));

    btStaticPlaneShape* floorShape = new btStaticPlaneShape(btVector3(0, 1, 0), 0);
    btDefaultMotionState* floorMotion = new btDefaultMotionState();
    btRigidBody::btRigidBodyConstructionInfo floorCI(0, floorMotion, floorShape);
    floorCI.m_restitution = 0.5f;
    floorCI.m_friction = 0.8f;
    floorBody = new btRigidBody(floorCI);
    world->addRigidBody(floorBody);
}

static void ThrowDie() {
    if (dieBody) {
        world->removeRigidBody(dieBody);
        delete dieBody->getMotionState();
        delete dieBody->getCollisionShape();
        delete dieBody;
        dieBody = nullptr;
    }

    btConvexHullShape* shape = new btConvexHullShape();
    for (int i = 0; i < dieNumVerts; i++)
        shape->addPoint(btVector3(dieVerts[i].x, dieVerts[i].y, dieVerts[i].z));

    btScalar mass = 1.0f;
    btVector3 inertia;
    shape->calculateLocalInertia(mass, inertia);

    btQuaternion rot;
    rot.setEulerZYX(RandF(0, 6.28f), RandF(0, 6.28f), RandF(0, 6.28f));

    btTransform startTransform;
    startTransform.setIdentity();
    startTransform.setOrigin(btVector3(RandF(-0.3f, 0.3f), 4.0f, RandF(-0.3f, 0.3f)));
    startTransform.setRotation(rot);

    btDefaultMotionState* ms = new btDefaultMotionState(startTransform);
    btRigidBody::btRigidBodyConstructionInfo ci(mass, ms, shape, inertia);
    ci.m_restitution = 0.4f;
    ci.m_friction = 0.6f;

    dieBody = new btRigidBody(ci);
    dieBody->setAngularVelocity(btVector3(RandF(-8, 8), RandF(-8, 8), RandF(-8, 8)));
    dieBody->setLinearVelocity(btVector3(RandF(-1, 1), 0, RandF(-1, 1)));
    world->addRigidBody(dieBody);
}

static void CleanupPhysics() {
    if (dieBody) {
        world->removeRigidBody(dieBody);
        delete dieBody->getMotionState();
        delete dieBody->getCollisionShape();
        delete dieBody;
    }
    world->removeRigidBody(floorBody);
    delete floorBody->getMotionState();
    delete floorBody->getCollisionShape();
    delete floorBody;
    delete world;
    delete solver;
    delete broadphase;
    delete dispatcher;
    delete collisionConfig;
}

// ═══════════════════════════════════════════════════════════════════
// Die reading — settle detection + face-up value
// ═══════════════════════════════════════════════════════════════════

static bool IsDieSettled() {
    btVector3 v = dieBody->getLinearVelocity();
    btVector3 w = dieBody->getAngularVelocity();
    return v.length() < 0.3f && w.length() < 0.3f;
}

static Vector3 FaceNormal(const Vector3* wv, const Face& f) {
    Vector3 e1 = Vector3Subtract(wv[f.idx[1]], wv[f.idx[0]]);
    Vector3 e2 = Vector3Subtract(wv[f.idx[2]], wv[f.idx[0]]);
    return Vector3Normalize(Vector3CrossProduct(e1, e2));
}

static int GetFaceUpValue(const Matrix& xform, bool invertUp) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < dieNumVerts; i++)
        wv[i] = Vector3Transform(dieVerts[i], xform);

    Vector3 upDir = invertUp ? (Vector3){0, -1, 0} : (Vector3){0, 1, 0};
    int bestFace = -1;
    float bestDot = -2.0f;

    for (int f = 0; f < dieNumFaces; f++) {
        if (dieFaces[f].value < 0) continue;
        Vector3 n = FaceNormal(wv, dieFaces[f]);
        float d = Vector3DotProduct(n, upDir);
        if (d > bestDot) { bestDot = d; bestFace = f; }
    }
    return bestFace >= 0 ? dieFaces[bestFace].value : -1;
}

// ═══════════════════════════════════════════════════════════════════
// Rendering
// ═══════════════════════════════════════════════════════════════════

static const Vector3 LIGHT_DIR = Vector3Normalize((Vector3){0.3f, 0.6f, -0.7f});

static void DrawDie(const Matrix& xform) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < dieNumVerts; i++)
        wv[i] = Vector3Transform(dieVerts[i], xform);

    // Filled faces with lighting
    for (int f = 0; f < dieNumFaces; f++) {
        const Face& face = dieFaces[f];
        Vector3 n = FaceNormal(wv, face);
        float ndotl = Vector3DotProduct(n, LIGHT_DIR);
        if (ndotl < 0) ndotl = 0;
        float intensity = 0.25f + 0.75f * ndotl;

        Color col;
        if (face.value >= 0) {
            unsigned char c = (unsigned char)(235 * intensity);
            col = {c, c, (unsigned char)(255 * intensity), 255};
        } else {
            unsigned char c = (unsigned char)(150 * intensity);
            col = {c, c, (unsigned char)(170 * intensity), 255};
        }

        for (int j = 1; j < face.count - 1; j++)
            DrawTriangle3D(wv[face.idx[0]], wv[face.idx[j]], wv[face.idx[j+1]], col);
    }

    // Wireframe edges
    Color wire = {50, 55, 65, 255};
    for (int f = 0; f < dieNumFaces; f++) {
        const Face& face = dieFaces[f];
        for (int j = 0; j < face.count; j++)
            DrawLine3D(wv[face.idx[j]], wv[face.idx[(j+1) % face.count]], wire);
    }
}

// Project 3D point to 2D screen coordinates (manual MVP, since
// GetWorldToScreen may not be available on all raylib builds).
static Vector2 Project3D(Vector3 pos, Camera3D cam, int sw, int sh) {
    Matrix view = MatrixLookAt(cam.position, cam.target, cam.up);
    Matrix proj = MatrixPerspective(cam.fovy * DEG2RAD, (float)sw / sh, 0.01f, 1000.0f);
    Matrix mvp = MatrixMultiply(view, proj);
    float x = pos.x, y = pos.y, z = pos.z;
    float cx = mvp.m0*x + mvp.m4*y + mvp.m8*z  + mvp.m12;
    float cy = mvp.m1*x + mvp.m5*y + mvp.m9*z  + mvp.m13;
    float cw = mvp.m3*x + mvp.m7*y + mvp.m11*z + mvp.m15;
    if (cw < 0.001f) return {-1000, -1000};
    return {(cx/cw + 1.0f) * 0.5f * sw, (1.0f - cy/cw) * 0.5f * sh};
}

static void DrawFaceNumbers(const Matrix& xform, Camera3D cam, int sw, int sh) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < dieNumVerts; i++)
        wv[i] = Vector3Transform(dieVerts[i], xform);

    Vector3 camPos = cam.position;

    for (int f = 0; f < dieNumFaces; f++) {
        if (dieFaces[f].value < 0) continue;
        const Face& face = dieFaces[f];

        // Face center
        Vector3 center = {0, 0, 0};
        for (int i = 0; i < face.count; i++)
            center = Vector3Add(center, wv[face.idx[i]]);
        center = Vector3Scale(center, 1.0f / face.count);

        // Only draw if face is roughly toward camera
        Vector3 n = FaceNormal(wv, face);
        Vector3 toCamera = Vector3Normalize(Vector3Subtract(camPos, center));
        if (Vector3DotProduct(n, toCamera) < 0.15f) continue;

        Vector2 sp = Project3D(center, cam, sw, sh);
        if (sp.x < 0 || sp.x > sw || sp.y < 0 || sp.y > sh) continue;

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", face.value);
        int fontSize = 14;
        int tw = MeasureText(buf, fontSize);
        DrawText(buf, (int)sp.x - tw/2, (int)sp.y - fontSize/2,
                 fontSize, (Color){30, 30, 40, 255});
    }
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(void) {
    const int screenWidth = 750;
    const int screenHeight = 560;

    srand((unsigned)time(nullptr));
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(screenWidth, screenHeight, "Dice Roller - MMF");
    SetTargetFPS(30);

    int currentType = 1; // start on d6
    SetupDieGeometry(currentType);
    InitPhysics();
    ThrowDie();

    float camDist = 5.0f, camYaw = 45.0f, camPitch = 35.0f;
    Camera3D camera = {0};
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    int rolledValue = -1;
    int settledFrames = 0;
    bool showResult = false;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (dt > 0 && dt < 0.1f)
            world->stepSimulation(dt, 4, 1.0f/120.0f);

        // ── Controls ──
        if (IsKeyDown(MMF_DPAD_LEFT))  camYaw   -= 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_RIGHT)) camYaw   += 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_UP))    camPitch += 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_DOWN))  camPitch -= 90.0f * dt;
        if (IsKeyDown(MMF_L1))         camDist  -= 5.0f * dt;
        if (IsKeyDown(MMF_R1))         camDist  += 5.0f * dt;

        if (IsKeyPressed(MMF_A)) {
            ThrowDie();
            rolledValue = -1; settledFrames = 0; showResult = false;
        }
        if (IsKeyPressed(MMF_B)) {
            currentType = (currentType + 1) % NUM_DICE_TYPES;
            SetupDieGeometry(currentType);
            ThrowDie();
            rolledValue = -1; settledFrames = 0; showResult = false;
        }

        if (camPitch > 85) camPitch = 85;
        if (camPitch < -10) camPitch = -10;
        if (camDist < 2) camDist = 2;
        if (camDist > 15) camDist = 15;

        float yr = camYaw * DEG2RAD, pr = camPitch * DEG2RAD;
        camera.position = {
            camDist * cosf(pr) * sinf(yr),
            camDist * sinf(pr),
            camDist * cosf(pr) * cosf(yr),
        };
        camera.target = {0, 0.5f, 0};

        // ── Physics transform ──
        float m[16];
        dieBody->getWorldTransform().getOpenGLMatrix(m);
        Matrix xform = {
            m[0], m[4], m[8],  m[12],
            m[1], m[5], m[9],  m[13],
            m[2], m[6], m[10], m[14],
            m[3], m[7], m[11], m[15],
        };

        // ── Settle detection ──
        if (!showResult && IsDieSettled()) {
            if (++settledFrames > 30) {
                rolledValue = GetFaceUpValue(xform, DICE_DEFS[currentType].invertUpside);
                showResult = true;
            }
        } else if (!showResult) {
            settledFrames = 0;
        }

        // ── Draw ──
        BeginDrawing();
        ClearBackground({20, 24, 30, 255});

        BeginMode3D(camera);
        rlDisableBackfaceCulling();
        DrawDie(xform);
        rlEnableBackfaceCulling();

        // Floor
        DrawTriangle3D({-10,0,-10}, {10,0,-10}, {10,0,10}, {40,50,60,255});
        DrawTriangle3D({-10,0,-10}, {10,0,10},  {-10,0,10},{40,50,60,255});
        DrawGrid(20, 1.0f);
        EndMode3D();

        // Face numbers (2D overlay)
        DrawFaceNumbers(xform, camera, screenWidth, screenHeight);

        // HUD
        DrawText(TextFormat("Dice: %s", DICE_DEFS[currentType].name),
                 20, 20, 20, {230, 230, 230, 255});

        if (showResult && rolledValue >= 0)
            DrawText(TextFormat("Rolled: %d", rolledValue),
                     screenWidth/2 - 60, 20, 30, {255, 220, 50, 255});

        DrawText("[A] Throw  [B] Type  [D-Pad] Orbit  [L1/R1] Zoom",
                 20, screenHeight - 30, 14, {160, 160, 160, 255});

        if (getenv("RAYLIB_MMF_SHOWFPS"))
            DrawFPS(20, 50);

        EndDrawing();
    }

    CleanupPhysics();
    CloseWindow();
    return 0;
}
