#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <GL/gl.h>  // TinyGL — for direct glBlendFunc/glEnable(GL_BLEND) calls
#include "btBulletDynamicsCommon.h"

// MMF button → raylib key constants (from evdev probe)
#define MMF_DPAD_UP     265
#define MMF_DPAD_DOWN   264
#define MMF_DPAD_LEFT   263
#define MMF_DPAD_RIGHT  262
#define MMF_A           32   // KEY_SPACE
#define MMF_B           341  // KEY_LEFT_CONTROL
#define MMF_X           340  // KEY_LEFT_SHIFT
#define MMF_Y           342  // KEY_LEFT_ALT
#define MMF_L1          69   // KEY_E
#define MMF_L2          258  // KEY_TAB
#define MMF_R1          84   // KEY_T
#define MMF_R2          259  // KEY_BACKSPACE

static const int SCR_W = 752;
static const int SCR_H = 560;

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
    int value;  // face number (>=0), or -1 for non-numbered edge faces (d10)
};

struct DiceDef {
    const char* name;
    int numValues;
    float scaleFactor;
    bool invertUpside;
    int numVerts;
    float rawVerts[MAX_DIE_VERTS][3];
    int numFaces;
    Face faces[MAX_DIE_FACES];
};

static const DiceDef DICE_DEFS[] = {
    // D4 (tetrahedron)
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
    // D6 (cube)
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
    // D8 (octahedron)
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
    // D10 (pentagonal trapezohedron)
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
    // D12 (dodecahedron)
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
    // D20 (icosahedron)
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
// Pastel2 colormap (matplotlib)
// ═══════════════════════════════════════════════════════════════════

static const Color PASTEL2[] = {
    {179, 226, 205, 255},  // mint
    {253, 205, 172, 255},  // peach
    {203, 213, 232, 255},  // lavender
    {244, 202, 228, 255},  // pink
    {230, 245, 201, 255},  // lime
    {255, 242, 174, 255},  // butter
    {241, 226, 204, 255},  // tan
};
#define NUM_PASTEL2 7

// ═══════════════════════════════════════════════════════════════════
// Active dice state
// ═══════════════════════════════════════════════════════════════════

static const float DIE_RADIUS = 0.7f;
static const int DICE_ALPHA = 200;  // ~78% opacity — enough transparency to see through, vivid enough to read
#define MAX_ACTIVE_DICE 12

struct ActiveDie {
    int typeIdx;
    Color baseColor;
    btRigidBody* body;
    btConvexHullShape* shape;
    Vector3 verts[MAX_DIE_VERTS];
    int numVerts;
    Face faces[MAX_DIE_FACES];
    int numFaces;
    int rolledValue;
    int settledFrames;
    bool settled;
};

static ActiveDie dice[MAX_ACTIVE_DICE];
static int numDice = 0;

// Hot bar: per-type counts and selection cursor
static int hotbarCount[NUM_DICE_TYPES] = {1, 2, 1, 0, 0, 0}; // start 1xd4 + 2xd6 + 1xd8
static int hotbarSel = 1;

// Debounce for X/Y buttons — ignore repeat presses for N frames
static const int DEBOUNCE_FRAMES = 12;  // ~0.4s at 30 FPS
static int debounceY = 0;
static int debounceX = 0;

// ═══════════════════════════════════════════════════════════════════
// Bullet3 physics
// ═══════════════════════════════════════════════════════════════════

static btDiscreteDynamicsWorld* world;
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

static void ClearDice() {
    for (int i = 0; i < numDice; i++) {
        world->removeRigidBody(dice[i].body);
        delete dice[i].body->getMotionState();
        delete dice[i].body;
        delete dice[i].shape;
    }
    numDice = 0;
}

static void SetupDieVerts(ActiveDie& d) {
    const DiceDef& def = DICE_DEFS[d.typeIdx];
    d.numVerts = def.numVerts;
    d.numFaces = def.numFaces;
    float r = DIE_RADIUS * def.scaleFactor;
    for (int i = 0; i < def.numVerts; i++) {
        Vector3 v = {def.rawVerts[i][0], def.rawVerts[i][1], def.rawVerts[i][2]};
        d.verts[i] = Vector3Scale(Vector3Normalize(v), r);
    }
    for (int i = 0; i < def.numFaces; i++)
        d.faces[i] = def.faces[i];
}

static void ThrowAll() {
    ClearDice();
    int total = 0;
    for (int t = 0; t < NUM_DICE_TYPES; t++) total += hotbarCount[t];
    if (total == 0) return;
    if (total > MAX_ACTIVE_DICE) total = MAX_ACTIVE_DICE;

    int idx = 0;
    for (int t = 0; t < NUM_DICE_TYPES && idx < total; t++) {
        for (int c = 0; c < hotbarCount[t] && idx < total; c++, idx++) {
            ActiveDie& d = dice[idx];
            d.typeIdx = t;
            d.baseColor = PASTEL2[rand() % NUM_PASTEL2];
            d.rolledValue = -1;
            d.settledFrames = 0;
            d.settled = false;
            SetupDieVerts(d);

            d.shape = new btConvexHullShape();
            for (int i = 0; i < d.numVerts; i++)
                d.shape->addPoint(btVector3(d.verts[i].x, d.verts[i].y, d.verts[i].z));

            btScalar mass = 1.0f;
            btVector3 inertia;
            d.shape->calculateLocalInertia(mass, inertia);

            btQuaternion rot;
            rot.setEulerZYX(RandF(0, 6.28f), RandF(0, 6.28f), RandF(0, 6.28f));

            float spawnX = RandF(-1.5f, 1.5f);
            float spawnZ = RandF(-1.5f, 1.5f);
            float spawnY = 3.0f + RandF(0, 3.0f);

            btTransform startXform;
            startXform.setIdentity();
            startXform.setOrigin(btVector3(spawnX, spawnY, spawnZ));
            startXform.setRotation(rot);

            btDefaultMotionState* ms = new btDefaultMotionState(startXform);
            btRigidBody::btRigidBodyConstructionInfo ci(mass, ms, d.shape, inertia);
            ci.m_restitution = 0.4f;
            ci.m_friction = 0.6f;

            d.body = new btRigidBody(ci);
            d.body->setAngularVelocity(btVector3(RandF(-8,8), RandF(-8,8), RandF(-8,8)));
            d.body->setLinearVelocity(btVector3(RandF(-2,2), RandF(-1,1), RandF(-2,2)));
            world->addRigidBody(d.body);
        }
    }
    numDice = idx;
}

static void CleanupPhysics() {
    ClearDice();
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
// Helpers
// ═══════════════════════════════════════════════════════════════════

static Matrix GetDieTransform(const ActiveDie& d) {
    float m[16];
    d.body->getWorldTransform().getOpenGLMatrix(m);
    return {
        m[0], m[4], m[8],  m[12],
        m[1], m[5], m[9],  m[13],
        m[2], m[6], m[10], m[14],
        m[3], m[7], m[11], m[15],
    };
}

static Vector3 FaceNormal(const Vector3* wv, const Face& f) {
    Vector3 e1 = Vector3Subtract(wv[f.idx[1]], wv[f.idx[0]]);
    Vector3 e2 = Vector3Subtract(wv[f.idx[2]], wv[f.idx[0]]);
    return Vector3Normalize(Vector3CrossProduct(e1, e2));
}

static bool IsDieSettled(const ActiveDie& d) {
    btVector3 v = d.body->getLinearVelocity();
    btVector3 w = d.body->getAngularVelocity();
    return v.length() < 0.3f && w.length() < 0.3f;
}

static int GetFaceUpValue(const ActiveDie& d, const Matrix& xform) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    bool inv = DICE_DEFS[d.typeIdx].invertUpside;
    Vector3 upDir = inv ? (Vector3){0,-1,0} : (Vector3){0,1,0};
    int best = -1;
    float bestDot = -2.0f;
    for (int f = 0; f < d.numFaces; f++) {
        if (d.faces[f].value < 0) continue;
        Vector3 n = FaceNormal(wv, d.faces[f]);
        float dot = Vector3DotProduct(n, upDir);
        if (dot > bestDot) { bestDot = dot; best = f; }
    }
    return best >= 0 ? d.faces[best].value : -1;
}

// ═══════════════════════════════════════════════════════════════════
// 3D Rendering
// ═══════════════════════════════════════════════════════════════════

// Two-light setup: key light from upper-right-front, fill from left
static const Vector3 LIGHT_KEY  = Vector3Normalize((Vector3){0.4f, 0.8f, 0.5f});
static const Vector3 LIGHT_FILL = Vector3Normalize((Vector3){-0.5f, 0.3f, -0.3f});

// Project a die's silhouette onto the ground plane as a shadow.
// Projects world-space vertices along the key light direction to Y=0,
// computes 2D convex hull, then draws a dark inner polygon with an
// expanded soft penumbra ring.
static void DrawProjectedShadow(const ActiveDie& d, Matrix xform) {
    const float groundY = 0.002f;
    const float PENUMBRA = 0.18f;  // penumbra expansion distance

    // Transform die vertices to world space
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    // Project each vertex along the key light direction onto Y=0
    // Ray: P + t*L, solve for P.y + t*L.y = 0 => t = -P.y / L.y
    Vector2 proj[MAX_DIE_VERTS];
    int nProj = 0;
    for (int i = 0; i < d.numVerts; i++) {
        if (LIGHT_KEY.y <= 0.001f) continue;  // light must point upward
        float t = -wv[i].y / LIGHT_KEY.y;
        // Only project if the vertex is above the ground (shadow falls below)
        // Use negative t because light direction points FROM the surface
        t = wv[i].y / LIGHT_KEY.y;
        proj[nProj++] = {wv[i].x - LIGHT_KEY.x * t,
                         wv[i].z - LIGHT_KEY.z * t};
    }
    if (nProj < 3) return;

    // 2D convex hull (Andrew's monotone chain)
    // Sort by x, then y
    for (int i = 0; i < nProj - 1; i++)
        for (int j = i + 1; j < nProj; j++)
            if (proj[j].x < proj[i].x ||
                (proj[j].x == proj[i].x && proj[j].y < proj[i].y)) {
                Vector2 tmp = proj[i]; proj[i] = proj[j]; proj[j] = tmp;
            }

    Vector2 hull[MAX_DIE_VERTS * 2];
    int k = 0;
    // Lower hull
    for (int i = 0; i < nProj; i++) {
        while (k >= 2) {
            float cross = (hull[k-1].x - hull[k-2].x) * (proj[i].y - hull[k-2].y)
                        - (hull[k-1].y - hull[k-2].y) * (proj[i].x - hull[k-2].x);
            if (cross <= 0) k--; else break;
        }
        hull[k++] = proj[i];
    }
    // Upper hull
    int lower_size = k + 1;
    for (int i = nProj - 2; i >= 0; i--) {
        while (k >= lower_size) {
            float cross = (hull[k-1].x - hull[k-2].x) * (proj[i].y - hull[k-2].y)
                        - (hull[k-1].y - hull[k-2].y) * (proj[i].x - hull[k-2].x);
            if (cross <= 0) k--; else break;
        }
        hull[k++] = proj[i];
    }
    int hullCount = k - 1;  // last point == first point
    if (hullCount < 3) return;

    // Compute centroid for expansion and fan rendering
    float cx = 0, cz = 0;
    for (int i = 0; i < hullCount; i++) { cx += hull[i].x; cz += hull[i].y; }
    cx /= hullCount; cz /= hullCount;

    // Height-based intensity fade
    float height = xform.m13;  // translation Y
    if (height < 0) height = 0;
    float intensity = 1.0f - height / 8.0f;
    if (intensity < 0) return;
    unsigned char innerAlpha = (unsigned char)(intensity * 140);
    Color innerCol = {0, 0, 0, innerAlpha};

    // Expand hull outward for penumbra ring
    Vector2 outerHull[MAX_DIE_VERTS * 2];
    for (int i = 0; i < hullCount; i++) {
        float dx = hull[i].x - cx;
        float dz = hull[i].y - cz;
        float len = sqrtf(dx * dx + dz * dz);
        if (len > 0.001f) {
            outerHull[i] = {hull[i].x + dx / len * PENUMBRA,
                            hull[i].y + dz / len * PENUMBRA};
        } else {
            outerHull[i] = hull[i];
        }
    }

    // Draw inner shadow as triangle fan
    for (int i = 0; i < hullCount; i++) {
        int j = (i + 1) % hullCount;
        DrawTriangle3D({cx, groundY, cz},
                       {hull[i].x, groundY, hull[i].y},
                       {hull[j].x, groundY, hull[j].y}, innerCol);
    }

    // Draw penumbra ring (fades from innerAlpha to 0)
    for (int i = 0; i < hullCount; i++) {
        int j = (i + 1) % hullCount;
        // Two triangles per edge: inner[i], outer[i], outer[j] and inner[i], outer[j], inner[j]
        // Use averaged alpha for the penumbra triangles
        unsigned char midAlpha = innerAlpha / 3;
        Color midCol = {0, 0, 0, midAlpha};
        DrawTriangle3D({hull[i].x, groundY, hull[i].y},
                       {outerHull[i].x, groundY, outerHull[i].y},
                       {outerHull[j].x, groundY, outerHull[j].y}, midCol);
        DrawTriangle3D({hull[i].x, groundY, hull[i].y},
                       {outerHull[j].x, groundY, outerHull[j].y},
                       {hull[j].x, groundY, hull[j].y}, midCol);
    }
}

// Draw lit die face triangles with PBR-inspired shading:
// - Blinn-Phong diffuse + specular (key + fill lights)
// - Clearcoat specular layer (tight highlight, white)
// - Schlick Fresnel rim lighting (brighter at glancing angles)
// - Procedural bump perturbation (subtle surface texture)
// - Environment tint (sky vs ground reflection)
static void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    Color base = d.baseColor;

    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];
        Vector3 n = FaceNormal(wv, face);

        // Face center for view direction and bump seed
        Vector3 faceCtr = {0, 0, 0};
        for (int j = 0; j < face.count; j++) {
            faceCtr.x += wv[face.idx[j]].x;
            faceCtr.y += wv[face.idx[j]].y;
            faceCtr.z += wv[face.idx[j]].z;
        }
        faceCtr.x /= face.count; faceCtr.y /= face.count; faceCtr.z /= face.count;

        // Procedural bump: perturb normal with a hash-based micro-texture
        // Simulates the "packeddirt" normal map from the Three.js version
        {
            unsigned int seed = (unsigned int)(faceCtr.x * 1000) * 7919u
                              + (unsigned int)(faceCtr.z * 1000) * 104729u
                              + (unsigned int)(f * 31337u);
            float bx = ((seed & 0xFF) / 255.0f - 0.5f) * 0.08f;
            float bz = (((seed >> 8) & 0xFF) / 255.0f - 0.5f) * 0.08f;
            n.x += bx; n.z += bz;
            float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
            if (len > 0.001f) { n.x /= len; n.y /= len; n.z /= len; }
        }

        Vector3 viewDir = Vector3Normalize(Vector3Subtract(camPos, faceCtr));

        // ── Diffuse (two lights) ──
        float keyDot  = Vector3DotProduct(n, LIGHT_KEY);
        if (keyDot < 0) keyDot = 0;
        float fillDot = Vector3DotProduct(n, LIGHT_FILL);
        if (fillDot < 0) fillDot = 0;
        float diffuse = 0.50f + 0.35f * keyDot + 0.15f * fillDot;

        // ── Blinn-Phong specular (base material, roughness ~0.25) ──
        Vector3 halfVec = Vector3Normalize(Vector3Add(LIGHT_KEY, viewDir));
        float specDot = Vector3DotProduct(n, halfVec);
        if (specDot < 0) specDot = 0;
        // pow(specDot, 16) ≈ roughness 0.25
        float spec = specDot;
        for (int p = 0; p < 4; p++) spec *= spec;  // spec^16

        // ── Clearcoat specular (tighter highlight, white, 50% strength) ──
        // Simulates clearcoat=0.5 from the Three.js MeshPhysicalMaterial
        float ccSpec = specDot;
        for (int p = 0; p < 5; p++) ccSpec *= ccSpec;  // ccSpec^32
        float clearcoat = 0.5f * ccSpec;

        // Fill light specular (weaker)
        Vector3 halfFill = Vector3Normalize(Vector3Add(LIGHT_FILL, viewDir));
        float fillSpec = Vector3DotProduct(n, halfFill);
        if (fillSpec < 0) fillSpec = 0;
        fillSpec = fillSpec * fillSpec * fillSpec * fillSpec;  // ^4

        // ── Fresnel rim (Schlick approximation) ──
        float NdotV = Vector3DotProduct(n, viewDir);
        if (NdotV < 0) NdotV = 0;
        float fresnel = 0.04f + 0.96f * powf(1.0f - NdotV, 5.0f);
        // Clamp fresnel contribution for subtle rim
        float rim = fresnel * 0.35f;

        // ── Compose final color ──
        float dimFactor = (face.value >= 0) ? 1.0f : 0.6f;
        float totalSpec = spec * 0.5f + clearcoat + fillSpec * 0.15f;

        float sr = base.r * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;
        float sg = base.g * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;
        float sb = base.b * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;

        // Fake environment tint: sky (top) vs ground (bottom) based on normal.y
        float envUp = n.y * 0.5f + 0.5f;
        float envR = 140.0f * envUp + 100.0f * (1.0f - envUp);
        float envG = 150.0f * envUp +  80.0f * (1.0f - envUp);
        float envB = 170.0f * envUp +  60.0f * (1.0f - envUp);
        sr = sr * 0.88f + envR * 0.12f;
        sg = sg * 0.88f + envG * 0.12f;
        sb = sb * 0.88f + envB * 0.12f;

        if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;

        Color col = {(unsigned char)sr, (unsigned char)sg, (unsigned char)sb,
                     (unsigned char)DICE_ALPHA};

        for (int j = 1; j < face.count - 1; j++)
            DrawTriangle3D(wv[face.idx[0]], wv[face.idx[j]], wv[face.idx[j+1]], col);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Procedural hardwood floor texture
