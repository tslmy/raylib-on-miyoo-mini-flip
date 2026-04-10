#pragma once
// ═══════════════════════════════════════════════════════════════════
// physics.h — Bullet3 rigid-body physics for dice simulation
// ═══════════════════════════════════════════════════════════════════
//
// WHAT THIS DOES:
//   Uses the Bullet3 physics engine to simulate gravity, collisions,
//   and bouncing for the dice.  Each die is a "rigid body" — a solid
//   object that cannot deform.  Bullet3 tracks every body's position,
//   rotation, and velocity, and advances them each frame.
//
// WHY A PHYSICS ENGINE:
//   Realistic dice tumbling involves gravity, collision detection
//   between complex 3D shapes, bouncing (restitution), friction, and
//   angular momentum.  Implementing all of that from scratch would be
//   extremely difficult; Bullet3 handles it in one function call.
//
// LIFECYCLE:
//   InitPhysics()  → create the physics world and floor
//   ThrowAll()     → spawn dice with random positions/velocities
//   StepPhysics()  → advance simulation one frame (called every frame)
//   CleanupPhysics() → free everything
// ═══════════════════════════════════════════════════════════════════

#include "dice_defs.h"
#include "raymath.h"

// ── Shared mutable game state (owned by physics, read by rendering) ──

extern ActiveDie dice[MAX_ACTIVE_DICE];  // the dice currently in play
extern int numDice;                       // how many are active
extern int hotbarCount[NUM_DICE_TYPES];   // how many of each type the user has queued
extern int hotbarSel;                     // which die type is selected in the UI
extern int riggedValue;                   // -1 = fair roll; 0-20 = force this value

// Button debounce (prevents rapid repeat from a single press)
extern const int DEBOUNCE_FRAMES;
extern int debounceY;
extern int debounceX;

// ── Physics lifecycle ──

void InitPhysics();       // Create Bullet3 world with gravity and a floor plane
void CleanupPhysics();    // Destroy all bodies and the world
void ThrowAll();          // Spawn dice based on hotbar configuration
void StepPhysics(float dt); // Advance physics by dt seconds

// ── Helpers (used by both physics and rendering) ──

Matrix GetDieTransform(const ActiveDie& d);  // Get 4×4 transform matrix from Bullet
Vector3 FaceNormal(const Vector3* wv, const Face& f); // Cross-product face normal
bool IsDieSettled(const ActiveDie& d);       // True if velocity < threshold
int GetFaceUpValue(const ActiveDie& d, const Matrix& xform); // Which face points up?
void SnapDieToValue(ActiveDie& d, int targetValue); // Rotate die so target face is up
