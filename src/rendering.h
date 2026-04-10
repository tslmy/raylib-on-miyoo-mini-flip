#pragma once
// ═══════════════════════════════════════════════════════════════════
// rendering.h — Public API for all visual output
// ═══════════════════════════════════════════════════════════════════
//
// This header declares everything main.cpp needs to draw the scene.
// The implementation lives in rendering.cpp.
//
// RENDERING PIPELINE ORDER (called from main.cpp each frame):
//   1. DrawSkybox()             — background panorama
//   2. DrawTexturedGround()     — pre-lit hardwood floor
//   3. DrawProjectedShadow()    — per-die ground shadows
//   4. DrawFloorReflections()   — mirrored dice on floor
//   5. DrawDieFacesLit()        — Gouraud-shaded dice faces
//   6. DrawDieScratchOverlay()  — per-pixel scratch highlights (additive)
//   7. DrawDieNumberDecals()    — textured number labels
//   8. DrawDieEdges()           — wireframe edge highlights
//   9. DrawDieBloom()           — specular glow halos (if enabled)
//  10. ApplyBloomPostProcess()  — screen-space bloom + fog (if enabled)
//  11. DrawHotbar()             — 2D dice selection UI
//
// ═══════════════════════════════════════════════════════════════════

#include "dice_defs.h"
#include "raymath.h"

// ── Lighting constants ──
// Direction vectors for the two-light studio setup (key + fill).
// Defined in rendering.cpp.  Used by LitVertex for dice and floor baking.
extern const Vector3 LIGHT_KEY;
extern const Vector3 LIGHT_FILL;

// ── Initialization (call once at startup) ──
void InitWoodTexture();   // Load materials, compute SH/probes, bake floor texture
void InitSkybox();        // Load skybox panorama image
void InitNumberAtlas();   // Generate number texture atlas (0–20)
void InitScratchTexture();// Convert scratch normal map to highlight overlay
void InitDirtTexture();  // Convert dirt normal map to bump overlay

// ── 3D drawing (call each frame in the order listed above) ──
void DrawSkybox(Vector3 camPos);                                    // Cylindrical panorama background
void DrawTexturedGround(float halfSize, float tileRepeat);          // Pre-lit hardwood floor quad
void DrawProjectedShadow(const ActiveDie& d, Matrix xform);        // Ground shadow for one die
void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos);  // Gouraud-lit die faces
void DrawDieScratchOverlay(const ActiveDie& d, const Matrix& xform,      // Per-pixel scratch highlights
                           Vector3 camPos);
void DrawDieDirtOverlay(const ActiveDie& d, const Matrix& xform,        // Per-pixel bump highlights/shadows
                        Vector3 camPos);
void DrawDieEdges(const ActiveDie& d, Matrix xform, Vector3 camPos);     // Subtle wireframe edges
void DrawDieBloom(const ActiveDie& d, Matrix xform, Vector3 camPos);     // Geometry bloom halos
void DrawDieNumberDecals(const ActiveDie& d, const Matrix& xform, Vector3 camPos); // Face numbers
void DrawFloorReflections(Vector3 camPos);                          // Y-flipped mirror reflections

// ── 2D UI overlay ──
void DrawHotbar();                                                  // Dice selection bar at bottom
void DrawTextBold(const char* text, int x, int y, int sz, Color col); // Bold text with shadow
void DrawHelpOverlay();                                             // Semi-transparent controls reference

// ── Post-processing ──
extern bool enablePostProcess;  // Toggle bloom + depth fog (default: off on MMF)
void ApplyBloomPostProcess();   // Apply screen-space effects via glPostProcess

// ── Cleanup ──
void UnloadRenderingTextures();  // Free all GPU textures and CPU buffers
