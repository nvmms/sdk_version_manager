# SDK Version Manager（SVM）

`go` 分支将 SVM 迁移为纯 Go 命令行工具，不依赖 Qt 或 GUI。当前为迁移首阶段，已实现 Node.js、Go 和 Flutter 管理；旧 Qt 实现在原分支保留。

## 构建与使用

需要 Go 1.23+。终端交互使用 `golang.org/x/term` 和 `golang.org/x/sys`（BSD-3-Clause）：

```sh
go build -o ./build/svm ./cmd/svm
./build/svm --help
```

Windows 输出文件可命名为 `build/svm.exe`。将二进制所在目录加入用户 PATH 后：

```sh
svm install node 24.0.0
svm use node 24.0.0
svm use
svm list node
svm doctor
svm node --version
svm npm --version
svm npx --version
```

`use` 同时负责查询与设置当前项目版本。`use node` 依次选择最近 `.svmrc` 中的绑定、全局默认、最新已安装版本；`use node <version>` 使用显式版本。缺少安装时自动下载。没有任何可选版本时，交互终端会要求输入精确版本；非交互环境明确报错，不等待输入。

命令代理向父目录查找最近 `.svmrc`，未绑定时使用全局默认；不回退到任意已安装版本或系统 PATH。参数、工作目录、标准输入输出和正常退出码直接传给受管理的 Node.js。npm/npx 通过对应 SDK 内的 JavaScript 入口执行，不拼接 shell。

安装从 nodejs.org 官方 HTTPS 源下载，验证官方 SHA-256，在独立临时目录解压后发布；支持 Windows、macOS、Linux 的 x64/ARM64 发行包。测试使用伪 HTTP 源，不下载真实 SDK。归档中的路径穿越、危险链接和重复文件会被拒绝。现有不完整安装不会被自动覆盖。

## 查看版本

```sh
svm list node                 # 当前平台可安装版本，✓ 标记 SVM 已安装版本
svm list                      # 当前默认列出 Node.js
svm list lts node             # 仅 LTS 版本
svm list installed node       # 只查本地安装，可离线使用
svm list node --refresh       # 主动更新官方版本索引
```

在交互终端中，`list` 默认进入全屏版本选择器，不会一次打印全部版本：

- ↑ / ↓ 移动，PgUp / PgDn 翻页，Home / End 跳到首尾；也可用 j / k。
- `✓` 表示 SVM 已安装，当前行高亮。
- Enter 直接下载安装选中版本，无需二次菜单。
- 下载期间清空列表，只显示当前版本、下载百分比、大小、速度以及校验/解压阶段；未知总大小时显示已下载大小和速度。
- 安装成功后退出，输出项目绑定与运行命令；不会自动修改项目版本。失败返回列表显示错误，可再次 Enter 重试。
- Esc 返回或退出，q 退出，Ctrl+C 在操作中取消、空闲时退出；退出恢复终端。
- 根据终端窗口高度显示有限行数，调整窗口尺寸会重新布局。

`svm list node --plain` 强制打印表格；管道、输出重定向、非交互输入或 `TERM=dumb` 自动使用表格，适合脚本。