// ═══════════════════════════════════════════════════════════════════

static Texture2D woodTexture;

static void InitWoodTexture() {
    // Real hardwood2 diffuse texture from three.js examples (CC0)
    // Original: https://threejs.org/examples/textures/hardwood2_diffuse.jpg
    // Downscaled to 256×256 PNG for TinyGL
    Image img = LoadImage("hardwood2_diffuse.png");
    if (img.data == NULL) {
        // Fallback: plain brown if file missing
        img = GenImageColor(64, 64, (Color){140, 100, 58, 255});
        TraceLog(LOG_WARNING, "FLOOR: hardwood2_diffuse.png not found, using fallback");
    }
    woodTexture = LoadTextureFromImage(img);
    UnloadImage(img);
}

// Draw ground as textured quad (TinyGL wraps UVs automatically via bitmask)
static void DrawTexturedGround(float halfSize, float tileRepeat) {
    rlSetTexture(woodTexture.id);
    rlBegin(RL_TRIANGLES);
        rlColor4ub(255, 255, 255, 255);
        rlTexCoord2f(0, 0);           rlVertex3f(-halfSize, 0, -halfSize);
        rlTexCoord2f(tileRepeat, 0);   rlVertex3f( halfSize, 0, -halfSize);
        rlTexCoord2f(tileRepeat, tileRepeat); rlVertex3f( halfSize, 0,  halfSize);

        rlTexCoord2f(0, 0);           rlVertex3f(-halfSize, 0, -halfSize);
        rlTexCoord2f(tileRepeat, tileRepeat); rlVertex3f( halfSize, 0,  halfSize);
        rlTexCoord2f(0, tileRepeat);   rlVertex3f(-halfSize, 0,  halfSize);
    rlEnd();
    rlSetTexture(0);
}

