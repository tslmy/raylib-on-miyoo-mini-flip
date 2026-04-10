#pragma once
// ═══════════════════════════════════════════════════════════════════
// dice_defs.h — Dice geometry definitions and constants
// ═══════════════════════════════════════════════════════════════════
//
// This header is pure data: no logic, no GPU calls.  It defines the
// 3D shapes of six standard dice (d4, d6, d8, d10, d12, d20) using
// two fundamental building blocks of 3D graphics:
//
//   VERTICES — points in 3D space (x, y, z).  Together they form the
//              "skeleton" of a shape.
//
//   FACES    — ordered lists of vertex indices that form flat polygons
//              (triangles, quads, or pentagons).  The surface of a 3D
//              object is made entirely of faces; how you arrange and
//              light them determines what the object looks like.
//
// We also define the Miyoo Mini Flip button mapping, screen size,
// and the "ActiveDie" runtime struct that ties geometry to physics.
//
// Ported from tslmy/threejs-dice (JavaScript → C++).
// ═══════════════════════════════════════════════════════════════════

#include <cstdlib>
#include "raylib.h"

// ═══════════════════════════════════════════════════════════════════
// MMF button → raylib key constants
// ═══════════════════════════════════════════════════════════════════
// The Miyoo Mini Flip is a handheld game console.  Its buttons appear
// as keyboard keys through the Linux evdev input subsystem.  These
// constants map each physical button to its raylib key code so we can
// call IsKeyPressed(MMF_A), etc.

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
#define MMF_SELECT      345  // KEY_RIGHT_CONTROL
#define MMF_START       257  // KEY_ENTER

// ═══════════════════════════════════════════════════════════════════
// Screen size and global constants
// ═══════════════════════════════════════════════════════════════════

static const int SCR_W = 752;   // MMF native framebuffer width  (pixels)
static const int SCR_H = 560;   // MMF native framebuffer height (pixels)

// DIE_RADIUS scales every die to roughly this many "world units" from
// center to corner.  3D engines use arbitrary "world units"; here 1
// unit ≈ the size of a golf ball on screen.
static const float DIE_RADIUS = 0.7f;

// DICE_ALPHA controls how see-through the dice are.  255 = fully
// opaque, 0 = invisible.  Lower values give a more glass-like look.
// Combined with Fresnel-driven edge opacity in LitVertex(), this
// simulates glass with IOR ~1.5: transparent when viewed head-on,
// reflective and opaque at glancing angles.
static const int DICE_ALPHA = 130;

// Hard cap on simultaneous dice (memory is pre-allocated).
#define MAX_ACTIVE_DICE 12

// Utility: random float in [lo, hi].
inline float RandF(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

// ═══════════════════════════════════════════════════════════════════
// Dice geometry — vertices, faces, and shapes
// ═══════════════════════════════════════════════════════════════════
//
// 3D GEOMETRY PRIMER:
//
//   Every 3D object is defined by a MESH — a collection of flat
//   polygons (triangles, quads, pentagons) that tile together to
//   approximate a surface.  Each polygon is called a FACE.
//
//   A face is defined by listing its VERTICES (corner points) in
//   order.  The order matters: it determines which side is the
//   "front" (visible) and which is the "back" (usually invisible).
//   This convention is called WINDING ORDER.
//
//   VERTEX NORMALS — perpendicular vectors sticking out of a surface.
//   They tell the renderer which direction a surface faces, which is
//   critical for lighting (a face pointing toward the light gets
//   brighter; one pointing away stays dark).
//
// HOW DICE SHAPES WORK:
//
//   Standard dice are "platonic solids" — perfectly symmetric 3D
//   shapes where every face is the same regular polygon:
//     d4  = tetrahedron     (4 triangular faces)
//     d6  = cube            (6 square faces)
//     d8  = octahedron      (8 triangular faces)
//     d10 = trapezohedron   (10 kite-shaped faces + 10 edge faces)
//     d12 = dodecahedron    (12 pentagonal faces)
//     d20 = icosahedron     (20 triangular faces)
//
//   rawVerts are the "canonical" coordinates (usually -1 to +1).
//   At runtime they get normalized to unit length and scaled by
//   DIE_RADIUS × scaleFactor so every die type looks the right size.

// Max array sizes (pre-allocated; d20 has the most of each).
#define MAX_DIE_VERTS 20
#define MAX_DIE_FACES 20
#define MAX_FACE_VERTS 5   // pentagons are the biggest face type (d12)

// A single face of a die.
struct Face {
    int idx[MAX_FACE_VERTS];  // vertex indices (indexes into the verts array)
    int count;                // how many vertices this face has (3=tri, 4=quad, 5=pent)
    int value;                // the number printed on this face, or -1 for non-numbered
                              // edge faces (d10 has 10 extra "connector" faces with no number)
};

// Static definition of one die type (geometry + metadata).
struct DiceDef {
    const char* name;       // display name ("d4", "d6", etc.)
    int numValues;          // highest face value (e.g. 6 for a d6)
    float scaleFactor;      // size tweak so all dice look proportional
    bool invertUpside;      // d4 reads the BOTTOM face instead of the top
    int numVerts;           // number of vertex positions
    float rawVerts[MAX_DIE_VERTS][3];  // canonical (x,y,z) coords before scaling
    int numFaces;           // number of faces (including non-numbered ones)
    Face faces[MAX_DIE_FACES];         // face definitions
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
// Pastel2 colormap (from matplotlib)
// ═══════════════════════════════════════════════════════════════════
// Each die gets a random pastel base color.  These soft, muted tones
// look good when multiplied by lighting and seen through transparency.

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
// ActiveDie — runtime state for each die currently in play
// ═══════════════════════════════════════════════════════════════════
//
// This combines geometry (from DiceDef) with physics state (from
// Bullet3) and game logic (rolled value, settle detection).
// Each active die has its own Bullet rigid body that the physics
// engine simulates — position, rotation, velocity, collisions.

struct btRigidBody;         // forward declarations (defined in Bullet3)
struct btConvexHullShape;

struct ActiveDie {
    int typeIdx;                    // index into DICE_DEFS[] (0=d4, 1=d6, etc.)
    Color baseColor;                // random pastel tint for this die
    btRigidBody* body;              // Bullet3 physics body (position, velocity, forces)
    btConvexHullShape* shape;       // Bullet3 collision shape (convex hull of vertices)
    Vector3 verts[MAX_DIE_VERTS];   // scaled vertex positions (local/model space)
    int numVerts;
    Face faces[MAX_DIE_FACES];      // face definitions (copied from DiceDef)
    int numFaces;
    int rolledValue;                // final face-up value after settling, or -1
    int targetValue;                // -1 = random roll; >=0 = "rig" to show this number
    int settledFrames;              // consecutive frames the die has been nearly still
    bool settled;                   // true once the die has stopped and value is read
};