版本来自 [Node.js 官方发行索引](https://nodejs.org/dist/index.json)，按版本数字从新到旧排序，显示 LTS 名称、发布日期和本机安装标记。仅显示当前系统/架构有发行包的正式版本，并保留索引之外的本机安装。尚不支持安装的预发布版本不会列出。

普通查询优先使用 `cache/node/versions.json`，只有缓存不存在才联网；`--refresh` 强制更新，下载或解析失败保留原缓存并报错。损坏的缓存需要显式刷新修复。已安装标记实时检查 SVM 安装目录，不标记系统 PATH 或其他管理器的安装。空列表会显示提示；无缓存且网络失败时，可以使用 `list installed node`。

## Go Provider

```sh
svm list go                    # 当前平台的正式版本；上下选择，Enter 安装
svm list installed go          # 仅本机已安装版本
svm list go --refresh          # 刷新完整官方索引
svm install go 1.27.3
svm use go 1.27.3              # 项目绑定；缺少安装时优先复用本地包
svm default go 1.27.3          # 全局默认，并自动配置 Shell 接入
svm go version
svm go env GOROOT
svm go run main.go
svm gofmt -w main.go
svm uninstall go 1.27.3 --yes   # 保留版本索引和下载包
```

Go 从 [官方完整发行索引](https://go.dev/dl/?mode=json&include=all) 读取正式版本、归档文件和 SHA-256。按系统与 CPU 架构选择 `.zip` 或 `.tar.gz`，不使用系统安装程序，不需要管理员权限。版本参数不带 `go` 前缀，例如 `1.27.3`；官方发布过的 `1.20` 这类两段版本也可使用。不展示预发布或缺少适配归档的版本；Go 没有 LTS 通道，`list lts go` 会明确报错。

Go 复用 Node.js 的下载续传、校验、私有解压、原子发布、文件锁和卸载保护。索引和完整下载包都保留时，可离线重新 `use`，不会再次下载。Go 安装保存在 `installs/go/<version>`，解包后使用归档中的 `go` 根目录。

项目可同时绑定 Node.js 与 Go。Shell 激活会一次构造两个 SDK 的 PATH，设置并在离开环境时恢复 `GOROOT` 与 `GOTOOLCHAIN`，不修改 `GOPATH`。`svm go`、`svm gofmt` 始终执行所选安装中的入口；为防止 Go 自行下载并切换到另一个编译器，使用 `GOTOOLCHAIN=local`。项目需要更高工具链时，通过 SVM 显式安装与选择。

本轮已在 macOS 用官方 Go 包验证安装、版本解析、`go run`、`gofmt`、健康检查、卸载与离线重装。Windows/Linux 交叉编译通过，原生平台运行仍需对应环境验证。

## Flutter Provider

```sh
svm list flutter                  # 全部可用 stable / beta 发行版本
svm list stable flutter           # 稳定通道
svm list beta flutter             # Beta 通道
svm list flutter --refresh        # 手动刷新官方平台索引
svm install flutter 3.47.9
svm use flutter 3.47.9
svm default flutter 3.47.9
svm flutter --version
svm flutter doctor
svm dart --version                # 使用所选 Flutter 内置的 Dart
svm uninstall flutter 3.47.9 --yes # 保留下载包供重新 use
```

从 [Flutter 官方发行索引](https://storage.googleapis.com/flutter_infra_release/releases/releases_macos.json) 按当前平台读取版本、通道、架构、日期和 SHA-256；Windows/Linux 使用各自的 `releases_windows.json` / `releases_linux.json`。没有独立 LTS 通道，使用 stable / beta 筛选。保留原始版本身份，支持 `3.50.0-0.1.pre` 和旧版本 hotfix，按数值与预发布规则比较；只展示匹配当前 CPU 架构且提供可信 SHA-256 的归档。

安装复用下载续传、校验、解压安全检查、原子发布与文件锁。macOS/Windows 使用 ZIP，Linux 使用 tar.xz。SDK 根目录为 `installs/flutter/<version>`，项目稳定链接为 `.svm/sdks/flutter`。完整包与版本索引保留时，卸载后可离线重新 `use`。Flutter 包较大，下载时间和解压空间显著高于 Node.js。

`svm flutter` 和 `svm dart` 都解析 Flutter 项目绑定或全局默认。Unix 使用 SDK 官方启动脚本；Windows 使用内置 Dart 执行 Flutter 工具快照（必要时回退到工具 Dart 源入口），不把用户参数拼接给 cmd.exe。首次运行如缺少工具包配置，会在该 SDK 内准备 Dart 依赖，可能需要联网。Windows 代理路径已做参数保真单元测试与交叉编译，仍需 Windows 原生 Flutter 验证。

Shell 接入支持 Node.js、Go 和 Flutter 同时激活，添加 Flutter/Dart 路径，并设置、切换与恢复 `FLUTTER_ROOT`。不会安装 Android SDK、Xcode、CocoaPods 或修改系统信任。`svm doctor` 检查 Flutter 文件布局、版本元数据与 Git 可用性；详细平台构建依赖使用 `svm flutter doctor` 检查。Flutter/Dart 首次启动和后续构建可能按官方行为下载更多工具或引擎资源。

可选真实包测试（会下载超过 2 GiB 的官方归档，全部在临时目录执行）：

```sh
SVM_TEST_FLUTTER_VERSION=3.47.9 python3 tests/integration/flutter_sdk_smoke.py
```

## 配置与本机状态

`.svmrc` 沿用原格式：

```json
{
  "schemaVersion": 1,
  "sdks": {
    "node": "24.0.0"
  }
}
```

`init` 不覆盖有效声明，将 `.svm/` 加入 `.gitignore`；`use` 保留未知配置字段。损坏或未知 schema 会报错。SVM 自身写入使用锁与原子替换；外部编辑器不参与锁，仍存在读取检查与替换之间的并发窗口。

本机状态默认沿用原 Qt 应用的数据目录：

- Windows：`%LOCALAPPDATA%\appsdk_version_manager`
- macOS：`~/Library/Application Support/appsdk_version_manager`
- Linux：`${XDG_DATA_HOME:-~/.local/share}/appsdk_version_manager`

通过 `SVM_HOME` 可指定其他目录。保留 `installs/<provider>/<version>` 和 `settings/default-versions.json` 布局，旧 Windows Node.js 安装可按可执行文件重新识别。不会删除已有安装或下载。旧缓存格式尚未迁移；不同平台/架构的数据目录不能混用。

## Node.js 管理闭环

```sh
svm default node 24.0.0          # 设置默认，并自动完成当前 Shell 接入
svm default node 24.0.0 --no-shell # 脚本只保存默认，不修改 Shell 配置
svm default node 24.0.0 --shell zsh # 显式指定要接入的 Shell
svm default node                 # 查询全局默认
svm default node --clear          # 清除全局默认
svm use node 24.0.0 --global      # 同 default node 24.0.0
svm uninstall node 24.0.0 --yes   # 将受管理安装移入恢复目录
svm proxy set http://127.0.0.1:7890
svm proxy show
svm proxy clear
```

卸载只删除经过所有权与路径验证的 SDK 安装目录，不删除 `downloads/node/<version>` 中的本地安装包和校验缓存，也不修改项目声明。全局默认或当前项目绑定的版本、正在通过 SVM 运行的版本会被保护。重复卸载安全；中断卸载保留外部操作记录，重试可继续清理。

重新执行 `svm use node <version>` 或 `svm install node <version>` 会校验并复用本地安装包；缓存完整且可信校验已保存时可以离线重装，不会重新下载。只有缓存缺失或校验不通过时才请求官方源。旧版本未保存校验缓存的下载可能需要先获取一次官方校验文件，但不会重复下载有效安装包。

`restore`、`repair` 公共命令已移除。安装损坏时通过卸载后重新 `use` 或 `install` 重建。没有 SVM 所有权标记的外部安装不会被删除。卸载删除安装目录内的软件（包括在该目录安装的全局 npm 包）；安装目录以外的项目、npm 用户配置和缓存不受影响。

下载中断保留 `.part`；下次使用 HTTP Range 续传并对完整文件校验 SHA-256。服务器忽略 Range 时重新下载，返回不一致范围时拒绝拼接。旧恢复目录不会自动清理。

`doctor` 检查项目声明、默认设置、代理配置和本机 Node.js 安装，验证 Node/npm/npx 入口及 Node.js 实际版本。失败时输出重新安装建议。仅按可执行文件识别的旧安装不等于已通过健康检查。

安装、卸载使用独占 OS 文件锁，SDK 代理和健康检查持有共享锁。进程退出自动释放锁；锁文件会保留，请勿手动删除正在使用的锁文件。

代理配置仅支持无账号密码的 HTTP 代理；未配置时使用 Go 标准代理环境变量。旧 Qt 代理设置不会自动导入。所有下载保持 TLS 验证，并拒绝降级到 HTTP 的重定向。

## 终端激活与 IDE

`svm use` 为项目建立 `.svm/sdks/node` 稳定路径，Windows 使用无需管理员权限的 junction，Unix 使用目录符号链接。已有普通目录或未记录的不同链接不会被覆盖。`svm use` 无参数查询时也会重建缺失链接。

SVM 不猜测项目入口，也不自动覆盖 VS Code 的 `launch.json`；需要时可直接使用 `.svm/sdks/node` 中的可执行文件路径。

首次执行 `svm default node <version>` 时，会自动识别并安装或更新当前 Shell 的接入配置，包含 SVM 命令路径、Node.js 激活和提示符钩子，无需再单独运行 `shell install`。新窗口会加载配置并直接使用 `node`、`npm` 和 `svm`；当前窗口按命令输出执行一次 `source <配置文件>`（PowerShell 使用点加载）即可。子进程无法直接改变已有父 Shell 的环境，因此不会把“默认已保存”当作当前终端已经激活。

重复执行不会重复添加钩子。Zsh 使用 `$ZDOTDIR/.zshrc`（未设置时为 `~/.zshrc`），保留现有配置、符号链接和文件权限。Shell 接入失败会返回非零退出码并明确说明默认值已保存、接入未完成。使用 `--no-shell` 可以显式跳过配置修改；无法识别的 Shell 必须通过 `--shell` 指定支持的类型，或使用 `--no-shell`。

`init`、`shell`、`ide` 公共命令暂时停用；初始化由 `use` 自动完成，Shell 接入由 `default` 自动完成。`env` 从帮助与使用流程中移除，仅作为旧钩子的隐藏兼容入口保留，新钩子使用内部 `__env` 协议。底层实现保留，便于后续恢复。

## 当前边界

- 当前支持 Node.js、Go 和 Flutter；其余 Provider 尚未迁移。
- 默认测试使用伪源；本次已在 macOS 用官方 Node.js 包验证完整管理流程。
- Windows/Linux 交叉编译通过；Windows 控制台、junction、PowerShell 钩子仍需对应平台原生验证。当前机器未安装 PowerShell。
- Windows 的单进程 Interrupt 转发受 Go/控制台能力限制，Ctrl+C 依赖同一控制台的系统广播；Unix 代理支持 INT/TERM/HUP 转发。
- 暂不管理系统包管理器或其他版本管理器安装的 Node.js，不自动接管其目录。
- 不提供自动永久清理恢复目录或下载缓存；保留恢复能力优先。
- 解压当前只使用固定官方源，尚无总解压大小配额。

## 验证

```sh
go test ./...
go vet ./...
go build -o build/svm ./cmd/svm
# macOS / Linux 终端集成测试
python3 tests/integration/terminal_smoke.py
python3 tests/integration/node_shell_smoke.py
python3 tests/integration/default_shell_smoke.py
python3 tests/integration/go_shell_smoke.py
# 可选：下载真实官方 Go 包的完整测试
SVM_TEST_GO_VERSION=1.27.3 python3 tests/integration/go_sdk_smoke.py
```

CI 在 Windows、macOS、Linux 运行测试与构建。新增 Provider 前先完善当前闭环的失败恢复和平台测试，协作规则见 [AGENTS.md](AGENTS.md)。

### 终端依赖

`golang.org/x/term v0.29.0` 提供终端检测、原始输入、尺寸与恢复；`golang.org/x/sys v0.30.0` 提供 Windows 控制台与 Unix 非阻塞输入支持。两者为 BSD-3-Clause。下载后的源码目录约为 96 KiB 和 10 MiB（不同平台源码总和，不等于最终二进制增量）。标准库没有跨平台 raw-mode API；使用这两个小型底层库避免为每个平台自行复制终端系统调用。它们不提供 GUI，也不引入完整 TUI 框架。Windows 终端交互需原生验证。

### XZ 解码依赖

Flutter Linux 包使用 `github.com/ulikunitz/xz v0.5.12`（BSD-3-Clause，[许可证](docs/third-party/xz-LICENSE.txt)）。下载源码约 11 MiB，主要包含测试样本，编译只引入所需 XZ/LZMA 解码代码；不依赖 CGo、系统 xz/tar 或额外可执行程序。标准库不提供 XZ 解码，直接使用外部解压程序无法复用当前逐项路径、符号链接与取消检查，因此使用纯 Go 解码器。Windows、macOS、Linux 共用同一解压校验逻辑。
