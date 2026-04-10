// ═══════════════════════════════════════════════════════════════════
// rendering.cpp — All visual output: lighting, shadows, reflections,
//                 textures, skybox, post-processing, and UI.
// ═══════════════════════════════════════════════════════════════════
//
// RENDERING PIPELINE OVERVIEW:
//
//   "Rendering" means turning 3D geometry into a 2D image on screen.
//   We use a SOFTWARE RENDERER (TinyGL) — the CPU does all the math
//   that a GPU would normally handle.  This is slow but works on the
//   MMF which has no GPU.
//
//   Each frame, we draw layers in a specific order (back to front):
//     1. Skybox          — the background "sky dome"
//     2. Textured floor  — a pre-lit hardwood quad
//     3. Floor reflections — upside-down copies of dice, faded
//     4. Shadows         — dark silhouettes projected onto the floor
//     5. Dice faces      — translucent, Gouraud-shaded, PBR-lit
//     6. Number decals   — digits painted on each face
//     7. Edge wireframes — subtle white outlines
//     8. Bloom glow      — bright halos on specular highlights
//     9. Post-processing — per-pixel bloom boost + depth fog
//    10. UI overlay      — hotbar, result text, FPS
//
// KEY GRAPHICS CONCEPTS USED HERE:
//
//   DIFFUSE LIGHTING — how bright a surface is based on its angle
//     to the light.  A surface facing the light is bright; one
//     facing away is dark.  Computed via dot product of the surface
//     normal and light direction.
//
//   SPECULAR HIGHLIGHT — the shiny "hot spot" you see on glossy
//     objects.  Computed using the "half vector" (halfway between
//     the light direction and the view direction).  Raising it to
//     a high power (e.g. pow-16) makes it tight and shiny.
//
//   FRESNEL EFFECT — edges of transparent objects look more opaque
//     and reflective than the center.  Look at a glass from the
//     side vs. straight on — the side is mirror-like.  Computed
//     from the angle between the surface normal and view direction.
//
//   GOURAUD SHADING — instead of one color per face ("flat shading"),
//     we compute a color at each VERTEX and let the rasterizer
//     smoothly interpolate between them.  This creates soft gradients
//     across faces, making curved objects look smooth.
//
//   ALPHA BLENDING — mixing a semi-transparent foreground pixel with
//     the background behind it.  alpha=255 is opaque, alpha=0 is
//     invisible.  Formula: result = src×alpha + dst×(1-alpha).
//
//   DEPTH SORTING — transparent objects must be drawn back-to-front
//     (the "painter's algorithm") so that further dice are behind
//     nearer ones.  Opaque objects can rely on the Z-buffer, but
//     transparent ones cannot.
// ═══════════════════════════════════════════════════════════════════

#include "rendering.h"
#include "physics.h"
#include "rlgl.h"
#include <GL/gl.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
// Lighting — two-light studio setup
// ═══════════════════════════════════════════════════════════════════
//
// Real-world photography and film use a "key + fill" lighting setup:
//   KEY LIGHT  — the main, brightest light (like the sun).  It comes
//                from upper-right-front and creates the primary
//                shadows and highlights.
//   FILL LIGHT — a softer, dimmer light from the opposite side.  It
//                fills in the shadows so they're not pitch black.
//
// These are DIRECTIONAL lights (infinitely far away, like the sun),
// represented as normalized direction vectors pointing FROM the
// surface TOWARD the light source.

const Vector3 LIGHT_KEY  = Vector3Normalize((Vector3){0.4f, 0.8f, 0.5f});
const Vector3 LIGHT_FILL = Vector3Normalize((Vector3){-0.5f, 0.3f, -0.3f});

// ═══════════════════════════════════════════════════════════════════
// Fast math helpers — avoid libm overhead in hot per-vertex loops
// ═══════════════════════════════════════════════════════════════════

// Fast x^5 via repeated multiplication (3 muls instead of libm powf).
// Used for Schlick's Fresnel approximation: F = F0 + (1-F0)(1-cosθ)^5
static inline float pow5f(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}

// Fast inverse square root (Quake III "Q_rsqrt" trick).
// Uses a clever bit-level hack: reinterpret the float's bits as an
// integer, apply a magic constant that gives a good initial estimate,
// then refine with one Newton-Raphson iteration.
// Accuracy: ~0.2% error — plenty for lighting math.
static inline float fast_rsqrtf(float x) {
    union { float f; uint32_t i; } conv = {x};
    conv.i = 0x5f3759df - (conv.i >> 1);            // magic number initial guess
    conv.f *= 1.5f - (x * 0.5f * conv.f * conv.f);  // Newton-Raphson step
    return conv.f;
}

// Fast normalize using fast_rsqrtf (avoids sqrtf + division).
static inline Vector3 fast_normalize(Vector3 v) {
    float len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (len2 < 1e-8f) return v;
    float inv = fast_rsqrtf(len2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

// ═══════════════════════════════════════════════════════════════════
// Spherical Harmonics L2 — environment lighting from the skybox
// ═══════════════════════════════════════════════════════════════════
//
// WHAT ARE SPHERICAL HARMONICS (SH)?
//
//   Imagine you're standing in the center of a room.  Light comes at
//   you from every direction — bright from the window, dim from the
//   floor, tinted from colored walls.  This "light from all around"
//   is called ENVIRONMENT LIGHTING or IMAGE-BASED LIGHTING (IBL).
//
//   To capture all that information, you'd need a value for every
//   possible direction — infinitely many.  SH compresses this into
//   just a few numbers using special mathematical functions (like
//   how a music equalizer captures a song with just bass/mid/treble
//   levels instead of every individual frequency).
//
//   "L2" means we use 9 basis functions (levels 0, 1, and 2),
//   giving us 9 coefficients × 3 color channels = 27 floats total.
//   This captures the overall brightness (L0), which direction is
//   brightest (L1), and broad color variation (L2).  It's not
//   detailed enough for sharp reflections, but perfect for soft
//   ambient lighting.
//
// HOW WE USE IT:
//
//   At startup, we read every pixel of the skybox image, convert
//   each pixel's position to a direction on a sphere, and
//   "project" (accumulate) its color into the 9 SH coefficients.
//
//   At render time, given a surface normal direction, we evaluate
//   the SH to get the average light color coming from that
//   direction — e.g., a surface pointing up gets sky color, a
//   surface pointing down gets ground color.
//
// WHY NOT JUST SAMPLE THE SKYBOX DIRECTLY?
//
//   SH evaluation is just 9 multiplies + adds (blazing fast).
//   Sampling a texture per-vertex would require UV math + memory
//   access, and can't easily capture light from ALL directions
//   at once — SH integrates the entire hemisphere automatically.

static float shCoeffs[9][3] = {};  // 9 basis functions × RGB channels
static bool  shReady = false;

// Project the skybox image onto SH basis functions.
// This is the offline "encoding" step — runs once at startup.
static void ProjectSkyboxToSH(const char* path) {
    Image sky = LoadImage(path);
    if (sky.data == NULL) return;
    ImageFormat(&sky, PIXELFORMAT_UNCOMPRESSED_R8G8B8);

    int w = sky.width, h = sky.height;
    unsigned char* px = (unsigned char*)sky.data;

    // Treat the skybox as an "equirectangular" image:
    // - x axis = longitude (0° to 360°), called φ (phi)
    // - y axis = latitude (north pole to south pole), called θ (theta)
    // For each pixel, we compute the 3D direction it represents,
    // then multiply its color by each SH basis function and accumulate.
    double acc[9][3] = {};    // running sum of color × basis × solid_angle
    double weightSum = 0;     // total solid angle (should sum to ~4π)

    for (int y = 0; y < h; y++) {
        float v = ((float)y + 0.5f) / h;
        float theta = v * PI;           // polar angle: 0 = north pole, π = south pole
        float sinTheta = sinf(theta);
        float cosTheta = cosf(theta);
        // Pixels near the poles represent less area on the sphere than
        // pixels near the equator.  sinTheta corrects for this distortion.
        float dOmega = sinTheta;        // solid angle weight

        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) / w;
            float phi = u * 2.0f * PI;  // azimuthal [0, 2π]

            // Convert pixel position to a unit direction vector on the sphere.
            // This is the standard spherical-to-Cartesian conversion:
            //   x = sin(θ)·sin(φ)    (right/left)
            //   y = cos(θ)           (up/down)
            //   z = sin(θ)·cos(φ)    (forward/back)
            float dx = sinTheta * sinf(phi);
            float dy = cosTheta;
            float dz = sinTheta * cosf(phi);

            // Read the pixel color (normalized to 0..1)
            int i = (y * w + x) * 3;
            float r = px[i] / 255.0f;
            float g = px[i+1] / 255.0f;
            float b = px[i+2] / 255.0f;

            // Evaluate the 9 SH basis functions at this direction.
            // These are the real-valued spherical harmonics up to order 2.
            // The magic numbers (0.282095, 0.488603, etc.) are normalization
            // constants that make the basis functions orthonormal — each
            // function captures an independent "frequency" of variation.
            //
            //   L0 (1 function):  constant (average brightness)
            //   L1 (3 functions): linear (which direction is brightest)
            //   L2 (5 functions): quadratic (broad color variation)
            float basis[9];
            basis[0] = 0.282095f;                          // Y(0,0): constant
            basis[1] = 0.488603f * dy;                     // Y(1,0): up/down
            basis[2] = 0.488603f * dz;                     // Y(1,1): front/back
            basis[3] = 0.488603f * dx;                     // Y(1,-1): left/right
            basis[4] = 1.092548f * dx * dy;                // Y(2,-1): diagonal
            basis[5] = 1.092548f * dy * dz;                // Y(2,0): diagonal
            basis[6] = 0.315392f * (3*dy*dy - 1);         // Y(2,1): top-vs-sides
            basis[7] = 1.092548f * dx * dz;                // Y(2,-2): diagonal
            basis[8] = 0.546274f * (dx*dx - dz*dz);       // Y(2,2): left-vs-front

            // Accumulate: color × basis × solid_angle for each channel.
            // This is a numerical integral over the sphere.
            for (int k = 0; k < 9; k++) {
                acc[k][0] += r * basis[k] * dOmega;
                acc[k][1] += g * basis[k] * dOmega;
                acc[k][2] += b * basis[k] * dOmega;
            }
            weightSum += dOmega;
        }
    }

    // Normalize: integral over sphere is 4π, so scale = 4π / weightSum
    float scale = (4.0f * PI) / (float)weightSum;
    for (int k = 0; k < 9; k++) {
        shCoeffs[k][0] = (float)(acc[k][0] * scale);
        shCoeffs[k][1] = (float)(acc[k][1] * scale);
        shCoeffs[k][2] = (float)(acc[k][2] * scale);
    }
    shReady = true;
    UnloadImage(sky);
    TraceLog(LOG_INFO, "SH: L2 projected from %s (%dx%d), DC=%.3f,%.3f,%.3f",
             path, w, h, shCoeffs[0][0], shCoeffs[0][1], shCoeffs[0][2]);
}

