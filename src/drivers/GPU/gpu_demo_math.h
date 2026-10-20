// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef GPU_DEMO_MATH_H
#define GPU_DEMO_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define DEMO_PI 3.14159265358979f

static inline void dm_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static inline void dm_mul(float *r, const float *a, const float *b) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            t[c * 4 + rw] = a[0 * 4 + rw] * b[c * 4 + 0] + a[1 * 4 + rw] * b[c * 4 + 1] +
                            a[2 * 4 + rw] * b[c * 4 + 2] + a[3 * 4 + rw] * b[c * 4 + 3];
    memcpy(r, t, sizeof(t));
}

static inline void dm_rotate(float *m, float ang, float x, float y, float z) {
    float c = (float)cos((double)ang), s = (float)sin((double)ang), t = 1.0f - c;
    dm_identity(m);
    m[0] = t * x * x + c;
    m[1] = t * x * y + s * z;
    m[2] = t * x * z - s * y;
    m[4] = t * x * y - s * z;
    m[5] = t * y * y + c;
    m[6] = t * y * z + s * x;
    m[8] = t * x * z + s * y;
    m[9] = t * y * z - s * x;
    m[10] = t * z * z + c;
}

static inline void dm_translate(float *m, float x, float y, float z) {
    dm_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static inline void dm_perspective(float *m, float fovy, float aspect, float zn, float zf,
                                  int zero_to_one, int flip_y) {
    float f = (float)(1.0 / tan((double)fovy / 2.0));
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = flip_y ? -f : f;
    m[11] = -1.0f;
    if (zero_to_one) {
        m[10] = zf / (zn - zf);
        m[14] = zf * zn / (zn - zf);
    } else {
        m[10] = (zf + zn) / (zn - zf);
        m[14] = 2.0f * zf * zn / (zn - zf);
    }
}

#define DEMO_CUBE_VERTS 36
#define DEMO_CUBE_FLOATS (DEMO_CUBE_VERTS * 6)

static inline void dm_cube_build(float *out) {
    static const struct {
        int axis;
        float sign;
        float col[3];
    } faces[6] = {
        {2, -1.0f, {1.0f, 0.2f, 0.2f}},
        {2, +1.0f, {0.2f, 1.0f, 0.2f}},
        {0, -1.0f, {0.2f, 0.2f, 1.0f}},
        {0, +1.0f, {1.0f, 0.2f, 1.0f}},
        {1, -1.0f, {1.0f, 1.0f, 0.2f}},
        {1, +1.0f, {0.2f, 1.0f, 1.0f}},
    };
    static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    static const int tri[6] = {0, 1, 2, 0, 2, 3};
    float *p = out;
    for (int f = 0; f < 6; f++) {
        int u = (faces[f].axis + 1) % 3, v = (faces[f].axis + 2) % 3;
        for (int k = 0; k < 6; k++) {
            float pos[3];
            pos[faces[f].axis] = faces[f].sign;
            pos[u] = corner[tri[k]][0];
            pos[v] = corner[tri[k]][1];
            p[0] = pos[0];
            p[1] = pos[1];
            p[2] = pos[2];
            p[3] = faces[f].col[0];
            p[4] = faces[f].col[1];
            p[5] = faces[f].col[2];
            p += 6;
        }
    }
}

static inline void dm_cube_mvp(float *mvp, float t, uint32_t w, uint32_t h, int zero_to_one) {
    float rx[16], ry[16], rz[16], tr[16], mv[16], proj[16];
    dm_rotate(rx, t * 0.9f, 1.0f, 0.0f, 0.0f);
    dm_rotate(ry, t * 1.3f, 0.0f, 1.0f, 0.0f);
    dm_rotate(rz, t * 0.7f, 0.0f, 0.0f, 1.0f);
    dm_translate(tr, 0.0f, 0.0f, -6.0f);
    dm_mul(mv, rx, ry);
    dm_mul(mv, mv, rz);
    dm_mul(mv, tr, mv);
    dm_perspective(proj, 45.0f * DEMO_PI / 180.0f, (float)w / (float)(h ? h : 1), 0.5f, 50.0f,
                   zero_to_one, 1);
    dm_mul(mvp, proj, mv);
}

#endif