// ═══════════════════════════════════════════════════════════════════
// Skybox — equirectangular panorama on a cylinder (CC0 Polyhaven)
// ═══════════════════════════════════════════════════════════════════

static Texture2D skyboxTex;
static bool skyboxLoaded = false;

static void InitSkybox() {
    Image img = LoadImage("skybox.png");
    if (img.data != NULL) {
        skyboxTex = LoadTextureFromImage(img);
        UnloadImage(img);
        skyboxLoaded = true;
    }
}

// Draw equirectangular panorama mapped onto a vertical cylinder centered on camera.
// Depth write is disabled so the sky is always behind everything.
static void DrawSkybox(Vector3 camPos) {
    if (!skyboxLoaded) return;

    rlDisableDepthMask();
    rlSetTexture(skyboxTex.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 255);

    const int SEGS = 16;
    const float radius = 50.0f;
    const float halfH  = 30.0f;

    for (int i = 0; i < SEGS; i++) {
        float a0 = (float)i / SEGS * 2.0f * PI;
        float a1 = (float)(i + 1) / SEGS * 2.0f * PI;
        float u0 = (float)i / SEGS;
        float u1 = (float)(i + 1) / SEGS;

        float x0 = camPos.x + cosf(a0) * radius;
        float z0 = camPos.z + sinf(a0) * radius;
        float x1 = camPos.x + cosf(a1) * radius;
        float z1 = camPos.z + sinf(a1) * radius;
        float yTop = camPos.y + halfH;
        float yBot = camPos.y - halfH;

        // Two triangles per segment (quad)
        rlTexCoord2f(u0, 0); rlVertex3f(x0, yTop, z0);
        rlTexCoord2f(u1, 0); rlVertex3f(x1, yTop, z1);
        rlTexCoord2f(u1, 1); rlVertex3f(x1, yBot, z1);

        rlTexCoord2f(u0, 0); rlVertex3f(x0, yTop, z0);
        rlTexCoord2f(u1, 1); rlVertex3f(x1, yBot, z1);
        rlTexCoord2f(u0, 1); rlVertex3f(x0, yBot, z0);
    }

    rlEnd();
    rlSetTexture(0);
    rlEnableDepthMask();
}

