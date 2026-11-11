#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace se
{
constexpr float Pi = 3.14159265358979323846f;
struct Vec2 { float x = 0, y = 0; };
struct Vec3
{
    float x = 0, y = 0, z = 0;
    constexpr Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    constexpr Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(Vec3 b) { *this = *this + b; return *this; }
    Vec3& operator-=(Vec3 b) { *this = *this - b; return *this; }
    Vec3& operator*=(float s) { *this = *this * s; return *this; }
};
constexpr Vec3 operator*(float s, Vec3 v) { return v * s; }
constexpr Vec3 Hadamard(Vec3 a, Vec3 b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
constexpr float Dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
constexpr Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline float LengthSquared(Vec3 v) { return Dot(v,v); }
inline float Length(Vec3 v) { return std::sqrt(Dot(v,v)); }
inline Vec3 Normalize(Vec3 v) { const float n = Length(v); return n > 1e-8f ? v/n : Vec3{}; }
inline bool IsFinite(float value) { return std::isfinite(value); }
inline bool IsFinite(Vec3 v) { return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z); }
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct Quat
{
    float x = 0, y = 0, z = 0, w = 1;
};
inline bool IsFinite(Quat q) { return IsFinite(q.x) && IsFinite(q.y) && IsFinite(q.z) && IsFinite(q.w); }
inline float LengthSquared(Quat q) { return q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w; }
inline Quat Normalize(Quat q)
{
    const float n = std::sqrt(LengthSquared(q));
    return n > 1e-8f ? Quat{q.x/n,q.y/n,q.z/n,q.w/n} : Quat{};
}
constexpr Quat Conjugate(Quat q) { return {-q.x,-q.y,-q.z,q.w}; }
constexpr Quat operator*(Quat a, Quat b)
{
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline Quat AxisAngle(Vec3 axis, float radians)
{
    axis = Normalize(axis);
    if (LengthSquared(axis) == 0) return {};
    const float s = std::sin(radians * 0.5f);
    return {axis.x*s,axis.y*s,axis.z*s,std::cos(radians*0.5f)};
}
inline Vec3 Rotate(Quat q, Vec3 v)
{
    q = Normalize(q);
    const Vec3 t = 2.0f * Cross({q.x,q.y,q.z}, v);
    return v + q.w*t + Cross({q.x,q.y,q.z},t);
}

// 列主序存储，列向量；乘积 A*B 表示先应用 B，再应用 A。
struct Mat4
{
    std::array<float,16> values{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float& operator()(std::size_t row, std::size_t column) { return values[column*4+row]; }
    float operator()(std::size_t row, std::size_t column) const { return values[column*4+row]; }
    static Mat4 Identity() { return {}; }
};
inline Mat4 operator*(const Mat4& a, const Mat4& b)
{
    Mat4 result; result.values.fill(0);
    for (std::size_t row=0; row<4; ++row)
        for (std::size_t column=0; column<4; ++column)
            for (std::size_t k=0; k<4; ++k) result(row,column) += a(row,k)*b(k,column);
    return result;
}
inline Vec4 operator*(const Mat4& m, Vec4 v)
{
    return {m(0,0)*v.x+m(0,1)*v.y+m(0,2)*v.z+m(0,3)*v.w,
        m(1,0)*v.x+m(1,1)*v.y+m(1,2)*v.z+m(1,3)*v.w,
        m(2,0)*v.x+m(2,1)*v.y+m(2,2)*v.z+m(2,3)*v.w,
        m(3,0)*v.x+m(3,1)*v.y+m(3,2)*v.z+m(3,3)*v.w};
}
inline Vec3 TransformPoint(const Mat4& m, Vec3 v)
{
    const Vec4 p = m * Vec4{v.x,v.y,v.z,1};
    return std::abs(p.w)>1e-8f ? Vec3{p.x/p.w,p.y/p.w,p.z/p.w} : Vec3{p.x,p.y,p.z};
}
inline Vec3 TransformVector(const Mat4& m, Vec3 v)
{
    const Vec4 p = m * Vec4{v.x,v.y,v.z,0}; return {p.x,p.y,p.z};
}
inline Mat4 Translation(Vec3 position)
{
    Mat4 result; result(0,3)=position.x; result(1,3)=position.y; result(2,3)=position.z; return result;
}
inline Mat4 Scaling(Vec3 scale)
{
    Mat4 result; result(0,0)=scale.x; result(1,1)=scale.y; result(2,2)=scale.z; return result;
}
inline Mat4 Rotation(Quat rotation)
{
    const Quat q=Normalize(rotation);
    Mat4 result;
    result(0,0)=1-2*(q.y*q.y+q.z*q.z); result(0,1)=2*(q.x*q.y-q.z*q.w); result(0,2)=2*(q.x*q.z+q.y*q.w);
    result(1,0)=2*(q.x*q.y+q.z*q.w); result(1,1)=1-2*(q.x*q.x+q.z*q.z); result(1,2)=2*(q.y*q.z-q.x*q.w);
    result(2,0)=2*(q.x*q.z-q.y*q.w); result(2,1)=2*(q.y*q.z+q.x*q.w); result(2,2)=1-2*(q.x*q.x+q.y*q.y);
    return result;
}
inline std::optional<Mat4> Inverse(const Mat4& matrix)
{
    double a[4][8]{};
    for (std::size_t row=0; row<4; ++row)
        for (std::size_t column=0; column<4; ++column)
        { a[row][column]=matrix(row,column); a[row][column+4]=row==column?1.0:0.0; }
    for (std::size_t column=0; column<4; ++column)
    {
        std::size_t pivot=column;
        for(std::size_t row=column+1; row<4; ++row) if(std::abs(a[row][column])>std::abs(a[pivot][column])) pivot=row;
        if(std::abs(a[pivot][column])<1e-12) return std::nullopt;
        if(pivot!=column) for(std::size_t k=0;k<8;++k) {const double t=a[pivot][k];a[pivot][k]=a[column][k];a[column][k]=t;}
        const double divisor=a[column][column]; for(double& value:a[column]) value/=divisor;
        for(std::size_t row=0; row<4;++row) if(row!=column)
        {const double factor=a[row][column];for(std::size_t k=0;k<8;++k)a[row][k]-=factor*a[column][k];}
    }
    Mat4 result;
    for(std::size_t row=0;row<4;++row)for(std::size_t column=0;column<4;++column)
    {result(row,column)=static_cast<float>(a[row][column+4]);if(!IsFinite(result(row,column)))return std::nullopt;}
    return result;
}
inline Mat4 Transpose(const Mat4& m)
{
    Mat4 result; for(std::size_t row=0;row<4;++row)for(std::size_t column=0;column<4;++column)result(row,column)=m(column,row);return result;
}
inline std::optional<Vec3> TransformNormal(const Mat4& m, Vec3 normal)
{
    const auto inverse=Inverse(m); if(!inverse)return std::nullopt; return Normalize(TransformVector(Transpose(*inverse),normal));
}
struct Transform
{
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{1,1,1};
    Mat4 Matrix() const { return Translation(position)*Rotation(rotation)*Scaling(scale); }
};
inline bool IsValid(const Transform& transform)
{
    return IsFinite(transform.position) && IsFinite(transform.rotation) && IsFinite(transform.scale)
        && IsFinite(LengthSquared(transform.rotation)) && LengthSquared(transform.rotation)>1e-12f;
}
}
