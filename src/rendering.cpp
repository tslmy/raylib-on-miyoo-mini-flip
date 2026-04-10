#include "rendering.h"
#include "physics.h"
#include "rlgl.h"
#include <GL/gl.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════
// Lighting
// ═══════════════════════════════════════════════════════════════════

const Vector3 LIGHT_KEY  = Vector3Normalize((Vector3){0.4f, 0.8f, 0.5f});
const Vector3 LIGHT_FILL = Vector3Normalize((Vector3){-0.5f, 0.3f, -0.3f});

// ═══════════════════════════════════════════════════════════════════
// Spherical Harmonics L2 (9 coefficients × RGB) for environment IBL
// ═══════════════════════════════════════════════════════════════════

static float shCoeffs[9][3] = {};  // [basis_index][r,g,b]
static bool  shReady = false;

static void ProjectSkyboxToSH(const char* path) {
    Image sky = LoadImage(path);
    if (sky.data == NULL) return;
    ImageFormat(&sky, PIXELFORMAT_UNCOMPRESSED_R8G8B8);

    int w = sky.width, h = sky.height;
    unsigned char* px = (unsigned char*)sky.data;

    double acc[9][3] = {};
    double weightSum = 0;

    for (int y = 0; y < h; y++) {
        float v = ((float)y + 0.5f) / h;
        float theta = v * PI;           // polar angle [0, π]
        float sinTheta = sinf(theta);
        float cosTheta = cosf(theta);
        float dOmega = sinTheta;        // solid angle weight

        for (int x = 0; x < w; x++) {
            float u = ((float)x + 0.5f) / w;
            float phi = u * 2.0f * PI;  // azimuthal [0, 2π]

            // Direction vector
            float dx = sinTheta * sinf(phi);
            float dy = cosTheta;
            float dz = sinTheta * cosf(phi);

            int i = (y * w + x) * 3;
            float r = px[i] / 255.0f;
            float g = px[i+1] / 255.0f;
            float b = px[i+2] / 255.0f;

            // SH L2 basis functions (real, orthonormal)
            float basis[9];
            basis[0] = 0.282095f;                          // Y00
            basis[1] = 0.488603f * dy;                     // Y1,0
            basis[2] = 0.488603f * dz;                     // Y1,1
            basis[3] = 0.488603f * dx;                     // Y1,-1
            basis[4] = 1.092548f * dx * dy;                // Y2,-1
            basis[5] = 1.092548f * dy * dz;                // Y2,0 (note: yz)
            basis[6] = 0.315392f * (3*dy*dy - 1);         // Y2,1
            basis[7] = 1.092548f * dx * dz;                // Y2,-2
            basis[8] = 0.546274f * (dx*dx - dz*dz);       // Y2,2

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

// Evaluate SH irradiance at a given normal direction → returns RGB [0,1]
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
// MatCap (CPU-side image for per-vertex environment lookup)
// ═══════════════════════════════════════════════════════════════════

static Color* matcapPixels = NULL;
static int    matcapSize   = 0;

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

// Sample matcap using view-space normal
static Color SampleMatCap(Vector3 normal, Vector3 viewDir) {
    if (!matcapPixels) return {180, 180, 190, 255};

    // Build view-space basis from viewDir
    Vector3 forward = {-viewDir.x, -viewDir.y, -viewDir.z};  // camera looks along -Z in view space
    Vector3 up = {0, 1, 0};
    Vector3 right = Vector3Normalize(Vector3CrossProduct(up, forward));
    up = Vector3CrossProduct(forward, right);

    // Project world normal into view space
    float nx = Vector3DotProduct(normal, right);
    float ny = Vector3DotProduct(normal, up);

    // Map to UV [0,1]
    float u = nx * 0.5f + 0.5f;
    float v = 0.5f - ny * 0.5f;  // flip Y for image coords

    int px = (int)(u * (matcapSize - 1));
    int py = (int)(v * (matcapSize - 1));
    if (px < 0) px = 0; if (px >= matcapSize) px = matcapSize - 1;
    if (py < 0) py = 0; if (py >= matcapSize) py = matcapSize - 1;

    return matcapPixels[py * matcapSize + px];
}

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

// Per-vertex lighting computation for Gouraud shading
static Color LitVertex(Vector3 pos, Vector3 normal, Color base, Vector3 camPos,
                       float dimFactor) {
    Vector3 viewDir = Vector3Normalize(Vector3Subtract(camPos, pos));

    // Diffuse: SH environment irradiance + directional lights
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

    // Blinn-Phong specular (pow-16) from key light
    Vector3 halfVec = Vector3Normalize(Vector3Add(LIGHT_KEY, viewDir));
    float specDot = Vector3DotProduct(normal, halfVec);
    if (specDot < 0) specDot = 0;
    float spec = specDot;
    for (int p = 0; p < 4; p++) spec *= spec;

    // Clearcoat specular (pow-32) with per-vertex procedural scratch
    Vector3 ccN = normal;
    {
        unsigned int s2 = (unsigned int)(pos.x * 5000) * 48271u
                        + (unsigned int)(pos.z * 5000) * 16807u
                        + (unsigned int)(pos.y * 3000) * 65537u;
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
    float fillSpec = Vector3DotProduct(normal, halfFill);
    if (fillSpec < 0) fillSpec = 0;
    fillSpec = fillSpec * fillSpec * fillSpec * fillSpec;

    // Fresnel rim glow
    float NdotV = Vector3DotProduct(normal, viewDir);
    if (NdotV < 0) NdotV = 0;
    float fresnel = 0.04f + 0.96f * powf(1.0f - NdotV, 5.0f);
    float rim = fresnel * 0.55f;

    // Compose: per-channel SH diffuse + specular + rim
    float totalSpec = spec * 0.5f + clearcoat + fillSpec * 0.15f;
    float sr = base.r * diffR * dimFactor + 255.0f * totalSpec + 255.0f * rim;
    float sg = base.g * diffG * dimFactor + 255.0f * totalSpec + 255.0f * rim;
    float sb = base.b * diffB * dimFactor + 255.0f * totalSpec + 255.0f * rim;

    // Environment reflection via MatCap (replaces crude up-down gradient)
    Color mc = SampleMatCap(normal, viewDir);
    float envR = mc.r, envG = mc.g, envB = mc.b;
    float envMix = 0.15f + fresnel * 0.25f;
    sr = sr * (1.0f - envMix) + envR * envMix;
    sg = sg * (1.0f - envMix) + envG * envMix;
    sb = sb * (1.0f - envMix) + envB * envMix;

    if (sr > 255) sr = 255; if (sg > 255) sg = 255; if (sb > 255) sb = 255;

    // Fresnel-based alpha: edges more opaque (glass refraction)
    unsigned char alpha = (unsigned char)(DICE_ALPHA + (255 - DICE_ALPHA) * fresnel * 0.6f);

    return {(unsigned char)sr, (unsigned char)sg, (unsigned char)sb, alpha};
}

void DrawDieFacesLit(const ActiveDie& d, Matrix xform, Vector3 camPos) {
    Vector3 wv[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        wv[i] = Vector3Transform(d.verts[i], xform);

    Color base = d.baseColor;

    // Pre-compute per-vertex averaged normals for smooth shading
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
        vertNormals[i] = Vector3Normalize(vertNormals[i]);

    // Pre-compute per-vertex lit colors
    Color vertColors[MAX_DIE_VERTS];
    for (int i = 0; i < d.numVerts; i++)
        vertColors[i] = LitVertex(wv[i], vertNormals[i], base, camPos, 1.0f);

    // GL_SMOOTH for Gouraud interpolation (now works with alpha!)
    glShadeModel(GL_SMOOTH);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

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

    glShadeModel(GL_FLAT);
}

// Draw subtle edge wireframe for dice shape definition
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
    Vector3 toCamera = Vector3Normalize(Vector3Subtract(camPos, center));

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
// Screen-space bloom post-processing (via TinyGL glPostProcess)
// ═══════════════════════════════════════════════════════════════════

// Bloom + depth fog post-processing
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

void ApplyBloomPostProcess() {
    glPostProcess(BloomPostProcessCallback);
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
    if (matcapPixels) { RL_FREE(matcapPixels); matcapPixels = NULL; }
}
