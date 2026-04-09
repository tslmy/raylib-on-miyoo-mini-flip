#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#define MMF_L1          69   // KEY_E
#define MMF_R1          84   // KEY_T

static float RandF(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

// Bullet3 world objects
static btDiscreteDynamicsWorld* world;
static btRigidBody* cubeBody;
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

    // Floor (static infinite plane at y=0)
    btStaticPlaneShape* floorShape = new btStaticPlaneShape(btVector3(0, 1, 0), 0);
    btDefaultMotionState* floorMotion = new btDefaultMotionState();
    btRigidBody::btRigidBodyConstructionInfo floorCI(0, floorMotion, floorShape);
    floorCI.m_restitution = 0.5f;
    floorCI.m_friction = 0.8f;
    floorBody = new btRigidBody(floorCI);
    world->addRigidBody(floorBody);
}

static void ResetCube() {
    // Remove existing cube if any
    if (cubeBody) {
        world->removeRigidBody(cubeBody);
        delete cubeBody->getMotionState();
        delete cubeBody;
        cubeBody = nullptr;
    }

    float halfSize = 0.6f;
    btBoxShape* cubeShape = new btBoxShape(btVector3(halfSize, halfSize, halfSize));

    btScalar mass = 1.0f;
    btVector3 inertia;
    cubeShape->calculateLocalInertia(mass, inertia);

    // Random orientation
    btQuaternion rot;
    rot.setEulerZYX(RandF(0, 6.28f), RandF(0, 6.28f), RandF(0, 6.28f));

    btTransform startTransform;
    startTransform.setIdentity();
    startTransform.setOrigin(btVector3(0, 6.0f, 0));
    startTransform.setRotation(rot);

    btDefaultMotionState* motionState = new btDefaultMotionState(startTransform);
    btRigidBody::btRigidBodyConstructionInfo ci(mass, motionState, cubeShape, inertia);
    ci.m_restitution = 0.4f;
    ci.m_friction = 0.6f;

    cubeBody = new btRigidBody(ci);
    cubeBody->setAngularVelocity(btVector3(RandF(-5, 5), RandF(-5, 5), RandF(-5, 5)));
    world->addRigidBody(cubeBody);
}

