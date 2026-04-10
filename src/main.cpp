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
        if (screenshotFrames > 0) dt = 1.0f / 30.0f;
        StepPhysics(dt);

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
            if (riggedValue >= 0) {
                for (int i = 0; i < numDice; i++) {
                    int maxVal = DICE_DEFS[dice[i].typeIdx].numValues;
                    dice[i].targetValue = (riggedValue <= maxVal) ? riggedValue : maxVal;
                }
            }
            allSettled = false;
            totalResult = 0;
        }

        // B button: cycle rigged value
        if (IsKeyPressed(MMF_B)) {
            riggedValue++;
            if (riggedValue > 20) riggedValue = -1;
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

        // ── Draw 3D ──
        BeginDrawing();
        ClearBackground({20, 18, 16, 255});

        BeginMode3D(camera);
        rlDisableBackfaceCulling();

        DrawSkybox(camera.position);
        DrawTexturedGround(10.0f, 4.0f);
        DrawFloorReflections(camera.position);

        // Enable blend for transparent geometry.
        rlEnableColorBlend();
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glShadeModel(GL_FLAT);  // shadows/decals use flat; DrawDieFacesLit switches to smooth

        // Projected outline shadows
        for (int i = 0; i < numDice; i++) {
            Matrix xf = GetDieTransform(dice[i]);
            DrawProjectedShadow(dice[i], xf);
        }

        // Sort dice back-to-front for correct transparency
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

        // Per-die: faces + decals (interleaved for occlusion correctness)
        rlDisableDepthMask();
        for (int i = 0; i < numDice; i++) {
            int di = diceOrder[i];
            Matrix xf = GetDieTransform(dice[di]);
            DrawDieFacesLit(dice[di], xf, camera.position);
            DrawDieNumberDecals(dice[di], xf, camera.position);
            DrawDieEdges(dice[di], xf, camera.position);
        }

        // Bloom glow pass
        for (int i = 0; i < numDice; i++) {
            int di = diceOrder[i];
            Matrix xf = GetDieTransform(dice[di]);
            DrawDieBloom(dice[di], xf, camera.position);
        }

        rlEnableDepthMask();
        glShadeModel(GL_SMOOTH);
        rlEnableBackfaceCulling();

        EndMode3D();

        // Screen-space bloom on the 3D scene (before UI overlay)
        ApplyBloomPostProcess();

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

        rlDisableColorBlend();

        EndDrawing();

        // Headless screenshot mode
        if (screenshotFrames > 0) {
            static int frameCounter = 0;
            if (++frameCounter >= screenshotFrames) {
                TakeScreenshot("screenshot.png");
                break;
            }
        }
    }

    UnloadRenderingTextures();
    CleanupPhysics();
    CloseWindow();
    return 0;
}
