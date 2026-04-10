// ═══════════════════════════════════════════════════════════════════
// main.cpp — Game loop, input handling, and render pipeline orchestration
// ═══════════════════════════════════════════════════════════════════
//
// This file ties together all the subsystems:
//   - Physics (physics.h/cpp) — Bullet3 rigid body simulation
//   - Rendering (rendering.h/cpp) — all visual output
//   - Dice definitions (dice_defs.h) — geometry and shared state
//
// ARCHITECTURE:
//   Each frame follows this pipeline:
//     1. Step physics
//     2. Handle gamepad input (hotbar, camera, throw)
//     3. Detect settled dice, read face-up values
//     4. Draw the 3D scene in correct order (see rendering.h)
//     5. Draw 2D UI overlay
//     6. Optionally save screenshot (headless mode)
//
// CAMERA:
//   Orbit camera around a pannable target point (default: table center).
//   L1/R1 = rotate, L2/R2 = zoom, X/Y = tilt.
//   SELECT + D-pad = pan, SELECT + X/Y = dolly.
//   START + D-pad/X/Y = orbit alternative.
//   Position computed from spherical coords (yaw, pitch, distance) + target.
//
// TRANSPARENCY:
//   Dice are semi-transparent (DICE_ALPHA).  For correct blending:
//     - Sort dice back-to-front (painter's algorithm)
//     - Disable depth writes during dice drawing
//     - Draw faces + decals interleaved per die (not all faces then all decals)
//
// ═══════════════════════════════════════════════════════════════════

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"
#include <GL/gl.h>

#include "dice_defs.h"
#include "physics.h"
#include "rendering.h"

