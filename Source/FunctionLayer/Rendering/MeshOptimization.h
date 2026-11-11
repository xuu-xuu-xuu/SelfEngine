#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace se
{
// G03：FIFO 顶点缓存的平均未命中率（ACMR）。用于离线重排前后对比；
// 只统计索引流，不改变三角形顺序。
// G03：小网格的贪心三角形重排：每次选当前缓存命中数最高的未处理三角形。
// 仅用于离线烘焙（<=4096 三角形），大网格原样返回，避免 O(T^2) 失控。
inline std::vector<std::uint32_t> OptimizeVertexCache(std::span<const std::uint32_t> indices,std::size_t cacheSize=32)
{
    const std::size_t triangles=indices.size()/3;
    std::vector<std::uint32_t> result(indices.begin(),indices.end());
    if(triangles<2||triangles>4096)return result;
    std::vector<std::uint8_t> done(triangles,0);
    std::vector<std::uint32_t> cache;cache.reserve(cacheSize);
    std::size_t write=0;
    for(std::size_t step=0;step<triangles;++step)
    {
        std::size_t best=triangles;int bestScore=-1;
        for(std::size_t t=0;t<triangles;++t)
        {
            if(done[t])continue;
            int score=0;
            for(int k=0;k<3;++k)
                if(std::find(cache.begin(),cache.end(),indices[t*3+k])!=cache.end())++score;
            if(score>bestScore){bestScore=score;best=t;if(score==3)break;}
        }
        if(best==triangles)break;
        done[best]=1;
        for(int k=0;k<3;++k)
        {
            const std::uint32_t vertex=indices[best*3+k];
            result[write++]=vertex;
            if(std::find(cache.begin(),cache.end(),vertex)==cache.end())
            {
                if(cache.size()==cacheSize)cache.erase(cache.begin());
                cache.push_back(vertex);
            }
        }
    }
    return result;
}

inline float AverageCacheMissRatio(std::span<const std::uint32_t> indices,std::size_t cacheSize=32)
{
    if(indices.empty())return 0;
    std::vector<std::uint32_t> cache;
    cache.reserve(cacheSize);
    std::size_t misses=0;
    for(const std::uint32_t vertex:indices)
    {
        if(std::find(cache.begin(),cache.end(),vertex)==cache.end())
        {
            ++misses;
            if(cache.size()==cacheSize)cache.erase(cache.begin());
            cache.push_back(vertex);
        }
    }
    return static_cast<float>(misses)/static_cast<float>(indices.size());
}
}
