#pragma once
// Dice geometry definitions — pure data, no code dependencies.
// Ported from tslmy/threejs-dice.

#include <cstdlib>
#include "raylib.h"

// ═══════════════════════════════════════════════════════════════════
// MMF button → raylib key constants (from evdev probe)
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// Screen and constants
// ═══════════════════════════════════════════════════════════════════

static const int SCR_W = 752;
static const int SCR_H = 560;

static const float DIE_RADIUS = 0.7f;
static const int DICE_ALPHA = 160;  // ~63% opacity (more glass-like)
#define MAX_ACTIVE_DICE 12

inline float RandF(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

// ═══════════════════════════════════════════════════════════════════
// Dice geometry
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
// ActiveDie — runtime state for each die in play
// ═══════════════════════════════════════════════════════════════════

struct btRigidBody;
struct btConvexHullShape;

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
    int targetValue;  // -1 = random, >=0 = snap to this value after settling
    int settledFrames;
    bool settled;
};
