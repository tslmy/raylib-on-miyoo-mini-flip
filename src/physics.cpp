// ═══════════════════════════════════════════════════════════════════
// physics.cpp — Bullet3 rigid-body simulation for dice
// ═══════════════════════════════════════════════════════════════════
//
// PHYSICS SIMULATION OVERVIEW:
//
//   Every frame, the physics engine:
//     1. Applies forces (gravity pulls dice downward).
//     2. Detects collisions (dice vs. floor, dice vs. dice).
//     3. Resolves collisions (bounce, slide, spin).
//     4. Updates positions and rotations.
//
//   We then read those updated transforms to draw the dice in their
//   new positions.  The rendering code never moves dice directly;
//   it only reads where physics says they are.
//
// KEY CONCEPTS:
//
//   RIGID BODY — an object that has mass, position, rotation, and
//     velocity.  "Rigid" means it doesn't deform (no squishing).
//
//   CONVEX HULL — the simplest 3D shape that contains all vertices.
//     Think of wrapping the vertices in shrink-wrap.  Bullet uses
//     this for fast collision detection.
//
//   RESTITUTION — "bounciness" (0 = no bounce, 1 = perfect bounce).
//
//   FRICTION — resistance to sliding (0 = ice, 1 = sandpaper).
//
//   INERTIA TENSOR — describes how mass is distributed in the shape,
//     which determines how it spins (e.g., a dumbbell spins
//     differently than a sphere).
// ═══════════════════════════════════════════════════════════════════

#include "physics.h"
#include "btBulletDynamicsCommon.h"
#include <cmath>

// ═══════════════════════════════════════════════════════════════════
// Shared mutable game state
// ═══════════════════════════════════════════════════════════════════
// These globals are written here and read by rendering and main.

ActiveDie dice[MAX_ACTIVE_DICE];
int numDice = 0;
int hotbarCount[NUM_DICE_TYPES] = {1, 2, 1, 0, 0, 0};
int hotbarSel = 1;           // d6 selected by default
int riggedValue = -1;        // -1 = fair; 0-20 = always show this value

// ═══════════════════════════════════════════════════════════════════
// Bullet3 physics world
// ═══════════════════════════════════════════════════════════════════
//
// Bullet3 requires several cooperating objects:
//   - CollisionConfiguration: settings for collision algorithms
//   - Dispatcher:  selects which algorithm to use for each pair
//   - Broadphase:  fast rejection of distant objects (AABB tree)
//   - Solver:      resolves contact forces and constraints
//   - DynamicsWorld: ties them all together and steps the simulation

static btDiscreteDynamicsWorld* world;
static btRigidBody* floorBody;  // infinite horizontal plane at y=0
static btDefaultCollisionConfiguration* collisionConfig;
static btCollisionDispatcher* dispatcher;
static btDbvtBroadphase* broadphase;
static btSequentialImpulseConstraintSolver* solver;

void InitPhysics() {
    // Create the Bullet3 pipeline: config → dispatcher → broadphase → solver → world
    collisionConfig = new btDefaultCollisionConfiguration();
    dispatcher = new btCollisionDispatcher(collisionConfig);
    broadphase = new btDbvtBroadphase();   // bounding-volume hierarchy for fast culling
    solver = new btSequentialImpulseConstraintSolver();
    world = new btDiscreteDynamicsWorld(dispatcher, broadphase, solver, collisionConfig);
    world->setGravity(btVector3(0, -9.81f, 0));  // Earth gravity (m/s²), pointing down

    // Create the floor: an infinite static plane at y=0, facing up (+Y normal).
    // Mass = 0 means "static" — the floor never moves.
    btStaticPlaneShape* floorShape = new btStaticPlaneShape(btVector3(0, 1, 0), 0);
    btDefaultMotionState* floorMotion = new btDefaultMotionState();
    btRigidBody::btRigidBodyConstructionInfo floorCI(0, floorMotion, floorShape);
    floorCI.m_restitution = 0.5f;  // moderate bounce
    floorCI.m_friction = 0.8f;     // fairly grippy (dice don't slide much)
    floorBody = new btRigidBody(floorCI);
    world->addRigidBody(floorBody);
}

