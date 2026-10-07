// ==WindhawkMod==
// @id              taskbar-full-width-progress
// @name            Taskbar Full Width Progress
// @name:zh-CN      任务栏进度条铺满底部
// @description     Expand active task progress across the taskbar button while preserving the native progress value and running indicator.
// @description:zh-CN 仅将任务进度条扩展到整个按钮底部，保留真实进度，普通运行横条保持原样。
// @version         0.1.0
// @author          Local
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject
// @license         GPL-3.0-or-later
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# 任务栏进度条铺满底部

用于 Windows 11 x64 原生任务栏，尤其适合显示窗口标题的长按钮。

- 只有应用正在显示任务进度时才扩展底部进度条。
- 进度轨道覆盖整个按钮底部；50% 进度仍填充一半，并非强制显示完成。
- 保留 Windows 的普通、暂停、错误颜色和不确定进度动画。
- 普通运行状态横条不改动；不强制显示原本隐藏的进度。
- 默认左右各留 3 个逻辑像素，设置为 0 可延伸到按钮两端。
- 按钮标题展开、收起或任务栏宽度变化后约 100 ms 内更新宽度。
- 在各任务栏 UI 线程分别管理按钮；禁用时恢复本模组修改的属性。

安装：Windhawk → 创建新模组 → 全选替换代码 → 编译并启用。
首次启用后，在任务栏按钮上移动鼠标或切换窗口；副屏首次使用也同样操作。
本模组依靠应用提供的系统任务进度，不会把聊天窗口里的文字或网页进度转换成任务栏进度。

仅适用于原生水平任务栏，不支持 ExplorerPatcher / StartAllBack 经典任务栏。
不要同时让 Taskbar Styler 或其他模组修改 ProgressIndicator 的宽度、对齐和边距。
找不到所需内部接口时初始化失败，不会尝试修改未知地址。

## 来源和许可

原生接口访问与固定图标列的负边距方案参考 m417z 的 GPL-3.0 项目：
https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-labels.wh.cpp
本文件以 GPL-3.0-or-later 发布。
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- edgeInset: 3
  $name: 左右留白（逻辑像素）
  $description: 默认 3，设为 0 可延伸到按钮两端。范围 0–24，跟随 Windows 显示缩放。
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::UI::Xaml;
using winrt::Windows::Foundation::IInspectable;

std::atomic<bool> g_stopping{false}, g_ready{false}, g_initializing{false};
std::atomic<int> g_inset{3};
HMODULE g_module = nullptr;
void* g_buttonVtable = nullptr;
constexpr wchar_t kClass[] = L"Windhawk.FullWidthProgress.Controller.v1";
constexpr UINT kStop = WM_APP + 0x5A1;
constexpr UINT kRefresh = WM_APP + 0x5A2;
constexpr UINT_PTR kTimer = 1;
thread_local bool g_updating = false;

FrameworkElement FindChild(FrameworkElement const& parent, PCWSTR name) {
    for (int i = 0, n = Media::VisualTreeHelper::GetChildrenCount(parent); i < n; ++i) {
        auto child = Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (child && child.Name() == name) return child;
    }
    return nullptr;
}

FrameworkElement FindDescendant(FrameworkElement const& parent, PCWSTR name, int depth = 0) {
    if (!parent || depth > 8) return nullptr;
    if (parent.Name() == name) return parent;
    for (int i = 0, n = Media::VisualTreeHelper::GetChildrenCount(parent); i < n; ++i) {
        auto child = Media::VisualTreeHelper::GetChild(parent, i).try_as<FrameworkElement>();
        if (auto result = FindDescendant(child, name, depth + 1)) return result;
    }
    return nullptr;
}

enum class ValueKind { Number, Integer, Alignment, Margin };
bool EqualValue(IInspectable const& a, IInspectable const& b, ValueKind kind) {
    if (!a || !b) return a == b;
    switch (kind) {
    case ValueKind::Number: {
        double x = winrt::unbox_value<double>(a), y = winrt::unbox_value<double>(b);
        return x == y || (std::isnan(x) && std::isnan(y)) || std::abs(x - y) < 0.01;
    }
    case ValueKind::Integer: return winrt::unbox_value<int>(a) == winrt::unbox_value<int>(b);
    case ValueKind::Alignment:
        return winrt::unbox_value<HorizontalAlignment>(a) == winrt::unbox_value<HorizontalAlignment>(b);
    case ValueKind::Margin: {
        auto x = winrt::unbox_value<Thickness>(a), y = winrt::unbox_value<Thickness>(b);
        return x == y;
    }
    }
    return false;
}

