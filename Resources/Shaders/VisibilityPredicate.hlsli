// 调用者先声明六个 float4 planes；O01/O02共用同一保守谓词。
struct VisibilityItem {float3 minimum;uint id;float3 maximum;uint reserved;};
bool Visible(VisibilityItem item)
{
    bool valid=all(isfinite(item.minimum))&&all(isfinite(item.maximum))&&all(item.minimum<=item.maximum);
    bool visible=true;
    [unroll] for(uint i=0;i<6;++i)
    {
        float4 p=planes[i];
        precise float x=p.x*(p.x>=0?item.maximum.x:item.minimum.x);
        precise float y=p.y*(p.y>=0?item.maximum.y:item.minimum.y);
        precise float z=p.z*(p.z>=0?item.maximum.z:item.minimum.z);
        precise float distance=x+y;distance=distance+z;distance=distance+p.w;
        precise float magnitude=abs(x)+abs(y);magnitude=magnitude+abs(z);magnitude=magnitude+abs(p.w);
        precise float error=(8.0f*1.1920928955078125e-7f)*magnitude;
        precise float margin=1e-5f+error;
        bool rejected=valid&&isfinite(distance)&&isfinite(margin)&&distance < -margin;
        visible=visible&&!rejected;
    }
    return visible;
}