// Remove all dice from the physics world and free their memory.
static void ClearDice() {
    for (int i = 0; i < numDice; i++) {
        world->removeRigidBody(dice[i].body);
        delete dice[i].body->getMotionState();
        delete dice[i].body;
        delete dice[i].shape;
    }
    numDice = 0;
}

// Copy a die type's raw vertices, normalize them to unit length, and
// scale to the desired radius.  This turns the "canonical" coordinates
// (e.g. {1,1,1}) into the actual 3D positions used for rendering and
// physics collision shapes.
static void SetupDieVerts(ActiveDie& d) {
    const DiceDef& def = DICE_DEFS[d.typeIdx];
    d.numVerts = def.numVerts;
    d.numFaces = def.numFaces;
    float r = DIE_RADIUS * def.scaleFactor;
    for (int i = 0; i < def.numVerts; i++) {
        Vector3 v = {def.rawVerts[i][0], def.rawVerts[i][1], def.rawVerts[i][2]};
        // Normalize pushes all vertices to the unit sphere, then
        // scale gives the desired radius.  This ensures every die
        // type has consistent size regardless of raw coordinate range.
        d.verts[i] = Vector3Scale(Vector3Normalize(v), r);
    }
    for (int i = 0; i < def.numFaces; i++)
        d.faces[i] = def.faces[i];
}

void ThrowAll() {
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
            d.targetValue = -1;
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

void CleanupPhysics() {
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

void StepPhysics(float dt) {
    if (dt <= 0) return;
    if (dt > 0.5f) dt = 0.5f;  // clamp to avoid spiral of death
    world->stepSimulation(dt, 10, 1.0f/60.0f);
}

// ═══════════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════════

Matrix GetDieTransform(const ActiveDie& d) {
    float m[16];
    d.body->getWorldTransform().getOpenGLMatrix(m);
    return {
        m[0], m[4], m[8],  m[12],
        m[1], m[5], m[9],  m[13],
        m[2], m[6], m[10], m[14],
        m[3], m[7], m[11], m[15],
    };
}

Vector3 FaceNormal(const Vector3* wv, const Face& f) {
    Vector3 e1 = Vector3Subtract(wv[f.idx[1]], wv[f.idx[0]]);
    Vector3 e2 = Vector3Subtract(wv[f.idx[2]], wv[f.idx[0]]);
    return Vector3Normalize(Vector3CrossProduct(e1, e2));
}

bool IsDieSettled(const ActiveDie& d) {
    btVector3 v = d.body->getLinearVelocity();
    btVector3 w = d.body->getAngularVelocity();
    return v.length() < 0.3f && w.length() < 0.3f;
}

int GetFaceUpValue(const ActiveDie& d, const Matrix& xform) {
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

void SnapDieToValue(ActiveDie& d, int targetValue) {
    if (targetValue < 0) return;

    int targetFace = -1;
    for (int f = 0; f < d.numFaces; f++) {
        if (d.faces[f].value == targetValue) { targetFace = f; break; }
    }
    if (targetFace < 0) return;

    btTransform xf = d.body->getWorldTransform();

    Matrix mat = GetDieTransform(d);
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], mat);
    Vector3 faceN = FaceNormal(wv, d.faces[targetFace]);

    bool inv = DICE_DEFS[d.typeIdx].invertUpside;
    Vector3 upDir = inv ? (Vector3){0,-1,0} : (Vector3){0,1,0};

    Vector3 axis = Vector3CrossProduct(faceN, upDir);
    float axisLen = Vector3Length(axis);
    if (axisLen < 0.001f) return;
    axis = Vector3Scale(axis, 1.0f / axisLen);
    float angle = acosf(fminf(fmaxf(Vector3DotProduct(faceN, upDir), -1.0f), 1.0f));

    btQuaternion correction(btVector3(axis.x, axis.y, axis.z), angle);
    btQuaternion current = xf.getRotation();
    xf.setRotation(correction * current);

    d.body->setWorldTransform(xf);
    d.body->getMotionState()->setWorldTransform(xf);
    d.body->setLinearVelocity(btVector3(0, 0, 0));
    d.body->setAngularVelocity(btVector3(0, 0, 0));
}
