#pragma once
// Rendering — all 3D drawing, textures, UI.

#include "dice_defs.h"
#include "raymath.h"

// Lighting constants
extern const Vector3 LIGHT_KEY;
extern const Vector3 LIGHT_FILL;

// Texture initialization
void InitWoodTexture();
void InitSkybox();
void InitNumberAtlas();

// 3D drawing functions
void DrawSkybox(Vector3 camPos);
void DrawTexturedGround(float halfSize, float tileRepeat);
void DrawProjectedShadow(const ActiveDie& d, Matrix xform);
void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos);
void DrawDieEdges(const ActiveDie& d, Matrix xform, Vector3 camPos);
void DrawDieBloom(const ActiveDie& d, Matrix xform, Vector3 camPos);
void DrawDieNumberDecals(const ActiveDie& d, const Matrix& xform, Vector3 camPos);
void DrawFloorReflections(Vector3 camPos);

// 2D UI
void DrawHotbar();
void DrawTextBold(const char* text, int x, int y, int sz, Color col);

// Post-processing
extern bool enablePostProcess;  // toggle for bloom + depth fog (default: off)
void ApplyBloomPostProcess();

// Cleanup
void UnloadRenderingTextures();
