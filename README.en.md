# SDK Version Manager (SVM)

The `go` branch migrates SVM to a standalone Go CLI without Qt or a GUI. This first migration slice supports Node.js; other Providers from the Qt application have not been ported.

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
