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

### 使用 `launcher.cfg`

注入器默认读取**注入器所在目录**中的 `launcher.cfg`。可以复制仓库中的
`launcher.cfg.example` 并改名为 `launcher.cfg`：

```ini
# launcher configuration - edit paths here, not in the .bat files
[launcher]
game_x86 = D:\Games\Grim Dawn\Grim Dawn.exe
game_x64 = D:\Games\Grim Dawn\x64\Grim Dawn.exe
dll_x86  = dpyes_ext-x86.dll
dll_x64  = dpyes_ext-x64.dll

injector = ignored
wait_settle = 6
wait_process = 40
```

两个注入器只读取与自身架构对应的字段：

- `dpyes_injector-x64.exe` 使用 `game_x64` 和 `dll_x64`
- `dpyes_injector-x86.exe` 使用 `game_x86` 和 `dll_x86`

配置中的绝对路径直接使用；相对路径以 `launcher.cfg` 所在目录为基准，
所以 DLL 通常只需填写文件名。路径支持 `%ENV_VAR%` 环境变量，并可使用成对的
单引号或双引号。配置文件支持 UTF-8、UTF-8 BOM、UTF-16 BOM，以及系统 ANSI
代码页，因此包含中文的 Windows 路径也可以读取。

`injector`、`wait_settle`、`wait_process` 和其他未知字段会被忽略。它们可以保留，
以兼容已有的启动配置。

如果配置指定的游戏已经运行，注入器会同时核对**进程架构和可执行文件完整路径**，
避免机器上存在多个 Grim Dawn 版本时注入到错误进程；否则启动配置指定的游戏，
再注入对应 DLL。

### 未提供配置文件时

如果注入器旁边没有 `launcher.cfg`，仍保留旧的默认行为。把四个构建产物复制到
Grim Dawn 安装目录，然后运行对应架构的注入器：

- x64 注入器默认使用 `dpyes_ext-x64.dll`，并启动 `x64\Grim Dawn.exe`
- x86 注入器默认使用 `dpyes_ext-x86.dll`，并启动根目录下的 `Grim Dawn.exe`

如果已有同架构的 `Grim Dawn.exe` 正在运行，则直接附加到现有进程。

### 命令行参数

```text
--pid PID       注入指定 PID
--dll PATH      使用指定 DLL
--exe PATH      查找或启动指定完整路径的可执行文件
--no-launch     只附加，不自动启动游戏
--help          显示帮助
```

路径的优先级是：

```text
--dll / --exe 命令行参数 > launcher.cfg > 内置默认路径
```

例如：

```bat
dpyes_injector-x64.exe
dpyes_injector-x64.exe --exe "D:\Games\Grim Dawn\x64\Grim Dawn.exe"
dpyes_injector-x86.exe --pid 12345
```

注入器只会读取与其同目录的 `launcher.cfg`，不接受通过命令行指定其他配置文件。
同目录没有 `launcher.cfg` 时会自动回退到旧行为；配置文件存在但无法解析时会报错退出。
如果 Grim Dawn 以管理员身份运行，注入器也需要以管理员身份运行。

## 第三方代码

Dear ImGui v1.92.9 源码位于 `third_party/imgui`，上游项目为 `ocornut/imgui`。MinHook 位于 `third_party/minhook`。各自许可证保留在对应目录中。