// Evaluate SH irradiance at a given surface normal direction.
// Returns the environment light color (RGB, 0..~2) that a surface
// facing direction `n` would receive from all directions.
// This is the runtime "decoding" step — called per-vertex.
static void EvalSH(Vector3 n, float* outR, float* outG, float* outB) {
    if (!shReady) { *outR = 0.5f; *outG = 0.5f; *outB = 0.5f; return; }
    float basis[9];
    basis[0] = 0.282095f;
    basis[1] = 0.488603f * n.y;
    basis[2] = 0.488603f * n.z;
    basis[3] = 0.488603f * n.x;
    basis[4] = 1.092548f * n.x * n.y;
    basis[5] = 1.092548f * n.y * n.z;
    basis[6] = 0.315392f * (3*n.y*n.y - 1);
    basis[7] = 1.092548f * n.x * n.z;
    basis[8] = 0.546274f * (n.x*n.x - n.z*n.z);

    float r = 0, g = 0, b = 0;
    for (int k = 0; k < 9; k++) {
        r += shCoeffs[k][0] * basis[k];
        g += shCoeffs[k][1] * basis[k];
        b += shCoeffs[k][2] * basis[k];
    }
    *outR = r < 0 ? 0 : r;
    *outG = g < 0 ? 0 : g;
    *outB = b < 0 ? 0 : b;
}

// ═══════════════════════════════════════════════════════════════════
// MatCap — fake environment reflections from a 2D texture
// ═══════════════════════════════════════════════════════════════════
//
// WHAT IS A MATCAP?
//
//   "Material Capture" — a photograph of a sphere that captures an
//   entire material's appearance: color, reflections, highlights,
//   and shadows, all baked into a single small image.
//
//   The trick: if you know which direction a surface faces relative
//   to the camera (its "view-space normal"), you can look up the
//   corresponding color in the matcap image.  Surfaces facing the
//   camera center get the matcap center color; surfaces at the edge
//   get the matcap edge color (usually brighter, like a rim light).
//
// WHY CPU-SIDE?
//
//   Normally matcaps are applied per-pixel in a GPU shader.  Since
//   we have no GPU, we sample per-VERTEX on the CPU and let TinyGL's
//   Gouraud interpolation blend between vertices.  This is fast
//   enough (only ~20 vertices per die) and still looks good.
//
// HOW THE UV LOOKUP WORKS:
//
//   1. Transform the world-space surface normal into view space
//      (relative to the camera's orientation).
//   2. Take the x and y components of the view-space normal.
//   3. Map them to texture coordinates: u = nx*0.5+0.5, v = 0.5-ny*0.5
//      This maps the center of the sphere (normal pointing at camera)
//      to the center of the texture, and edges to the texture edges.

static Color* matcapPixels = NULL;  // raw pixel data (CPU-side, not a GPU texture)
static int    matcapSize   = 0;     // width = height (square image)

// Load a matcap image into CPU memory for per-vertex sampling.
// We keep the raw pixels (not a GPU texture) because we sample them
// from C++ code, not from a shader.
static void LoadMatCap(const char* path) {
    Image img = LoadImage(path);
    if (img.data == NULL) {
        TraceLog(LOG_WARNING, "MATCAP: %s not found", path);
        return;
    }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    matcapSize = img.width;  // assume square
    matcapPixels = (Color*)RL_MALLOC(matcapSize * matcapSize * sizeof(Color));
    memcpy(matcapPixels, img.data, matcapSize * matcapSize * sizeof(Color));
    UnloadImage(img);
    TraceLog(LOG_INFO, "MATCAP: Loaded %dx%d from %s", matcapSize, matcapSize, path);
}

// Sample the matcap at a given world-space normal, viewed from viewDir.
// Returns the matcap color that this surface orientation would show.
static Color SampleMatCap(Vector3 normal, Vector3 viewDir) {
    if (!matcapPixels) return {180, 180, 190, 255};

    // Build a "view space" coordinate system from the camera direction.
    // View space has: right = camera's right, up = camera's up,
    // forward = direction the camera is looking.
    Vector3 forward = {-viewDir.x, -viewDir.y, -viewDir.z};
    Vector3 up = {0, 1, 0};
    Vector3 right = fast_normalize(Vector3CrossProduct(up, forward));
    up = Vector3CrossProduct(forward, right);  // re-orthogonalize

    // Project the world normal into view space by dotting with right/up.
    // This tells us "how much does this surface face left/right and up/down
    // relative to the camera?"
    float nx = Vector3DotProduct(normal, right);
    float ny = Vector3DotProduct(normal, up);

    // Map from [-1,+1] normal range to [0,1] texture UV range.
    float u = nx * 0.5f + 0.5f;
    float v = 0.5f - ny * 0.5f;  // flip Y because image Y goes downward

    int px = (int)(u * (matcapSize - 1));
    int py = (int)(v * (matcapSize - 1));
    if (px < 0) px = 0; if (px >= matcapSize) px = matcapSize - 1;
    if (py < 0) py = 0; if (py >= matcapSize) py = matcapSize - 1;

    return matcapPixels[py * matcapSize + px];
}

// ═══════════════════════════════════════════════════════════════════
// Projected outline shadows
// ═══════════════════════════════════════════════════════════════════
//
// HOW PROJECTED SHADOWS WORK:
//
//   For each die, we project every vertex onto the floor along the
//   key light direction (like holding a flashlight above the die and
//   tracing where the shadow falls).  The projected points form a
//   2D shape.  We compute the CONVEX HULL of those points (the
//   tightest outline) and draw it as a dark polygon on the floor.
//
//   PENUMBRA — real shadows have soft edges because light sources
//   aren't infinitely small.  We fake this by drawing a second,
//   slightly larger, more transparent polygon around the inner
//   shadow.  This creates a soft fade-out at the edges.
//
//   HEIGHT ATTENUATION — dice high in the air cast fainter shadows
//   (in real life, shadows spread and soften with distance from the
//   surface).  We reduce shadow opacity based on die height.

void DrawProjectedShadow(const ActiveDie& d, Matrix xform) {
    const float groundY = 0.002f;   // slightly above y=0 to avoid Z-fighting with floor
    const float PENUMBRA = 0.18f;   // how far the soft edge extends (world units)

    // Transform die vertices from local space to world space.
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    // Project each vertex onto the floor along the light direction.
    // This is like tracing a ray from each vertex toward the light and
    // finding where it intersects the y=0 plane.
    Vector2 proj[MAX_DIE_VERTS];
    int nProj = 0;
    for (int i = 0; i < d.numVerts; i++) {
        if (LIGHT_KEY.y <= 0.001f) continue;
        float t = wv[i].y / LIGHT_KEY.y;  // parametric distance to floor
        proj[nProj++] = {wv[i].x - LIGHT_KEY.x * t,
                         wv[i].z - LIGHT_KEY.z * t};
    }
    if (nProj < 3) return;

    // 2D CONVEX HULL (Andrew's monotone chain algorithm):
    // Finds the smallest convex polygon containing all projected points.
    // First sort points by x (then y), then build lower and upper hulls.
    for (int i = 0; i < nProj - 1; i++)
        for (int j = i + 1; j < nProj; j++)
            if (proj[j].x < proj[i].x ||
                (proj[j].x == proj[i].x && proj[j].y < proj[i].y)) {
                Vector2 tmp = proj[i]; proj[i] = proj[j]; proj[j] = tmp;
            }

    Vector2 hull[MAX_DIE_VERTS * 2];
    int k = 0;
    for (int i = 0; i < nProj; i++) {
        while (k >= 2) {
            float cross = (hull[k-1].x - hull[k-2].x) * (proj[i].y - hull[k-2].y)
                        - (hull[k-1].y - hull[k-2].y) * (proj[i].x - hull[k-2].x);
            if (cross <= 0) k--; else break;
        }
        hull[k++] = proj[i];
    }
    int lower_size = k + 1;
    for (int i = nProj - 2; i >= 0; i--) {
        while (k >= lower_size) {
            float cross = (hull[k-1].x - hull[k-2].x) * (proj[i].y - hull[k-2].y)
                        - (hull[k-1].y - hull[k-2].y) * (proj[i].x - hull[k-2].x);
            if (cross <= 0) k--; else break;
        }
        hull[k++] = proj[i];
    }
    int hullCount = k - 1;
    if (hullCount < 3) return;

    // Find the centroid (center point) of the shadow hull.
    float cx = 0, cz = 0;
    for (int i = 0; i < hullCount; i++) { cx += hull[i].x; cz += hull[i].y; }
    cx /= hullCount; cz /= hullCount;

    // Fade shadow based on die height — higher dice = weaker shadow.
    float height = xform.m13;  // m13 is the Y position in the transform matrix
    if (height < 0) height = 0;
    float intensity = 1.0f - height / 8.0f;
    if (intensity < 0) return;
    unsigned char innerAlpha = (unsigned char)(intensity * 140);
    Color innerCol = {0, 0, 0, innerAlpha};

    // Create the outer (penumbra) hull by pushing each vertex outward
    // from the centroid.  This creates the soft-edge ring around the
    // sharp inner shadow.
    Vector2 outerHull[MAX_DIE_VERTS * 2];
    for (int i = 0; i < hullCount; i++) {
        float dx = hull[i].x - cx;
        float dz = hull[i].y - cz;
        float len = sqrtf(dx * dx + dz * dz);
        if (len > 0.001f) {
            outerHull[i] = {hull[i].x + dx / len * PENUMBRA,
                            hull[i].y + dz / len * PENUMBRA};
        } else {
            outerHull[i] = hull[i];
        }
    }

    for (int i = 0; i < hullCount; i++) {
        int j = (i + 1) % hullCount;
        DrawTriangle3D({cx, groundY, cz},
                       {hull[i].x, groundY, hull[i].y},
                       {hull[j].x, groundY, hull[j].y}, innerCol);
    }

    for (int i = 0; i < hullCount; i++) {
        int j = (i + 1) % hullCount;
        unsigned char midAlpha = innerAlpha / 3;
        Color midCol = {0, 0, 0, midAlpha};
        DrawTriangle3D({hull[i].x, groundY, hull[i].y},
                       {outerHull[i].x, groundY, outerHull[i].y},
                       {outerHull[j].x, groundY, outerHull[j].y}, midCol);
        DrawTriangle3D({hull[i].x, groundY, hull[i].y},
                       {outerHull[j].x, groundY, outerHull[j].y},
                       {hull[j].x, groundY, hull[j].y}, midCol);
    }
}

