# Taskbar Full Width Progress · 任务栏全宽进度条

将 Windows 11 任务栏的任务进度条扩展到整个按钮底部，尤其适合显示窗口标题的长按钮。Windhawk 模组，当前版本 **0.1.0**。

## 功能

- 仅在应用提供任务进度时扩展进度条，普通运行状态的小横条保持原样。
- 保留真实进度比例：50% 进度仍只填充一半。
- 保留暂停、错误颜色和不确定进度动画。
- 默认左右各留 3 个逻辑像素，可设为 0 铺到两端；按钮宽度变化后随之更新。
- 禁用时恢复本模组修改的属性。

## 安装

1. 安装 [Windhawk](https://windhawk.net/)。需要 Windows 11 x64 原生水平任务栏。
2. 打开 [taskbar-full-width-progress.wh.cpp](taskbar-full-width-progress.wh.cpp)，复制完整源码。
3. Windhawk → 创建新模组 → 全选替换代码 → 编译并启用。
4. 首次启用后，在任务栏按钮上移动鼠标或切换窗口；副屏首次使用也同样操作。

不支持 ExplorerPatcher / StartAllBack 经典任务栏。避免同时使用其他模组修改 `ProgressIndicator` 的宽度、对齐或边距。它只显示应用提供的系统任务进度，不会把网页内的进度转换成任务栏进度。

## 本地编译

```powershell
.\build.ps1 -WindhawkRoot 'C:\Program Files\Windhawk'
```

仅支持 x64，生成文件位于 `build/`。脚本只编译，不安装或启用模组。

此前已通过编译、当前系统符号检查和隔离 XAML 测试；真实任务栏效果仍需验收。

## 来源与许可

由 SinCircle 维护。原生接口访问与固定图标列的负边距方案参考 m417z 的 [Taskbar Labels](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-labels.wh.cpp)，保留源码中的来源说明。本模组使用 [GPL-3.0-or-later](LICENSE)。
