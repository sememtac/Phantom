#pragma once
#include <math.h>

// Minimal 3-vector / 3x3-matrix maths. Everything is float: the S3 has a
// single-precision FPU, so fixed-point would cost clarity for no speed.

struct Vec3 { float x, y, z; };

static inline Vec3  v3(float x, float y, float z)  { Vec3 v = {x, y, z}; return v; }
static inline Vec3  vadd(Vec3 a, Vec3 b)           { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3  vsub(Vec3 a, Vec3 b)           { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3  vmul(Vec3 a, float s)          { return v3(a.x * s, a.y * s, a.z * s); }
static inline float vdot(Vec3 a, Vec3 b)           { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float vlen2(Vec3 a)                  { return vdot(a, a); }
static inline float vlen(Vec3 a)                   { return sqrtf(vdot(a, a)); }

static inline Vec3 vcross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

static inline Vec3 vnorm(Vec3 a) {
    float l = vlen(a);
    return (l > 1e-6f) ? vmul(a, 1.0f / l) : v3(0, 0, 1);
}

// Row-major 3x3.
struct Mat3 { float m[9]; };

static inline Vec3 mat3_apply(const Mat3& M, Vec3 v) {
    return v3(M.m[0] * v.x + M.m[1] * v.y + M.m[2] * v.z,
              M.m[3] * v.x + M.m[4] * v.y + M.m[5] * v.z,
              M.m[6] * v.x + M.m[7] * v.y + M.m[8] * v.z);
}

// A * B, i.e. apply B then A. Needed to accumulate an orientation frame by
// frame instead of integrating angles, which does not compose.
static inline Mat3 mat3_mul(const Mat3& A, const Mat3& B) {
    Mat3 M;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            M.m[r * 3 + c] = A.m[r * 3 + 0] * B.m[0 + c]
                           + A.m[r * 3 + 1] * B.m[3 + c]
                           + A.m[r * 3 + 2] * B.m[6 + c];
    return M;
}

static inline Mat3 mat3_identity(void) {
    Mat3 M = {{ 1,0,0, 0,1,0, 0,0,1 }};
    return M;
}

// Composite rotation applied in the order X, then Y, then Z (M = Rz*Ry*Rx).
static inline Mat3 mat3_euler(float rx, float ry, float rz) {
    float sx = sinf(rx), cx = cosf(rx);
    float sy = sinf(ry), cy = cosf(ry);
    float sz = sinf(rz), cz = cosf(rz);

    Mat3 M;
    M.m[0] =  cz * cy;
    M.m[1] =  cz * sy * sx - sz * cx;
    M.m[2] =  cz * sy * cx + sz * sx;
    M.m[3] =  sz * cy;
    M.m[4] =  sz * sy * sx + cz * cx;
    M.m[5] =  sz * sy * cx - cz * sx;
    M.m[6] = -sy;
    M.m[7] =  cy * sx;
    M.m[8] =  cy * cx;
    return M;
}