// ═══════════════════════════════════════════════════════════════════
// PBR-inspired dice face lighting (the core visual quality)
// ═══════════════════════════════════════════════════════════════════
//
// PBR = "Physically Based Rendering" — a lighting model that tries
// to approximate how light interacts with real materials.  Our version
// is simplified for CPU rendering but captures the key visual effects:
//
//   DIFFUSE  — base color × how much light hits the surface
//   SPECULAR — tight bright spots where light reflects toward camera
//   CLEARCOAT — a second specular layer (like a shiny lacquer coat)
//   FRESNEL  — edges glow brighter (glass/water effect)
//   MATCAP   — pre-baked environment reflections from a 2D texture
//   SH IBL   — soft ambient light color from the skybox
//
// All of this is computed PER-VERTEX and then Gouraud-interpolated
// by TinyGL across each triangle.  This gives smooth color gradients
// without the expense of per-pixel shading.

// Compute the lit color for a single vertex.
// Inputs: vertex position, surface normal, base color, camera position,
//         dimFactor (0.6 for non-numbered faces, 1.0 for numbered).
// Returns: final RGBA color including transparency.
static Color LitVertex(Vector3 pos, Vector3 normal, Color base, Vector3 camPos,
                       float dimFactor) {
    // VIEW DIRECTION: vector from vertex toward the camera.
    // Many lighting calculations need to know where the viewer is.
    Vector3 viewDir = fast_normalize(Vector3Subtract(camPos, pos));

    // ── DIFFUSE LIGHTING ──
    // How bright is this surface?  Combine SH environment (soft, colored,
    // comes from all directions) with directional key/fill lights (sharp).
    // The DOT PRODUCT of normal and light direction gives cos(angle):
    //   = 1 when surface directly faces the light (bright)
    //   = 0 when perpendicular (no contribution)
    //   < 0 when facing away (clamped to 0 — no negative light)
    float shR, shG, shB;
    EvalSH(normal, &shR, &shG, &shB);
    // SH provides ambient; directional lights add contrast
    float keyDot = Vector3DotProduct(normal, LIGHT_KEY);
    if (keyDot < 0) keyDot = 0;
    float fillDot = Vector3DotProduct(normal, LIGHT_FILL);
    if (fillDot < 0) fillDot = 0;
    // Blend: SH ambient (normalized ~0.5 base) + directional contribution
    float diffR = shR * 0.6f + 0.30f * keyDot + 0.10f * fillDot;
    float diffG = shG * 0.6f + 0.30f * keyDot + 0.10f * fillDot;
    float diffB = shB * 0.6f + 0.30f * keyDot + 0.10f * fillDot;

    // ── SPECULAR HIGHLIGHT (Blinn-Phong, pow-16 from key light) ──
    // The HALF VECTOR is halfway between light direction and view direction.
    // When the surface normal aligns with it, light reflects straight into
    // the camera — you see a bright "hot spot".  Raising to pow-16 makes
    // it tight and shiny (higher power = smaller, sharper highlight).
    // Specular: Blinn-Phong pow-16 (key light)
    Vector3 halfVec = fast_normalize(Vector3Add(LIGHT_KEY, viewDir));
    float specDot = Vector3DotProduct(normal, halfVec);
    if (specDot < 0) specDot = 0;
    float spec = specDot;
    for (int p = 0; p < 4; p++) spec *= spec;

    // ── CLEARCOAT (second specular layer with procedural scratches) ──
    // Real dice have a glossy lacquer coating that adds an extra
    // highlight layer.  We perturb the normal slightly per-vertex using
    // a hash of the position — this fakes micro-scratches on the surface
    // that make the clearcoat highlight shimmer and break up.
    Vector3 ccN = normal;
    {
        // Procedural noise: hash vertex position to get pseudo-random
        // scratch direction.  Different constants produce different patterns.
        unsigned int s2 = (unsigned int)(pos.x * 5000) * 48271u
                        + (unsigned int)(pos.z * 5000) * 16807u
                        + (unsigned int)(pos.y * 3000) * 65537u;
        float sx = ((s2 & 0xFF) / 255.0f - 0.5f) * 0.15f;
        float sz = (((s2 >> 8) & 0xFF) / 255.0f - 0.5f) * 0.05f;
        ccN.x += sx; ccN.z += sz;
        float len2 = ccN.x*ccN.x + ccN.y*ccN.y + ccN.z*ccN.z;
        if (len2 > 1e-6f) {
            float inv = fast_rsqrtf(len2);
            ccN.x *= inv; ccN.y *= inv; ccN.z *= inv;
        }
    }
    float ccSpecDot = Vector3DotProduct(ccN, halfVec);
    if (ccSpecDot < 0) ccSpecDot = 0;
    float ccSpec = ccSpecDot;
    for (int p = 0; p < 5; p++) ccSpec *= ccSpec;
    float clearcoat = 0.5f * ccSpec;

    // ── FILL SPECULAR (softer, from the fill light, pow-4) ──
    // A subtle secondary highlight from the fill light.  Lower power
    // = broader, softer spot.  This prevents the fill side from looking
    // completely flat.
    Vector3 halfFill = fast_normalize(Vector3Add(LIGHT_FILL, viewDir));
    float fillSpec = Vector3DotProduct(normal, halfFill);
    if (fillSpec < 0) fillSpec = 0;
    fillSpec = fillSpec * fillSpec * fillSpec * fillSpec;

    // ── FRESNEL RIM GLOW ──
    // The Fresnel effect: surfaces viewed at a grazing angle (edges)
    // become more reflective than surfaces viewed head-on.  This is why
    // a glass looks transparent when you look through it, but mirror-like
    // when you look at its edge.
    //
    // Schlick's approximation: F = F0 + (1-F0) × (1 - cos(θ))^5
    //   F0 = 0.04 (glass reflectance at normal incidence)
    //   θ  = angle between normal and view direction
    float NdotV = Vector3DotProduct(normal, viewDir);
    if (NdotV < 0) NdotV = 0;
    float fresnel = 0.04f + 0.96f * pow5f(1.0f - NdotV);
    float rim = fresnel * 0.70f;  // stronger rim for glass-like IOR 1.5

    // ── COMPOSE: add up all lighting contributions ──
    // Final color = base_color × diffuse × dim + white × specular + white × rim
    // (specular and rim are additive white because they're light reflections,
    // not colored by the surface material)
    float totalSpec = spec * 0.5f + clearcoat + fillSpec * 0.15f;
    float sr = base.r * diffR * dimFactor + 255.0f * totalSpec + 255.0f * rim;
    float sg = base.g * diffG * dimFactor + 255.0f * totalSpec + 255.0f * rim;
    float sb = base.b * diffB * dimFactor + 255.0f * totalSpec + 255.0f * rim;

    // ── MATCAP ENVIRONMENT REFLECTION ──
    // Blend in the matcap color based on Fresnel — edges get more
    // reflection (physically correct for glass/glossy materials).
    Color mc = SampleMatCap(normal, viewDir);
    float envR = mc.r, envG = mc.g, envB = mc.b;
    float envMix = 0.12f + fresnel * 0.45f;  // 12% base + strong Fresnel-driven edge reflections
    sr = sr * (1.0f - envMix) + envR * envMix;  // linear interpolation
    sg = sg * (1.0f - envMix) + envG * envMix;
    sb = sb * (1.0f - envMix) + envB * envMix;

    if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;

    // ── ALPHA (TRANSPARENCY) ──
    // Glass-like IOR behavior: center of face is mostly transparent
    // (low DICE_ALPHA), but edges become nearly opaque due to Fresnel.
    // This mimics how real glass is see-through head-on but mirror-like
    // at grazing angles (total internal reflection).
    unsigned char alpha = (unsigned char)(DICE_ALPHA + (255 - DICE_ALPHA) * fresnel * 0.85f);

    return {(unsigned char)sr, (unsigned char)sg, (unsigned char)sb, alpha};
}

// Draw all faces of a die with per-vertex Gouraud shading.
//
// GOURAUD SHADING explained:
//   Instead of giving each face one flat color, we compute a different
//   color at each vertex (using LitVertex above), then tell TinyGL to
//   smoothly interpolate between them across the triangle.  This makes
//   curved surfaces look smooth and gives nice color gradients.
//
// VERTEX NORMAL AVERAGING:
//   Each vertex is shared by multiple faces.  To get smooth shading,
//   we average the normals of all faces that share a vertex.  This
//   "smooths out" the hard edges between faces.
void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    // Transform all vertices from local (die) space to world space.
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    Color base = d.baseColor;

    // Pre-compute per-vertex averaged normals for smooth shading.
    // For each face, compute its face normal (cross product of two edges),
    // then add it to every vertex that belongs to that face.
    // After all faces, normalize each vertex normal to unit length.
    // This "averages" the surrounding face normals at shared vertices.
    Vector3 vertNormals[MAX_DIE_VERTS] = {};
    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];
        Vector3 fn = FaceNormal(wv, face);
        for (int j = 0; j < face.count; j++) {
            int vi = face.idx[j];
            vertNormals[vi].x += fn.x;
            vertNormals[vi].y += fn.y;
            vertNormals[vi].z += fn.z;
        }
    }
    for (int i = 0; i < d.numVerts; i++)
        vertNormals[i] = fast_normalize(vertNormals[i]);

    // Pre-compute per-vertex lit colors
    Color vertColors[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        vertColors[i] = LitVertex(wv[i], vertNormals[i], base, camPos, 1.0f);

    // GL_SMOOTH tells TinyGL to interpolate colors between vertices
    // (Gouraud shading).  Without this, every pixel in a triangle would
    // have the same color (flat shading).
    glShadeModel(GL_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Draw all faces as triangles.  Polygons with >3 vertices are
    // "fan-triangulated": vertex 0 connects to every consecutive pair.
    //   e.g., quad {0,1,2,3} → triangles {0,1,2} and {0,2,3}
    rlBegin(RL_TRIANGLES);
    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];
        float dimFactor = (face.value >= 0) ? 1.0f : 0.6f;

        for (int j = 1; j < face.count - 1; j++) {
            int i0 = face.idx[0], i1 = face.idx[j], i2 = face.idx[j+1];

            // Dim non-value faces
            Color c0 = vertColors[i0], c1 = vertColors[i1], c2 = vertColors[i2];
            if (dimFactor < 1.0f) {
                c0.r *= dimFactor; c0.g *= dimFactor; c0.b *= dimFactor;
                c1.r *= dimFactor; c1.g *= dimFactor; c1.b *= dimFactor;
                c2.r *= dimFactor; c2.g *= dimFactor; c2.b *= dimFactor;
            }

            rlColor4ub(c0.r, c0.g, c0.b, c0.a);
            rlVertex3f(wv[i0].x, wv[i0].y, wv[i0].z);
            rlColor4ub(c1.r, c1.g, c1.b, c1.a);
            rlVertex3f(wv[i1].x, wv[i1].y, wv[i1].z);
            rlColor4ub(c2.r, c2.g, c2.b, c2.a);
            rlVertex3f(wv[i2].x, wv[i2].y, wv[i2].z);
        }
    }
    rlEnd();

    glShadeModel(GL_FLAT);  // restore default for subsequent drawing
}

