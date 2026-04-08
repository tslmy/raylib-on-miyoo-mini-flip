#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "raylib.h"
#include "rlgl.h"
#include "raymath.h"

int main(void) {
    // MMF native panel is 750x560; keep fullscreen.
    const int screenWidth = 750;
    const int screenHeight = 560;

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(screenWidth, screenHeight, "Raylib Cube - MMF");
    SetTargetFPS(30);

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 3.0f, 3.0f, 3.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    float rotation = 0.0f;
    const float cubeSize = 1.2f;
    const float half = cubeSize * 0.5f;
    const Vector3 baseVerts[8] = {
        {-half, -half, -half},
        {half, -half, -half},
        {half, half, -half},
        {-half, half, -half},
        {-half, -half, half},
        {half, -half, half},
        {half, half, half},
        {-half, half, half},
    };
    const int faces[6][4] = {
        {0, 1, 2, 3}, // -Z
        {5, 4, 7, 6}, // +Z
        {4, 0, 3, 7}, // -X
        {1, 5, 6, 2}, // +X
        {3, 2, 6, 7}, // +Y
        {4, 5, 1, 0}, // -Y
    };
    const Vector3 faceNormals[6] = {
        {0, 0, -1},
        {0, 0, 1},
        {-1, 0, 0},
        {1, 0, 0},
        {0, 1, 0},
        {0, -1, 0},
    };
    const int edges[12][2] = {
        {0, 1},
        {1, 2},
        {2, 3},
        {3, 0},
        {4, 5},
        {5, 6},
        {6, 7},
        {7, 4},
        {0, 4},
        {1, 5},
        {2, 6},
        {3, 7},
    };
    const Vector3 lightDir = Vector3Normalize((Vector3){0.3f, 0.6f, -0.7f});
    double fpsTimer = GetTime();
    int fpsFrames = 0;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        rotation += 60.0f * dt;
        if (rotation > 360.0f) rotation -= 360.0f;

        BeginDrawing();
        ClearBackground((Color){ 20, 24, 30, 255 });

        Vector3 verts[8];
        Matrix rot = MatrixRotateY(rotation * DEG2RAD);
        for (int i = 0; i < 8; i++)
            verts[i] = Vector3Transform(baseVerts[i], rot);

        BeginMode3D(camera);
        rlDisableBackfaceCulling();
        // Shaded faces
        for (int f = 0; f < 6; f++)
        {
            Vector3 n = Vector3Transform(faceNormals[f], rot);
            float ndotl = Vector3DotProduct(Vector3Normalize(n), lightDir);
            if (ndotl < 0.0f)
                ndotl = 0.0f;
            float intensity = 0.2f + 0.8f * ndotl;
            if (intensity > 1.0f)
                intensity = 1.0f;
            Color c = {
                (unsigned char)(0 * intensity),
                (unsigned char)(170 * intensity),
                (unsigned char)(255 * intensity),
                255};

            int i0 = faces[f][0], i1 = faces[f][1], i2 = faces[f][2], i3 = faces[f][3];
            DrawTriangle3D(verts[i0], verts[i1], verts[i2], c);
            DrawTriangle3D(verts[i0], verts[i2], verts[i3], c);
        }

        // Wireframe from rotated vertices (matches shaded faces)
        Color wire = (Color){255, 255, 255, 255};
        for (int e = 0; e < 12; e++)
        {
            DrawLine3D(verts[edges[e][0]], verts[edges[e][1]], wire);
        }
        rlEnableBackfaceCulling();
        DrawGrid(10, 1.0f);
        EndMode3D();

        DrawText("Raylib on MMF", 20, 20, 20, (Color){ 230, 230, 230, 255 });
        if (getenv("RAYLIB_MMF_SHOWFPS") != NULL)
            DrawFPS(20, 50);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
