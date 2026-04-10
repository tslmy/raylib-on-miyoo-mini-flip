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
//   L1/R1 = rotate, L2/R2 = zoom, Y/X = tilt.
//   SELECT + D-pad = pan, SELECT + Y/X = dolly.
//   START + D-pad/Y/X = freelook (turn head from current position).
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

// Screen dimensions — initialized to a reasonable default, then updated
// to match the actual render buffer after InitWindow/InitPlatform.
int SCR_W = 752;
int SCR_H = 560;

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

    // Update SCR_W/SCR_H to match the actual render buffer.
    // On the real device, InitPlatform() may have overridden the size
    // to match the framebuffer (e.g., 640×480 instead of 752×560).
    SCR_W = GetRenderWidth();
    SCR_H = GetRenderHeight();
    TraceLog(LOG_INFO, "APP: Using render resolution %dx%d", SCR_W, SCR_H);

    SetTargetFPS(30);

    // ── BOOT SPLASH ──
    // Show each initialization step on screen so the user knows
    // the app is loading (bootup takes several seconds on device).
    struct BootStep { const char* label; void (*fn)(); };
    BootStep steps[] = {
        {"Number atlas",     InitNumberAtlas},
        {"Materials & floor",InitWoodTexture},
        {"Skybox",           InitSkybox},
        {"Scratch texture",  InitScratchTexture},
        {"Dirt texture",     InitDirtTexture},
        {"Physics engine",   InitPhysics},
    };
    const int numSteps = sizeof(steps) / sizeof(steps[0]);
    const int titleSize = 24;
    const int stepSize  = 16;
    const int lineGap   = 24;
    // Track completed step timings for display
    const char* doneLabels[16];
    float       doneTimes[16];
    int         doneCount = 0;

    for (int s = 0; s < numSteps; s++) {
        double t0 = GetTime();

        // Draw the splash screen showing progress so far
        BeginDrawing();
        ClearBackground({20, 18, 16, 255});

        const char* title = "Dice Roller";
        int tw = MeasureText(title, titleSize);
        DrawText(title, (SCR_W - tw)/2, 30, titleSize, {255, 220, 50, 255});

        int y = 70;
        for (int i = 0; i < doneCount; i++) {
            const char* line = TextFormat("  %s  %.2fs", doneLabels[i], doneTimes[i]);
            DrawText(line, 40, y, stepSize, {100, 200, 100, 255});
            y += lineGap;
        }
        // Current step (in progress)
        const char* cur = TextFormat("> %s ...", steps[s].label);
        DrawText(cur, 40, y, stepSize, {255, 255, 255, 255});

        EndDrawing();

        // Run the actual init function
        steps[s].fn();

        double elapsed = GetTime() - t0;
        doneLabels[doneCount] = steps[s].label;
        doneTimes[doneCount]  = (float)elapsed;
        doneCount++;
    }

    // Final splash frame showing all timings
    {
        BeginDrawing();
        ClearBackground({20, 18, 16, 255});
        const char* title = "Dice Roller";
        int tw = MeasureText(title, titleSize);
        DrawText(title, (SCR_W - tw)/2, 30, titleSize, {255, 220, 50, 255});
        int y = 70;
        float total = 0;
        for (int i = 0; i < doneCount; i++) {
            const char* line = TextFormat("  %s  %.2fs", doneLabels[i], doneTimes[i]);
            DrawText(line, 40, y, stepSize, {100, 200, 100, 255});
            total += doneTimes[i];
            y += lineGap;
        }
        const char* ready = TextFormat("  Ready!  Total: %.2fs", total);
        DrawText(ready, 40, y + 8, stepSize, {255, 220, 50, 255});
        EndDrawing();
    }

    ThrowAll();          // Spawn initial dice

    // ── CAMERA (orbit + pan + freelook) ──
    // Spherical coordinates around a movable target point.
    // Yaw = horizontal rotation, Pitch = vertical angle, Dist = zoom.
    // Target can be panned via SELECT + D-pad / X / Y.
    // START + D-pad freelooks (rotates view direction from current position).
    float camDist = 9.0f, camYaw = 45.0f, camPitch = 40.0f;
    Vector3 camTarget = {0, 0.5f, 0};
    // Freelook offsets: temporarily shift the look-at point while START is held
    float freelookYaw = 0.0f, freelookPitch = 0.0f;
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

    // ── KEY REPEAT (OS-style) ──
    // On first press, action fires immediately.
    // If held, waits INITIAL_DELAY frames, then repeats every REPEAT_INTERVAL.
    // This mimics how desktop OSes handle held-down keys.
    const int KEY_INITIAL_DELAY = 12;   // ~0.4s at 30fps before repeating
    const int KEY_REPEAT_INTERVAL = 3;  // ~0.1s between repeats
    struct KeyRepeat {
        int holdFrames = 0;

        bool Update(bool keyDown, int initDelay, int interval) {
            if (!keyDown) { holdFrames = 0; return false; }
            bool fire = (holdFrames == 0)
                || (holdFrames >= initDelay
                    && (holdFrames - initDelay) % interval == 0);
            holdFrames++;
            return fire;
        }
    };
    KeyRepeat repLeft, repRight, repUp, repDown;

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
        // Uses OS-style key repeat: immediate on first press, then auto-repeat.
        if (!selectHeld && !startHeld) {
            if (repLeft.Update(IsKeyDown(MMF_DPAD_LEFT), KEY_INITIAL_DELAY, KEY_REPEAT_INTERVAL))
                hotbarSel = (hotbarSel + NUM_DICE_TYPES - 1) % NUM_DICE_TYPES;
            if (repRight.Update(IsKeyDown(MMF_DPAD_RIGHT), KEY_INITIAL_DELAY, KEY_REPEAT_INTERVAL))
                hotbarSel = (hotbarSel + 1) % NUM_DICE_TYPES;

            if (repUp.Update(IsKeyDown(MMF_DPAD_UP), KEY_INITIAL_DELAY, KEY_REPEAT_INTERVAL)) {
                int total = 0;
                for (int t = 0; t < NUM_DICE_TYPES; t++) total += hotbarCount[t];
                if (total < MAX_ACTIVE_DICE && hotbarCount[hotbarSel] < 6)
                    hotbarCount[hotbarSel]++;
            }
            if (repDown.Update(IsKeyDown(MMF_DPAD_DOWN), KEY_INITIAL_DELAY, KEY_REPEAT_INTERVAL)) {
                if (hotbarCount[hotbarSel] > 0) hotbarCount[hotbarSel]--;
            }
        } else {
            // Reset repeat state when modifiers are held (D-pad is used for pan/look)
            repLeft.holdFrames = repRight.holdFrames = 0;
            repUp.holdFrames = repDown.holdFrames = 0;
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
        // Default: L1/R1 = rotate horizontally, L2/R2 = zoom, Y/X = tilt
        // SELECT held: D-pad = pan L/R/U/D, Y/X = dolly forward/back
        // START held: D-pad + Y/X = freelook (turn head from current position)

        if (selectHeld) {
            // PAN mode: translate camera target in screen-space directions
            float yr = camYaw * DEG2RAD;
            Vector3 right = {cosf(yr), 0, -sinf(yr)};
            Vector3 forward = {sinf(yr), 0, cosf(yr)};
            float panSpeed = 4.0f * dt;

            if (IsKeyDown(MMF_DPAD_LEFT))  camTarget = Vector3Add(camTarget, Vector3Scale(right, -panSpeed));
            if (IsKeyDown(MMF_DPAD_RIGHT)) camTarget = Vector3Add(camTarget, Vector3Scale(right,  panSpeed));
            if (IsKeyDown(MMF_DPAD_UP))    camTarget.y += panSpeed;
            if (IsKeyDown(MMF_DPAD_DOWN))  camTarget.y -= panSpeed;
            if (IsKeyDown(MMF_Y))          camTarget = Vector3Add(camTarget, Vector3Scale(forward,  panSpeed));
            if (IsKeyDown(MMF_X))          camTarget = Vector3Add(camTarget, Vector3Scale(forward, -panSpeed));
        } else if (startHeld) {
            // FREELOOK mode: rotate view direction from current camera position
            // (like turning your head — camera stays in place, look-at changes)
            if (IsKeyDown(MMF_DPAD_LEFT))  freelookYaw   -= 90.0f * dt;
            if (IsKeyDown(MMF_DPAD_RIGHT)) freelookYaw   += 90.0f * dt;
            if (IsKeyDown(MMF_DPAD_UP))    freelookPitch += 60.0f * dt;
            if (IsKeyDown(MMF_DPAD_DOWN))  freelookPitch -= 60.0f * dt;
            if (IsKeyDown(MMF_Y))          freelookPitch += 60.0f * dt;
            if (IsKeyDown(MMF_X))          freelookPitch -= 60.0f * dt;
        } else {
            // Reset freelook smoothly when START is released
            freelookYaw   *= 0.85f;
            freelookPitch *= 0.85f;
            if (fabsf(freelookYaw) < 0.5f) freelookYaw = 0;
            if (fabsf(freelookPitch) < 0.5f) freelookPitch = 0;
        }

        // Always-active camera controls (L/R rotate, L2/R2 zoom, Y/X tilt)
        if (!selectHeld && !startHeld) {
            if (IsKeyDown(MMF_Y))  camPitch += 90.0f * dt;
            if (IsKeyDown(MMF_X))  camPitch -= 90.0f * dt;
        }
        if (IsKeyDown(MMF_L1))  camYaw  -= 90.0f * dt;
        if (IsKeyDown(MMF_R1))  camYaw  += 90.0f * dt;
        if (IsKeyDown(MMF_L2))  camDist -= 5.0f * dt;
        if (IsKeyDown(MMF_R2))  camDist += 5.0f * dt;

        if (camPitch > 85) camPitch = 85;
        if (camPitch < 5) camPitch = 5;
        if (camDist < 3) camDist = 3;
        if (camDist > 20) camDist = 20;

        // Clamp freelook range
        if (freelookYaw > 60) freelookYaw = 60;
        if (freelookYaw < -60) freelookYaw = -60;
        if (freelookPitch > 40) freelookPitch = 40;
        if (freelookPitch < -40) freelookPitch = -40;

        // Convert spherical → Cartesian camera position (orbit around target)
        float yr = camYaw * DEG2RAD, pr = camPitch * DEG2RAD;
        camera.position = {
            camTarget.x + camDist * cosf(pr) * sinf(yr),
            camTarget.y + camDist * sinf(pr),
            camTarget.z + camDist * cosf(pr) * cosf(yr),
        };

        // Apply freelook: offset the look-at point from the orbit target
        if (freelookYaw != 0 || freelookPitch != 0) {
            float flyaw = (camYaw + freelookYaw) * DEG2RAD;
            float flpitch = freelookPitch * DEG2RAD;
            // Look-at point shifted in the freelook direction
            camera.target = {
                camTarget.x - sinf(flyaw) * cosf(flpitch) * 2.0f,
                camTarget.y + sinf(flpitch) * 2.0f,
                camTarget.z - cosf(flyaw) * cosf(flpitch) * 2.0f,
            };
        } else {
            camera.target = camTarget;
        }

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

        // Cache per-die transforms (used for shadows, sorting, and rendering)
        Matrix xforms[MAX_ACTIVE_DICE];
        for (int i = 0; i < numDice; i++)
            xforms[i] = GetDieTransform(dice[i]);

        // ── SHADOWS ──
        // Projected outline shadows on the floor for each die.
        for (int i = 0; i < numDice; i++)
            DrawProjectedShadow(dice[i], xforms[i]);

        // ── BACK-TO-FRONT SORT for transparency ──
        // The "painter's algorithm": draw far objects first so near objects
        // blend on top correctly.  Without this, you'd see background through
        // a die that was drawn before a closer die.
        int diceOrder[MAX_ACTIVE_DICE];
        float diceDist[MAX_ACTIVE_DICE];  // cached squared distances
        for (int i = 0; i < numDice; i++) {
            diceOrder[i] = i;
            diceDist[i] = Vector3LengthSqr(Vector3Subtract(
                {xforms[i].m12, xforms[i].m13, xforms[i].m14}, camera.position));
        }
        for (int i = 0; i < numDice - 1; i++)
            for (int j = i + 1; j < numDice; j++) {
                if (diceDist[diceOrder[i]] < diceDist[diceOrder[j]]) {
                    int tmp = diceOrder[i]; diceOrder[i] = diceOrder[j]; diceOrder[j] = tmp;
                }
            }

        // ── DICE RENDERING ──
        // Disable depth writes so overlapping transparent dice don't clip each other.
        // Draw faces + decals + edges per die (interleaved, not batched by type)
        // to maintain correct occlusion between faces and their decals.
        rlDisableDepthMask();
        for (int i = 0; i < numDice; i++) {
            int di = diceOrder[i];
            DrawDieFacesLit(dice[di], xforms[di], camera.position);
            DrawDieDirtOverlay(dice[di], xforms[di], camera.position);
            DrawDieScratchOverlay(dice[di], xforms[di], camera.position);
            DrawDieNumberDecals(dice[di], xforms[di], camera.position);
            DrawDieEdges(dice[di], xforms[di], camera.position);
        }

        // ── GEOMETRY BLOOM ──
        // Optional enlarged specular halos around bright spots.
        // Gated behind enablePostProcess (off by default on MMF).
        if (enablePostProcess) {
            for (int i = 0; i < numDice; i++) {
                int di = diceOrder[i];
                DrawDieBloom(dice[di], xforms[di], camera.position);
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
