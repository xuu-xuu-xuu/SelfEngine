# 光照着色、材质接口与 Shader 包

## 基础光照与 PBR

基础光照的 [PSMain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/BasicLit.hlsl#L43) 从法线、光照方向和观察方向计算漫反射与高光。PBR 着色器的 [PSMain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/Pbr.hlsl#L69) 使用金属度、粗糙度及法线等材质输入。两种路径接收的网格顶点可以共用，使用哪套着色器由管线描述决定。

## 环境与材质绑定

[ConfigurePbrMaterial()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/StaticModelRender.cpp#L57) 将渲染材质准备为绘制绑定。环境资产提供漫反射辐照度、预滤波辐射和 BRDF 查找表，[PrepareEnvironmentBindings()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L50) 为环境光照路径组织这三种输入。

## 自定义着色器如何进入运行时

[ReadD3D12ShaderSource()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L412) 记录源码、预处理结果和依赖。[WriteD3D12UserPbrPackage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L535) 将固定 PBR 接口的程序打包；[LoadD3D12UserPbrPackage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L555) 在创建管线时读取并校验接口。修改材质绑定布局时需要同时核对 CPU 常量、HLSL 声明和接口摘要。
