# Scripts 使用说明

本目录包含两类脚本：

- 项目专用脚本：针对 `Gles2AsyncRender` 的固定路径、固定目标和常用工作流。
- 通用 Qt 构建脚本：可复制到其它 Qt 项目，支持 `CMakeLists.txt` 和 `.pro`。

## 脚本列表

### `run-gles2asyncrender.ps1`

用于构建并启动当前项目的 Qt/qmake 版本应用。

适用场景：

- 本机手动运行 `Gles2AsyncRender.exe`。
- 使用 `.pro` / qmake / nmake 路径构建应用。
- 启动前可选关闭已有应用进程。

常用命令：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-gles2asyncrender.ps1 -Build -KillExisting
```

只启动已存在的程序：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-gles2asyncrender.ps1
```

运行并等待退出：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-gles2asyncrender.ps1 -Wait
```

### `build-gles2asyncrender.ps1`

用于当前项目的 CMake + Ninja 构建，默认目标是 `async_future_tests`。

适用场景：

- Codex / VS Code / 命令行快速构建测试目标。
- 避免 Windows 下 Codex 偶发吞输出或留下 `cmake.exe` / `ninja.exe` 残留进程。
- 构建输出自动写入 build 目录下的 `codex-build-logs/`。

常用命令：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gles2asyncrender.ps1 -Target async_future_tests -KillStale
```

构建并运行测试：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gles2asyncrender.ps1 -Target async_future_tests -RunTest -KillStale
```

重新配置 CMake：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gles2asyncrender.ps1 -Configure -Target async_future_tests
```

清理并重建目标：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gles2asyncrender.ps1 -CleanTarget -Target async_future_tests
```

如果本机 Qt 或 Visual Studio 路径不同，显式传入：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-gles2asyncrender.ps1 `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64 `
  -VsDevCmd "D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

### `qt-project-build.ps1`

通用 Qt 项目构建脚本。支持：

- `CMakeNinja`：CMake + Ninja，适合自动化、Codex、VS Code、CI、Linux/macOS。
- `CMakeVS`：CMake + Visual Studio generator，适合 Windows 下生成 `.sln` 调试和分析。
- `QMake`：qmake + `.pro`，用于旧 Qt 项目。
- `Auto`：优先使用 `CMakeLists.txt`，没有 CMake 时使用 `.pro`。

## 推荐构建目录

建议不同后端使用不同 build 目录，避免互相污染：

```text
build/cmake-ninja-debug/
build/cmake-ninja-release/
build/cmake-vs2022/
build/qmake-debug/
build/qmake-release/
```

当前项目已有的 CMake + Ninja 目录是：

```text
build/win-local-clang-debug/
```

## 通用脚本用法

CMake + Ninja：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -BuildSystem CMakeNinja `
  -Configuration Debug `
  -Target async_future_tests `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

使用当前项目已有的 Ninja build 目录：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -BuildSystem CMakeNinja `
  -Configuration Debug `
  -Target async_future_tests `
  -BuildDir build\win-local-clang-debug `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

CMake + Visual Studio，生成 `.sln`：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -BuildSystem CMakeVS `
  -Configuration Debug `
  -Configure `
  -ConfigureOnly `
  -BuildDir build\cmake-vs2022 `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

生成后可以打开：

```text
build/cmake-vs2022/Gles2AsyncRender.sln
```

CMake + Visual Studio 构建指定目标：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -BuildSystem CMakeVS `
  -Configuration Debug `
  -Target async_future_tests `
  -BuildDir build\cmake-vs2022 `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

qmake / `.pro`：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -BuildSystem QMake `
  -Configuration Debug `
  -QtRoot D:\CodePrograms\Qt\5.15.1\msvc2019_64
```

其它项目中使用：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\qt-project-build.ps1 `
  -ProjectRoot D:\path\to\OtherQtProject `
  -BuildSystem Auto `
  -Configuration Debug `
  -QtRoot D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

## 重要参数

`-BuildSystem`

选择构建后端：`Auto`、`CMakeNinja`、`CMakeVS`、`QMake`。

`-Configuration`

选择构建配置：`Debug`、`Release`、`RelWithDebInfo`、`MinSizeRel`。

`-Target`

指定构建目标。CMake 目标例如 `async_future_tests`、`execution_tests`、`Gles2AsyncRender`。

`-Configure`

强制重新配置 CMake 或 qmake。

`-ConfigureOnly`

只生成构建系统，不执行实际编译。适合生成 `.sln` 后手动用 Visual Studio 打开。

`-Clean`

执行构建系统的 clean 目标。

`-KillStale`

在 Windows 下清理当前 build 目录相关的残留构建进程。脚本会根据命令行是否包含当前 build 目录判断，不会主动清理所有 `cmake.exe` / `ninja.exe`。

`-TimeoutSeconds`

外部命令超时时间。超时后脚本会停止进程；Windows 下会尝试停止整棵进程树。

`-TailLines`

命令结束后打印日志尾部的行数。

`-QtRoot`

Qt 安装目录，例如：

```text
D:\CodePrograms\Qt\5.15.2\msvc2019_64
```

脚本会设置 `QTDIR`、`CMAKE_PREFIX_PATH`，并把 `QtRoot\bin` 加到 `Path` 前面。

`-VsDevCmd`

Visual Studio C++ 环境脚本。Windows 下使用 `CMakeVS` 或 `QMake` 时需要。

示例：

```text
D:\CodePrograms\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
```

## Windows / Codex 注意事项

Windows 下 Codex 运行 CMake、Ninja、MSBuild 等长命令时，可能出现命令已结束但输出没有及时返回、或中断后留下子进程的情况。

建议：

- 构建命令使用 `-KillStale`。
- 构建命令设置合理的 `-TimeoutSeconds`。
- 不要在同一轮 Codex 操作中并发运行多个长构建或长测试命令。
- 优先用 `CMakeNinja` 做自动化构建和测试。
- 用 `CMakeVS` 生成 `.sln` 后，在 Visual Studio 中调试和分析。
- 如果 Codex 没有显示完整输出，查看 build 目录下的日志：

```text
build/<build-dir>/qt-build-logs/
build/<build-dir>/codex-build-logs/
```

## 平台建议

Windows：

- 自动化/快速验证：`CMakeNinja`
- 调试/分析：`CMakeVS`
- 旧项目兼容：`QMake`

Linux/macOS：

- 优先使用 `CMakeNinja`。
- `.pro` 项目可使用 `QMake`。
- `CMakeVS` 仅支持 Windows。