// Draw subtle white wireframe edges on the die for shape definition.
// These thin lines help the eye perceive the die's geometry, especially
// on faces that are similarly lit.  Lines are offset slightly toward the
// camera to prevent "Z-fighting" (flickering where the line and face
// occupy the exact same depth).
void DrawDieEdges(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    // Collect unique edges from faces
    struct Edge { int a, b; };
    Edge edges[128];
    int numEdges = 0;

    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];
        for (int i = 0; i < face.count; i++) {
            int a = face.idx[i], b = face.idx[(i + 1) % face.count];
            if (a > b) { int t = a; a = b; b = t; }
            bool dup = false;
            for (int e = 0; e < numEdges; e++)
                if (edges[e].a == a && edges[e].b == b) { dup = true; break; }
            if (!dup && numEdges < 128)
                edges[numEdges++] = {a, b};
        }
    }

    // Draw edges as thin lines offset slightly toward camera
    Vector3 center = {xform.m12, xform.m13, xform.m14};
    Vector3 toCamera = fast_normalize(Vector3Subtract(camPos, center));

    rlBegin(RL_LINES);
    for (int i = 0; i < numEdges; i++) {
        Vector3 a = Vector3Add(wv[edges[i].a], Vector3Scale(toCamera, 0.008f));
        Vector3 b = Vector3Add(wv[edges[i].b], Vector3Scale(toCamera, 0.008f));
        rlColor4ub(255, 255, 255, 40);
        rlVertex3f(a.x, a.y, a.z);
        rlVertex3f(b.x, b.y, b.z);
    }
    rlEnd();
}

// ═══════════════════════════════════════════════════════════════════
// Geometry-based bloom (specular glow halos)
// ═══════════════════════════════════════════════════════════════════
//
// WHAT IS BLOOM?
//
//   In real cameras, very bright light "bleeds" into surrounding pixels
//   due to lens imperfections, creating a soft glow around bright spots.
//   This is called BLOOM.  It makes highlights look more intense and
//   gives a cinematic feel.
//
// OUR APPROACH:
//
//   For each face with a strong specular highlight, we draw a slightly
//   enlarged, transparent copy of the face on top.  This creates a
//   glow halo around the highlight.  Only faces above a brightness
//   threshold (0.15) get bloom — otherwise we'd waste cycles on
//   invisible effects.
//
//   Gated behind enablePostProcess flag (off by default on MMF due to
//   low framerate).

void DrawDieBloom(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];
        Vector3 n = FaceNormal(wv, face);

        Vector3 faceCtr = {0, 0, 0};
        for (int j = 0; j < face.count; j++) {
            faceCtr.x += wv[face.idx[j]].x;
            faceCtr.y += wv[face.idx[j]].y;
            faceCtr.z += wv[face.idx[j]].z;
        }
        faceCtr.x /= face.count; faceCtr.y /= face.count; faceCtr.z /= face.count;

        Vector3 viewDir = fast_normalize(Vector3Subtract(camPos, faceCtr));
        Vector3 halfVec = fast_normalize(Vector3Add(LIGHT_KEY, viewDir));
        float specDot = Vector3DotProduct(n, halfVec);
        if (specDot < 0) specDot = 0;
        float spec = specDot;
        for (int p = 0; p < 4; p++) spec *= spec;

        if (spec < 0.15f) continue;

        float bloom = (spec - 0.15f) / 0.85f;
        unsigned char ba = (unsigned char)(bloom * 40.0f);

        Vector3 glow[MAX_FACE_VERTS];
        for (int j = 0; j < face.count; j++) {
            Vector3 dir = Vector3Subtract(wv[face.idx[j]], faceCtr);
            glow[j] = Vector3Add(faceCtr, Vector3Scale(dir, 1.3f));
            glow[j] = Vector3Add(glow[j], Vector3Scale(n, 0.01f));
        }

        Color gc = {255, 250, 240, ba};
        for (int j = 1; j < face.count - 1; j++)
            DrawTriangle3D(glow[0], glow[j], glow[j+1], gc);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Planar floor reflections (mirror-like hardwood)
// ═══════════════════════════════════════════════════════════════════
//
// HOW PLANAR REFLECTIONS WORK:
//
//   The simplest way to fake a reflective floor: draw the entire scene
//   a second time, but FLIPPED upside down (Y-axis mirrored).  This
//   creates a mirror image "below" the floor surface.
//
//   We draw the reflected dice with low alpha (transparency) so they
//   look like faint reflections on a glossy surface, not a second set
//   of dice.  Dice closer to the floor have stronger reflections
//   (just like in real life).
//
//   The Y-FLIP inverts the winding order of triangles (clockwise
//   becomes counter-clockwise), so we reverse the backface culling
//   check to compensate.

void DrawFloorReflections(Vector3 camPos) {
    if (numDice == 0) return;

    // Y-flip matrix to mirror dice below the floor plane (y=0)
    Matrix flipY = MatrixIdentity();
    flipY.m5 = -1.0f;

    // Mirror the camera position for correct face-culling perception
    Vector3 mirrorCam = camPos;
    mirrorCam.y = -mirrorCam.y;

    // Disable depth test so reflections draw on top of the floor
    rlDisableDepthTest();
    rlDisableDepthMask();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glShadeModel(GL_FLAT);

    // Sort back-to-front relative to mirrored camera
    int order[MAX_ACTIVE_DICE];
    for (int i = 0; i < numDice; i++) order[i] = i;
    for (int i = 0; i < numDice - 1; i++)
        for (int j = i + 1; j < numDice; j++) {
            Matrix a = GetDieTransform(dice[order[i]]);
            Matrix b = GetDieTransform(dice[order[j]]);
            float da = Vector3LengthSqr(Vector3Subtract({a.m12,a.m13,a.m14}, mirrorCam));
            float db = Vector3LengthSqr(Vector3Subtract({b.m12,b.m13,b.m14}, mirrorCam));
            if (da < db) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }

    for (int i = 0; i < numDice; i++) {
        int di = order[i];
        Matrix xf = GetDieTransform(dice[di]);
        Matrix reflXf = MatrixMultiply(xf, flipY);

        const DiceDef& def = DICE_DEFS[dice[di].typeIdx];
        int nf = dice[di].numFaces;

        // Draw reflected die faces with low alpha for subtle reflection
        rlBegin(RL_TRIANGLES);
        for (int f = 0; f < nf; f++) {
            int nv = dice[di].faces[f].count;
            Vector3 wv[MAX_FACE_VERTS];
            for (int v = 0; v < nv; v++) {
                wv[v] = Vector3Transform(dice[di].verts[dice[di].faces[f].idx[v]], reflXf);
            }
            // Face normal in reflected space
            Vector3 fn = fast_normalize(Vector3CrossProduct(
                Vector3Subtract(wv[1], wv[0]), Vector3Subtract(wv[2], wv[0])));
            Vector3 toEye = fast_normalize(Vector3Subtract(mirrorCam, wv[0]));
            if (Vector3DotProduct(fn, toEye) > 0) continue;  // flipped cull (Y-flip inverts winding)

            // Use the die's pastel color, heavily faded
            Color pastel = PASTEL2[dice[di].typeIdx % 8];
            // Simple lighting on reflected die
            float keyDot = Vector3DotProduct(fn, LIGHT_KEY);
            if (keyDot < 0) keyDot = 0;
            float lum = 0.4f + 0.4f * keyDot;
            unsigned char cr = (unsigned char)(pastel.r * lum > 255 ? 255 : pastel.r * lum);
            unsigned char cg = (unsigned char)(pastel.g * lum > 255 ? 255 : pastel.g * lum);
            unsigned char cb = (unsigned char)(pastel.b * lum > 255 ? 255 : pastel.b * lum);

            // Fade reflection based on distance from floor — closer dice = stronger reflection
            float dieY = xf.m13;  // original die Y position
            float fade = 1.0f / (1.0f + dieY * 1.5f);
            unsigned char alpha = (unsigned char)(120 * fade);  // visible glossy reflection

            rlColor4ub(cr, cg, cb, alpha);
            for (int j = 1; j < nv - 1; j++) {
                rlVertex3f(wv[0].x, wv[0].y, wv[0].z);
                rlVertex3f(wv[j].x, wv[j].y, wv[j].z);
                rlVertex3f(wv[j+1].x, wv[j+1].y, wv[j+1].z);
            }
        }
        rlEnd();
    }

    rlEnableDepthTest();
    rlEnableDepthMask();
    glDisable(GL_BLEND);
    glShadeModel(GL_SMOOTH);
}

// ═══════════════════════════════════════════════════════════════════
// Pre-baked floor texture with lighting
// ═══════════════════════════════════════════════════════════════════
//
// OFFLINE TEXTURE BAKING explained:
//
//   Instead of computing floor lighting every frame (expensive on MMF),
//   we "bake" it once at startup.  We take the raw hardwood diffuse
//   texture, apply bump mapping, specular highlights, and environment
//   lighting, then save the result as a single texture.  At runtime,
//   drawing the floor is just one textured quad — almost free!
//
//   This is a very common game optimization: pre-compute expensive
//   lighting offline, store the result, and draw it cheaply at runtime.
//
// SKY PROBES:
//
//   We extract the 6 brightest light directions from the skybox image
//   (splitting it into a 6×3 grid, picking the 2 brightest columns per
//   row).  These "probes" approximate the environment lighting without
//   the cost of full Spherical Harmonics evaluation at every texel.
//
// ═══════════════════════════════════════════════════════════════════

static Texture2D bakedFloorTex;  // the single pre-lit texture used at runtime

// Simplified irradiance probe: dominant light directions extracted from skybox.
// Each probe stores a direction (where the light comes from) and its RGB color.
#define NUM_SKY_PROBES 6
struct SkyProbe { Vector3 dir; float r, g, b; };
static SkyProbe skyProbes[NUM_SKY_PROBES];
static int numSkyProbes = 0;

