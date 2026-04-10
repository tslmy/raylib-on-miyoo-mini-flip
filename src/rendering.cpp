#include "rendering.h"
#include "physics.h"
#include "rlgl.h"
#include <GL/gl.h>
#include <cmath>
#include <cstdio>

// ═══════════════════════════════════════════════════════════════════
// Lighting
// ═══════════════════════════════════════════════════════════════════

const Vector3 LIGHT_KEY  = Vector3Normalize((Vector3){0.4f, 0.8f, 0.5f});
const Vector3 LIGHT_FILL = Vector3Normalize((Vector3){-0.5f, 0.3f, -0.3f});

// ═══════════════════════════════════════════════════════════════════
// Projected outline shadows
// ═══════════════════════════════════════════════════════════════════

void DrawProjectedShadow(const ActiveDie& d, Matrix xform) {
    const float groundY = 0.002f;
    const float PENUMBRA = 0.18f;

    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    Vector2 proj[MAX_DIE_VERTS];
    int nProj = 0;
    for (int i = 0; i < d.numVerts; i++) {
        if (LIGHT_KEY.y <= 0.001f) continue;
        float t = wv[i].y / LIGHT_KEY.y;
        proj[nProj++] = {wv[i].x - LIGHT_KEY.x * t,
                         wv[i].z - LIGHT_KEY.z * t};
    }
    if (nProj < 3) return;

    // 2D convex hull (Andrew's monotone chain)
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

    float cx = 0, cz = 0;
    for (int i = 0; i < hullCount; i++) { cx += hull[i].x; cz += hull[i].y; }
    cx /= hullCount; cz /= hullCount;

    float height = xform.m13;
    if (height < 0) height = 0;
    float intensity = 1.0f - height / 8.0f;
    if (intensity < 0) return;
    unsigned char innerAlpha = (unsigned char)(intensity * 140);
    Color innerCol = {0, 0, 0, innerAlpha};

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
// PBR-inspired dice face lighting
// ═══════════════════════════════════════════════════════════════════

void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    Color base = d.baseColor;

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

        // Procedural bump (packeddirt normal map)
        {
            unsigned int seed = (unsigned int)(faceCtr.x * 1000) * 7919u
                              + (unsigned int)(faceCtr.z * 1000) * 104729u
                              + (unsigned int)(f * 31337u);
            float bx = ((seed & 0xFF) / 255.0f - 0.5f) * 0.08f;
            float bz = (((seed >> 8) & 0xFF) / 255.0f - 0.5f) * 0.08f;
            n.x += bx; n.z += bz;
            float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
            if (len > 0.001f) { n.x /= len; n.y /= len; n.z /= len; }
        }

        Vector3 viewDir = Vector3Normalize(Vector3Subtract(camPos, faceCtr));

        // Diffuse
        float keyDot  = Vector3DotProduct(n, LIGHT_KEY);
        if (keyDot < 0) keyDot = 0;
        float fillDot = Vector3DotProduct(n, LIGHT_FILL);
        if (fillDot < 0) fillDot = 0;
        float diffuse = 0.50f + 0.35f * keyDot + 0.15f * fillDot;

        // Blinn-Phong specular (pow-16)
        Vector3 halfVec = Vector3Normalize(Vector3Add(LIGHT_KEY, viewDir));
        float specDot = Vector3DotProduct(n, halfVec);
        if (specDot < 0) specDot = 0;
        float spec = specDot;
        for (int p = 0; p < 4; p++) spec *= spec;

        // Clearcoat with scratch normal (pow-32)
        Vector3 ccN = n;
        {
            unsigned int s2 = (unsigned int)(faceCtr.x * 5000) * 48271u
                            + (unsigned int)(faceCtr.z * 5000) * 16807u
                            + (unsigned int)(f * 65537u);
            float sx = ((s2 & 0xFF) / 255.0f - 0.5f) * 0.15f;
            float sz = (((s2 >> 8) & 0xFF) / 255.0f - 0.5f) * 0.05f;
            ccN.x += sx; ccN.z += sz;
            float len = sqrtf(ccN.x*ccN.x + ccN.y*ccN.y + ccN.z*ccN.z);
            if (len > 0.001f) { ccN.x /= len; ccN.y /= len; ccN.z /= len; }
        }
        float ccSpecDot = Vector3DotProduct(ccN, halfVec);
        if (ccSpecDot < 0) ccSpecDot = 0;
        float ccSpec = ccSpecDot;
        for (int p = 0; p < 5; p++) ccSpec *= ccSpec;
        float clearcoat = 0.5f * ccSpec;

        // Fill specular (pow-4)
        Vector3 halfFill = Vector3Normalize(Vector3Add(LIGHT_FILL, viewDir));
        float fillSpec = Vector3DotProduct(n, halfFill);
        if (fillSpec < 0) fillSpec = 0;
        fillSpec = fillSpec * fillSpec * fillSpec * fillSpec;

        // Fresnel rim
        float NdotV = Vector3DotProduct(n, viewDir);
        if (NdotV < 0) NdotV = 0;
        float fresnel = 0.04f + 0.96f * powf(1.0f - NdotV, 5.0f);
        float rim = fresnel * 0.35f;

        // Compose
        float dimFactor = (face.value >= 0) ? 1.0f : 0.6f;
        float totalSpec = spec * 0.5f + clearcoat + fillSpec * 0.15f;

        float sr = base.r * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;
        float sg = base.g * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;
        float sb = base.b * diffuse * dimFactor + 255.0f * totalSpec + 255.0f * rim;

        float envUp = n.y * 0.5f + 0.5f;
        float envR = 140.0f * envUp + 100.0f * (1.0f - envUp);
        float envG = 150.0f * envUp +  80.0f * (1.0f - envUp);
        float envB = 170.0f * envUp +  60.0f * (1.0f - envUp);
        sr = sr * 0.88f + envR * 0.12f;
        sg = sg * 0.88f + envG * 0.12f;
        sb = sb * 0.88f + envB * 0.12f;

        if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;

        Color col = {(unsigned char)sr, (unsigned char)sg, (unsigned char)sb,
                     (unsigned char)DICE_ALPHA};

        for (int j = 1; j < face.count - 1; j++)
            DrawTriangle3D(wv[face.idx[0]], wv[face.idx[j]], wv[face.idx[j+1]], col);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Geometry-based bloom
// ═══════════════════════════════════════════════════════════════════

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

        Vector3 viewDir = Vector3Normalize(Vector3Subtract(camPos, faceCtr));
        Vector3 halfVec = Vector3Normalize(Vector3Add(LIGHT_KEY, viewDir));
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
// Hardwood floor texture + bump displacement + skybox-derived IBL
// ═══════════════════════════════════════════════════════════════════

static Texture2D woodTexture;
static Image bumpImage;         // kept in CPU memory for per-vertex sampling
static bool bumpLoaded = false;
static float bumpMin = 0, bumpScale = 1;  // for normalizing to full 0..1 range

// Simplified irradiance probe: dominant light directions extracted from skybox
#define NUM_SKY_PROBES 6
struct SkyProbe { Vector3 dir; float r, g, b; };
static SkyProbe skyProbes[NUM_SKY_PROBES];
static bool skyProbesReady = false;

// Extract dominant light directions from the skybox texture.
// Scans the equirectangular image for bright regions and converts
// pixel positions to 3D directions — a simplified version of the
// HDR environment lighting in the Three.js version.
static void BuildSkyProbes(const char* skyboxPath) {
    Image sky = LoadImage(skyboxPath);
    if (sky.data == NULL) return;
    ImageFormat(&sky, PIXELFORMAT_UNCOMPRESSED_R8G8B8);

    int w = sky.width, h = sky.height;
    unsigned char* px = (unsigned char*)sky.data;

    // Divide into a 6×3 grid, find the brightest cell in each row-band
    const int COLS = 6, ROWS = 3;
    int probeIdx = 0;
    for (int gy = 0; gy < ROWS && probeIdx < NUM_SKY_PROBES; gy++) {
        // Find the two brightest columns in this row band
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
            float avgR = sumR / count, avgG = sumG / count, avgB = sumB / count;
            float lum = avgR * 0.299f + avgG * 0.587f + avgB * 0.114f;
            for (int k = 0; k < 2; k++) {
                if (lum > bestLum[k]) {
                    // Shift down
                    if (k == 0) { bestLum[1] = bestLum[0]; bestCol[1] = bestCol[0]; }
                    bestLum[k] = lum;
                    bestCol[k] = gx;
                    break;
                }
            }
        }

        for (int k = 0; k < 2 && probeIdx < NUM_SKY_PROBES; k++) {
            int gx = bestCol[k];
            // Cell center in equirectangular coords
            float u = ((float)gx + 0.5f) / COLS;
            float v = ((float)gy + 0.5f) / ROWS;
            // Convert to spherical: u→azimuth, v→elevation
            float azimuth = u * 2.0f * PI;
            float elevation = (0.5f - v) * PI;  // top=+PI/2, bottom=-PI/2
            Vector3 dir = {
                cosf(elevation) * sinf(azimuth),
                sinf(elevation),
                cosf(elevation) * cosf(azimuth),
            };

            // Average color of this cell (normalized to 0..1, boosted)
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
    skyProbesReady = true;
    TraceLog(LOG_INFO, "SKY: Built %d irradiance probes from skybox", probeIdx);
}

void InitWoodTexture() {
    Image img = LoadImage("hardwood2_diffuse.png");
    if (img.data == NULL) {
        img = GenImageColor(64, 64, (Color){140, 100, 58, 255});
        TraceLog(LOG_WARNING, "FLOOR: hardwood2_diffuse.png not found, using fallback");
    }
    woodTexture = LoadTextureFromImage(img);
    UnloadImage(img);

    bumpImage = LoadImage("hardwood2_bump.png");
    if (bumpImage.data != NULL) {
        ImageFormat(&bumpImage, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
        bumpLoaded = true;
        // Find actual min/max to normalize the narrow 183-255 range to full 0..1
        unsigned char* bp = (unsigned char*)bumpImage.data;
        int total = bumpImage.width * bumpImage.height;
        unsigned char lo = 255, hi = 0;
        for (int i = 0; i < total; i++) {
            if (bp[i] < lo) lo = bp[i];
            if (bp[i] > hi) hi = bp[i];
        }
        bumpMin = lo / 255.0f;
        bumpScale = (hi > lo) ? 1.0f / ((hi - lo) / 255.0f) : 1.0f;
        TraceLog(LOG_INFO, "BUMP: range %d-%d, normalized scale=%.2f", lo, hi, bumpScale);
    }

    BuildSkyProbes("skybox.png");
}

// Sample bump map at a UV coordinate (tiled), returning normalized 0..1 height
static float SampleBump(float u, float v) {
    if (!bumpLoaded) return 0.0f;
    u = u - floorf(u);
    v = v - floorf(v);
    int px = (int)(u * bumpImage.width) % bumpImage.width;
    int py = (int)(v * bumpImage.height) % bumpImage.height;
    if (px < 0) px += bumpImage.width;
    if (py < 0) py += bumpImage.height;
    unsigned char* pixels = (unsigned char*)bumpImage.data;
    float raw = pixels[py * bumpImage.width + px] / 255.0f;
    return (raw - bumpMin) * bumpScale;  // full 0..1 range
}

static float FloorBumpHeight(float x, float z, float halfSize, float tileRepeat) {
    float u = (x + halfSize) / (2.0f * halfSize) * tileRepeat;
    float v = (z + halfSize) / (2.0f * halfSize) * tileRepeat;
    return SampleBump(u, v) * 0.035f;  // stronger displacement
}

static Vector3 FloorBumpNormal(float x, float z, float halfSize, float tileRepeat) {
    const float eps = 0.02f;  // tighter sampling for sharper gradients
    float hc = FloorBumpHeight(x, z, halfSize, tileRepeat);
    float hx = FloorBumpHeight(x + eps, z, halfSize, tileRepeat);
    float hz = FloorBumpHeight(x, z + eps, halfSize, tileRepeat);
    Vector3 n = {-(hx - hc) / eps, 1.0f, -(hz - hc) / eps};
    return Vector3Normalize(n);
}

void DrawTexturedGround(float halfSize, float tileRepeat) {
    const int SUBDIV = 40;
    float step = 2.0f * halfSize / SUBDIV;
    float uvStep = tileRepeat / SUBDIV;

    rlSetTexture(woodTexture.id);
    rlBegin(RL_TRIANGLES);

    for (int zi = 0; zi < SUBDIV; zi++) {
        for (int xi = 0; xi < SUBDIV; xi++) {
            float x0 = -halfSize + xi * step;
            float z0 = -halfSize + zi * step;
            float x1 = x0 + step;
            float z1 = z0 + step;
            float u0 = xi * uvStep, u1 = (xi + 1) * uvStep;
            float v0 = zi * uvStep, v1 = (zi + 1) * uvStep;

            float y00 = FloorBumpHeight(x0, z0, halfSize, tileRepeat);
            float y10 = FloorBumpHeight(x1, z0, halfSize, tileRepeat);
            float y01 = FloorBumpHeight(x0, z1, halfSize, tileRepeat);
            float y11 = FloorBumpHeight(x1, z1, halfSize, tileRepeat);

            float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
            Vector3 n = FloorBumpNormal(cx, cz, halfSize, tileRepeat);

            // Lighting: low ambient + strong directional for visible bump contrast
            float ambient = 0.30f;
            float keyDot = Vector3DotProduct(n, LIGHT_KEY);
            if (keyDot < 0) keyDot = 0;
            float fillDot = Vector3DotProduct(n, LIGHT_FILL);
            if (fillDot < 0) fillDot = 0;
            float lr = ambient + 0.45f * keyDot + 0.15f * fillDot;
            float lg = lr, lb = lr;

            // Skybox-derived irradiance: add colored light from dominant sky directions
            if (skyProbesReady) {
                for (int p = 0; p < NUM_SKY_PROBES; p++) {
                    float d = Vector3DotProduct(n, skyProbes[p].dir);
                    if (d < 0) d = 0;
                    float w = d * 0.25f;  // weight per probe
                    lr += skyProbes[p].r * w;
                    lg += skyProbes[p].g * w;
                    lb += skyProbes[p].b * w;
                }
            }

            if (lr > 1.2f) lr = 1.2f;
            if (lg > 1.2f) lg = 1.2f;
            if (lb > 1.2f) lb = 1.2f;

            unsigned char cr = (unsigned char)(lr * 212 > 255 ? 255 : lr * 212);
            unsigned char cg = (unsigned char)(lg * 212 > 255 ? 255 : lg * 212);
            unsigned char cb = (unsigned char)(lb * 212 > 255 ? 255 : lb * 212);

            rlColor4ub(cr, cg, cb, 255);

            rlTexCoord2f(u0, v0); rlVertex3f(x0, y00, z0);
            rlTexCoord2f(u1, v0); rlVertex3f(x1, y10, z0);
            rlTexCoord2f(u1, v1); rlVertex3f(x1, y11, z1);

            rlTexCoord2f(u0, v0); rlVertex3f(x0, y00, z0);
            rlTexCoord2f(u1, v1); rlVertex3f(x1, y11, z1);
            rlTexCoord2f(u0, v1); rlVertex3f(x0, y01, z1);
        }
    }

    rlEnd();
    rlSetTexture(0);
}

// ═══════════════════════════════════════════════════════════════════
// Skybox
// ═══════════════════════════════════════════════════════════════════

static Texture2D skyboxTex;
static bool skyboxLoaded = false;

void InitSkybox() {
    Image img = LoadImage("skybox.png");
    if (img.data != NULL) {
        skyboxTex = LoadTextureFromImage(img);
        UnloadImage(img);
        skyboxLoaded = true;
    }
}

void DrawSkybox(Vector3 camPos) {
    if (!skyboxLoaded) return;

    rlDisableDepthMask();
    rlSetTexture(skyboxTex.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(255, 255, 255, 255);

    const int SEGS = 16;
    const float radius = 50.0f;
    const float halfH  = 30.0f;

    for (int i = 0; i < SEGS; i++) {
        float a0 = (float)i / SEGS * 2.0f * PI;
        float a1 = (float)(i + 1) / SEGS * 2.0f * PI;
        float u0 = (float)i / SEGS;
        float u1 = (float)(i + 1) / SEGS;

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
    rlEnableDepthMask();
}

// ═══════════════════════════════════════════════════════════════════
// Number atlas + decals
// ═══════════════════════════════════════════════════════════════════

#define ATLAS_COLS 4
#define ATLAS_ROWS 6
#define ATLAS_CELL_W 64
#define ATLAS_CELL_H 42
#define ATLAS_SIZE 256

static Texture2D numberAtlas;

void InitNumberAtlas() {
    Image img = GenImageColor(ATLAS_SIZE, ATLAS_SIZE, BLANK);
    for (int n = 0; n <= 20; n++) {
        int col = n % ATLAS_COLS, row = n / ATLAS_COLS;
        int cx = col * ATLAS_CELL_W, cy = row * ATLAS_CELL_H;
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", n);
        int fontSize = 28;
        int tw = MeasureText(buf, fontSize);
        Color numCol = {30, 20, 40, 255};
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2 + 1,
                      cy + (ATLAS_CELL_H - fontSize)/2, fontSize, numCol);
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2,
                      cy + (ATLAS_CELL_H - fontSize)/2 + 1, fontSize, numCol);
        ImageDrawText(&img, buf, cx + (ATLAS_CELL_W - tw)/2,
                      cy + (ATLAS_CELL_H - fontSize)/2, fontSize, numCol);
    }
    numberAtlas = LoadTextureFromImage(img);
    UnloadImage(img);
}

static void GetNumberUV(int value, float* u0, float* v0, float* u1, float* v1) {
    int col = value % ATLAS_COLS, row = value / ATLAS_COLS;
    *u0 = (float)(col * ATLAS_CELL_W) / ATLAS_SIZE;
    *v0 = (float)(row * ATLAS_CELL_H) / ATLAS_SIZE;
    *u1 = (float)((col + 1) * ATLAS_CELL_W) / ATLAS_SIZE;
    *v1 = (float)((row + 1) * ATLAS_CELL_H) / ATLAS_SIZE;
}

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
        Vector3 toCamera = Vector3Normalize(Vector3Subtract(camPos, center));
        if (Vector3DotProduct(n, toCamera) < 0.1f) continue;

        Vector3 edge = Vector3Subtract(wv[face.idx[1]], wv[face.idx[0]]);
        Vector3 t1 = Vector3Normalize(edge);
        Vector3 t2 = Vector3Normalize(Vector3CrossProduct(n, t1));

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
// UI
// ═══════════════════════════════════════════════════════════════════

void DrawTextBold(const char* text, int x, int y, int sz, Color col) {
    DrawText(text, x+1, y, sz, col);
    DrawText(text, x, y+1, sz, col);
    DrawText(text, x, y, sz, col);
}

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

    const char* hint;
    if (riggedValue >= 0)
        hint = TextFormat("Y:+  X:-  L2/R2:Sel  A:Throw  B:Rig[%d]", riggedValue);
    else
        hint = "Y:+  X:-  L2/R2:Sel  A:Throw  B:Rig";
    int hw = MeasureText(hint, 12);
    DrawText(hint, (SCR_W - hw)/2, y0 - 16, 12,
             riggedValue >= 0 ? (Color){200, 100, 100, 255} : (Color){120, 120, 120, 255});
}

void UnloadRenderingTextures() {
    UnloadTexture(numberAtlas);
    if (skyboxLoaded) UnloadTexture(skyboxTex);
    UnloadTexture(woodTexture);
    if (bumpLoaded) UnloadImage(bumpImage);
}
