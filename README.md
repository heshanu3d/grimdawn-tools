# dpyes-ext

Grim Dawn 的 DPYes DPS 统计功能重实现。目前保留原有 Win32 外部统计窗口，并增加了一个用于验证游戏内渲染链路的 Dear ImGui 简单窗口。

## 游戏内 ImGui 测试窗口

DLL 会通过 MinHook 同时安装两套渲染 Hook：

- Direct3D 11：`IDXGISwapChain::Present` 和 `ResizeBuffers`
- Direct3D 9：`IDirect3DDevice9::EndScene` 和 `Reset`

实际游戏开始渲染后，最先收到真实游戏帧的后端会初始化 ImGui。游戏内窗口会显示当前 DirectX 后端、DLL 架构和帧率。

按 `F10` 显示或隐藏 ImGui 测试窗口。现阶段 DPS 统计 UI 仍是外部 Win32 窗口，尚未迁移到 ImGui。

## 构建

需要 llvm-mingw，确保下列编译器位于 `PATH`，或者设置 `LLVM_MINGW_ROOT`：

```text
x86_64-w64-mingw32-clang
x86_64-w64-mingw32-clang++
i686-w64-mingw32-clang
i686-w64-mingw32-clang++
```

一次构建 x64 和 x86：

```bash
./build.sh
```

生成：

```text
dpyes_ext-x64.dll
dpyes_injector-x64.exe
dpyes_ext-x86.dll
dpyes_injector-x86.exe
```

也可以只构建一个架构：

```bash
ARCH=x64 ./build.sh
ARCH=x86 ./build.sh
```

## 注入和启动

注入器和目标进程必须具有相同架构：

```text
dpyes_injector-x64.exe  -> dpyes_ext-x64.dll  -> x64 Grim Dawn
dpyes_injector-x86.exe  -> dpyes_ext-x86.dll  -> x86 Grim Dawn
```

### 默认方式

把四个构建产物复制到 Grim Dawn 安装目录，然后运行对应架构的注入器。

如果已运行对应架构的 `Grim Dawn.exe`，注入器会附加到现有进程。如果没有运行游戏：

- x64 注入器默认启动 `x64\Grim Dawn.exe`
- x86 注入器默认启动根目录下的 `Grim Dawn.exe`

### 命令行参数

```text
--pid PID       注入指定 PID
--dll PATH      使用指定 DLL
--exe PATH      找不到运行中的游戏时启动指定可执行文件
--no-launch     只附加，不自动启动游戏
--help          显示帮助
```

例如：

```bat
dpyes_injector-x64.exe --exe "D:\Games\Grim Dawn\x64\Grim Dawn.exe"
dpyes_injector-x86.exe --pid 12345
```

如果 Grim Dawn 以管理员身份运行，注入器也需要以管理员身份运行。

## 第三方代码

Dear ImGui v1.92.9 源码位于 `third_party/imgui`，上游项目为 `ocornut/imgui`。MinHook 位于 `third_party/minhook`。各自许可证保留在对应目录中。
