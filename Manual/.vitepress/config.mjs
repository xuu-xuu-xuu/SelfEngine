import { defineConfig } from 'vitepress'

export default defineConfig({
  base: process.env.FLARE_DOCS_BASE || '/',
  lang: 'zh-CN',
  title: 'Flare 开发者文档',
  description: 'Flare 引擎的功能、类型与接口',
  appearance: 'force-dark',
  themeConfig: {
    siteTitle: 'Flare 开发者文档',
    nav: [{ text: '引擎源码', link: 'https://github.com/xuu-xuu-xuu/FlareEngine' }],
    sidebar: [
      { text: "开发者文档", link: '/' },
      { text: "渲染与光照", collapsed: true, items: [
        { text: "灯光组件", link: "/runtime/light" },
        { text: "前向管线与帧图", link: "/render/pipeline" },
        { text: "光照着色、材质接口与 Shader 包", link: "/render/materials" },
        { text: "阴影、分簇灯光与环境遮蔽", link: "/render/shadows" },
        { text: "环境光照、大气与反射", link: "/render/environment" },
        { text: "后处理、抗锯齿与调色", link: "/render/post" },
        { text: "可见性、Hi-Z 与间接绘制", link: "/render/visibility" },
        { text: "LOD、HLOD 与代理几何", link: "/render/detail" },
        { text: "D3D12 设备、命令与 GPU 资源寿命", link: "/render/device" },
        { text: "交换链、绘制与读回", link: "/render/presentation" }
      ] },
      { text: "世界与运行时", collapsed: true, items: [
        { text: "实体、层级与组件存储", link: "/runtime/entities" },
        { text: "组件基类与生命周期", link: "/runtime/component-base" },
        { text: "World 的灯光接口", link: "/runtime/world" },
        { text: "原生组件反射与属性序列化", link: "/runtime/metadata" },
        { text: "场景加载、相机与空间查询", link: "/runtime/scenes" },
        { text: "主循环、任务顺序与事件", link: "/runtime/scheduling" },
        { text: "昼夜、天空与场景环境", link: "/runtime/daylight" },
        { text: "刚体、碰撞与角色控制", link: "/runtime/physics" }
      ] },
      { text: "资产与资源", collapsed: true, items: [
        { text: "FBX 导入", link: "/assets/import" },
        { text: "模型、纹理与材质资产", link: "/assets/models" },
        { text: "文件解码、引用与派生缓存", link: "/assets/storage" },
        { text: "异步加载、上传与纹理驻留", link: "/assets/loading" }
      ] },
      { text: "C# 脚本与宿主", collapsed: true, items: [
        { text: "C# 行为、绑定与事件", link: "/scripts/behaviours" },
        { text: "脚本反射、程序集卸载与 GC", link: "/scripts/lifetime" },
        { text: "托管宿主与原生 ABI", link: "/scripts/bridge" }
      ] },
      { text: "UI 系统", collapsed: true, items: [
        { text: "自研 UI 文档、样式与布局", link: "/ui/toolkit" },
        { text: "字体图集、文本编辑与 DPI", link: "/ui/text" },
        { text: "控件状态、列表与叠层", link: "/ui/widgets" },
        { text: "游戏 UI、绑定与合成", link: "/ui/game" }
      ] },
      { text: "编辑器", collapsed: true, items: [
        { text: "项目入口、启动进度与编辑器外壳", link: "/editor/workspace" },
        { text: "面板模型、默认场景与界面合成", link: "/editor/panels" },
        { text: "面板拖出与吸附", link: "/editor/docking" },
        { text: "场景视口、变换手柄与选中描边", link: "/editor/viewport" },
        { text: "编辑会话、撤销与播放", link: "/editor/session" },
        { text: "资产操作、属性面板与项目文档", link: "/editor/documents" }
      ] },
      { text: "平台与基础服务", collapsed: true, items: [
        { text: "从窗口消息到一帧画面", link: "/platform/frame" },
        { text: "输入、文字事件与窗口适配", link: "/platform/input" },
        { text: "基础服务、任务与诊断", link: "/platform/services" }
      ] },
      { text: "构建与分发", collapsed: true, items: [
        { text: "构建工具、预览与导出", link: "/tools/delivery" },
        { text: "构建环境、资源复制与包内启动", link: "/tools/package" }
      ] }
    ],
    outline: { level: [2, 3], label: '本页内容' },
    sidebarMenuLabel: '目录',
    returnToTopLabel: '回到顶部',
    docFooter: { prev: '上一篇', next: '下一篇' },
    search: {
      provider: 'local',
      options: {
        miniSearch: {
          options: {
            tokenize: (text) => Array.from(
              new Intl.Segmenter('zh-CN', { granularity: 'word' }).segment(text)
            ).filter(part => part.isWordLike).map(part => part.segment)
          }
        },
        translations: {
          button: { buttonText: '搜索文档', buttonAriaLabel: '搜索文档' },
          modal: {
            displayDetails: '显示详情', resetButtonTitle: '清空搜索',
            backButtonTitle: '关闭搜索', noResultsText: '没有找到相关条目',
            footer: { selectText: '打开', navigateText: '切换结果', closeText: '关闭' }
          }
        }
      }
    }
  }
})