// Build sky probes by analyzing the skybox image.
// Splits the skybox into a 6-column × 3-row grid, finds the 2 brightest
// columns per row (by luminance), and creates directional probes from them.
static void BuildSkyProbes(const char* skyboxPath) {
    Image sky = LoadImage(skyboxPath);
    if (sky.data == NULL) return;
    ImageFormat(&sky, PIXELFORMAT_UNCOMPRESSED_R8G8B8);

    int w = sky.width, h = sky.height;
    unsigned char* px = (unsigned char*)sky.data;

    const int COLS = 6, ROWS = 3;
    int probeIdx = 0;
    for (int gy = 0; gy < ROWS && probeIdx < NUM_SKY_PROBES; gy++) {
        float bestLum[2] = {0, 0};
        int bestCol[2] = {0, 1};
        for (int gx = 0; gx < COLS; gx++) {
            float sumR = 0, sumG = 0, sumB = 0;
            int count = 0;
            int x0 = gx * w / COLS, x1 = (gx + 1) * w / COLS;
            int y0 = gy * h / ROWS, y1 = (gy + 1) * h / ROWS;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    int i = (y * w + x) * 3;
                    sumR += px[i]; sumG += px[i+1]; sumB += px[i+2];
                    count++;
                }
            float lum = (sumR * 0.299f + sumG * 0.587f + sumB * 0.114f) / count;
            for (int k = 0; k < 2; k++) {
                if (lum > bestLum[k]) {
                    if (k == 0) { bestLum[1] = bestLum[0]; bestCol[1] = bestCol[0]; }
                    bestLum[k] = lum; bestCol[k] = gx;
                    break;
                }
            }
        }
        for (int k = 0; k < 2 && probeIdx < NUM_SKY_PROBES; k++) {
            int gx = bestCol[k];
            float u = ((float)gx + 0.5f) / COLS;
            float v = ((float)gy + 0.5f) / ROWS;
            float azimuth = u * 2.0f * PI;
            float elevation = (0.5f - v) * PI;
            Vector3 dir = {
                cosf(elevation) * sinf(azimuth),
                sinf(elevation),
                cosf(elevation) * cosf(azimuth),
            };
            int x0 = gx * w / COLS, x1 = (gx + 1) * w / COLS;
            int y0 = gy * h / ROWS, y1 = (gy + 1) * h / ROWS;
            float sumR = 0, sumG = 0, sumB = 0;
            int count = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    int i = (y * w + x) * 3;
                    sumR += px[i]; sumG += px[i+1]; sumB += px[i+2];
                    count++;
                }
            skyProbes[probeIdx].dir = Vector3Normalize(dir);
            skyProbes[probeIdx].r = (sumR / count) / 255.0f;
            skyProbes[probeIdx].g = (sumG / count) / 255.0f;
            skyProbes[probeIdx].b = (sumB / count) / 255.0f;
            probeIdx++;
        }
    }
    UnloadImage(sky);
    numSkyProbes = probeIdx;
    TraceLog(LOG_INFO, "SKY: Built %d irradiance probes from skybox", probeIdx);
}

// Sample a grayscale image at UV coordinates, returning 0.0 to 1.0.
// UVs are "tiled" (wrapped): e.g., u=1.5 is treated as u=0.5.
// Used for bump map and roughness map sampling.
static float SampleGray(Image* img, float u, float v) {
    if (img->data == NULL) return 0.5f;
    u = u - floorf(u); v = v - floorf(v);
    int px = (int)(u * img->width) % img->width;
    int py = (int)(v * img->height) % img->height;
    if (px < 0) px += img->width;
    if (py < 0) py += img->height;
    return ((unsigned char*)img->data)[py * img->width + px] / 255.0f;
}

// Compute a bump-mapped surface normal from a height/bump map.
//
// BUMP MAPPING explained:
//   A bump map is a grayscale image where brightness = "height".
//   We sample 3 nearby points and compute finite differences (slopes)
//   to figure out which way the surface tilts at this point.
//   This tilted normal makes flat geometry appear to have 3D detail
//   (bumps, grooves, cracks) when lit.
//
//   The "scale" parameter controls how pronounced the bumps appear.
//   Higher values = more dramatic bumps.
static Vector3 BumpNormal(Image* bumpImg, float bumpMin, float bumpRange,
                          float u, float v, float texelSize) {
    // Sample 3 heights: center, +U, +V (finite difference method)
    float hc = (SampleGray(bumpImg, u, v)         - bumpMin) / bumpRange;
    float hx = (SampleGray(bumpImg, u + texelSize, v) - bumpMin) / bumpRange;
    float hy = (SampleGray(bumpImg, u, v + texelSize)  - bumpMin) / bumpRange;
    // Tangent-space normal: slopes in U and V become X and Z tilts.
    // Y=1.0 keeps the normal mostly pointing "up" (the floor's base direction).
    float scale = 2.5f;  // bump strength
    Vector3 n = {-(hx - hc) * scale, 1.0f, -(hy - hc) * scale};
    return Vector3Normalize(n);
}