static void CleanupPhysics() {
    if (cubeBody) {
        world->removeRigidBody(cubeBody);
        delete cubeBody->getMotionState();
        delete cubeBody->getCollisionShape();
        delete cubeBody;
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

int main(int argc, char **argv) {
    // --screenshot N: run N frames headlessly, save screenshot.png, exit
    int screenshotFrames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotFrames = atoi(argv[++i]);
    }

    const int screenWidth = 752;
    const int screenHeight = 560;

    srand((unsigned)time(nullptr));

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(screenWidth, screenHeight, "Bullet3 Cube - MMF");
    SetTargetFPS(30);

    InitPhysics();
    ResetCube();

    // Orbit camera state
    float camDist = 10.0f;
    float camYaw = 45.0f;
    float camPitch = 25.0f;
    const float CAM_ORBIT_SPEED = 90.0f;
    const float CAM_ZOOM_SPEED = 5.0f;

    Camera3D camera = { 0 };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // Cube mesh data (same as before)
    const float cubeSize = 1.2f;
    const float half = cubeSize * 0.5f;
    const Vector3 baseVerts[8] = {
        {-half, -half, -half}, {half, -half, -half},
        {half, half, -half},   {-half, half, -half},
        {-half, -half, half},  {half, -half, half},
        {half, half, half},    {-half, half, half},
    };
    const int faces[6][4] = {
        {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {3,2,6,7}, {4,5,1,0},
    };
    const Vector3 faceNormals[6] = {
        {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0}, {0,1,0}, {0,-1,0},
    };
    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7},
    };
    const Vector3 lightDir = Vector3Normalize((Vector3){0.3f, 0.6f, -0.7f});

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        // Step physics
        if (dt > 0 && dt < 0.1f)
            world->stepSimulation(dt, 4, 1.0f/120.0f);

        // Camera control
        if (IsKeyDown(MMF_DPAD_LEFT))  camYaw   -= CAM_ORBIT_SPEED * dt;
        if (IsKeyDown(MMF_DPAD_RIGHT)) camYaw   += CAM_ORBIT_SPEED * dt;
        if (IsKeyDown(MMF_DPAD_UP))    camPitch += CAM_ORBIT_SPEED * dt;
        if (IsKeyDown(MMF_DPAD_DOWN))  camPitch -= CAM_ORBIT_SPEED * dt;
        if (IsKeyDown(MMF_L1))         camDist  -= CAM_ZOOM_SPEED * dt;
        if (IsKeyDown(MMF_R1))         camDist  += CAM_ZOOM_SPEED * dt;
        if (IsKeyPressed(MMF_A))       ResetCube(); // Re-throw

        if (camPitch > 85.0f) camPitch = 85.0f;
        if (camPitch < -10.0f) camPitch = -10.0f;
        if (camDist < 3.0f) camDist = 3.0f;
        if (camDist > 25.0f) camDist = 25.0f;

        float yawRad = camYaw * DEG2RAD;
        float pitchRad = camPitch * DEG2RAD;
        camera.position = (Vector3){
            camDist * cosf(pitchRad) * sinf(yawRad),
            camDist * sinf(pitchRad),
            camDist * cosf(pitchRad) * cosf(yawRad),
        };
        camera.target = (Vector3){ 0.0f, 1.0f, 0.0f };

        // Get cube transform from Bullet (column-major float[16])
        float m[16];
        cubeBody->getWorldTransform().getOpenGLMatrix(m);
        // Raylib's Matrix struct is {m0,m4,m8,m12, m1,m5,m9,m13, ...}
        // in memory — NOT sequential. Must map by field name, not cast.
        Matrix cubeTransform = {
            m[0], m[4], m[8],  m[12],
            m[1], m[5], m[9],  m[13],
            m[2], m[6], m[10], m[14],
            m[3], m[7], m[11], m[15],
        };

        BeginDrawing();
        ClearBackground((Color){ 20, 24, 30, 255 });

        BeginMode3D(camera);
        rlDisableBackfaceCulling();

        // Draw cube using physics transform
        Vector3 verts[8];
        for (int i = 0; i < 8; i++)
            verts[i] = Vector3Transform(baseVerts[i], cubeTransform);

        for (int f = 0; f < 6; f++) {
            Vector3 n = Vector3Transform(faceNormals[f], cubeTransform);
            // Remove translation from normal
            n = Vector3Subtract(n, (Vector3){cubeTransform.m12, cubeTransform.m13, cubeTransform.m14});
            float ndotl = Vector3DotProduct(Vector3Normalize(n), lightDir);
            if (ndotl < 0.0f) ndotl = 0.0f;
            float intensity = 0.2f + 0.8f * ndotl;
            Color c = {
                (unsigned char)(0 * intensity),
                (unsigned char)(170 * intensity),
                (unsigned char)(255 * intensity),
                255
            };
            int i0 = faces[f][0], i1 = faces[f][1], i2 = faces[f][2], i3 = faces[f][3];
            DrawTriangle3D(verts[i0], verts[i1], verts[i2], c);
            DrawTriangle3D(verts[i0], verts[i2], verts[i3], c);
        }

        Color wire = {255, 255, 255, 255};
        for (int e = 0; e < 12; e++)
            DrawLine3D(verts[edges[e][0]], verts[edges[e][1]], wire);

        rlEnableBackfaceCulling();

        // Draw floor grid
        DrawGrid(20, 1.0f);

        // Draw a simple floor quad
        DrawTriangle3D(
            (Vector3){-10, 0, -10}, (Vector3){10, 0, -10}, (Vector3){10, 0, 10},
            (Color){40, 50, 60, 255});
        DrawTriangle3D(
            (Vector3){-10, 0, -10}, (Vector3){10, 0, 10}, (Vector3){-10, 0, 10},
            (Color){40, 50, 60, 255});

        EndMode3D();

        DrawText("Bullet3 Physics on MMF", 20, 20, 20, (Color){230, 230, 230, 255});
        DrawText("[A] Throw  [D-Pad] Orbit  [L1/R1] Zoom", 20, screenHeight - 30, 14, (Color){160, 160, 160, 255});
        if (getenv("RAYLIB_MMF_SHOWFPS"))
            DrawFPS(20, 50);
        EndDrawing();

        // Headless screenshot mode: capture after N frames and exit
        if (screenshotFrames > 0) {
            static int frameCounter = 0;
            if (++frameCounter >= screenshotFrames) {
                TakeScreenshot("screenshot.png");
                break;
            }
        }
    }

    CleanupPhysics();
    CloseWindow();
    return 0;
}
