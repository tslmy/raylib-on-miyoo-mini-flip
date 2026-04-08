#include <math.h>
#include "raylib.h"

int main(void) {
    // MMF native panel is 750x560; keep fullscreen.
    const int screenWidth = 750;
    const int screenHeight = 560;

    SetConfigFlags(FLAG_FULLSCREEN_MODE);
    InitWindow(screenWidth, screenHeight, "Raylib Cube - MMF");
    SetTargetFPS(60);

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 3.0f, 3.0f, 3.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    float rotation = 0.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        rotation += 60.0f * dt;
        if (rotation > 360.0f) rotation -= 360.0f;

        BeginDrawing();
        ClearBackground((Color){ 20, 24, 30, 255 });

        BeginMode3D(camera);
        DrawCube((Vector3){ 0.0f, 0.0f, 0.0f }, 1.2f, 1.2f, 1.2f, (Color){ 0, 170, 255, 255 });
        DrawCubeWires((Vector3){ 0.0f, 0.0f, 0.0f }, 1.2f, 1.2f, 1.2f, (Color){ 230, 230, 230, 255 });
        DrawGrid(10, 1.0f);
        EndMode3D();

        // Rotate the camera around the cube for a more dynamic view.
        camera.position = (Vector3){
            3.0f * sinf(rotation * DEG2RAD),
            2.0f,
            3.0f * cosf(rotation * DEG2RAD)
        };

        DrawText("Raylib on MMF", 20, 20, 20, (Color){ 230, 230, 230, 255 });
        DrawFPS(20, 50);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
