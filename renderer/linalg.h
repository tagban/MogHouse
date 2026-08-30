#pragma once

// Just enough linear algebra to point a camera at something. Deliberately not a
// dependency - the renderer needs perspective, look-at and a multiply, and
// nothing else yet.
//
// Not called math.h: the moment this directory lands on an include path,
// <cmath> picks this up instead of the system header and the standard library
// stops compiling.

#include <cmath>

namespace mh
{
struct Vec3
{
    float x{}, y{}, z{};
};

inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 normalise(const Vec3& v)
{
    const float length = std::sqrt(dot(v, v));
    return length > 0.0f ? Vec3{v.x / length, v.y / length, v.z / length} : Vec3{0, 1, 0};
}

/// Column-major 4x4, matching what WGSL expects in a uniform buffer.
struct Mat4
{
    float m[16]{};

    static Mat4 identity()
    {
        Mat4 out;
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
        return out;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 out;
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

/// Right-handed, depth mapped to 0..1 the way WebGPU wants rather than -1..1.
///
/// The x scale is negative on purpose, and it is not a fudge - it is half of
/// the conversion out of FFXI's coordinates.
///
/// FFXI's vertical points down. The DAT readers negate it so everything above
/// them can assume y is up, and negating a single axis is a reflection: it
/// converts the handedness of the frame and mirrors what is drawn. Flipping a
/// horizontal axis as well restores it. Doing that here, once, covers
/// everything that reaches the screen through a projection.
///
/// The symptom was a mirrored world that reads as plausible: in Bastok Markets
/// the water sits left of the auction house, and it was on the right.
inline Mat4 perspective(float fovY, float aspect, float near, float far)
{
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 out;
    out.m[0] = -f / aspect;
    out.m[5] = f;
    out.m[10] = far / (near - far);
    out.m[11] = -1.0f;
    out.m[14] = (near * far) / (near - far);
    return out;
}

/// Orthographic, depth mapped to 0..1 the way WebGPU wants. Used to bake a
/// zone into a flat map: perspective would give the middle of the zone a
/// different scale from the edges, which is the one thing a map must not do.
/// The same handedness correction as perspective - see there. The map bake
/// used to swap its left and right arguments to get this; now both projections
/// carry it in the same place.
inline Mat4 orthographic(float left, float right, float bottom, float top, float near, float far)
{
    Mat4 out = Mat4::identity();
    out.m[0] = -2.0f / (right - left);
    out.m[5] = 2.0f / (top - bottom);
    out.m[10] = 1.0f / (near - far);
    out.m[12] = (left + right) / (left - right);
    out.m[13] = (bottom + top) / (bottom - top);
    out.m[14] = near / (near - far);
    return out;
}

inline Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
{
    const Vec3 forward = normalise(target - eye);
    const Vec3 right = normalise(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    Mat4 out = Mat4::identity();
    out.m[0] = right.x;   out.m[4] = right.y;   out.m[8] = right.z;
    out.m[1] = trueUp.x;  out.m[5] = trueUp.y;  out.m[9] = trueUp.z;
    out.m[2] = -forward.x; out.m[6] = -forward.y; out.m[10] = -forward.z;
    out.m[12] = -dot(right, eye);
    out.m[13] = -dot(trueUp, eye);
    out.m[14] = dot(forward, eye);
    return out;
}
} // namespace mh
