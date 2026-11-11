SelfEngine G2 图形演示（Windows x64 / Release）

双击 materials.cmd、shadows.cmd 或 postprocess.cmd 启动三组固定配置。
也可在本目录运行 EngineSandbox.exe --demo materials（或 shadows/postprocess）。
三个配置共用 960×720、位置 (0,3,9)、yaw=0、pitch=-0.3 和固定更新 1/60 秒。
WASD/鼠标移动相机，R 回到配置相机；其他开关见控制台提示，Alt+F4 退出。

需要支持 D3D12 FL11_0 / SM5_1 的显卡、驱动、Windows 10/11 x64 和已安装的 VC++ x64 运行库。
当前机器运行通过不等于干净机、双系统和所有驱动均已验证；不需要 Visual Studio 或 Windows SDK 来启动此包。
输出为 SDR。后处理 UI 仅展示基础矩形，完整字形/图片/编辑器仍在后续工作范围。

Resources 包含实际 Shader 源与校验 artifact、normal、环境容器和配置；运行时 Cache 可重新生成。
环境无需原 HDR 和旁路环境 JSON。资源与样例均为本项目自制，未下载外部模型/环境图。
第三方解析器/JSON/编辑器代码许可证保留在 Licenses，功能使用与具体许可范围见源码仓库。

manifest.json 校验包内输入，source-inputs.json 记录构建相关源码/资源/脚本内容摘要。
Git HEAD 和 dirty 状态分开记录；dirty=true 时不得把截图称为该提交自身的结果。
自动测试从其他工作目录、仅系统 PATH 启动，逐字节比较两次第 30 帧并单独采集无截图的计时。
截图读回会显式等待 GPU，不能把采图帧当作正常性能数据；PostProcess 总计包含细项，不要相加。
