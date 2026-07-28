# SDK Version Manager (SVM)

简体中文 | [English](README.en.md)

SDK Version Manager（SVM）是一个基于 Qt 6 的本地优先、跨平台开发环境管理工具。它通过桌面 GUI 和 `svm` 命令行统一管理项目使用的 SDK 版本。

> [!IMPORTANT]
> 项目目前处于早期开发阶段（`0.1`），优先支持 Windows。当前可用 Provider 为 **Node.js** 和 **Flutter**；Java、Python、Web 服务、数据库等能力仍在规划中。

## 下载与安装

1. 打开 [GitHub Releases](https://github.com/nvmms/sdk_version_manager/releases/latest)；
2. 根据系统架构下载 Windows 安装程序：
   - `SVM-<版本>-windows-x64-setup.exe`：适用于大多数 Intel / AMD Windows 电脑；
   - `SVM-<版本>-windows-arm64-setup.exe`：适用于 ARM64 Windows 设备；
3. 运行安装程序并完成安装；
4. 从开始菜单启动 **SDK Version Manager**。

默认安装目录为：

```text
%LOCALAPPDATA%\Programs\SDK Version Manager
```

安装程序会创建 GUI 的开始菜单快捷方式。目前不会自动将 CLI 加入 `PATH`。如需在任意终端直接运行 `svm`，请将下面的目录加入当前用户的 `PATH`：

```text
%LOCALAPPDATA%\Programs\SDK Version Manager\bin
```

也可以直接使用完整路径：

```powershell
& "$env:LOCALAPPDATA\Programs\SDK Version Manager\bin\svm.exe" --help
```

## 使用方法

### GUI

1. 启动 **SDK Version Manager**；
2. 选择 Node.js 或 Flutter；
3. 首次进入会优先读取本地版本缓存；需要获取最新版本时点击刷新；
4. 筛选并选择目标版本，然后下载、安装；
5. 按需将已安装版本设置为全局默认版本。

下载和安装期间可以在界面中查看进度或取消操作。

### CLI

在项目目录中运行：

```text
# 查看帮助
svm --help

# 初始化项目，创建 .svmrc 和 .svm/
svm init

# 绑定项目版本；未安装时会自动下载并安装
svm use node 24.0.0
svm use flutter 3.38.0

# 查看项目绑定和全局默认版本
svm use

# 按解析规则选择版本并写入当前项目
svm use node

# 查看本地缓存中的版本
svm list
svm list all node
svm list lts node
svm list stable flutter
svm list downloaded node

# 使用项目绑定的 SDK 执行原生命令
svm node --version
svm node app.js
svm flutter doctor
```

`svm use <provider> [version]` 的版本选择顺序为：

1. 命令中显式指定的版本；
2. 最近的 `.svmrc` 项目绑定；
3. 全局默认版本；
4. 最新的已安装版本。

当前 CLI 支持的 Provider ID 为 `node` 和 `flutter`。

## 当前功能

- 在 GUI 中浏览、刷新和筛选 Node.js / Flutter 版本；
- 下载、校验、安装和取消 SDK 任务；
- 支持断点续传，并使用缓存优先的版本索引；
- 设置全局默认 SDK 版本；
- 使用 `.svmrc` 为项目绑定多个 SDK 版本；
- 从当前目录向父目录查找最近的 `.svmrc`；
- 通过 `svm node ...` 和 `svm flutter ...` 代理执行项目选定的 SDK；
- GUI 与 CLI 通过本机 EventBus 同步任务状态；
- 构建 Windows x64 / ARM64 安装程序。

## 项目配置

`svm init` 会在当前目录中：

- 创建可提交到版本控制的 `.svmrc`；
- 创建用于本机生成内容的 `.svm/`；
- 将 `.svm/` 添加到 `.gitignore`（不会重复添加）。
- `svm use` 在 `.svm/sdks/<provider>` 创建指向全局受管理版本的项目级目录链接；
  该链接可随时根据 `.svmrc` 重建，不是版本解析的事实来源。
- 无参数 `svm use` 在显示当前绑定时也会检查并修复缺失或过期的项目级目录链接。
- Windows 无符号链接权限时自动回退为目录联接（junction），无需管理员权限。

`.svmrc` 示例：

```json
{
  "schemaVersion": 1,
  "sdks": {
    "flutter": "3.38.0",
    "node": "24.0.0"
  }
}
```

配置只保存 Provider 与版本，不保存机器相关的绝对安装路径。项目绑定优先于全局默认版本。

## 从源码构建

### 环境要求

- Windows 10/11；
- Qt 6.10 或兼容版本，包含 Qt Quick、Qt Network 和 Qt Concurrent；
- CMake 3.16 或更高版本；
- 支持 C++17 的编译器（推荐 Visual Studio 2022）。

克隆并构建：

```powershell
git clone https://github.com/nvmms/sdk_version_manager.git
cd sdk_version_manager
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

构建目标：

- `appsdk_version_manager`：桌面 GUI；
- `svm`：命令行工具。

### 创建发布目录

```powershell
cmake --install build --config Release --prefix release
```

该命令会部署程序、Qt 运行库及所需插件到 `release/`。

### 打包 Windows 安装程序

先安装 [NSIS](https://nsis.sourceforge.io/)，然后在仓库根目录运行：

```powershell
New-Item -ItemType Directory -Force dist
makensis `
  "/DAPP_VERSION=0.1.0" `
  "/DAPP_ARCH=x64" `
  "/DRELEASE_DIR=$((Resolve-Path release).Path)" `
  "/DOUTPUT_FILE=$((Resolve-Path dist).Path)\SVM-0.1.0-windows-x64-setup.exe" `
  "installer/windows/installer.nsi"
```

`APP_VERSION` 必须是三段数字版本号。ARM64 构建时将 CMake 架构改为 `ARM64`，并将 `APP_ARCH` 改为 `arm64`。推送 `v*` 标签也会触发 GitHub Actions 自动构建 x64 / ARM64 安装程序并发布到 Releases。

## 项目结构

```text
.
├── cli/                         # svm 命令行入口
├── installer/windows/           # NSIS Windows 安装程序
├── src/                         # Provider 控制器与本机 EventBus
├── Main.qml                     # 桌面界面
├── ProviderCatalog.js           # Provider 展示信息
├── main.cpp                     # GUI 程序入口
└── CMakeLists.txt               # 构建配置
```

## 路线图

- 完善 Provider、Operation、InstalledVersion 和 PlatformAdapter 抽象；
- 增加 Java 与 Python Provider；
- 补充 `install`、`doctor` 等 CLI 命令；
- 加强失败恢复、取消、健康检查和自动化测试；
- 在 SDK MVP 稳定后扩展 Nginx、数据库和本地基础服务；
- 完善 macOS 与 Linux 支持。

## 参与开发

提交改动前请阅读 [`AGENTS.md`](AGENTS.md)。新增功能应保持 GUI 与 CLI 共用核心逻辑，并遵守安全下载、原子配置写入、结构化进程参数和用户数据保护要求。
