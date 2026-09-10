#pragma once

/**
 * @file Math3D.h
 * @brief The matrix operations the engine's Mat4 does not carry yet.
 *
 * Deki's Mat4 (deki/Vector.h) is column-major, m[column][row], and ships with
 * Identity and Ortho only. Everything a perspective pipeline needs lives here
 * until it earns a place in the engine header.
 *
 * Conventions, chosen to match OpenGL so a GPU backend can hand these same
 * matrices to a driver without transposing anything:
 *   - right-handed world, camera looks down -Z
 *   - clip space x, y in [-1, 1], z in [-1, 1]
 *   - Mul(a, b) applies b first, then a
 *
 * Rotation composes X then Y then Z (R = Rz * Ry * Rx), which is the order
 * COMPATIBILITY.md fixes for Deki objects, and `rotation` is the Z angle.
 * Angles are radians.
 */

#include <deki/Vector.h>

#include <cmath>

namespace Deki3D
{

using Deki::Mat4;
using Deki::Vector3;

/// C = a * b, applying b first. C.m[c][r] = sum_k a.m[k][r] * b.m[c][k].
inline Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
                sum += a.m[k][r] * b.m[c][k];
            out.m[c][r] = sum;
        }
    }
    return out;
}

/// A point through the matrix, with the w it lands on. w is what perspective
/// division needs, so the caller gets it rather than a silently divided point.
inline Vector3 TransformPoint(const Mat4& m, const Vector3& p, float& outW)
{
    const float x = m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0];
    const float y = m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1];
    const float z = m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2];
    outW = m.m[0][3] * p.x + m.m[1][3] * p.y + m.m[2][3] * p.z + m.m[3][3];
    return Vector3(x, y, z);
}

/// A direction: the translation column is not applied. Correct for normals
/// only while the matrix has uniform scale; non-uniform scale needs the
/// inverse transpose, which the mesh path does on the CPU when it must.
inline Vector3 TransformDirection(const Mat4& m, const Vector3& v)
{
    return Vector3(m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
                   m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
                   m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z);
}

inline Mat4 Translate(float x, float y, float z)
{
    Mat4 m;
    m.m[3][0] = x;
    m.m[3][1] = y;
    m.m[3][2] = z;
    return m;
}

inline Mat4 Scale(float x, float y, float z)
{
    Mat4 m;
    m.m[0][0] = x;
    m.m[1][1] = y;
    m.m[2][2] = z;
    return m;
}

inline Mat4 RotateX(float radians)
{
    const float s = std::sin(radians), c = std::cos(radians);
    Mat4 m;
    m.m[1][1] = c;
    m.m[1][2] = s;
    m.m[2][1] = -s;
    m.m[2][2] = c;
    return m;
}

inline Mat4 RotateY(float radians)
{
    const float s = std::sin(radians), c = std::cos(radians);
    Mat4 m;
    m.m[0][0] = c;
    m.m[0][2] = -s;
    m.m[2][0] = s;
    m.m[2][2] = c;
    return m;
}

inline Mat4 RotateZ(float radians)
{
    const float s = std::sin(radians), c = std::cos(radians);
    Mat4 m;
    m.m[0][0] = c;
    m.m[0][1] = s;
    m.m[1][0] = -s;
    m.m[1][1] = c;
    return m;
}

/// A Deki object's transform as a matrix: scale, then R = Rz * Ry * Rx, then
/// translation. `rotationZ` is the object's `rotation`.
inline Mat4 Compose(float x, float y, float z,
                    float rotationX, float rotationY, float rotationZ,
                    float scaleX, float scaleY, float scaleZ)
{
    Mat4 r = Mul(RotateZ(rotationZ), Mul(RotateY(rotationY), RotateX(rotationX)));
    return Mul(Translate(x, y, z), Mul(r, Scale(scaleX, scaleY, scaleZ)));
}

/// Right-handed perspective, looking down -Z, clip z in [-1, 1].
/// `fovY` is the vertical field of view in radians.
inline Mat4 Perspective(float fovY, float aspect, float nearZ, float farZ)
{
    Mat4 m;
    const float t = std::tan(fovY * 0.5f);
    if (t == 0.0f || aspect == 0.0f || nearZ == farZ)
        return m;  // identity rather than infinities on a degenerate camera

    const float f = 1.0f / t;
    m.m[0][0] = f / aspect;
    m.m[1][1] = f;
    m.m[2][2] = (farZ + nearZ) / (nearZ - farZ);
    m.m[2][3] = -1.0f;
    m.m[3][2] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    m.m[3][3] = 0.0f;
    return m;
}

/// A view matrix for a camera at `eye` looking at `target`.
inline Mat4 LookAt(const Vector3& eye, const Vector3& target, const Vector3& up)
{
    const Vector3 f = (target - eye).Normalized();  // forward, toward the target
    Vector3 s = f.Cross(up);                        // right
    const float sLen = s.Length();
    if (sLen <= 0.0f)
        return Mat4::Identity();  // degenerate: forward parallel to up
    s = s / sLen;
    const Vector3 u = s.Cross(f);  // true up

    Mat4 m;
    m.m[0][0] = s.x;  m.m[1][0] = s.y;  m.m[2][0] = s.z;
    m.m[0][1] = u.x;  m.m[1][1] = u.y;  m.m[2][1] = u.z;
    m.m[0][2] = -f.x; m.m[1][2] = -f.y; m.m[2][2] = -f.z;
    m.m[3][0] = -s.Dot(eye);
    m.m[3][1] = -u.Dot(eye);
    m.m[3][2] = f.Dot(eye);
    return m;
}

}  // namespace Deki3D
