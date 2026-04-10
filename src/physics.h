#pragma once
// Physics — Bullet3 world management and dice lifecycle.

#include "dice_defs.h"
#include "raymath.h"

// Shared mutable game state
extern ActiveDie dice[MAX_ACTIVE_DICE];
extern int numDice;
extern int hotbarCount[NUM_DICE_TYPES];
extern int hotbarSel;
extern int riggedValue;

// Debounce for X/Y buttons
extern const int DEBOUNCE_FRAMES;
extern int debounceY;
extern int debounceX;

// Physics lifecycle
void InitPhysics();
void CleanupPhysics();
void ThrowAll();
void StepPhysics(float dt);

// Helpers
Matrix GetDieTransform(const ActiveDie& d);
Vector3 FaceNormal(const Vector3* wv, const Face& f);
bool IsDieSettled(const ActiveDie& d);
int GetFaceUpValue(const ActiveDie& d, const Matrix& xform);
void SnapDieToValue(ActiveDie& d, int targetValue);
