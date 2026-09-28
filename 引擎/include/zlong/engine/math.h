// 烛龙 (ZhuLong) - the little bit of linear algebra a first 3D scene needs.
//
// Row-major throughout, because that is what the vertex shader's constant buffer
// holds: row i at byte offset i * 16, so `clip[i] = dot(row_i, position)`.
//
// Right-handed, looking down -Z, clip depth in [-1, 1] -- the convention the
// software rasterizer's `1 / w` and viewport mapping assume.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace zlong::engine {

/// Radians, written once. A rounded literal here -- 0.8727 for 50 degrees --
/// shifts a projection by 4e-5 and flips depth tests along near-coplanar edges,
/// which is not something a reader of the constant could guess.
inline constexpr float kPi = 3.14159265358979323846f;

inline constexpr float Radians(float degrees) noexcept { return degrees * kPi / 180.0f; }

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 Normalize(const Vec3& v) noexcept {
    const float length = std::sqrt(Dot(v, v));
    if (length <= 0.0f) {
        return Vec3{};
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

/// Row-major 4x4. `m[row][column]`.
struct Mat4 {
    float m[4][4] = {{1.0f, 0.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f, 0.0f},
                     {0.0f, 0.0f, 1.0f, 0.0f},
                     {0.0f, 0.0f, 0.0f, 1.0f}};
};

inline Mat4 Identity() noexcept { return Mat4{}; }

inline Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    Mat4 r;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            r.m[row][column] = 0.0f;
            for (int k = 0; k < 4; ++k) {
                r.m[row][column] += a.m[row][k] * b.m[k][column];
            }
        }
    }
    return r;
}

inline Mat4 Translation(float x, float y, float z) noexcept {
    Mat4 r;
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}

/// Rotation about +Y, which is the one axis a first scene needs to turn objects
/// so their faces are not all parallel.
inline Mat4 RotationY(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 r;
    r.m[0][0] = c;
    r.m[0][2] = s;
    r.m[2][0] = -s;
    r.m[2][2] = c;
    return r;
}

/// Rotation about +X. `RotationY` was the only one a ground-and-boxes scene needed;
/// the piece figures need all three, and the same row-vector convention as RotationY.
inline Mat4 RotationX(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 r;
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

/// Rotation about +Z.
inline Mat4 RotationZ(float radians) noexcept {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 r;
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    return r;
}

/// Scale per axis. Safe for the scene shader, which normalizes the interpolated normal
/// (`n /= |n|`) -- and for an axis-aligned primitive the direction survives a per-axis
/// scale exactly, since the scale only lengthens each axis's normal, not tilts it.
inline Mat4 Scale(float x, float y, float z) noexcept {
    Mat4 r;
    r.m[0][0] = x;
    r.m[1][1] = y;
    r.m[2][2] = z;
    return r;
}

/// Uniform scale.
inline Mat4 Scale(float factor) noexcept {
    return Scale(factor, factor, factor);
}

/// The world-to-camera matrix: what `eye` sees, with -Z as its forward axis.
inline Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) noexcept {
    const Vec3 forward = Normalize(target - eye);
    const Vec3 right = Normalize(Cross(forward, up));
    const Vec3 true_up = Cross(right, forward);

    Mat4 r;
    r.m[0][0] = right.x;
    r.m[0][1] = right.y;
    r.m[0][2] = right.z;
    r.m[0][3] = -Dot(right, eye);
    r.m[1][0] = true_up.x;
    r.m[1][1] = true_up.y;
    r.m[1][2] = true_up.z;
    r.m[1][3] = -Dot(true_up, eye);
    r.m[2][0] = -forward.x;
    r.m[2][1] = -forward.y;
    r.m[2][2] = -forward.z;
    r.m[2][3] = Dot(forward, eye);
    return r;
}

/// Camera-to-clip. `clip.w` ends up as -z_camera, so anything in front of the eye
/// has w > 0 -- which is exactly what the rasterizer checks before dividing.
inline Mat4 Perspective(float fov_y_radians, float aspect, float near_z, float far_z) noexcept {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 r;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            r.m[row][column] = 0.0f;
        }
    }
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    r.m[2][2] = (far_z + near_z) / (near_z - far_z);
    r.m[2][3] = (2.0f * far_z * near_z) / (near_z - far_z);
    r.m[3][2] = -1.0f;
    return r;
}