struct SavedProperty {
    DependencyProperty property{nullptr};
    IInspectable originalLocal{nullptr};
    IInspectable applied{nullptr};
    ValueKind kind;
};

// Only local layout values are changed. Bindings are never overwritten.
// Changes made by Windows/another mod after our write take precedence on restore.
struct LayoutPatch {
    winrt::weak_ref<FrameworkElement> element;
    std::vector<SavedProperty> properties;

    void Restore() noexcept {
        try {
            if (auto target = element.get()) {
                for (auto it = properties.rbegin(); it != properties.rend(); ++it) {
                    try {
                        if (target.GetBindingExpression(it->property) ||
                            !EqualValue(target.GetValue(it->property), it->applied, it->kind)) continue;
                        if (it->originalLocal == DependencyProperty::UnsetValue()) target.ClearValue(it->property);
                        else target.SetValue(it->property, it->originalLocal);
                    } catch (...) {}
                }
            }
        } catch (...) {}
        properties.clear();
        element = {};
    }

    void Target(FrameworkElement const& target) {
        if (element.get() != target) {
            Restore();
            element = winrt::make_weak(target);
        }
    }

    void Set(DependencyProperty const& property, IInspectable const& value, ValueKind kind) {
        auto target = element.get();
        if (!target || target.GetBindingExpression(property)) return;
        auto current = target.GetValue(property);
        if (EqualValue(current, value, kind)) return; // No layout invalidation when unchanged.
        auto found = std::find_if(properties.begin(), properties.end(), [&](auto const& item) {
            return item.property == property;
        });
        if (found == properties.end()) {
            properties.push_back({property, target.ReadLocalValue(property), value, kind});
        } else {
            if (!EqualValue(current, found->applied, kind)) found->originalLocal = target.ReadLocalValue(property);
            found->applied = value;
        }
        target.SetValue(property, value);
    }
};

struct ButtonState {
    winrt::weak_ref<FrameworkElement> button;
    LayoutPatch indicator, track;
    void Restore() noexcept { track.Restore(); indicator.Restore(); }
};

void UpdateButton(ButtonState& state, FrameworkElement const& button) {
    auto panel = FindChild(button, L"IconPanel").try_as<Controls::Grid>();
    auto progress = panel ? FindChild(panel, L"ProgressIndicator") : nullptr;
    if (!progress || progress.Visibility() != Visibility::Visible || progress.Opacity() <= 0 ||
        progress.Width() == 0) {
        state.Restore();
        return;
    }
    // Keep the indicator in the fixed icon column. Spanning the auto-sized label
    // column feeds ActualWidth into DesiredSize and causes taskbar layout cycles.
    const auto columns = panel.ColumnDefinitions();
    if (columns.Size() != 2 || columns.GetAt(0).Width().GridUnitType != GridUnitType::Pixel ||
        Controls::Grid::GetColumn(progress) != 0) {
        state.Restore();
        return;
    }
    const double width = button.ActualWidth();
    const double inset = std::min(static_cast<double>(g_inset.load()), width / 2);
    const double barWidth = width - 2 * inset;
    const double columnWidth = columns.GetAt(0).Width().Value;
    if (!std::isfinite(barWidth) || barWidth <= 0 || columnWidth <= 0) return;
    for (auto const& dp : {FrameworkElement::MinWidthProperty(), FrameworkElement::MaxWidthProperty(),
                          FrameworkElement::MarginProperty(), FrameworkElement::HorizontalAlignmentProperty(),
                          Controls::Grid::ColumnSpanProperty()}) {
        if (progress.GetBindingExpression(dp)) { state.Restore(); return; }
    }
    const auto origin = panel.TransformToVisual(button).TransformPoint({0, 0});
    const double left = inset - origin.X - panel.Padding().Left;
    auto margin = progress.Margin();
    margin.Left = left;
    margin.Right = -std::max(0.0, left + barWidth - columnWidth);

    state.indicator.Target(progress);
    state.indicator.Set(Controls::Grid::ColumnSpanProperty(), winrt::box_value(1), ValueKind::Integer);
    state.indicator.Set(FrameworkElement::MaxWidthProperty(), winrt::box_value(barWidth), ValueKind::Number);
    state.indicator.Set(FrameworkElement::MinWidthProperty(), winrt::box_value(barWidth), ValueKind::Number);
    state.indicator.Set(FrameworkElement::MarginProperty(), winrt::box_value(margin), ValueKind::Margin);
    state.indicator.Set(FrameworkElement::HorizontalAlignmentProperty(),
                        winrt::box_value(HorizontalAlignment::Left), ValueKind::Alignment);

    // Expand the native track, never the filled segment and never Value/Maximum.
    auto track = FindDescendant(progress, L"ProgressBarTrack");
    if (track) {
        state.track.Target(track);
        state.track.Set(FrameworkElement::MinWidthProperty(), winrt::box_value(barWidth), ValueKind::Number);
    } else state.track.Restore();
}