// Bake a fully-lit floor texture offline.
//
// For every texel, we:
//   1. Read the diffuse color (wood grain)
//   2. Compute a bump-perturbed surface normal
//   3. Read the roughness value (glossy vs matte)
//   4. Light the texel: ambient + directional (key/fill) + sky probes
//   5. Add specular highlights (Blinn-Phong, strength varies with glossiness)
//   6. Output the final lit color
//
// The result is a single RGBA texture that captures all lighting detail.
// At runtime, we just draw a textured quad — no per-pixel lighting needed.
static void BakeLitFloorTexture(Image diffuseImg, Image bumpImg, Image roughImg) {
    int W = diffuseImg.width, H = diffuseImg.height;

    // Normalize bump range for maximum contrast.
    // Find the darkest and brightest values in the bump map, then
    // remap all heights to 0..1 within that range.  This maximizes
    // the visible bump effect regardless of the original image's range.
    float bumpLo = 1.0f, bumpHi = 0.0f;
    if (bumpImg.data) {
        unsigned char* bp = (unsigned char*)bumpImg.data;
        for (int i = 0; i < bumpImg.width * bumpImg.height; i++) {
            float v = bp[i] / 255.0f;
            if (v < bumpLo) bumpLo = v;
            if (v > bumpHi) bumpHi = v;
        }
    }
    float bumpRange = (bumpHi > bumpLo) ? (bumpHi - bumpLo) : 1.0f;
    TraceLog(LOG_INFO, "BAKE: bump range %.3f-%.3f (span %.3f)", bumpLo, bumpHi, bumpRange);

    float texelSize = 1.0f / W;

    // Ensure diffuse is RGB
    Image diffRgb = ImageCopy(diffuseImg);
    ImageFormat(&diffRgb, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    unsigned char* diffPx = (unsigned char*)diffRgb.data;

    // Allocate output RGBA
    Image result = GenImageColor(W, H, BLACK);
    ImageFormat(&result, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    unsigned char* out = (unsigned char*)result.data;

    // A fixed "view direction" for specular — looking down at the floor
    Vector3 viewDir = Vector3Normalize((Vector3){0.0f, 1.0f, 0.3f});

    for (int py = 0; py < H; py++) {
        float v = (py + 0.5f) / H;
        for (int px = 0; px < W; px++) {
            float u = (px + 0.5f) / W;

            // Diffuse color
            int di = (py * W + px) * 3;
            float dr = diffPx[di] / 255.0f;
            float dg = diffPx[di+1] / 255.0f;
            float db = diffPx[di+2] / 255.0f;

            // Bump-perturbed normal
            Vector3 n = {0, 1, 0};
            if (bumpImg.data)
                n = BumpNormal((Image*)&bumpImg, bumpLo, bumpRange, u, v, texelSize);

            // Roughness (low = glossy, high = matte)
            float roughness = 0.7f;
            if (roughImg.data)
                roughness = SampleGray((Image*)&roughImg, u, v);
            float glossiness = 1.0f - roughness;

            // Diffuse lighting: ambient + directional lights
            float ambient = 0.25f;
            float keyDot = Vector3DotProduct(n, LIGHT_KEY);
            if (keyDot < 0) keyDot = 0;
            float fillDot = Vector3DotProduct(n, LIGHT_FILL);
            if (fillDot < 0) fillDot = 0;
            float lr = ambient + 0.50f * keyDot + 0.15f * fillDot;
            float lg = lr, lb = lr;

            // Specular (Blinn-Phong) from key light
            Vector3 halfVec = Vector3Normalize(Vector3Add(LIGHT_KEY, viewDir));
            float specDot = Vector3DotProduct(n, halfVec);
            if (specDot < 0) specDot = 0;
            // Sharper exponent for glossy areas, wider for rough
            float specPow = 8.0f + glossiness * 48.0f;
            float spec = powf(specDot, specPow) * glossiness * 0.6f;

            // Sky probe irradiance + specular
            for (int p = 0; p < numSkyProbes; p++) {
                float d = Vector3DotProduct(n, skyProbes[p].dir);
                if (d < 0) d = 0;
                float w = d * 0.25f;
                lr += skyProbes[p].r * w;
                lg += skyProbes[p].g * w;
                lb += skyProbes[p].b * w;

                // Per-probe specular
                Vector3 hv = Vector3Normalize(Vector3Add(skyProbes[p].dir, viewDir));
                float sd = Vector3DotProduct(n, hv);
                if (sd > 0) {
                    float ps = powf(sd, specPow) * glossiness * 0.15f;
                    float lum = skyProbes[p].r * 0.3f + skyProbes[p].g * 0.6f + skyProbes[p].b * 0.1f;
                    spec += ps * lum;
                }
            }

            // Combine: diffuse * lighting + additive specular
            float fr = dr * lr + spec;
            float fg = dg * lg + spec;
            float fb = db * lb + spec;

            if (fr > 1.0f) fr = 1.0f;
            if (fg > 1.0f) fg = 1.0f;
            if (fb > 1.0f) fb = 1.0f;

            int oi = (py * W + px) * 4;
            out[oi]   = (unsigned char)(fr * 255);
            out[oi+1] = (unsigned char)(fg * 255);
            out[oi+2] = (unsigned char)(fb * 255);
            out[oi+3] = 255;
        }
    }

    bakedFloorTex = LoadTextureFromImage(result);
    UnloadImage(result);
    UnloadImage(diffRgb);
    TraceLog(LOG_INFO, "BAKE: Lit floor texture %dx%d ready", W, H);
}

// Master initialization: load all floor materials, MatCap, SH, and bake.
// Called once at startup.  Loads:
//   - MatCap texture (for dice environment reflections)
//   - Skybox → SH coefficients (for dice diffuse IBL)
//   - Skybox → sky probes (for floor environment lighting)
//   - Hardwood diffuse, bump, and roughness maps (for floor baking)
// Falls back to a solid brown color if diffuse texture is missing.
void InitWoodTexture() {
    LoadMatCap("matcap.png");
    ProjectSkyboxToSH("skybox.png");
    BuildSkyProbes("skybox.png");

    Image diffuse = LoadImage("hardwood2_diffuse.png");
    if (diffuse.data == NULL) {
        diffuse = GenImageColor(64, 64, (Color){140, 100, 58, 255});
        TraceLog(LOG_WARNING, "FLOOR: hardwood2_diffuse.png not found, using fallback");
    }
    Image bump = LoadImage("hardwood2_bump.png");
    if (bump.data) ImageFormat(&bump, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
    Image rough = LoadImage("hardwood2_roughness.png");
    if (rough.data) ImageFormat(&rough, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);

    BakeLitFloorTexture(diffuse, bump, rough);

    UnloadImage(diffuse);
    if (bump.data) UnloadImage(bump);
    if (rough.data) UnloadImage(rough);
}

// Draw the floor as a simple textured quad.
// All lighting is pre-baked into bakedFloorTex — we just need to draw
// two triangles with texture coordinates.  tileRepeat controls how many
// times the texture repeats across the floor (higher = smaller planks).
void DrawTexturedGround(float halfSize, float tileRepeat) {
    // Simple textured quad — all lighting is pre-baked into the texture
    rlSetTexture(bakedFloorTex.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 255);

    rlTexCoord2f(0, 0);            rlVertex3f(-halfSize, 0, -halfSize);
    rlTexCoord2f(tileRepeat, 0);   rlVertex3f( halfSize, 0, -halfSize);
    rlTexCoord2f(tileRepeat, tileRepeat); rlVertex3f( halfSize, 0,  halfSize);

    rlTexCoord2f(0, 0);            rlVertex3f(-halfSize, 0, -halfSize);
    rlTexCoord2f(tileRepeat, tileRepeat); rlVertex3f( halfSize, 0,  halfSize);
    rlTexCoord2f(0, tileRepeat);   rlVertex3f(-halfSize, 0,  halfSize);

    rlEnd();
    rlSetTexture(0);
}

// ═══════════════════════════════════════════════════════════════════
// Skybox (cylindrical panoramic background)
// ═══════════════════════════════════════════════════════════════════
//
// WHAT IS A SKYBOX?
//
//   A skybox is a large shape that surrounds the entire scene, textured
//   with an environment image so it looks like a real background (sky,
//   studio, landscape).  The camera sits inside it.
//
// OUR APPROACH:
//
//   We use a CYLINDER (not a cube) because our skybox image is a
//   360° equirectangular panorama — it maps naturally onto a cylinder.
//   64 vertical segments approximate the circle.  The cylinder follows
//   the camera position so you can never "walk to the edge."
//
//   TILING: TinyGL forces all textures to 256×256.  A single 256-wide
//   texture for 360° gives only ~0.7 px/degree — very coarse.  Instead,
//   we slice the panorama into SKYBOX_TILES horizontal strips at load
//   time.  Each strip becomes its own 256×256 texture covering only
//   360°/TILES degrees, giving TILES× higher effective resolution.
//
//   Depth writing is disabled so the skybox is always behind everything.
// ═══════════════════════════════════════════════════════════════════

#define SKYBOX_TILES 8   // 8 tiles × 256px = ~2048 effective horizontal texels

static Texture2D skyboxTiles[SKYBOX_TILES];
static int skyboxTileCount = 0;
static bool skyboxLoaded = false;

// Load the skybox panorama and split into horizontal tiles.
// Each tile covers 360°/SKYBOX_TILES of the panorama.
void InitSkybox() {
    Image img = LoadImage("skybox.png");
    if (img.data == NULL) return;

    // Ensure uniform pixel format for slicing
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int w = img.width, h = img.height;
    int tileW = w / SKYBOX_TILES;

    for (int t = 0; t < SKYBOX_TILES; t++) {
        // Extract a vertical strip from the panorama
        Image tile = GenImageColor(tileW, h, BLANK);
        ImageFormat(&tile, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

        unsigned char* srcPx = (unsigned char*)img.data;
        unsigned char* dstPx = (unsigned char*)tile.data;
        int srcX = t * tileW;

        for (int y = 0; y < h; y++) {
            memcpy(dstPx + y * tileW * 4,
                   srcPx + (y * w + srcX) * 4,
                   tileW * 4);
        }

        // TinyGL will resize this tile to 256×256, but now each tile
        // covers only 1/SKYBOX_TILES of the panorama — much sharper.
        skyboxTiles[t] = LoadTextureFromImage(tile);
        UnloadImage(tile);
    }
    skyboxTileCount = SKYBOX_TILES;
    skyboxLoaded = true;
    UnloadImage(img);
    TraceLog(LOG_INFO, "SKYBOX: Loaded %dx%d panorama as %d tiles (%d px/tile → 256×256)",
             w, h, SKYBOX_TILES, tileW);
}

// Draw the skybox cylinder centered on the camera.
// Each tile texture covers 1/SKYBOX_TILES of the 360° panorama.
// Within each tile, segments subdivide the arc for smooth curvature.
// UV coordinates map 0→1 across each tile's angular range.
void DrawSkybox(Vector3 camPos) {
    if (!skyboxLoaded) return;

    rlDisableDepthMask();

    const int SEGS_PER_TILE = 8;  // 8 segments × 8 tiles = 64 total
    const float radius = 50.0f;
    const float halfH  = 30.0f;

    for (int t = 0; t < skyboxTileCount; t++) {
        rlSetTexture(skyboxTiles[t].id);
        rlBegin(RL_TRIANGLES);
        rlColor4ub(255, 255, 255, 255);

        // Angular range for this tile
        float tileStart = (float)t / skyboxTileCount;
        float tileEnd   = (float)(t + 1) / skyboxTileCount;

        for (int i = 0; i < SEGS_PER_TILE; i++) {
            // Fraction within this tile [0, 1]
            float frac0 = (float)i / SEGS_PER_TILE;
            float frac1 = (float)(i + 1) / SEGS_PER_TILE;

            // Global angle
            float a0 = (tileStart + frac0 * (tileEnd - tileStart)) * 2.0f * PI;
            float a1 = (tileStart + frac1 * (tileEnd - tileStart)) * 2.0f * PI;

            // UV within this tile's texture [0, 1]
            float u0 = frac0;
            float u1 = frac1;

            float x0 = camPos.x + cosf(a0) * radius;
            float z0 = camPos.z + sinf(a0) * radius;
            float x1 = camPos.x + cosf(a1) * radius;
            float z1 = camPos.z + sinf(a1) * radius;
            float yTop = camPos.y + halfH;
            float yBot = camPos.y - halfH;

            rlTexCoord2f(u0, 0); rlVertex3f(x0, yTop, z0);
            rlTexCoord2f(u1, 0); rlVertex3f(x1, yTop, z1);
            rlTexCoord2f(u1, 1); rlVertex3f(x1, yBot, z1);

            rlTexCoord2f(u0, 0); rlVertex3f(x0, yTop, z0);
            rlTexCoord2f(u1, 1); rlVertex3f(x1, yBot, z1);
            rlTexCoord2f(u0, 1); rlVertex3f(x0, yBot, z0);
        }

        rlEnd();
        rlSetTexture(0);
    }

    rlEnableDepthMask();
}

// ═══════════════════════════════════════════════════════════════════
// Number atlas + face decals
// ═══════════════════════════════════════════════════════════════════
//
// TEXTURE ATLAS explained:
//
//   Drawing each number as a separate texture would require many
//   texture switches (slow).  Instead, we pack ALL numbers (0–20) into
//   one big texture called an ATLAS.  Each number occupies a grid cell.
//   When we need number "7", we compute its UV rectangle in the atlas.
//
//   Layout: 4 columns × 6 rows in a 256×256 texture.
//   Each cell is 64×42 pixels.
//
// FACE DECALS:
//
//   To show numbers on dice faces, we draw a tiny textured quad
//   (2 triangles) floating just above (+0.005) each face.  The quad
//   is aligned to the face using its tangent vectors (computed from
//   the first edge + cross product with the face normal).
//   Only front-facing faces (dot > 0.1 toward camera) get decals,
//   since back-facing numbers would be invisible anyway.
// ═══════════════════════════════════════════════════════════════════

#define ATLAS_COLS 4
#define ATLAS_ROWS 6
#define ATLAS_CELL_W 64
#define ATLAS_CELL_H 42
#define ATLAS_SIZE 256

static Texture2D numberAtlas;

// Generate the number atlas at startup.
// Renders numbers 0–20 into a grid using raylib's CPU text rendering.
// Each number is drawn 3 times with slight offsets for a bold/shadow effect.
// Multi-digit numbers use per-character rendering with fixed spacing to
// prevent cramped layouts (e.g. "12" where '1' is narrow).
void InitNumberAtlas() {
    Image img = GenImageColor(ATLAS_SIZE, ATLAS_SIZE, BLANK);
    for (int n = 0; n <= 20; n++) {
        int col = n % ATLAS_COLS, row = n / ATLAS_COLS;
        int cx = col * ATLAS_CELL_W, cy = row * ATLAS_CELL_H;
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", n);
        int fontSize = 28;
        int numChars = (int)strlen(buf);
        Color numCol = {30, 20, 40, 255};

        if (numChars == 1) {
            // Single digit: center normally
            int tw = MeasureText(buf, fontSize);
            int x0 = cx + (ATLAS_CELL_W - tw) / 2;
            int y0 = cy + (ATLAS_CELL_H - fontSize) / 2;
            ImageDrawText(&img, buf, x0 + 1, y0, fontSize, numCol);
            ImageDrawText(&img, buf, x0, y0 + 1, fontSize, numCol);
            ImageDrawText(&img, buf, x0, y0, fontSize, numCol);
        } else {
            // Multi-digit: render each character with fixed-width spacing
            // to prevent "12" looking cramped
            int charWidth = MeasureText("8", fontSize);  // widest digit
            int spacing = charWidth + 2;  // gap between characters
            int totalW = spacing * numChars - 2;
            int x0 = cx + (ATLAS_CELL_W - totalW) / 2;
            int y0 = cy + (ATLAS_CELL_H - fontSize) / 2;
            for (int c = 0; c < numChars; c++) {
                char ch[2] = {buf[c], '\0'};
                int cw = MeasureText(ch, fontSize);
                int charX = x0 + c * spacing + (charWidth - cw) / 2;
                ImageDrawText(&img, ch, charX + 1, y0, fontSize, numCol);
                ImageDrawText(&img, ch, charX, y0 + 1, fontSize, numCol);
                ImageDrawText(&img, ch, charX, y0, fontSize, numCol);
            }
        }
    }
    numberAtlas = LoadTextureFromImage(img);
    UnloadImage(img);
}

// Look up UV coordinates for a given number in the atlas grid.
static void GetNumberUV(int value, float* u0, float* v0, float* u1, float* v1) {
    int col = value % ATLAS_COLS, row = value / ATLAS_COLS;
    *u0 = (float)(col * ATLAS_CELL_W) / ATLAS_SIZE;
    *v0 = (float)(row * ATLAS_CELL_H) / ATLAS_SIZE;
    *u1 = (float)((col + 1) * ATLAS_CELL_W) / ATLAS_SIZE;
    *v1 = (float)((row + 1) * ATLAS_CELL_H) / ATLAS_SIZE;
}

// Draw number decals on all visible faces of a die.
// For each face with a valid number (value >= 0):
//   1. Compute face center and normal (in world space)
//   2. Backface cull: skip if facing away from camera
//   3. Build a tangent frame (t1, t2) aligned to the face
//   4. Place a small quad slightly above the face surface
//   5. Draw 2 textured triangles with atlas UVs for the number
void DrawDieNumberDecals(const ActiveDie& d, const Matrix& xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    for (int f = 0; f < d.numFaces; f++) {
        if (d.faces[f].value < 0) continue;
        const Face& face = d.faces[f];

        Vector3 center = {0, 0, 0};
        for (int i = 0; i < face.count; i++)
            center = Vector3Add(center, wv[face.idx[i]]);
        center = Vector3Scale(center, 1.0f / face.count);

        Vector3 n = FaceNormal(wv, face);
        Vector3 toCamera = fast_normalize(Vector3Subtract(camPos, center));
        if (Vector3DotProduct(n, toCamera) < 0.1f) continue;

        Vector3 edge = Vector3Subtract(wv[face.idx[1]], wv[face.idx[0]]);
        Vector3 t1 = fast_normalize(edge);
        Vector3 t2 = fast_normalize(Vector3CrossProduct(n, t1));

        float minEdge = 1e9f;
        for (int i = 0; i < face.count; i++) {
            Vector3 e = Vector3Subtract(wv[face.idx[(i+1) % face.count]], wv[face.idx[i]]);
            float len = Vector3Length(e);
            if (len < minEdge) minEdge = len;
        }
        float halfSize = minEdge * 0.35f;

        Vector3 off = Vector3Scale(n, 0.005f);
        Vector3 qCenter = Vector3Add(center, off);

        Vector3 q0 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1, -halfSize)), Vector3Scale(t2,  halfSize));
        Vector3 q1 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1,  halfSize)), Vector3Scale(t2,  halfSize));
        Vector3 q2 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1,  halfSize)), Vector3Scale(t2, -halfSize));
        Vector3 q3 = Vector3Add(Vector3Add(qCenter, Vector3Scale(t1, -halfSize)), Vector3Scale(t2, -halfSize));

        float u0, v0, u1, v1;
        GetNumberUV(face.value, &u0, &v0, &u1, &v1);

        rlSetTexture(numberAtlas.id);
        rlBegin(RL_TRIANGLES);
            rlColor4ub(255, 255, 255, 255);
            rlTexCoord2f(u0, v0); rlVertex3f(q0.x, q0.y, q0.z);
            rlTexCoord2f(u1, v0); rlVertex3f(q1.x, q1.y, q1.z);
            rlTexCoord2f(u1, v1); rlVertex3f(q2.x, q2.y, q2.z);
            rlTexCoord2f(u0, v0); rlVertex3f(q0.x, q0.y, q0.z);
            rlTexCoord2f(u1, v1); rlVertex3f(q2.x, q2.y, q2.z);
            rlTexCoord2f(u0, v1); rlVertex3f(q3.x, q3.y, q3.z);
        rlEnd();
        rlSetTexture(0);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Face overlay system (shared by scratch + dirt overlays)
