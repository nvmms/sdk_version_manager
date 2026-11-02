# SDK Version Manager (SVM)

The `go` branch migrates SVM to a standalone Go CLI without Qt or a GUI. The Go CLI supports Node.js, Go and Flutter; other Providers have not been ported.

## Install on macOS / Linux

After a compatible CLI Release is published:

```sh
curl -fsSL https://github.com/nvmms/sdk_version_manager/releases/latest/download/install.sh -o /tmp/svm-install.sh
sh /tmp/svm-install.sh
```

The [installer](install.sh) detects macOS/Linux and x64/ARM64, downloads a pinned GitHub Release asset and verifies SHA-256 before installing to ~/.svm/bin/svm. No Go compiler or sudo is needed. It configures Bash/Zsh PATH while preserving profile contents, permissions and symlinks. Open a new terminal or apply the printed export command.

Use `--version v0.1.0`, `--no-profile`, SVM_INSTALL_DIR or SVM_PROFILE for custom setups. Repeat installation upgrades atomically and keeps bin/svm.previous. Unowned executables are not overwritten. Only expected regular archive files are extracted to explicit paths. SDK storage and existing SDKs are untouched. Windows uses the portable ZIP; WSL uses Linux assets.

Requires curl, tar, awk, sed, grep, mktemp, stat and sha256sum/shasum. An interrupted installer may leave .installer-lock; remove it only after confirming the installer has stopped. Run `python3 tests/integration/installer_smoke.py` for offline tests in an isolated HOME.

Requires Go 1.23+. Terminal handling uses golang.org/x/term and golang.org/x/sys (BSD-3-Clause):

```sh
go build -o ./build/svm ./cmd/svm
./build/svm --help
svm install node 24.0.0
svm use node 24.0.0
svm use
svm list node
svm doctor
svm node --version
svm npm --version
```

Add the binary's directory to PATH to use `svm` directly. On Windows, name the output `svm.exe`.

Projects keep the existing `.svmrc` JSON schema (`schemaVersion: 1`, `sdks` mapping). `use node` selects the nearest project binding, global default, then newest installed version; an explicit version takes priority. Missing versions are installed automatically. Proxy commands use project bindings or global defaults, never arbitrary system executables.

The state directory retains the previous `appsdk_version_manager` application name under the platform's local application data directory. Override it with `SVM_HOME`. Existing Windows Node.js installations can be discovered; no old installations are deleted. Do not share a state directory across platforms or architectures.

Downloads use the official Node.js HTTPS source and SHA-256 checksums. Extraction uses private staging and rejects traversal, unsafe links, and duplicate files. Installation locks prevent competing SVM installations. Configuration writes preserve unknown fields and use atomic replacement; external editors do not participate in locking, so a small concurrent-update window remains.

Node.js management includes `default node [version|--clear]`, `uninstall node <version> --yes`, and proxy settings. Uninstall removes only the owned SDK installation directory; download archives and checksum caches remain. Active project bindings, global defaults, and SVM-running versions are protected. Unmarked legacy installations are not deleted. Software installed inside the SDK directory, including global npm packages there, is removed; projects and user npm configuration outside it remain intact.

Run `use node <version>` or `install node <version>` to reinstall. A valid local archive plus saved official checksum is reused offline, without downloading again. Missing or invalid caches use the official source. Older caches without saved checksums may require a checksum request. Public restore and repair commands have been removed; old recovery directories are untouched.

Downloads resume using HTTP Range and verify the entire SHA-256. Interrupted uninstall operations can be retried using persisted ownership records. Process-scoped locks release on exit.

`use` creates a stable `.svm/sdks/node` link. The public init, shell and ide commands are temporarily disabled. Project initialization is automatic through use, and shell setup through default. env is hidden and retained only for existing shell hooks; new hooks use the internal __env protocol. Prompt hooks refresh the selected Node.js and restore the original PATH when leaving its environment. Restart the terminal after changing hooks. Bash setup handles both `.bashrc` and the effective login profile.

`doctor` checks declarations, defaults, proxy configuration, SDK entry files, and the Node.js executable's reported version. Other Providers remain out of scope. Windows console, junction, and PowerShell behaviors still need native validation; PowerShell is unavailable on the current host. Unix INT/TERM/HUP forwarding is supported; Windows Ctrl+C relies on console broadcast. Recovery data and caches are not automatically purged. Archive extraction has no total-size quota. Legacy Qt proxy settings are not imported.

```sh
go test ./...
go vet ./...
go build ./cmd/svm
```

See [Chinese documentation](README.md) and [contributor instructions](AGENTS.md).

## Version lists

