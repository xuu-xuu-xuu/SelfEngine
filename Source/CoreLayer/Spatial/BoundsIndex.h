#pragma once
#include "CoreLayer/Math/Bounds.h"
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace se::spatial
{
struct Leaf {std::uint64_t key=0;Aabb bounds;};
struct Ray {Vec3 origin{},direction{};double maximumDistance=0;};
struct PreparedRay {std::array<double,3> origin{},direction{};double maximumDistance=0;};
bool Overlaps(const Aabb& a,const Aabb& b)noexcept;
std::optional<PreparedRay> PrepareRay(const Ray& ray)noexcept;
std::optional<double> Intersect(const Aabb& bounds,const PreparedRay& ray)noexcept;
struct TreeStatistics {std::size_t leaves=0,allocatedNodes=0;int height=0;std::uint64_t rebuilds=0,reinserts=0;double rebuildMilliseconds=0;};

// 不拥有领域对象。Build 用于静态 BVH；Upsert/Remove 用于动态 AABB 树。
// key 由调用方提供且非零；只返回自有 key 数组，绝不保存调用方叶子地址。
class BoundsIndex
{
public:
    bool Build(std::span<const Leaf> leaves);
    bool Upsert(Leaf leaf);
    bool Remove(std::uint64_t key);
    void Clear()noexcept;
    std::vector<std::uint64_t> Query(const Aabb& bounds,std::size_t* visited=nullptr)const;
    std::vector<std::uint64_t> Query(const PreparedRay& ray,std::size_t* visited=nullptr)const;
    TreeStatistics Statistics()const noexcept;
    bool Validate()const; // 诊断结构不变量；不在普通查询中遍历整棵树。
private:
    struct Node {Aabb bounds;std::uint64_t key=0;int parent=-1,left=-1,right=-1,height=0;};
    int Allocate();
    void Release(int node);
    int BuildRange(std::vector<Leaf>& leaves,std::size_t first,std::size_t last,int parent);
    void Insert(int leaf);
    void Detach(int leaf);
    void Refit(int node);
    void RebuildIfDegenerate();
    std::vector<Node> nodes_;
    std::vector<int> free_;
    std::unordered_map<std::uint64_t,int> leaves_;
    int root_=-1;
    std::uint64_t rebuilds_=0,reinserts_=0;
    double rebuildMilliseconds_=0;
};
}
