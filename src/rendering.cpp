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
// Planar floor reflections
// ═══════════════════════════════════════════════════════════════════

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
            Vector3 wv[4];
            for (int v = 0; v < nv; v++) {
                wv[v] = Vector3Transform(dice[di].verts[dice[di].faces[f].idx[v]], reflXf);
            }
            // Face normal in reflected space
            Vector3 fn = Vector3Normalize(Vector3CrossProduct(
                Vector3Subtract(wv[1], wv[0]), Vector3Subtract(wv[2], wv[0])));
            Vector3 toEye = Vector3Normalize(Vector3Subtract(mirrorCam, wv[0]));
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
            float fade = 1.0f / (1.0f + dieY * 2.0f);
            unsigned char alpha = (unsigned char)(70 * fade);  // subtle glossy reflection

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

static Texture2D bakedFloorTex;  // the single pre-lit texture used at runtime

// Simplified irradiance probe: dominant light directions extracted from skybox
#define NUM_SKY_PROBES 6
struct SkyProbe { Vector3 dir; float r, g, b; };
static SkyProbe skyProbes[NUM_SKY_PROBES];
static int numSkyProbes = 0;

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

// Sample a grayscale image at UV with tiling, returning 0..1
static float SampleGray(Image* img, float u, float v) {
    if (img->data == NULL) return 0.5f;
    u = u - floorf(u); v = v - floorf(v);
    int px = (int)(u * img->width) % img->width;
    int py = (int)(v * img->height) % img->height;
    if (px < 0) px += img->width;
    if (py < 0) py += img->height;
    return ((unsigned char*)img->data)[py * img->width + px] / 255.0f;
}

// Compute bump normal from finite-difference height sampling
static Vector3 BumpNormal(Image* bumpImg, float bumpMin, float bumpRange,
                          float u, float v, float texelSize) {
    // Sample 3 points for finite-difference normal
    float hc = (SampleGray(bumpImg, u, v)         - bumpMin) / bumpRange;
    float hx = (SampleGray(bumpImg, u + texelSize, v) - bumpMin) / bumpRange;
    float hy = (SampleGray(bumpImg, u, v + texelSize)  - bumpMin) / bumpRange;
    // Tangent-space normal: dh/du and dh/dv become X and Z tilts
    float scale = 2.5f;  // bump strength
    Vector3 n = {-(hx - hc) * scale, 1.0f, -(hy - hc) * scale};
    return Vector3Normalize(n);
}

// Bake a fully-lit floor texture: diffuse × (bump_lighting + specular + IBL)
// This runs once at init; at runtime we just draw a textured quad.
static void BakeLitFloorTexture(Image diffuseImg, Image bumpImg, Image roughImg) {
    int W = diffuseImg.width, H = diffuseImg.height;

    // Normalize bump range for maximum contrast
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

void InitWoodTexture() {
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
    UnloadTexture(bakedFloorTex);
}