int main(int argc, char **argv) {
    // ── HEADLESS SCREENSHOT MODE ──
    // Used by `just try N`: run N frames with fixed dt, save screenshot, exit.
    // Enables visual testing without a real display device.
    int screenshotFrames = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotFrames = atoi(argv[++i]);
    }

    srand((unsigned)time(nullptr));
    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(SCR_W, SCR_H, "Dice Roller - MMF");
    SetTargetFPS(30);

    // ── INITIALIZATION ──
    // Order matters: InitWoodTexture loads matcap, computes SH, builds probes,
    // and bakes the floor texture — all of which depend on skybox.png being readable.
    InitNumberAtlas();   // Generate 0–20 number atlas texture
    InitWoodTexture();   // Load materials, SH, probes, bake floor
    InitSkybox();        // Load skybox panorama
    InitScratchTexture();// Convert scratch normal map to highlight overlay
    InitDirtTexture();   // Convert dirt normal map to bump overlay
    InitPhysics();       // Set up Bullet3 world + ground plane
    ThrowAll();          // Spawn initial dice

    // ── CAMERA (orbit + pan) ──
    // Spherical coordinates around a movable target point.
    // Yaw = horizontal rotation, Pitch = vertical angle, Dist = zoom.
    // Target can be panned via SELECT + D-pad / X / Y.
    float camDist = 9.0f, camYaw = 45.0f, camPitch = 40.0f;
    Vector3 camTarget = {0, 0.5f, 0};
    Camera3D camera = {0};
    camera.up = {0, 1, 0};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    bool allSettled = false;  // true once all dice have stopped moving
    int totalResult = 0;     // sum of all face-up values

    // Modifier key tracking: SELECT/START act as modifiers when held with
    // other buttons, but toggle the help overlay when tapped alone.
    bool selectUsedAsModifier = false;
    bool startUsedAsModifier = false;
    bool showHelp = false;

    // Rig mode: hidden behind env var by default
    bool rigEnabled = (getenv("RAYLIB_MMF_RIG") != NULL);

    // ═══════════════════════════════════════════════════════════════
    // MAIN GAME LOOP
    // ═══════════════════════════════════════════════════════════════
    while (!WindowShouldClose()) {
        // ── PHYSICS ──
        // In headless mode, use fixed 1/30s timestep (deterministic).
        // On real hardware, use actual frame time (variable dt).
        float dt = GetFrameTime();
        if (screenshotFrames > 0) dt = 1.0f / 30.0f;  // fixed dt for deterministic screenshots
        StepPhysics(dt);

        // ── MODIFIER KEY TRACKING ──
        // SELECT and START serve dual purpose: modifier (when held with
        // other buttons) and toggle (when tapped alone).
        bool selectHeld = IsKeyDown(MMF_SELECT);
        bool startHeld  = IsKeyDown(MMF_START);

        if (IsKeyPressed(MMF_SELECT)) selectUsedAsModifier = false;
        if (IsKeyPressed(MMF_START))  startUsedAsModifier = false;

        // Helper: any action button pressed this frame?
        bool anyActionPressed = IsKeyDown(MMF_DPAD_LEFT) || IsKeyDown(MMF_DPAD_RIGHT) ||
                                IsKeyDown(MMF_DPAD_UP) || IsKeyDown(MMF_DPAD_DOWN) ||
                                IsKeyDown(MMF_X) || IsKeyDown(MMF_Y) ||
                                IsKeyDown(MMF_L1) || IsKeyDown(MMF_R1) ||
                                IsKeyDown(MMF_L2) || IsKeyDown(MMF_R2);
        if (selectHeld && anyActionPressed) selectUsedAsModifier = true;
        if (startHeld && anyActionPressed)  startUsedAsModifier = true;

        // On release, if not used as modifier → toggle help overlay
        if (IsKeyReleased(MMF_SELECT) && !selectUsedAsModifier) showHelp = !showHelp;
        if (IsKeyReleased(MMF_START) && !startUsedAsModifier)   showHelp = !showHelp;

        // ── HOTBAR INPUT (D-pad, default mode) ──
        // D-pad Left/Right: cycle through dice types
        // D-pad Up: add die (max 6 per type, MAX_ACTIVE_DICE total)
        // D-pad Down: remove die
        if (!selectHeld && !startHeld) {
            if (IsKeyPressed(MMF_DPAD_LEFT))
                hotbarSel = (hotbarSel + NUM_DICE_TYPES - 1) % NUM_DICE_TYPES;
            if (IsKeyPressed(MMF_DPAD_RIGHT))
                hotbarSel = (hotbarSel + 1) % NUM_DICE_TYPES;

            if (debounceY > 0) debounceY--;
            if (debounceX > 0) debounceX--;

            if (IsKeyPressed(MMF_DPAD_UP) && debounceY == 0) {
                int total = 0;
                for (int t = 0; t < NUM_DICE_TYPES; t++) total += hotbarCount[t];
                if (total < MAX_ACTIVE_DICE && hotbarCount[hotbarSel] < 6)
                    hotbarCount[hotbarSel]++;
                debounceY = DEBOUNCE_FRAMES;
            }
            if (IsKeyPressed(MMF_DPAD_DOWN) && debounceX == 0) {
                if (hotbarCount[hotbarSel] > 0) hotbarCount[hotbarSel]--;
                debounceX = DEBOUNCE_FRAMES;
            }
        }

        // A button: throw all configured dice
        if (IsKeyPressed(MMF_A)) {
            ThrowAll();
            if (rigEnabled && riggedValue >= 0) {
                for (int i = 0; i < numDice; i++) {
                    int maxVal = DICE_DEFS[dice[i].typeIdx].numValues;
                    dice[i].targetValue = (riggedValue <= maxVal) ? riggedValue : maxVal;
                }
            }
            allSettled = false;
            totalResult = 0;
        }

        // B button: cycle rigged value (only if rig mode enabled via env var)
        if (rigEnabled && IsKeyPressed(MMF_B)) {
            riggedValue++;
            if (riggedValue > 20) riggedValue = -1;
        }

        // ── CAMERA CONTROLS ──
        // Default: L1/R1 = rotate horizontally, L2/R2 = zoom, X/Y = tilt
        // SELECT held: D-pad = pan L/R/U/D, X/Y = pan forward/back
        // START held: D-pad = orbit (rotate + tilt), X/Y = tilt

        if (selectHeld) {
            // PAN mode: translate camera target in screen-space directions
            float yr = camYaw * DEG2RAD;
            // Camera's local right vector (horizontal, in XZ plane)
            Vector3 right = {cosf(yr), 0, -sinf(yr)};
            // Camera's local forward vector (horizontal, in XZ plane)
            Vector3 forward = {sinf(yr), 0, cosf(yr)};
            float panSpeed = 4.0f * dt;

            if (IsKeyDown(MMF_DPAD_LEFT))  camTarget = Vector3Add(camTarget, Vector3Scale(right, -panSpeed));
            if (IsKeyDown(MMF_DPAD_RIGHT)) camTarget = Vector3Add(camTarget, Vector3Scale(right,  panSpeed));
            if (IsKeyDown(MMF_DPAD_UP))    camTarget.y += panSpeed;
            if (IsKeyDown(MMF_DPAD_DOWN))  camTarget.y -= panSpeed;
            if (IsKeyDown(MMF_X))          camTarget = Vector3Add(camTarget, Vector3Scale(forward, -panSpeed));
            if (IsKeyDown(MMF_Y))          camTarget = Vector3Add(camTarget, Vector3Scale(forward,  panSpeed));
        } else if (startHeld) {
            // ORBIT mode via D-pad: same as L/R and X/Y defaults
            if (IsKeyDown(MMF_DPAD_LEFT))  camYaw   -= 90.0f * dt;
            if (IsKeyDown(MMF_DPAD_RIGHT)) camYaw   += 90.0f * dt;
            if (IsKeyDown(MMF_DPAD_UP))    camPitch += 90.0f * dt;
            if (IsKeyDown(MMF_DPAD_DOWN))  camPitch -= 90.0f * dt;
            if (IsKeyDown(MMF_X))          camPitch -= 90.0f * dt;
            if (IsKeyDown(MMF_Y))          camPitch += 90.0f * dt;
        }

        // Always-active camera controls (L/R, L2/R2, X/Y default)
        if (!selectHeld && !startHeld) {
            if (IsKeyDown(MMF_X))  camPitch -= 90.0f * dt;
            if (IsKeyDown(MMF_Y))  camPitch += 90.0f * dt;
        }
        if (IsKeyDown(MMF_L1))  camYaw  -= 90.0f * dt;
        if (IsKeyDown(MMF_R1))  camYaw  += 90.0f * dt;
        if (IsKeyDown(MMF_L2))  camDist -= 5.0f * dt;
        if (IsKeyDown(MMF_R2))  camDist += 5.0f * dt;

        if (camPitch > 85) camPitch = 85;
        if (camPitch < 5) camPitch = 5;
        if (camDist < 3) camDist = 3;
        if (camDist > 20) camDist = 20;

        // Convert spherical → Cartesian camera position
        float yr = camYaw * DEG2RAD, pr = camPitch * DEG2RAD;
        camera.position = {
            camTarget.x + camDist * cosf(pr) * sinf(yr),
            camTarget.y + camDist * sinf(pr),
            camTarget.z + camDist * cosf(pr) * cosf(yr),
        };
        camera.target = camTarget;

        // ── SETTLE DETECTION ──
        // After throwing, check each die every frame.  A die is "settled"
        // when it has been nearly motionless for 30+ consecutive frames.
        // Once ALL dice settle, we read their face-up values and sum them.
        if (!allSettled && numDice > 0) {
            bool all = true;
            for (int i = 0; i < numDice; i++) {
                ActiveDie& d = dice[i];
                if (!d.settled) {
                    if (IsDieSettled(d)) {
                        if (++d.settledFrames > 30) {
                            if (d.targetValue >= 0)
                                SnapDieToValue(d, d.targetValue);
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

        // ══════════════════════════════════════════════════════════════
        // DRAW 3D SCENE
        // ══════════════════════════════════════════════════════════════
        // Order matters for correct visual layering:
        //   skybox → floor → reflections → shadows → dice → decals → edges → bloom
        BeginDrawing();
        ClearBackground({20, 18, 16, 255});  // dark warm background

        BeginMode3D(camera);
        rlDisableBackfaceCulling();  // needed for skybox (we're inside the cylinder)

        DrawSkybox(camera.position);
        DrawTexturedGround(10.0f, 4.0f);
        DrawFloorReflections(camera.position);

        // Enable blend for transparent geometry (dice, shadows, reflections).
        // GL_SRC_ALPHA means: new_color × alpha + old_color × (1 - alpha)
        rlEnableColorBlend();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glShadeModel(GL_FLAT);  // shadows/decals use flat; DrawDieFacesLit switches to smooth

        // ── SHADOWS ──
        // Projected outline shadows on the floor for each die.
        for (int i = 0; i < numDice; i++) {
            Matrix xf = GetDieTransform(dice[i]);
            DrawProjectedShadow(dice[i], xf);
        }

        // ── BACK-TO-FRONT SORT for transparency ──
        // The "painter's algorithm": draw far objects first so near objects
        // blend on top correctly.  Without this, you'd see background through
        // a die that was drawn before a closer die.
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

        // ── DICE RENDERING ──
        // Disable depth writes so overlapping transparent dice don't clip each other.
        // Draw faces + decals + edges per die (interleaved, not batched by type)
        // to maintain correct occlusion between faces and their decals.
        rlDisableDepthMask();
        for (int i = 0; i < numDice; i++) {
            int di = diceOrder[i];
            Matrix xf = GetDieTransform(dice[di]);
            DrawDieFacesLit(dice[di], xf, camera.position);
            DrawDieDirtOverlay(dice[di], xf, camera.position);
            DrawDieScratchOverlay(dice[di], xf, camera.position);
            DrawDieNumberDecals(dice[di], xf, camera.position);
            DrawDieEdges(dice[di], xf, camera.position);
        }

        // ── GEOMETRY BLOOM ──
        // Optional enlarged specular halos around bright spots.
        // Gated behind enablePostProcess (off by default on MMF).
        if (enablePostProcess) {
            for (int i = 0; i < numDice; i++) {
                int di = diceOrder[i];
                Matrix xf = GetDieTransform(dice[di]);
                DrawDieBloom(dice[di], xf, camera.position);
            }
        }

        rlEnableDepthMask();      // restore depth writes
        glShadeModel(GL_SMOOTH);  // restore default shade model
        rlEnableBackfaceCulling();

        EndMode3D();

        // ── SCREEN-SPACE POST-PROCESSING ──
        // Bloom brightness boost + depth fog (only if enabled).
        ApplyBloomPostProcess();

        // ══════════════════════════════════════════════════════════════
        // 2D UI OVERLAY
        // ══════════════════════════════════════════════════════════════

        // ── RESULT DISPLAY ──
        // Show "3 + 5 + 2 = 10" style sum at the top of the screen.
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

        DrawHotbar();  // dice selection UI at bottom

        // Help overlay (toggled by tapping SELECT or START)
        if (showHelp) DrawHelpOverlay();

        // Show FPS counter if environment variable is set (debug aid)
        if (getenv("RAYLIB_MMF_SHOWFPS"))
            DrawFPS(20, 50);

        rlDisableColorBlend();  // clean state for next frame

        EndDrawing();

        // ── HEADLESS SCREENSHOT ──
        // In screenshot mode, count frames and exit after N.
        // The screenshot is taken by raylib's TakeScreenshot() which
        // reads the current framebuffer.
        if (screenshotFrames > 0) {
            static int frameCounter = 0;
            if (++frameCounter >= screenshotFrames) {
                TakeScreenshot("screenshot.png");
                break;
            }
        }
    }

    // ── CLEANUP ──
    UnloadRenderingTextures();  // free GPU textures + CPU buffers
    CleanupPhysics();           // destroy Bullet3 world + rigid bodies
    CloseWindow();              // close raylib window + context
    return 0;
}
