# VSCode CMake 配置说明

本项目已配置 VSCode CMake Tools 扩展以支持多编译器构建。

## 前置要求

1. 安装 VSCode 扩展：
   - CMake Tools (ms-vscode.cmake-tools)
   - 可选：C/C++ (ms-vscode.cpptools)

2. 确保 Ninja 在系统 PATH 中可用

## 初次设置

**重要**: 配置完成后，需要让 VSCode 重新扫描 CMake Kits：

**方法一（推荐）**: 重新加载窗口
- 按 `Ctrl+Shift+P`
- 输入 `Developer: Reload Window`
- 回车

**方法二**: 手动扫描
- 按 `Ctrl+Shift+P`
- 输入 `CMake: Scan for Kits`
- 回车

完成后，你应该能在 Kit 选择器中看到：clang-cl、msvc、clang、mingw 四个选项。

## 快速开始

### 1. 选择编译器（Kit）

- 点击 VSCode 底部状态栏的 Kit 选择器
- 或按 `Ctrl+Shift+P` 输入 `CMake: Select a Kit`
- 选择以下之一：
  - **clang-cl** - Clang with MSVC interface (推荐)
  - **msvc** - Microsoft Visual C++
  - **clang** - LLVM Clang
  - **mingw** - MinGW64 GCC

### 2. 选择构建类型（Variant）

- 点击 VSCode 底部状态栏的构建类型
- 或按 `Ctrl+Shift+P` 输入 `CMake: Select Variant`
- 选择：Debug / Release / RelWithDebInfo / MinSizeRel

### 3. 配置和构建

- **配置**: 按 `F7` 或点击状态栏 "Configure"
- **构建**: 按 `F7` 或点击状态栏 "Build"
- **运行测试**: 点击状态栏测试图标

## 构建目录

构建文件将生成在：`build-{kit}-{buildType}/`

示例：
- `build-clang-cl-release/`
- `build-msvc-debug/`
- `build-mingw-release/`

## 配置文件说明

### `.vscode/settings.json`
- 全局 CMake 配置
- Qt 路径配置
- 构建目录格式

### `.vscode/cmake-kits.json`
- 定义可用的编译器工具链
- 配置编译器路径和环境

### `.vscode/cmake-variants.json`
- 定义构建类型（Debug/Release 等）

## 自定义配置

### 修改 Qt 路径

编辑 `.vscode/settings.json`:
```json
"cmake.configureArgs": [
    "-DCMAKE_PREFIX_PATH=你的Qt路径"
]
```

### 添加新的编译器

编辑 `.vscode/cmake-kits.json`，参考现有配置添加新条目。

### 修改编译器路径

如果编译器路径不同，请修改 `.vscode/cmake-kits.json` 中的路径。

## 注意事项

1. **clang-cl 和 msvc** 需要 Visual Studio 2022
2. **mingw** 需要 MinGW64 安装在 `D:\DevPrograms\msys64\mingw64`
3. **MinGW 构建**需要使用 MinGW 编译的 Qt（如 `mingw_64`）而非 MSVC 版本
4. 首次使用时可能需要重新加载 VSCode 窗口

## 常见问题

### Q: 找不到编译器
A: 确认编译器路径正确，并且 PATH 环境变量包含必要的工具（如 Ninja）

### Q: CMake 配置失败
A: 检查 Qt 路径是否正确，确认所有必要的工具已安装

### Q: 切换 Kit 后需要重新配置吗？
A: 是的，切换 Kit 后需要重新运行 CMake 配置（按 F7）