`svm list node` (or `svm list`) displays stable releases available for the current platform from the [official index](https://nodejs.org/dist/index.json). Installed versions are marked with ✓, alongside LTS names and release dates. Versions are sorted numerically, newest first. Installations missing from the index remain visible. Installation status reflects SVM-managed executables, not system PATH.

Use `svm list lts node` for LTS releases, `svm list installed node` for an offline local list, and `svm list node --refresh` to update the index. Normal queries use the cache and only fetch when missing. Failed refreshes preserve the previous cache and report the error; corrupt caches require an explicit refresh. Empty lists display a message.

In an interactive terminal, lists open a persistent full-screen picker. Use arrows (or j/k), Page Up/Down, and Home/End to navigate; ✓ marks managed installations. Enter directly downloads and installs the selected version. While installing, the list is replaced with a dedicated progress screen. On success the picker exits and prints project binding and run instructions, without changing project bindings. Failures return to the list for retry. Esc or q exits; during installation it cancels first. Ctrl+C cancels an operation or exits when idle. Terminal modes and the previous screen are restored on exit. The viewport follows terminal resizing.

Use `--plain` for a printed table. Redirected output, nonterminal input, and TERM=dumb automatically use plain output. Windows console support is cross-compiled but still needs native interaction validation.

Setting a default with `svm default node <version>` now automatically installs or updates integration for the detected shell, including the SVM binary directory, SDK activation and prompt hooks. New terminals work without a separate shell-install step. Existing terminals receive a profile-source command. `--shell zsh|bash|powershell` explicitly chooses integration, while `--no-shell` saves configuration without modifying shell profiles. Setup failures return nonzero and report the saved default as partial completion. Zsh respects ZDOTDIR. Repeat setup is idempotent and preserves profile contents and permissions.

The fresh-home acceptance test `python3 tests/integration/default_shell_smoke.py` verifies default selection followed by a new shell, without manually installing hooks or evaluating environment scripts.

## Go Provider

```sh
svm list go
svm install go 1.27.3
svm use go 1.27.3
svm default go 1.27.3
svm go version
svm go env GOROOT
svm go run main.go
svm gofmt -w main.go
svm uninstall go 1.27.3 --yes
```

Go uses the [official complete release index](https://go.dev/dl/?mode=json&include=all), stable platform archives and SHA-256 hashes. Versions omit the go prefix. There is no Go LTS filter. Download, extraction, locks, project/global selection, terminal picker and uninstall use the shared workflow. Cached indexes and verified archives allow offline reinstall.

Node.js and Go can be bound together. Activation and proxies set GOROOT and pin GOTOOLCHAIN=local so Go cannot silently switch compilers. GOPATH is preserved. Shell deactivation restores previous GOROOT/GOTOOLCHAIN values. Official install, go run, gofmt, health, uninstall and offline reinstall were verified on macOS; other native platforms need their own runtime validation.

Run `python3 tests/integration/go_shell_smoke.py` for offline multi-Provider activation checks. Opt into a real download with `SVM_TEST_GO_VERSION=1.27.3 python3 tests/integration/go_sdk_smoke.py`.

## Flutter Provider

```sh
svm list flutter
svm list stable flutter
svm list beta flutter
svm use flutter 3.47.9
svm default flutter 3.47.9
svm flutter --version
svm flutter doctor
svm dart --version
svm uninstall flutter 3.47.9 --yes
```

Uses the platform-specific [official Flutter indexes](https://storage.googleapis.com/flutter_infra_release/releases/releases_macos.json), CPU architecture metadata and SHA-256 checksums. Stable/beta channels, prerelease and hotfix version ordering, cached/offline reinstall, the terminal picker and safe uninstall share the existing workflow. macOS/Windows use ZIP and Linux uses tar.xz. XZ decoding uses the pure Go BSD-3-Clause library github.com/ulikunitz/xz v0.5.12; see [license](docs/third-party/xz-LICENSE.txt). Its source download is about 11 MiB including test data, with no external executable or CGo requirement.

Flutter and bundled Dart are proxied from the selected SDK. Unix uses the official launchers; Windows directly invokes bundled Dart with Flutter's tool snapshot or Dart entry source, preserving arguments without cmd.exe. Missing tool dependency configuration may require a first-run pub get. Windows native Flutter execution remains unverified. Shell activation supports all three Providers and restores FLUTTER_ROOT on deactivation. SVM does not install platform build dependencies; use svm flutter doctor for Android/Xcode/CocoaPods checks.

Opt into the full SDK lifecycle test with `SVM_TEST_FLUTTER_VERSION=3.47.9 python3 tests/integration/flutter_sdk_smoke.py` (over 2 GiB download in a temporary directory).

## Windows, macOS and Linux packages

[Package SVM](.github/workflows/release.yml) supports manual runs and pushed `v*` tags. Manual runs upload Actions artifacts only; tagged runs publish/update GitHub Releases after checks succeed. Use a SemVer tag such as `v0.1.0`; prerelease tags are marked accordingly.

Produces Windows x64/ARM64 ZIPs and macOS/Linux x64/ARM64 tar.gz files. Also publishes install.sh and its checksum. Each includes the CLI, documentation, build/commit metadata and dependency licenses, with a SHA-256 sidecar. Releases also include SHA256SUMS. Artifacts are retained for 14 days. Packages are portable, unsigned and not notarized; there is no installer or Qt runtime.

Native tests and vet run on Windows/macOS/Linux; terminal/shell smoke tests run on macOS. Both CPU architectures are built with CGO disabled. A help smoke test runs when the target matches the runner architecture; the other target is only cross-compiled. The release job alone receives contents:write, using GITHUB_TOKEN without extra secrets.

Local equivalent: `python3 scripts/package_release.py --os darwin --arch arm64 --version v0.1.0` (also supports windows/amd64). Output defaults to dist. Set SVM_BUILD_GO to use a compiler outside PATH.