/// A right-handed orthographic projection. `half_width` and `half_height` are the
/// half-extents of the view volume; `near_z` and `far_z` are distances in front of
/// the eye, in the same convention Perspective uses (view forward is -Z), so clip
/// depth lands in [-1, 1] for both.
inline Mat4 Orthographic(float half_width, float half_height, float near_z,
                         float far_z) noexcept {
    Mat4 r;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            r.m[row][column] = 0.0f;
        }
    }
    r.m[0][0] = 1.0f / half_width;
    r.m[1][1] = 1.0f / half_height;
    r.m[2][2] = -2.0f / (far_z - near_z);
    r.m[2][3] = -(far_z + near_z) / (far_z - near_z);
    r.m[3][3] = 1.0f;
    return r;
}

/// The 64 bytes a shader transforms with, laid out so that one sixteen-byte load is
/// one *column*. Every shader that multiplies by a matrix here accumulates
/// `column_j * v_j`, because a four-lane lane-wise ALU has no horizontal add to
/// reduce a row with.
struct MvpBytes {
    float rows[16] = {};
};

inline MvpBytes ToBytesByColumn(const Mat4& matrix) noexcept {
    MvpBytes bytes;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            bytes.rows[column * 4 + row] = matrix.m[row][column];
        }
    }
    return bytes;
}

/// An axis-aligned box in some space, boxed by two opposite corners.
struct Bound {
    Vec3 min{};
    Vec3 max{};
};

/// The box's eight corners pushed through `m`, re-bounded. Conservative: the
/// result contains the transformed box, which is all a visibility test needs.
inline Bound TransformBound(const Mat4& m, const Bound& box) noexcept {
    Bound out;
    for (int corner = 0; corner < 8; ++corner) {
        const float x = (corner & 1) != 0 ? box.max.x : box.min.x;
        const float y = (corner & 2) != 0 ? box.max.y : box.min.y;
        const float z = (corner & 4) != 0 ? box.max.z : box.min.z;
        const float px = m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3];
        const float py = m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3];
        const float pz = m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3];
        if (corner == 0) {
            out.min = Vec3{px, py, pz};
            out.max = out.min;
            continue;
        }
        out.min.x = std::min(out.min.x, px);
        out.min.y = std::min(out.min.y, py);
        out.min.z = std::min(out.min.z, pz);
        out.max.x = std::max(out.max.x, px);
        out.max.y = std::max(out.max.y, py);
        out.max.z = std::max(out.max.z, pz);
    }
    return out;
}

/// A half-space: points with normal . p + distance >= 0 are inside.
struct Plane {
    Vec3 normal{};
    float distance = 0.0f;
};

struct Frustum {
    /// Left, right, bottom, top, near, far.
    Plane planes[6];
};

/// Gribb-Hartmann extraction from a combined view-projection matrix. Clip depth
/// is [-1, 1], which is what this file's Perspective produces.
inline Frustum FrustumFromViewProjection(const Mat4& vp) noexcept {
    const auto combine = [&vp](int row, float sign) {
        Plane plane;
        plane.normal = Vec3{vp.m[3][0] + sign * vp.m[row][0], vp.m[3][1] + sign * vp.m[row][1],
                            vp.m[3][2] + sign * vp.m[row][2]};
        plane.distance = vp.m[3][3] + sign * vp.m[row][3];
        return plane;
    };
    Frustum frustum;
    frustum.planes[0] = combine(0, +1.0f);  // w + x >= 0
    frustum.planes[1] = combine(0, -1.0f);  // w - x >= 0
    frustum.planes[2] = combine(1, +1.0f);  // w + y >= 0
    frustum.planes[3] = combine(1, -1.0f);  // w - y >= 0
    frustum.planes[4] = combine(2, +1.0f);  // w + z >= 0
    frustum.planes[5] = combine(2, -1.0f);  // w - z >= 0
    return frustum;
}

/// True unless the box lies wholly outside one plane. Conservative: a box that
/// straddles a plane is kept, so nothing visible is ever dropped.
inline bool Intersects(const Frustum& frustum, const Bound& box) noexcept {
    for (const Plane& plane : frustum.planes) {
        // The corner furthest along the plane normal decides: if even that one is
        // behind the plane, every corner is.
        const Vec3 farthest{plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                            plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                            plane.normal.z >= 0.0f ? box.max.z : box.min.z};
        if (Dot(plane.normal, farthest) + plane.distance < 0.0f) {
            return false;
        }
    }
    return true;
}

}  // namespace zlong::engine
