# SDK Version Manager (SVM)

[简体中文](README.md) | English

SDK Version Manager (SVM) is a local-first, cross-platform development environment manager built with Qt 6. It provides both a desktop GUI and the `svm` CLI for managing the SDK versions used by a project.

> [!IMPORTANT]
> SVM is currently an early-stage project (`0.1`) with a Windows-first focus. The available providers are **Node.js** and **Flutter**. Java, Python, web services, databases, and other capabilities remain on the roadmap.

## Download and install

1. Open [GitHub Releases](https://github.com/nvmms/sdk_version_manager/releases/latest);
2. Download the installer for your Windows architecture:
   - `SVM-<version>-windows-x64-setup.exe` for most Intel and AMD PCs;
   - `SVM-<version>-windows-arm64-setup.exe` for ARM64 Windows devices;
3. Run the installer;
4. Launch **SDK Version Manager** from the Start menu.

The default installation directory is:

```text
%LOCALAPPDATA%\Programs\SDK Version Manager
```

The installer creates a Start menu shortcut for the GUI. It does not currently add the CLI to `PATH`. To run `svm` from any terminal, add this directory to your user `PATH`:

```text
%LOCALAPPDATA%\Programs\SDK Version Manager\bin
```

Alternatively, run it by its full path:

```powershell
& "$env:LOCALAPPDATA\Programs\SDK Version Manager\bin\svm.exe" --help
```

## Usage

### GUI

1. Launch **SDK Version Manager**;
2. Select Node.js or Flutter;
3. SVM reads the local version cache first; click refresh when you need the latest index;
4. Filter and select a version, then download and install it;
5. Optionally set an installed version as the global default.

Download and installation progress can be monitored or cancelled from the interface.

### CLI

Run these commands inside your project directory:

```text
# Show help
svm --help

# Initialize the project and create .svmrc and .svm/
svm init

# Bind project versions; missing versions are downloaded and installed
svm use node 24.0.0
svm use flutter 3.38.0

# Show project bindings and global defaults
svm use

# Resolve and save a provider version for the current project
svm use node

# Configure or inspect project SDK paths for an IDE
svm ide vscode
svm ide idea

# List versions from the local cache
svm list
svm list all node
svm list lts node
svm list stable flutter
svm list downloaded node

# Run native commands with the project-selected SDK
svm node --version
svm node app.js
svm flutter doctor
```

### IDE configuration

SVM exposes `.svm/sdks/<provider>` as the stable project-local SDK path.
`svm use` does not guess which IDE a developer uses and does not automatically
create `.vscode` or `.idea`. It prints setup guidance for VS Code, IntelliJ IDEA,
and Android Studio. VS Code can use:

```json
{
  "dart.flutterSdkPath": ".svm/sdks/flutter",
  "python.defaultInterpreterPath": ".svm/sdks/python",
  "java.configuration.runtimes": [
    {
      "name": "JavaSE-21",
      "path": ".svm/sdks/java",
      "default": true
    }
  ]
}
```

Only providers present in `.svmrc` receive settings. Existing Java runtime
entries are preserved when the SVM runtime is merged.

VS Code does not expose a universal project-level Node.js SDK setting.
`runtimeExecutable` belongs to a specific `.vscode/launch.json` launch
configuration and cannot be generated safely without knowing the application
entry point. Use `svm node ...`, or point a specific debug configuration to
`${workspaceFolder}/.svm/sdks/node/node.exe`.

In IntelliJ IDEA or WebStorm, set the Node interpreter under
**Settings > Languages & Frameworks > Node.js** to
`<project>\.svm\sdks\node\node.exe`. Android Studio does not provide Node.js
project support by default; the same path can be used after installing an
appropriate JavaScript/Node.js plugin.

You can also run:

```powershell
svm ide vscode
```

to explicitly create or repair the VS Code integration without overwriting
unrelated settings.

If the existing file uses JSONC features such as comments or trailing commas,
SVM refuses to rewrite it and prints the setting to add manually.

IntelliJ IDEA and Android Studio currently do not expose a stable project-level
Flutter SDK configuration file that SVM can safely modify. `svm ide idea` prints
the resolved path for the project. You can also open
**Settings > Languages & Frameworks > Flutter** and select:

```text
<project>\.svm\sdks\flutter
```

For other IDEs, point the Flutter SDK root to `.svm/sdks/flutter`. An already
running IDE might require a window reload or restart after changing the setting.

`svm use <provider> [version]` resolves versions in this order:

1. An explicitly supplied version;
2. The nearest `.svmrc` project binding;
3. The global default;
4. The latest installed version.

The currently supported CLI provider IDs are `node` and `flutter`.

## Available today

- Browse, refresh, and filter Node.js and Flutter releases in the GUI;
- Download, verify, install, and cancel SDK operations;
- Resume partial downloads and use cached version indexes first;
- Set global default SDK versions;
- Bind multiple SDK versions to a project through `.svmrc`;
- Find the nearest `.svmrc` by walking up from the current directory;
- Run the selected SDK through `svm node ...` or `svm flutter ...`;
- Print project SDK setup guidance for common IDEs from `svm use` without writing
  IDE-specific files;
- Explicitly merge Flutter, Python, and Java settings with `svm ide vscode`;
- Synchronize GUI and CLI task state through a local EventBus;
- Build Windows x64 and ARM64 installers.

## Project configuration

`svm init` performs the following actions in the current directory:

- Creates a commit-friendly `.svmrc`;
- Creates `.svm/` for machine-local generated files;
- Adds `.svm/` to `.gitignore` without duplicating it.
- `svm use` creates `.svm/sdks/<provider>` as a project-local directory link to the
  globally managed version. The link is rebuildable from `.svmrc` and is not the
  source of truth for version resolution.
- Running `svm use` without arguments also checks and repairs missing or stale
  project-local directory links while displaying the active bindings.
- On Windows, SVM falls back to a directory junction when symbolic-link privileges
  are unavailable, without requiring administrator access.

Example `.svmrc`:

```json
{
  "schemaVersion": 1,
  "sdks": {
    "flutter": "3.38.0",
    "node": "24.0.0"
  }
}
```

The configuration stores provider/version bindings only, never machine-specific absolute installation paths. Project bindings take precedence over global defaults.

## Build from source

### Requirements

- Windows 10/11;
- Qt 6.10 or a compatible version with Qt Quick, Qt Network, and Qt Concurrent;
- CMake 3.16 or newer;
- A C++17 compiler (Visual Studio 2022 is recommended).

Clone and build:

```powershell
git clone https://github.com/nvmms/sdk_version_manager.git
cd sdk_version_manager
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

The build produces:

- `appsdk_version_manager`: desktop GUI;
- `svm`: command-line tool.

### Create a release directory

```powershell
cmake --install build --config Release --prefix release
```

This deploys the applications, Qt runtime libraries, and required plugins to `release/`.

### Package the Windows installer

Install [NSIS](https://nsis.sourceforge.io/) first, then run the following from the repository root:

```powershell
New-Item -ItemType Directory -Force dist
makensis `
  "/DAPP_VERSION=0.1.0" `
  "/DAPP_ARCH=x64" `
  "/DRELEASE_DIR=$((Resolve-Path release).Path)" `
  "/DOUTPUT_FILE=$((Resolve-Path dist).Path)\SVM-0.1.0-windows-x64-setup.exe" `
  "installer/windows/installer.nsi"
```

`APP_VERSION` must contain exactly three numeric parts. For ARM64, configure CMake with `-A ARM64` and set `APP_ARCH` to `arm64`. Pushing a `v*` tag also triggers GitHub Actions to build x64 and ARM64 installers and publish them to Releases.

## Repository layout

```text
.
├── cli/                         # svm CLI entry point
├── installer/windows/           # NSIS Windows installer
├── src/                         # Provider controller and local EventBus
├── Main.qml                     # Desktop interface
├── ProviderCatalog.js           # Provider presentation metadata
├── main.cpp                     # GUI entry point
└── CMakeLists.txt               # Build configuration
```

## Roadmap

- Establish Provider, Operation, InstalledVersion, and PlatformAdapter abstractions;
- Add Java and Python providers;
- Add CLI commands such as `install` and `doctor`;
- Improve recovery, cancellation, health checks, and automated tests;
- Expand into Nginx, databases, and local services after the SDK MVP is stable;
- Complete macOS and Linux support.

## Contributing

Read [`AGENTS.md`](AGENTS.md) before contributing. New features should keep the GUI and CLI on shared core logic and follow the project's requirements for safe downloads, atomic configuration writes, structured process arguments, and user data protection.