// ═══════════════════════════════════════════════════════════════════
// Face number texture atlas (3D decals on face surfaces)
// ═══════════════════════════════════════════════════════════════════

// Atlas layout: 4 columns × 6 rows in 256×256 texture
// Numbers 0-20 each get a 64×42 cell
#define ATLAS_COLS 4
#define ATLAS_ROWS 6
#define ATLAS_CELL_W 64
#define ATLAS_CELL_H 42
#define ATLAS_SIZE 256

static Texture2D numberAtlas;

static void InitNumberAtlas() {
    Image img = GenImageColor(ATLAS_SIZE, ATLAS_SIZE, BLANK);
    for (int n = 0; n <= 20; n++) {
        int col = n % ATLAS_COLS, row = n / ATLAS_COLS;
        int cx = col * ATLAS_CELL_W, cy = row * ATLAS_CELL_H;
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", n);
        int fontSize = 28;
        int tw = MeasureText(buf, fontSize);
        // Fake bold: draw 3 times with offset
        Color numCol = {30, 20, 40, 255};
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2 + 1,
                      cy + (ATLAS_CELL_H - fontSize)/2, fontSize, numCol);
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2,
                      cy + (ATLAS_CELL_H - fontSize)/2 + 1, fontSize, numCol);
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2,
                      cy + (ATLAS_CELL_H - fontSize)/2, fontSize, numCol);
    }
    numberAtlas = LoadTextureFromImage(img);
    UnloadImage(img);
}