struct Controller {
    HWND window = nullptr;
    bool published = false;
    std::unordered_map<void*, ButtonState> buttons;
    std::vector<winrt::weak_ref<UIElement>> roots;
};
thread_local Controller* g_controller = nullptr;
std::mutex g_windowsMutex;
std::vector<HWND> g_windows;

void Refresh(Controller& controller) {
    if (g_updating) return;
    g_updating = true;
    for (auto it = controller.buttons.begin(); it != controller.buttons.end();) {
        bool expired = false;
        try {
            if (auto button = it->second.button.get()) UpdateButton(it->second, button);
            else expired = true;
        } catch (...) { it->second.Restore(); }
        if (expired) { it->second.Restore(); it = controller.buttons.erase(it); }
        else ++it;
    }
    g_updating = false;
}

void Stop(Controller& controller) noexcept {
    for (auto& item : controller.buttons) item.second.Restore();
    controller.buttons.clear();
    controller.roots.clear();
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto controller = reinterpret_cast<Controller*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        controller = static_cast<Controller*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controller));
    }
    if (controller) {
        if (message == kStop) {
            KillTimer(window, kTimer);
            Stop(*controller);
            DestroyWindow(window);
            return 0;
        }
        if ((message == WM_TIMER && wParam == kTimer) || message == kRefresh) {
            if (!g_stopping) Refresh(*controller);
            return 0;
        }
        if (message == WM_NCDESTROY && controller->published) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            KillTimer(window, kTimer);
            Stop(*controller);
            { std::lock_guard lock(g_windowsMutex); std::erase(g_windows, window); }
            g_controller = nullptr;
            delete controller;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

Controller* EnsureController() {
    if (g_controller) return g_controller;
    auto controller = std::make_unique<Controller>();
    controller->window = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                        nullptr, g_module, controller.get());
    if (!controller->window) return nullptr;
    if (!SetTimer(controller->window, kTimer, 100, nullptr)) {
        DestroyWindow(controller->window);
        return nullptr;
    }
    {
        std::lock_guard lock(g_windowsMutex);
        // Serialize publication with unload's snapshot, so no controller can
        // retain a window procedure into an unloaded DLL.
        if (g_stopping) { DestroyWindow(controller->window); return nullptr; }
        g_windows.push_back(controller->window);
        controller->published = true;
    }
    g_controller = controller.release();
    return g_controller;
}

void Track(Controller& controller, FrameworkElement const& button) {
    auto key = winrt::get_abi(button);
    auto& state = controller.buttons[key];
    if (state.button.get() != button) { state.Restore(); state.button = winrt::make_weak(button); }
}

void ScanTree(Controller& controller, DependencyObject const& node, int depth, int& budget) {
    if (!node || depth > 48 || --budget < 0) return;
    if (winrt::get_class_name(node) == L"Taskbar.TaskListButton") {
        if (auto button = node.try_as<FrameworkElement>()) Track(controller, button);
        return;
    }
    for (int i = 0, n = Media::VisualTreeHelper::GetChildrenCount(node); i < n && budget > 0; ++i)
        ScanTree(controller, Media::VisualTreeHelper::GetChild(node, i), depth + 1, budget);
}

