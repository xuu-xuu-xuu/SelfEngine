SelfEngine / G3 独立作品候选包（Windows x64 / Release）

1. 运行与选择
双击 materials.cmd、shadows.cmd、postprocess.cmd：PBR 材质球、方向阴影、Bloom/ToneMap/FXAA。
双击 repeated.cmd、mixed.cmd、mostly_outside.cmd：1000 对象压力场景，默认 5 秒预热、60 秒采样，优化默认关。
双击 resources.cmd：24 轮资源创建/释放/Trim，默认 placed + batch + PSO cache。
双击 loading-sync.cmd / loading-async.cmd：相同固定 16 贴图场景，同步突发与后台预算上传；默认 480 帧。
双击 editor.cmd：基础 Hierarchy/Inspector/相机/变换手柄/撤销/保存/Play，演示两个物体。
每次启动写独立 Runs 子目录，console.log 和 invocation.json 记录参数；压力/资源/加载另写 data 原始 CSV/JSON。
图形/编辑器 Alt+F4 退出；压力场景关闭窗口视为取消，不伪造完成报告。后台加载可按 R 取消并重启。
受系统权限或包位于只读目录时，先复制到可写目录；不可覆盖原始报告和基准证据。

可复制命令（当前目录为包根；路径带空格也可）：
.\mixed.cmd -Culling on -Sorting on -BindingCache on -Instancing on
.\mostly_outside.cmd -Culling on
.\repeated.cmd -Instancing on -BindingCache on
.\resources.cmd -Memory committed -Upload individual -PsoCache off
.\loading-async.cmd -BudgetBytes 262144 -BudgetTasks 4
.\editor.cmd -Headless -Frames 30
优化须逐项比较。排序可能增加总 CPU，混合实例化可能增加 GPU；延迟加载通常更久，报告保留负收益。
全部开关细节见 SourceArchive/Samples/Benchmark 和 SourceArchive/Samples/ResourceLoading。

2. 机器与运行依赖
需要 Windows 10/11 x64、D3D12 FL11_0 / SM5_1 显卡和驱动；不要求 VS、Windows SDK、.NET SDK 或 Python。
同目录 VC runtime DLL 来自已安装 Microsoft VC143 x64 Redist，原件版本和摘要见 runtime-dependencies.json。
D3D12/DXGI/WIC/DirectWrite/系统字体由 Windows 提供；没有复制系统字体、系统 DLL 或 GPU 驱动。
三组 G2 固定配置为 960×720，固定更新 1/60 秒，第 30 帧可复现；环境仅需处理后的 .seenv。
资源为本项目自制；第三方解析器、JSON、ImGui 原始许可证见 Licenses；MSVC runtime 适用微软再分发许可。
实际本机+仅系统 PATH+外部含空格 cwd 通过不等于真实 Windows 10/11 干净机均通过。

3. 源码、构建与版本
manifest.json 逐文件 SHA-256，build_id 为本地候选标识；source-inputs.json 是当前完整构建输入身份。
SourceArchive 保存对应源原字节、中文设计/计划/环境与构建脚本，不依赖当前机器个人绝对路径。
源码构建需要 VS2022 C++ x64、Windows SDK、CMake；完整托管验收另需 .NET SDK，具体见 SourceArchive/Docs/Environment.md。
从 SourceArchive 根执行 powershell -NoProfile -File Scripts/Build-Test.ps1 -Configuration Release。
未提交变更由完整内容摘要标识，Git HEAD 仅是基底；归档不等于已提交该版本。没有自动提交、打 Git 标签或远程发布。
若要运行依赖 Git 的版本/打包脚本，先在源码归档中自行初始化和审查保存版本；编译原生演示本身不需 Git 历史。

4. 报告与复测
Docs/Reports/T39/Drawing.md、Resources.md 为中文报告；五幅图与 chart-data.json、verification.json 同目录。
out/validation/continuous-20261101-c39dc9ef 保留 57 次历史运行、六轮呈现、逐帧 CPU/GPU/内存/等待和独立抓图。
各组 source-inputs.json 对应 checkpoint-*/files 原字节；历史两份被忽略的缓存位于 t39-source-supplement-01。
必要 PIX 抓帧、事件列表、截图与比较证据位于 t32-pix、t34-pix；回放工具需自行安装兼容 PIX，不是启动演示的依赖。
当前程序不冒充旧组程序：各次 run.json/环境记录自己的程序与源码摘要，当前包的身份另记。
重新分析需要 Python 3.8+，不要求第三方 Python 包；从包根执行：
python SourceArchive/Scripts/Build-PerformanceReports.py --evidence out/validation/continuous-20261101-c39dc9ef --presentation out/validation/continuous-20261101-c39dc9ef/t39-presentation-formal-01 --output Runs/recomputed-reports
图表可用 Windows PowerShell -STA 和 SourceArchive/Scripts/Export-PerformanceCharts.ps1 从 chart-data.json 生成。
正式重测应从源码构建后使用 Samples 中的三次 5+60 秒协议；默认 cmd 单次演示/短检查不能代替正式测量。
CPU scope 嵌套、GPU 区间、Frame 外 Sleep 与呈现节奏不可相加；抓图读回与计时必须分进程。
32 对象普通场景可见平均提交 64.05–64.22Hz，不承诺物理扫描帧率、最小 FPS 或所有游戏；长帧原值保留。

5. 当前范围
这是可审阅的 PortfolioCandidate，完整 V1 尚未完成；完整属性/编辑器、脚本、物理、游戏 UI、环境与工作流仍按计划推进。
基础编辑器已真实绘制和往返 Player；中文 IME 候选、多屏 DPI 和用户工作流尚需人工验证。
真实双系统干净机另在 T66 验收。历史 ETW 丢事件已单列，不用于宣称完整 trace 已分析无问题。