// Compute UV rectangle for a given face value (0-20)
static void GetNumberUV(int value, float* u0, float* v0, float* u1, float* v1) {
    int col = value % ATLAS_COLS, row = value / ATLAS_COLS;
    *u0 = (float)(col * ATLAS_CELL_W) / ATLAS_SIZE;
    *v0 = (float)(row * ATLAS_CELL_H) / ATLAS_SIZE;
    *u1 = (float)((col + 1) * ATLAS_CELL_W) / ATLAS_SIZE;
    *v1 = (float)((row + 1) * ATLAS_CELL_H) / ATLAS_SIZE;
}

// Draw number decals on all visible faces of a die (call inside BeginMode3D)
static void DrawDieNumberDecals(const ActiveDie& d, const Matrix& xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    for (int f = 0; f < d.numFaces; f++) {
        if (d.faces[f].value < 0) continue;
        const Face& face = d.faces[f];

        // Face center
        Vector3 center = {0, 0, 0};
        for (int i = 0; i < face.count; i++)
            center = Vector3Add(center, wv[face.idx[i]]);
        center = Vector3Scale(center, 1.0f / face.count);

        // Back-face cull: skip faces pointing away from camera
        Vector3 n = FaceNormal(wv, face);
        Vector3 toCamera = Vector3Normalize(Vector3Subtract(camPos, center));
        if (Vector3DotProduct(n, toCamera) < 0.1f) continue;

        // Compute tangent frame on the face plane
        Vector3 edge = Vector3Subtract(wv[face.idx[1]], wv[face.idx[0]]);
        Vector3 t1 = Vector3Normalize(edge);
        Vector3 t2 = Vector3Normalize(Vector3CrossProduct(n, t1));

        // Quad size: proportional to face size (use shortest edge * 0.55)
        float minEdge = 1e9f;
        for (int i = 0; i < face.count; i++) {
            Vector3 e = Vector3Subtract(wv[face.idx[(i+1) % face.count]], wv[face.idx[i]]);
            float len = Vector3Length(e);
            if (len < minEdge) minEdge = len;
        }
        float halfSize = minEdge * 0.35f;

        // Offset slightly along normal to prevent z-fighting
        Vector3 off = Vector3Scale(n, 0.005f);
        Vector3 qCenter = Vector3Add(center, off);

        // Quad corners: TL, TR, BR, BL
        Vector3 q0 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1, -halfSize)), Vector3Scale(t2,  halfSize));
        Vector3 q1 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1,  halfSize)), Vector3Scale(t2,  halfSize));
        Vector3 q2 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1,  halfSize)), Vector3Scale(t2, -halfSize));
        Vector3 q3 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1, -halfSize)), Vector3Scale(t2, -halfSize));

        // UV coords for this face's number
        float u0, v0, u1, v1;
        GetNumberUV(face.value, &u0, &v0, &u1, &v1);

        // Draw textured quad (two triangles) with alpha blending
        rlSetTexture(numberAtlas.id);
        rlBegin(RL_TRIANGLES);
            rlColor4ub(255, 255, 255, 255);
            // Triangle 1: q0-q1-q2
            rlTexCoord2f(u0, v0); rlVertex3f(q0.x, q0.y, q0.z);
            rlTexCoord2f(u1, v0); rlVertex3f(q1.x, q1.y, q1.z);
            rlTexCoord2f(u1, v1); rlVertex3f(q2.x, q2.y, q2.z);
            // Triangle 2: q0-q2-q3
            rlTexCoord2f(u0, v0); rlVertex3f(q0.x, q0.y, q0.z);
            rlTexCoord2f(u1, v1); rlVertex3f(q2.x, q2.y, q2.z);
            rlTexCoord2f(u0, v1); rlVertex3f(q3.x, q3.y, q3.z);
        rlEnd();
        rlSetTexture(0);
    }
}

