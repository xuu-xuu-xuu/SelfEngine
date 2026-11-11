#include "BoundsIndex.h"
#include <algorithm>
#include <climits>
#include <chrono>
#include <unordered_set>

namespace se::spatial
{
namespace
{
Aabb Merge(const Aabb& a,const Aabb& b)
{
    return {{(std::min)(a.minimum.x,b.minimum.x),(std::min)(a.minimum.y,b.minimum.y),(std::min)(a.minimum.z,b.minimum.z)},
        {(std::max)(a.maximum.x,b.maximum.x),(std::max)(a.maximum.y,b.maximum.y),(std::max)(a.maximum.z,b.maximum.z)}};
}
bool Equal(const Aabb& a,const Aabb& b)
{return a.minimum.x==b.minimum.x&&a.minimum.y==b.minimum.y&&a.minimum.z==b.minimum.z&&a.maximum.x==b.maximum.x&&a.maximum.y==b.maximum.y&&a.maximum.z==b.maximum.z;}
double Area(const Aabb& a)
{const double x=double(a.maximum.x)-a.minimum.x,y=double(a.maximum.y)-a.minimum.y,z=double(a.maximum.z)-a.minimum.z;return 2*(x*y+x*z+y*z);}
double Center(const Aabb& a,int axis)
{return axis==0?(double(a.minimum.x)+a.maximum.x)*.5:axis==1?(double(a.minimum.y)+a.maximum.y)*.5:(double(a.minimum.z)+a.maximum.z)*.5;}
}
bool Overlaps(const Aabb& a,const Aabb& b)noexcept
{
    return a.minimum.x<=b.maximum.x&&a.maximum.x>=b.minimum.x&&a.minimum.y<=b.maximum.y&&a.maximum.y>=b.minimum.y&&a.minimum.z<=b.maximum.z&&a.maximum.z>=b.minimum.z;
}
std::optional<PreparedRay> PrepareRay(const Ray& ray)noexcept
{
    if(!IsFinite(ray.origin)||!IsFinite(ray.direction)||!std::isfinite(ray.maximumDistance)||ray.maximumDistance<0)return std::nullopt;
    const double x=ray.direction.x,y=ray.direction.y,z=ray.direction.z,length=std::sqrt(x*x+y*y+z*z);
    if(!(length>0)||!std::isfinite(length))return std::nullopt;
    return PreparedRay{{ray.origin.x,ray.origin.y,ray.origin.z},{x/length,y/length,z/length},ray.maximumDistance};
}
std::optional<double> Intersect(const Aabb& bounds,const PreparedRay& ray)noexcept
{
    double enter=0,leave=ray.maximumDistance;
    const double lo[]{bounds.minimum.x,bounds.minimum.y,bounds.minimum.z},hi[]{bounds.maximum.x,bounds.maximum.y,bounds.maximum.z};
    for(int axis=0;axis<3;++axis)
    {
        if(ray.direction[axis]==0){if(ray.origin[axis]<lo[axis]||ray.origin[axis]>hi[axis])return std::nullopt;continue;}
        auto a=(lo[axis]-ray.origin[axis])/ray.direction[axis],b=(hi[axis]-ray.origin[axis])/ray.direction[axis];if(a>b)std::swap(a,b);
        enter=(std::max)(enter,a);leave=(std::min)(leave,b);if(enter>leave)return std::nullopt;
    }
    return enter;
}
int BoundsIndex::Allocate()
{
    if(!free_.empty()){const auto node=free_.back();free_.pop_back();nodes_[node]={};return node;}
    nodes_.emplace_back();return static_cast<int>(nodes_.size()-1);
}
void BoundsIndex::Release(int node){nodes_[node]={};free_.push_back(node);}
void BoundsIndex::Clear()noexcept{nodes_.clear();free_.clear();leaves_.clear();root_=-1;rebuilds_=reinserts_=0;rebuildMilliseconds_=0;}
int BoundsIndex::BuildRange(std::vector<Leaf>& leaves,std::size_t first,std::size_t last,int parent)
{
    const auto node=Allocate();nodes_[node].parent=parent;
    if(last-first==1){nodes_[node].bounds=leaves[first].bounds;nodes_[node].key=leaves[first].key;leaves_.emplace(leaves[first].key,node);return node;}
    double lo[3]{Center(leaves[first].bounds,0),Center(leaves[first].bounds,1),Center(leaves[first].bounds,2)},hi[3]{lo[0],lo[1],lo[2]};
    for(auto i=first+1;i<last;++i)for(int axis=0;axis<3;++axis){const auto center=Center(leaves[i].bounds,axis);lo[axis]=(std::min)(lo[axis],center);hi[axis]=(std::max)(hi[axis],center);}
    int axis=0;for(int next=1;next<3;++next)if(hi[next]-lo[next]>hi[axis]-lo[axis])axis=next;
    const auto middle=first+(last-first)/2;
    std::nth_element(leaves.begin()+first,leaves.begin()+middle,leaves.begin()+last,[axis](const Leaf& a,const Leaf& b){const auto ca=Center(a.bounds,axis),cb=Center(b.bounds,axis);return ca<cb||(ca==cb&&a.key<b.key);});
    const auto left=BuildRange(leaves,first,middle,node),right=BuildRange(leaves,middle,last,node);
    nodes_[node].left=left;nodes_[node].right=right;nodes_[node].bounds=Merge(nodes_[left].bounds,nodes_[right].bounds);nodes_[node].height=1+(std::max)(nodes_[left].height,nodes_[right].height);return node;
}
bool BoundsIndex::Build(std::span<const Leaf> input)
{
    const auto start=std::chrono::steady_clock::now();
    if(input.size()>static_cast<std::size_t>(INT_MAX/2))return false;
    std::unordered_set<std::uint64_t> unique;unique.reserve(input.size());
    for(const auto& leaf:input)if(!leaf.key||!IsValid(leaf.bounds)||!unique.insert(leaf.key).second)return false;
    BoundsIndex next;next.nodes_.reserve(input.size()*2);next.leaves_.reserve(input.size());
    std::vector<Leaf> leaves(input.begin(),input.end());if(!leaves.empty())next.root_=next.BuildRange(leaves,0,leaves.size(),-1);
    next.rebuilds_=rebuilds_+1;next.reinserts_=reinserts_;next.rebuildMilliseconds_=rebuildMilliseconds_+std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();*this=std::move(next);return true;
}
void BoundsIndex::Refit(int node)
{
    while(node!=-1)
    {
        auto& current=nodes_[node];current.bounds=Merge(nodes_[current.left].bounds,nodes_[current.right].bounds);
        current.height=1+(std::max)(nodes_[current.left].height,nodes_[current.right].height);node=current.parent;
    }
}
void BoundsIndex::Insert(int leaf)
{
    if(root_==-1){root_=leaf;nodes_[leaf].parent=-1;return;}
    int sibling=root_;
    while(nodes_[sibling].left!=-1)
    {
        const auto left=nodes_[sibling].left,right=nodes_[sibling].right;
        const auto leftCost=Area(Merge(nodes_[left].bounds,nodes_[leaf].bounds))-Area(nodes_[left].bounds);
        const auto rightCost=Area(Merge(nodes_[right].bounds,nodes_[leaf].bounds))-Area(nodes_[right].bounds);
        sibling=leftCost<rightCost||(leftCost==rightCost&&nodes_[left].height<=nodes_[right].height)?left:right;
    }
    const auto oldParent=nodes_[sibling].parent,parent=Allocate();nodes_[parent].parent=oldParent;nodes_[parent].left=sibling;nodes_[parent].right=leaf;
    nodes_[leaf].parent=parent;nodes_[sibling].parent=parent;
    if(oldParent==-1)root_=parent;else if(nodes_[oldParent].left==sibling)nodes_[oldParent].left=parent;else nodes_[oldParent].right=parent;
    Refit(parent);
}
void BoundsIndex::Detach(int leaf)
{
    if(root_==leaf){root_=-1;return;}
    const auto parent=nodes_[leaf].parent,grandparent=nodes_[parent].parent,sibling=nodes_[parent].left==leaf?nodes_[parent].right:nodes_[parent].left;
    if(grandparent==-1)root_=sibling;else if(nodes_[grandparent].left==parent)nodes_[grandparent].left=sibling;else nodes_[grandparent].right=sibling;
    nodes_[sibling].parent=grandparent;Release(parent);nodes_[leaf].parent=-1;if(grandparent!=-1)Refit(grandparent);
}
void BoundsIndex::RebuildIfDegenerate()
{
    if(leaves_.size()<16)return;
    const auto limit=2*static_cast<int>(std::ceil(std::log2(double(leaves_.size()))))+4;
    if(nodes_[root_].height<=limit)return;
    std::vector<Leaf> leaves;leaves.reserve(leaves_.size());for(const auto& [key,index]:leaves_)leaves.push_back({key,nodes_[index].bounds});Build(leaves);
}
bool BoundsIndex::Upsert(Leaf leaf)
{
    if(!leaf.key||!IsValid(leaf.bounds)||nodes_.size()>static_cast<std::size_t>(INT_MAX-2))return false;
    if(nodes_.size()+2>nodes_.capacity())nodes_.reserve((std::max)(nodes_.size()+2,nodes_.capacity()*2));
    if(free_.size()+2>free_.capacity())free_.reserve((std::max)(free_.size()+2,free_.capacity()*2));
    if(const auto found=leaves_.find(leaf.key);found!=leaves_.end())
    {
        const auto node=found->second;if(Equal(nodes_[node].bounds,leaf.bounds))return true;
        Detach(node);nodes_[node].bounds=leaf.bounds;Insert(node);++reinserts_;
    }
    else
    {
        const auto node=Allocate();nodes_[node].key=leaf.key;nodes_[node].bounds=leaf.bounds;
        try{leaves_.emplace(leaf.key,node);}catch(...){Release(node);throw;}
        Insert(node);
    }
    RebuildIfDegenerate();return true;
}
bool BoundsIndex::Remove(std::uint64_t key)
{
    const auto found=leaves_.find(key);if(found==leaves_.end())return false;
    if(free_.size()+2>free_.capacity())free_.reserve((std::max)(free_.size()+2,free_.capacity()*2));const auto node=found->second;Detach(node);Release(node);leaves_.erase(found);return true;
}
std::vector<std::uint64_t> BoundsIndex::Query(const Aabb& bounds,std::size_t* visited)const
{
    if(visited)*visited=0;std::vector<std::uint64_t> result;if(root_==-1||!IsValid(bounds))return result;
    std::vector<int> stack{root_};while(!stack.empty())
    {
        const auto index=stack.back();stack.pop_back();const auto& node=nodes_[index];if(visited)++*visited;
        if(!Overlaps(bounds,node.bounds))continue;if(node.left==-1)result.push_back(node.key);else{stack.push_back(node.right);stack.push_back(node.left);}
    }
    std::sort(result.begin(),result.end());return result;
}
std::vector<std::uint64_t> BoundsIndex::Query(const PreparedRay& ray,std::size_t* visited)const
{
    if(visited)*visited=0;std::vector<std::uint64_t> result;if(root_==-1)return result;
    std::vector<int> stack{root_};while(!stack.empty())
    {
        const auto index=stack.back();stack.pop_back();const auto& node=nodes_[index];if(visited)++*visited;
        if(!Intersect(node.bounds,ray))continue;if(node.left==-1)result.push_back(node.key);else{stack.push_back(node.right);stack.push_back(node.left);}
    }
    std::sort(result.begin(),result.end());return result;
}
TreeStatistics BoundsIndex::Statistics()const noexcept
{return {leaves_.size(),nodes_.size(),root_==-1?0:nodes_[root_].height,rebuilds_,reinserts_,rebuildMilliseconds_};}
bool BoundsIndex::Validate()const
{
    if(root_==-1)return leaves_.empty();if(nodes_[root_].parent!=-1)return false;
    std::unordered_set<int> seen;std::vector<int> stack{root_};std::size_t leafCount=0;
    while(!stack.empty())
    {
        const auto index=stack.back();stack.pop_back();if(index<0||static_cast<std::size_t>(index)>=nodes_.size()||!seen.insert(index).second)return false;
        const auto& node=nodes_[index];if(!IsValid(node.bounds))return false;
        if(node.left==-1){const auto it=leaves_.find(node.key);if(node.right!=-1||node.height!=0||it==leaves_.end()||it->second!=index)return false;++leafCount;continue;}
        if(node.right<0||static_cast<std::size_t>(node.left)>=nodes_.size()||static_cast<std::size_t>(node.right)>=nodes_.size())return false;
        const auto& left=nodes_[node.left];const auto& right=nodes_[node.right];
        if(left.parent!=index||right.parent!=index||node.height!=1+(std::max)(left.height,right.height)||!Equal(node.bounds,Merge(left.bounds,right.bounds)))return false;
        stack.push_back(node.left);stack.push_back(node.right);
    }
    return leafCount==leaves_.size()&&seen.size()==leafCount*2-1;
}
}