// ═══════════════════════════════════════════════════════════════════
//
// Both scratch and dirt overlays share the same drawing logic:
//   1. Transform vertices to world space
//   2. For each front-facing face: compute center, normal, tangent frame
//   3. Project per-vertex UVs onto tangent frame
//   4. Fan-triangulate and draw textured triangles
//
// The only differences are:
//   - Texture and blend mode (additive for scratches, alpha for dirt)
//   - Normal offset height (scratches on top of dirt)
//   - UV hash seeds (so patterns don't align)
//   - Per-face color: scratches modulate by lighting, dirt is white
//
// This helper avoids duplicating ~60 lines between the two overlays.
// ═══════════════════════════════════════════════════════════════════

struct FaceOverlayParams {
    Texture2D tex;
    float normalOffset;       // distance above face surface
    int hashSeed1, hashSeed2; // per-face UV offset variety
    bool lightModulate;       // if true, darken faces away from key light
};

// UV scale: how many world units per full texture tile.
// Consistent across all overlays so scratch/dirt textures tile at the same density.
static const float OVERLAY_UV_SCALE = 1.0f / (DIE_RADIUS * 1.4f);

static void DrawFaceOverlay(const ActiveDie& d, const Matrix& xform,
                            Vector3 camPos, const FaceOverlayParams& p) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    for (int f = 0; f < d.numFaces; f++) {
        const Face& face = d.faces[f];

        // Face centroid (average of all vertices)
        Vector3 center = {0, 0, 0};
        for (int i = 0; i < face.count; i++)
            center = Vector3Add(center, wv[face.idx[i]]);
        center = Vector3Scale(center, 1.0f / face.count);

        // Backface cull: skip faces pointing away from camera
        Vector3 n = FaceNormal(wv, face);
        Vector3 toCamera = fast_normalize(Vector3Subtract(camPos, center));
        if (Vector3DotProduct(n, toCamera) < 0.1f) continue;

        // Tangent frame for UV projection (from first edge + normal)
        Vector3 edge = Vector3Subtract(wv[face.idx[1]], wv[face.idx[0]]);
        Vector3 t1 = fast_normalize(edge);
        Vector3 t2 = fast_normalize(Vector3CrossProduct(n, t1));

        // Per-face UV offset: hash the face index for variety
        float uOff = (float)(f * p.hashSeed1 % 8) / 8.0f;
        float vOff = (float)(f * p.hashSeed2 % 8) / 8.0f;

        // Per-face vertex color
        unsigned char r = 255, g = 255, b = 255, a = 255;
        if (p.lightModulate) {
            float lightDot = Vector3DotProduct(n, LIGHT_KEY);
            if (lightDot < 0) lightDot = 0;
            unsigned char intensity = (unsigned char)(40 + 160 * lightDot);
            r = g = b = intensity;
        }

        // Offset above face surface to prevent z-fighting
        Vector3 off = Vector3Scale(n, p.normalOffset);

        // Project each vertex onto the tangent frame for UVs
        float vertU[MAX_FACE_VERTS], vertV[MAX_FACE_VERTS];
        Vector3 vertPos[MAX_FACE_VERTS];
        for (int i = 0; i < face.count; i++) {
            Vector3 pos = Vector3Add(wv[face.idx[i]], off);
            vertPos[i] = pos;
            Vector3 rel = Vector3Subtract(pos, center);
            vertU[i] = Vector3DotProduct(rel, t1) * OVERLAY_UV_SCALE + uOff;
            vertV[i] = Vector3DotProduct(rel, t2) * OVERLAY_UV_SCALE + vOff;
        }

        // Fan-triangulate from vertex 0 (same winding as DrawDieFacesLit)
        rlSetTexture(p.tex.id);
        rlBegin(RL_TRIANGLES);
        rlColor4ub(r, g, b, a);
        for (int j = 1; j < face.count - 1; j++) {
            rlTexCoord2f(vertU[0], vertV[0]);
            rlVertex3f(vertPos[0].x, vertPos[0].y, vertPos[0].z);
            rlTexCoord2f(vertU[j], vertV[j]);
            rlVertex3f(vertPos[j].x, vertPos[j].y, vertPos[j].z);
            rlTexCoord2f(vertU[j+1], vertV[j+1]);
            rlVertex3f(vertPos[j+1].x, vertPos[j+1].y, vertPos[j+1].z);
        }
        rlEnd();
        rlSetTexture(0);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Scratch overlay (per-pixel surface detail via texture mapping)
// ═══════════════════════════════════════════════════════════════════
//
// PROBLEM:
//   Our Gouraud shading computes lighting per-VERTEX, so fine surface
//   details like scratches would need thousands of extra vertices to
//   look right.  With only ~20 vertices per die, per-vertex scratches
//   (like our procedural clearcoat hash) look very coarse.
//
// SOLUTION:
//   Load a "scratch normal map" (from the THREE.js Scratched_gold
//   texture), convert it at startup into a "scratch highlight" texture
//   (white where scratches are, transparent elsewhere), and draw it as
//   a textured overlay per face — just like number decals but using
//   the shared DrawFaceOverlay() helper.
//
//   TinyGL handles per-pixel texture sampling, so this gives us
//   per-pixel scratch detail at minimal extra CPU cost.  We use
//   ADDITIVE blending (GL_SRC_ALPHA, GL_ONE) so the scratches
//   brighten the underlying surface, simulating specular micro-detail.
//
// WHY NOT TEXTURE THE FACES DIRECTLY?
//   DrawDieFacesLit uses untextured Gouraud triangles (per-vertex
//   colors only).  Adding texturing there would change the entire
//   rasterizer variant (from Smooth to MappingPerspective), which is
//   slower.  A separate overlay pass is cleaner and keeps the fast
//   path for the main faces.
// ═══════════════════════════════════════════════════════════════════

static Texture2D scratchTex;
static bool scratchLoaded = false;

// Convert a tangent-space normal map into a scratch visibility texture.
//
// NORMAL MAP explained:
//   A normal map stores surface normal perturbations as RGB colors.
//   - R = X deviation (128 = flat, 0/-1 = left, 255/+1 = right)
//   - G = Y deviation (128 = flat)
//   - B = Z component (255 = pointing straight out)
//
//   Scratches appear as pixels where R and G deviate from 128 — the
//   surface normal is tilted.  We extract the magnitude of that tilt
//   and use it as alpha in a white RGBA texture: bright alpha = scratch,
//   zero alpha = smooth surface.
void InitScratchTexture() {
    Image img = LoadImage("scratches.png");
    if (img.data == NULL) return;

    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    unsigned char* px = (unsigned char*)img.data;
    int count = img.width * img.height;

    for (int i = 0; i < count; i++) {
        float dx = px[i*4 + 0] / 255.0f - 0.5f;
        float dy = px[i*4 + 1] / 255.0f - 0.5f;
        float strength = sqrtf(dx*dx + dy*dy) * 4.0f;
        if (strength > 1.0f) strength = 1.0f;

        px[i*4 + 0] = 255;
        px[i*4 + 1] = 255;
        px[i*4 + 2] = 255;
        px[i*4 + 3] = (unsigned char)(strength * 180);
    }

    scratchTex = LoadTextureFromImage(img);
    UnloadImage(img);
    scratchLoaded = true;
    TraceLog(LOG_INFO, "SCRATCH: Loaded scratch overlay texture");
}

// Draw scratch highlights on all visible faces of a die.
// Uses additive blending so scratches brighten the surface.
// Light-modulated: scratches on lit faces appear brighter.
void DrawDieScratchOverlay(const ActiveDie& d, const Matrix& xform,
                           Vector3 camPos) {
    if (!scratchLoaded) return;
    rlSetBlendMode(RL_BLEND_ADDITIVE);
    DrawFaceOverlay(d, xform, camPos, {
        scratchTex, 0.003f, 37, 53, true
    });
    rlSetBlendMode(RL_BLEND_ALPHA);
}

// ═══════════════════════════════════════════════════════════════════
// Dirt/bump overlay (per-pixel surface roughness via texture mapping)
// ═══════════════════════════════════════════════════════════════════
//
// Similar concept to scratches, but from a "packed dirt" normal map
// (opengameart.org CC0).  Instead of sharp linear scratches, this
// creates broad bumps and dents that make the dice look worn and
// tactile — like they've been handled many times.
//
// CONVERSION TECHNIQUE:
//   For each pixel in the dirt normal map, we compute how a bump at
//   that point would interact with a fixed overhead light:
//     - Bumps facing the light → white pixel (highlight)
//     - Crevices facing away  → black pixel (shadow)
//     - Alpha = deviation magnitude (how bumpy this pixel is)
//
//   Drawn with standard alpha blending, white pixels brighten the
//   surface and black pixels darken it — creating both highlights
//   AND shadows from a single overlay pass.
// ═══════════════════════════════════════════════════════════════════

static Texture2D dirtTex;
static bool dirtLoaded = false;

void InitDirtTexture() {
    Image img = LoadImage("dirt_bump.png");
    if (img.data == NULL) return;

    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    unsigned char* px = (unsigned char*)img.data;
    int count = img.width * img.height;

    // Fixed light direction for baking bump highlights/shadows.
    // Points from upper-right — similar to LIGHT_KEY but in tangent space.
    float lx = 0.4f, ly = 0.3f, lz = 0.86f;
    float flatDot = lz;  // NdotL for a perfectly flat surface

    for (int i = 0; i < count; i++) {
        float nx = px[i*4 + 0] / 255.0f * 2.0f - 1.0f;
        float ny = px[i*4 + 1] / 255.0f * 2.0f - 1.0f;
        float nz = px[i*4 + 2] / 255.0f;
        float invLen = fast_rsqrtf(nx*nx + ny*ny + nz*nz + 1e-6f);
        nx *= invLen; ny *= invLen; nz *= invLen;

        float NdotL = nx * lx + ny * ly + nz * lz;
        float delta = (NdotL - flatDot) * 5.0f;

        if (delta > 0) {
            float a = delta; if (a > 1.0f) a = 1.0f;
            px[i*4+0] = 255; px[i*4+1] = 255; px[i*4+2] = 255;
            px[i*4 + 3] = (unsigned char)(a * 180);
        } else {
            float a = -delta; if (a > 1.0f) a = 1.0f;
            px[i*4+0] = 0; px[i*4+1] = 0; px[i*4+2] = 0;
            px[i*4 + 3] = (unsigned char)(a * 130);
        }
    }

    dirtTex = LoadTextureFromImage(img);
    UnloadImage(img);
    dirtLoaded = true;
    TraceLog(LOG_INFO, "DIRT: Loaded dirt bump overlay texture");
}

// Draw dirt bump highlights/shadows on all visible faces.
// Uses standard alpha blending: white brightens, black darkens.
void DrawDieDirtOverlay(const ActiveDie& d, const Matrix& xform,
                        Vector3 camPos) {
    if (!dirtLoaded) return;
    DrawFaceOverlay(d, xform, camPos, {
        dirtTex, 0.002f, 61, 43, false
    });
}

// ═══════════════════════════════════════════════════════════════════
// Screen-space bloom post-processing (via TinyGL glPostProcess)
// ═══════════════════════════════════════════════════════════════════
//
// SCREEN-SPACE POST-PROCESSING explained:
//
//   After the entire scene is rendered, we can modify the final image
//   pixel-by-pixel.  TinyGL provides glPostProcess() which calls a
//   callback for every pixel with its color and depth.
//
// OUR EFFECTS:
//
//   1. BLOOM: Pixels above 80% brightness get a subtle boost (up to
//      1.4×).  This simulates camera lens bloom — bright highlights
//      "glow."  (Note: true bloom would blur the glow outward, but
//      per-pixel-only processing can't do spatial blur.)
//
//   2. DEPTH FOG: Distant pixels (z > 60% of depth range) fade toward
//      a warm haze color (160, 155, 150).  Max 15% blend — very subtle,
//      just adds a hint of atmosphere/distance.
//
// PIXEL FORMAT: TinyGL packs 32-bit color as 0x00RRGGBB.
//               z is 16-bit depth: 0 = far, 0xFFFF = near.
// ═══════════════════════════════════════════════════════════════════

// Per-pixel post-processing callback.
// TinyGL 32-bit pixel format: 0x00RRGGBB, z is 16-bit depth
static GLuint BloomPostProcessCallback(GLint x, GLint y, GLuint pixel, GLushort z) {
    unsigned int r = (pixel >> 16) & 0xFF;
    unsigned int g = (pixel >> 8)  & 0xFF;
    unsigned int b =  pixel        & 0xFF;

    // Luminance (perceptual)
    float lum = (r * 0.299f + g * 0.587f + b * 0.114f) / 255.0f;

    // Only boost pixels above brightness threshold
    if (lum > 0.80f) {
        float excess = (lum - 0.80f) / 0.20f;  // 0..1 above threshold
        float boost = 1.0f + excess * 0.40f;    // max 1.4x boost
        r = (unsigned int)(r * boost); if (r > 255) r = 255;
        g = (unsigned int)(g * boost); if (g > 255) g = 255;
        b = (unsigned int)(b * boost); if (b > 255) b = 255;
    }

    // Depth fog: distant objects fade toward a warm haze (very subtle)
    // z=0 is far, z=0xFFFF is near in TinyGL's 16-bit Z-buffer
    float depth = 1.0f - (float)z / 65535.0f;  // 0=near, 1=far
    if (depth > 0.6f) {
        float fogAmount = (depth - 0.6f) / 0.4f;
        if (fogAmount > 1.0f) fogAmount = 1.0f;
        fogAmount *= 0.15f;  // max 15% fog — just a hint of atmosphere
        r = (unsigned int)(r * (1.0f - fogAmount) + 160 * fogAmount);
        g = (unsigned int)(g * (1.0f - fogAmount) + 155 * fogAmount);
        b = (unsigned int)(b * (1.0f - fogAmount) + 150 * fogAmount);
    }


    return (r << 16) | (g << 8) | b;
}

bool enablePostProcess = false;  // off by default — bloom is blocky on MMF

// Apply bloom + fog to the entire framebuffer.
// Only runs if enablePostProcess is true (toggle with gamepad).
void ApplyBloomPostProcess() {
    if (!enablePostProcess) return;
    glPostProcess(BloomPostProcessCallback);
}

// ═══════════════════════════════════════════════════════════════════
// UI (2D overlay — drawn after all 3D content)
// ═══════════════════════════════════════════════════════════════════
//
// The UI is drawn in 2D screen space (pixel coordinates) on top of
// the 3D scene.  It includes:
//   - A "hotbar" at the bottom showing all dice types and their counts
//   - Control hints below the hotbar
//   - Result text (dice values) at the top (drawn in main.cpp)
// ═══════════════════════════════════════════════════════════════════

// Draw text with a subtle bold/shadow effect.
// We draw the same text 3 times with 1-pixel offsets — cheap "bold."
void DrawTextBold(const char* text, int x, int y, int sz, Color col) {
    DrawText(text, x+1, y, sz, col);
    DrawText(text, x, y+1, sz, col);
    DrawText(text, x, y, sz, col);
}

// Draw the dice selection hotbar at the bottom of the screen.
// Shows all 6 dice types (d4, d6, d8, d10, d12, d20) as cells.
// The selected cell is highlighted with a yellow border.
// Cells showing "d6 x3" mean 3 d6 dice are queued to throw.
void DrawHotbar() {
    const int cellW = 110, cellH = 38;
    const int totalW = NUM_DICE_TYPES * cellW;
    const int x0 = (SCR_W - totalW) / 2;
    const int y0 = SCR_H - cellH - 8;

    for (int i = 0; i < NUM_DICE_TYPES; i++) {
        int cx = x0 + i * cellW;
        bool sel = (i == hotbarSel);

        Color bg = sel ? (Color){60, 70, 90, 255} : (Color){35, 40, 50, 255};
        DrawRectangle(cx + 1, y0 + 1, cellW - 2, cellH - 2, bg);

        if (sel)
            DrawRectangleLines(cx, y0, cellW, cellH, (Color){255, 220, 50, 255});

        const char* name = DICE_DEFS[i].name;
        int cnt = hotbarCount[i];
        Color txtCol = sel ? (Color){255, 255, 255, 255} : (Color){180, 180, 180, 255};

        if (cnt > 0) {
            const char* label = TextFormat("%s x%d", name, cnt);
            int tw = MeasureText(label, 16);
            DrawText(label, cx + (cellW - tw)/2, y0 + 10, 16, txtCol);
        } else {
            int tw = MeasureText(name, 16);
            DrawText(name, cx + (cellW - tw)/2, y0 + 10, 16,
                     sel ? (Color){140, 140, 140, 255} : (Color){80, 80, 80, 255});
        }
    }

    const char* hint = "D-pad:Sel  A:Throw  SELECT/START:Help";
    int hw = MeasureText(hint, 12);
    DrawText(hint, (SCR_W - hw)/2, y0 - 16, 12, (Color){120, 120, 120, 255});
}

// Draw a semi-transparent overlay showing all button mappings.
// Toggled by tapping SELECT or START (when not used as modifier).
void DrawHelpOverlay() {
    // Dark semi-transparent backdrop
    DrawRectangle(0, 0, SCR_W, SCR_H, (Color){0, 0, 0, 180});

    int y = 60;
    const int lx = 100;   // left column
    const int rx = 420;   // right column
    const int sz = 16;
    const int gap = 24;
    Color title = {255, 220, 50, 255};
    Color body  = {220, 220, 220, 255};
    Color dim   = {150, 150, 150, 255};

    DrawTextBold("Controls", (SCR_W - MeasureText("Controls", 28)) / 2, y, 28, title);
    y += 44;

    DrawTextBold("--- Default ---", lx, y, sz, title);
    y += gap;
    DrawText("D-pad L/R    Cycle dice type",    lx, y, sz, body); y += gap;
    DrawText("D-pad Up     Add die",            lx, y, sz, body); y += gap;
    DrawText("D-pad Down   Remove die",         lx, y, sz, body); y += gap;
    DrawText("A            Throw dice",         lx, y, sz, body); y += gap;
    DrawText("L / R        Rotate camera",      lx, y, sz, body); y += gap;
    DrawText("L2 / R2      Zoom in/out",        lx, y, sz, body); y += gap;
    DrawText("Y / X        Tilt camera",        lx, y, sz, body); y += gap;

    y = 60 + 44;
    DrawTextBold("--- SELECT + ---", rx, y, sz, title);
    y += gap;
    DrawText("D-pad L/R    Pan left/right",     rx, y, sz, body); y += gap;
    DrawText("D-pad U/D    Pan up/down",        rx, y, sz, body); y += gap;
    DrawText("Y / X        Pan fwd/back",       rx, y, sz, body); y += gap;
    y += gap;
    DrawTextBold("--- START + ---", rx, y, sz, title);
    y += gap;
    DrawText("D-pad        Look around",        rx, y, sz, body); y += gap;
    DrawText("Y / X        Look up/down",       rx, y, sz, body); y += gap;

    DrawText("Press SELECT or START to close", (SCR_W - MeasureText("Press SELECT or START to close", 14)) / 2,
             SCR_H - 50, 14, dim);
}

// Free all GPU textures and CPU allocations.
// Called during shutdown to prevent resource leaks.
void UnloadRenderingTextures() {
    UnloadTexture(numberAtlas);
    if (skyboxLoaded) {
        for (int i = 0; i < skyboxTileCount; i++)
            UnloadTexture(skyboxTiles[i]);
    }
    if (scratchLoaded) UnloadTexture(scratchTex);
    if (dirtLoaded) UnloadTexture(dirtTex);
    UnloadTexture(bakedFloorTex);
    if (matcapPixels) { RL_FREE(matcapPixels); matcapPixels = NULL; }
}