// Fake bold: draw text with slight offset for weight
static void DrawTextBold(const char* text, int x, int y, int sz, Color col) {
    DrawText(text, x+1, y, sz, col);
    DrawText(text, x, y+1, sz, col);
    DrawText(text, x, y, sz, col);
}

// ═══════════════════════════════════════════════════════════════════
// Hot bar UI
// ═══════════════════════════════════════════════════════════════════

static void DrawHotbar() {
    const int cellW = 110, cellH = 38;
    const int totalW = NUM_DICE_TYPES * cellW;
    const int x0 = (SCR_W - totalW) / 2;
    const int y0 = SCR_H - cellH - 8;

    for (int i = 0; i < NUM_DICE_TYPES; i++) {
        int cx = x0 + i * cellW;
        bool sel = (i == hotbarSel);

        Color bg = sel ? (Color){60, 70, 90, 255} : (Color){35, 40, 50, 255};
        DrawRectangle(cx + 1, y0 + 1, cellW - 2, cellH - 2, bg);

        if (sel)
            DrawRectangleLines(cx, y0, cellW, cellH, (Color){255, 220, 50, 255});

        const char* name = DICE_DEFS[i].name;
        int cnt = hotbarCount[i];
        Color txtCol = sel ? (Color){255, 255, 255, 255} : (Color){180, 180, 180, 255};

        if (cnt > 0) {
            const char* label = TextFormat("%s x%d", name, cnt);
            int tw = MeasureText(label, 16);
            DrawText(label, cx + (cellW - tw)/2, y0 + 10, 16, txtCol);
        } else {
            int tw = MeasureText(name, 16);
            DrawText(name, cx + (cellW - tw)/2, y0 + 10, 16,
                     sel ? (Color){140, 140, 140, 255} : (Color){80, 80, 80, 255});
        }
    }

    const char* hint = "Y:+  X:-  L2/R2:Select  A:Throw";
    int hw = MeasureText(hint, 12);
    DrawText(hint, (SCR_W - hw)/2, y0 - 16, 12, (Color){120, 120, 120, 255});
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char **argv) {
    // --screenshot N: run N frames headlessly, save screenshot.png, exit
    int screenshotFrames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotFrames = atoi(argv[++i]);
    }

    srand((unsigned)time(nullptr));
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(SCR_W, SCR_H, "Dice Roller - MMF");
    SetTargetFPS(30);

    InitNumberAtlas();
    InitWoodTexture();
    InitSkybox();
    InitPhysics();
    ThrowAll();

    float camDist = 9.0f, camYaw = 45.0f, camPitch = 40.0f;
    Camera3D camera = {0};
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    bool allSettled = false;
    int totalResult = 0;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        // In screenshot mode, force 30fps-equivalent timestep since headless
        // renders at hundreds of FPS — otherwise physics barely advances.
        if (screenshotFrames > 0) dt = 1.0f / 30.0f;
        if (dt > 0 && dt < 0.1f)
            world->stepSimulation(dt, 4, 1.0f/120.0f);

        // Hot bar navigation
        if (IsKeyPressed(MMF_L2)) hotbarSel = (hotbarSel + NUM_DICE_TYPES - 1) % NUM_DICE_TYPES;
        if (IsKeyPressed(MMF_R2)) hotbarSel = (hotbarSel + 1) % NUM_DICE_TYPES;

        // Debounce countdowns
        if (debounceY > 0) debounceY--;
        if (debounceX > 0) debounceX--;

        if (IsKeyPressed(MMF_Y) && debounceY == 0) {
            int total = 0;
            for (int t = 0; t < NUM_DICE_TYPES; t++) total += hotbarCount[t];
            if (total < MAX_ACTIVE_DICE && hotbarCount[hotbarSel] < 6)
                hotbarCount[hotbarSel]++;
            debounceY = DEBOUNCE_FRAMES;
        }
        if (IsKeyPressed(MMF_X) && debounceX == 0) {
            if (hotbarCount[hotbarSel] > 0) hotbarCount[hotbarSel]--;
            debounceX = DEBOUNCE_FRAMES;
        }

        // Throw all configured dice
        if (IsKeyPressed(MMF_A)) {
            ThrowAll();
            allSettled = false;
            totalResult = 0;
        }

        // Camera orbit
        if (IsKeyDown(MMF_DPAD_LEFT))  camYaw   -= 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_RIGHT)) camYaw   += 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_UP))    camPitch += 90.0f * dt;
        if (IsKeyDown(MMF_DPAD_DOWN))  camPitch -= 90.0f * dt;
        if (IsKeyDown(MMF_L1))         camDist  -= 5.0f * dt;
        if (IsKeyDown(MMF_R1))         camDist  += 5.0f * dt;

        if (camPitch > 85) camPitch = 85;
        if (camPitch < 5) camPitch = 5;
        if (camDist < 3) camDist = 3;
        if (camDist > 20) camDist = 20;

        float yr = camYaw * DEG2RAD, pr = camPitch * DEG2RAD;
        camera.position = {
            camDist * cosf(pr) * sinf(yr),
            camDist * sinf(pr),
            camDist * cosf(pr) * cosf(yr),
        };
        camera.target = {0, 0.5f, 0};

        // Per-die settle detection
        if (!allSettled && numDice > 0) {
            bool all = true;
            for (int i = 0; i < numDice; i++) {
                ActiveDie& d = dice[i];
                if (!d.settled) {
                    if (IsDieSettled(d)) {
                        if (++d.settledFrames > 30) {
                            Matrix xf = GetDieTransform(d);
                            d.rolledValue = GetFaceUpValue(d, xf);
                            d.settled = true;
                        }
                    } else {
                        d.settledFrames = 0;
                    }
                }
                if (!d.settled) all = false;
            }
            if (all) {
                allSettled = true;
                totalResult = 0;
                for (int i = 0; i < numDice; i++)
                    if (dice[i].rolledValue >= 0) totalResult += dice[i].rolledValue;
            }
        }

        // ── Draw 3D ──
        BeginDrawing();
        ClearBackground({20, 18, 16, 255});  // dark warm gap fill (matches skybox bottom)

        BeginMode3D(camera);
        rlDisableBackfaceCulling();  // Ground, shadows, and dice all need both faces

        // Skybox: draw equirectangular panorama on cylinder (depth write off)
        DrawSkybox(camera.position);

        // Opaque geometry first: textured wood floor
        DrawTexturedGround(10.0f, 4.0f);  // 4×4 tile repeats

        // Enable blend for all transparent geometry (shadows, dice, decals, UI)
        rlEnableColorBlend();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // TinyGL's smooth-shaded blend path (TGL_BLEND_FUNC_RGB) hardcodes
        // alpha=255, ignoring vertex alpha.  GL_FLAT uses the flat blend path
        // (TGL_BLEND_FUNC) which reads actual vertex alpha from the packed color.
        glShadeModel(GL_FLAT);

        // Projected outline shadows (alpha-blended silhouettes on ground)
        for (int i = 0; i < numDice; i++) {
            Matrix xf = GetDieTransform(dice[i]);
            DrawProjectedShadow(dice[i], xf);
        }

        // Sort dice back-to-front for correct transparency compositing
        int diceOrder[MAX_ACTIVE_DICE];
        for (int i = 0; i < numDice; i++) diceOrder[i] = i;
        for (int i = 0; i < numDice - 1; i++)
            for (int j = i + 1; j < numDice; j++) {
                Matrix a = GetDieTransform(dice[diceOrder[i]]);
                Matrix b = GetDieTransform(dice[diceOrder[j]]);
                float da = Vector3LengthSqr(Vector3Subtract({a.m12,a.m13,a.m14}, camera.position));
                float db = Vector3LengthSqr(Vector3Subtract({b.m12,b.m13,b.m14}, camera.position));
                if (da < db) { int tmp = diceOrder[i]; diceOrder[i] = diceOrder[j]; diceOrder[j] = tmp; }
            }

        // Draw each die completely (faces + decals) before the next, so a
        // nearer die's semi-transparent faces correctly tint the numbers on
        // a farther die behind it.
        rlDisableDepthMask();
        for (int i = 0; i < numDice; i++) {
            int di = diceOrder[i];
            Matrix xf = GetDieTransform(dice[di]);
            DrawDieFacesLit(dice[di], xf, camera.position);
            DrawDieNumberDecals(dice[di], xf, camera.position);
        }
        rlEnableDepthMask();
        glShadeModel(GL_SMOOTH);  // Restore default shade model
        rlEnableBackfaceCulling();

        EndMode3D();

        // ── Result display ──
        if (allSettled && numDice > 0) {
            char resultBuf[128];
            int pos = 0;
            for (int i = 0; i < numDice; i++) {
                if (i > 0) pos += snprintf(resultBuf + pos, sizeof(resultBuf) - pos, " + ");
                pos += snprintf(resultBuf + pos, sizeof(resultBuf) - pos, "%d", dice[i].rolledValue);
            }
            if (numDice > 1)
                snprintf(resultBuf + pos, sizeof(resultBuf) - pos, " = %d", totalResult);

            int rw = MeasureText(resultBuf, 24);
            DrawTextBold(resultBuf, (SCR_W - rw)/2, 16, 24, (Color){255, 220, 50, 255});
        }

        DrawHotbar();

        if (getenv("RAYLIB_MMF_SHOWFPS"))
            DrawFPS(20, 50);

        // Disable blend after all UI drawing is done (font textures need it)
        rlDisableColorBlend();

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

    UnloadTexture(numberAtlas);
    CleanupPhysics();
    CloseWindow();
    return 0;
}