void Observe(void* implementation) noexcept {
    if (!g_ready || g_stopping || g_updating) return;
    g_updating = true;
    try {
        // Check the exact ITaskListButton vtable before using the upstream ABI.
        auto abi = reinterpret_cast<void**>(implementation) + 3;
        if (*abi == g_buttonVtable) {
            winrt::Windows::Foundation::IUnknown unknown;
            winrt::copy_from_abi(unknown, abi);
            auto button = unknown.as<FrameworkElement>();
            if (auto controller = EnsureController()) {
                Track(*controller, button);
                auto root = button.XamlRoot();
                auto content = root ? root.Content() : nullptr;
                if (content) {
                    std::erase_if(controller->roots, [](auto const& item) { return !item.get(); });
                    auto found = std::find_if(controller->roots.begin(), controller->roots.end(),
                        [&](auto const& item) { return item.get() == content; });
                    if (found == controller->roots.end()) {
                        controller->roots.push_back(winrt::make_weak(content));
                        int budget = 10000;
                        ScanTree(*controller, content, 0, budget);
                    }
                }
                UpdateButton(controller->buttons.at(winrt::get_abi(button)), button);
            }
        }
    } catch (...) { /* Unloaded or unsupported controls retain native behavior. */ }
    g_updating = false;
}

using VisualUpdate = void(WINAPI*)(void*);
VisualUpdate g_updateVisualStates = nullptr;
void WINAPI VisualStatesHook(void* implementation) {
    g_updateVisualStates(implementation);
    Observe(implementation);
}

bool HookTaskbar(HMODULE module) {
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const winrt::impl::produce<struct winrt::Taskbar::implementation::TaskListButton,struct winrt::Taskbar::ITaskListButton>::`vftable')"}, &g_buttonVtable},
        {{LR"(private: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateVisualStates(void))"}, &g_updateVisualStates, VisualStatesHook},
    };
    if (!WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks))) {
        Wh_Log(L"Required native taskbar symbols unavailable. Initialization stopped.");
        return false;
    }
    return true;
}

HMODULE TaskbarModule() {
    auto module = GetModuleHandleW(L"Taskbar.View.dll");
    return module ? module : GetModuleHandleW(L"ExplorerExtensions.dll");
}

using LoadLibraryFunction = decltype(&LoadLibraryExW);
LoadLibraryFunction g_loadLibrary = nullptr;
void TryLateInit(HMODULE module) {
    if (!module || g_ready || g_stopping || module != TaskbarModule() || g_initializing.exchange(true)) return;
    if (HookTaskbar(module)) { Wh_ApplyHookOperations(); g_ready = true; }
}
HMODULE WINAPI LoadLibraryHook(LPCWSTR path, HANDLE file, DWORD flags) {
    auto module = g_loadLibrary(path, file, flags);
    TryLateInit(module);
    return module;
}

void LoadSettings() { g_inset = std::clamp(Wh_GetIntSetting(L"edgeInset"), 0, 24); }

BOOL Wh_ModInit() {
    LoadSettings();
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&Wh_ModInit), &g_module);
    WNDCLASSW cls{};
    cls.hInstance = g_module;
    cls.lpfnWndProc = ControllerProc;
    cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls)) return FALSE;
    if (auto module = TaskbarModule()) {
        g_initializing = true;
        if (!HookTaskbar(module)) { UnregisterClassW(kClass, g_module); return FALSE; }
        g_ready = true;
    } else {
        auto kernel = GetModuleHandleW(L"kernelbase.dll");
        auto function = reinterpret_cast<LoadLibraryFunction>(GetProcAddress(kernel, "LoadLibraryExW"));
        if (!WindhawkUtils::SetFunctionHook(function, LoadLibraryHook, &g_loadLibrary)) {
            UnregisterClassW(kClass, g_module);
            return FALSE;
        }
    }
    return TRUE;
}

void Wh_ModAfterInit() { if (!g_ready) TryLateInit(TaskbarModule()); }
void Wh_ModSettingsChanged() {
    LoadSettings();
    std::lock_guard lock(g_windowsMutex);
    for (auto window : g_windows) PostMessageW(window, kRefresh, 0, 0);
}
void Wh_ModBeforeUninit() {
    std::vector<HWND> windows;
    { std::lock_guard lock(g_windowsMutex); g_stopping = true; windows = g_windows; }
    // Synchronous cleanup on each owning UI thread; no timers/delegates remain.
    for (auto window : windows) SendMessageW(window, kStop, 0, 0);
}
void Wh_ModUninit() { g_ready = false; UnregisterClassW(kClass, g_module); }
