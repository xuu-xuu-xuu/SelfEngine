# 阴影、分簇灯光与环境遮蔽

## 方向光阴影

[PrepareDirectionalShadow()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ShadowSystem.cpp#L6) 从方向光和场景阴影视图准备矩阵与阴影参数。绘制先在光源视角写入深度，再由着色器比较接收点深度与阴影图。D3D12 的记录入口是 [RecordD3D12Shadow()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp#L36)。

## 级联与灯光分簇

CPU 算法 [PrepareCascadeShadows()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L19) 根据相机视锥切片拟合各级阴影投影，[SelectCascade()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L78) 按视深选择级别和混合权重。点光源分簇由 [BuildLightClusters()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LightClustering.cpp#L32) 构建每个空间分区的光源列表，容量不足时可用 [FillLightClustersGlobal()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LightClustering.cpp#L111) 回退到全局列表。

## 屏幕空间遮蔽的 CPU 参考实现

[ComputeGtao()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L65) 从视空间深度和法线估算间接光遮蔽。[ComputeContactShadows()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L108) 沿光源方向检查近距离遮挡，输出直接光可见度。低分辨率结果通过 [UpsampleBilateral()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L144) 按深度和法线升采样。
