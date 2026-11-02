package cli

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/sdk"
)

func manage(ctx context.Context, command string, args []string, dir string, m *sdk.Manager, out, diagnostics io.Writer) (int, error) {
	switch command {
	case "default":
		return setDefault(args, m, out)
	case "uninstall":
		if len(args) != 3 || args[0] != "node" || args[2] != "--yes" {
			return 2, errors.New("usage: svm uninstall node <version> --yes; removes the owned SDK installation, preserves project declarations and downloads")
		}
		path, err := m.Uninstall(args[0], args[1], dir)
		if err != nil {
			return 1, err
		}
		if path == "" {
			fmt.Fprintln(out, "Already uninstalled.")
		} else {
			fmt.Fprintf(out, "Uninstalled node %s. Download cache preserved.\nReinstall with: svm use node %s\nOther projects may still reference this version.\n", args[1], args[1])
		}
		return 0, nil
	case "proxy":
		if len(args) == 0 || len(args) == 1 && args[0] == "show" {
			proxy, err := m.Proxy()
			if err != nil {
				return 1, err
			}
			if proxy == "" {
				proxy = "system proxy environment (HTTP_PROXY/HTTPS_PROXY/NO_PROXY)"
			}
			fmt.Fprintln(out, "Proxy:", proxy)
			return 0, nil
		}
		if len(args) == 1 && args[0] == "clear" {
			return result(m.SetProxy(""))
		}
		if len(args) == 2 && args[0] == "set" {
			return result(m.SetProxy(args[1]))
		}
		return 2, errors.New("usage: svm proxy [show|set <http-url>|clear]")
	case "env":
		if len(args) != 1 {
			return 2, errors.New("usage: svm env <bash|zsh|powershell>")
		}
		script, err := m.Environment(dir, args[0])
		if err != nil {
			return 1, err
		}
		fmt.Fprint(out, script)
		return 0, nil
	case "shell":
		if len(args) != 2 || (args[0] != "install" && args[0] != "uninstall") {
			return 2, errors.New("usage: svm shell <install|uninstall> <bash|zsh|powershell>")
		}
		path, err := m.ShellHook(args[1], args[0] == "install")
		if err != nil {
			return 1, err
		}
		fmt.Fprintln(out, "Shell hook", args[0]+"ed:", path)
		fmt.Fprintln(out, "Open a new terminal for the change to take effect.")
		return 0, nil
	case "ide":
		if len(args) > 1 {
			return 2, errors.New("usage: svm ide [vscode|idea]")
		}
		editor := "idea"
		if len(args) == 1 {
			editor = args[0]
		}
		if editor != "idea" && editor != "vscode" {
			return 2, errors.New("supported IDEs: vscode, idea")
		}
		path, err := config.Find(dir)
		if err != nil {
			return 3, fmt.Errorf("project declaration missing; run svm use node <version>: %w", err)
		}
		version, _, err := m.Resolve(dir, "node", false)
		if err != nil {
			return 3, err
		}
		root := filepath.Dir(path)
		if err = m.ProjectLink(root, "node", version); err != nil {
			return 1, err
		}
		if editor == "vscode" {
			fmt.Fprintln(out, "VS Code has no universal Node.js project interpreter setting. In the relevant .vscode/launch.json configuration, set runtimeExecutable to:")
		}
		entry, _ := (sdk.Node{}).Entry("node")
		fmt.Fprintln(out, filepath.Join(root, ".svm", "sdks", "node", entry.Executable))
		fmt.Fprintln(out, "Terminal commands: svm node ... / svm npm ...")
		return 0, nil
	}
	return 2, errors.New("unknown management command")
}
func doctor(ctx context.Context, args []string, dir string, m *sdk.Manager, out io.Writer) (int, error) {
	if len(args) != 0 {
		return 2, errors.New("usage: svm doctor")
	}
	fmt.Fprintln(out, "State:", m.Root)
	if path, err := exec.LookPath("node"); err == nil {
		fmt.Fprintln(out, "Node on PATH (discovery only):", path)
	}
	defaults, err := m.Defaults()
	if err != nil {
		return 1, err
	}
	if _, err = m.Proxy(); err != nil {
		return 1, err
	}
	needed := map[string]bool{}
	if v := defaults["node"]; v != "" {
		needed[v] = true
		fmt.Fprintln(out, "Global node:", v)
	}
	path, err := config.Find(dir)
	if err == nil {
		project, e := config.Read(path)
		if e != nil {
			return 1, e
		}
		fmt.Fprintln(out, "Project:", path)
		for p, v := range project.SDKs {
			if p != "node" {
				fmt.Fprintln(out, "Not migrated:", p)
				continue
			}
			needed[v] = true
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return 1, err
	} else {
		fmt.Fprintln(out, "No project declaration; run svm use node <version> to initialize it.")
	}
	installed, err := m.Versions("node")
	if err != nil {
		return 1, err
	}
	for _, v := range installed {
		needed[v] = true
	}
	// Include interrupted installs whose executable is absent.
	entries, err := os.ReadDir(filepath.Join(m.Root, "installs", "node"))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return 1, err
	}
	for _, entry := range entries {
		if entry.IsDir() && config.SafeName(entry.Name()) && entry.Name()[0] != '.' {
			needed[entry.Name()] = true
		}
	}
	versions := make([]string, 0, len(needed))
	for v := range needed {
		versions = append(versions, v)
	}
	sort.Strings(versions)
	failed := false
	for _, v := range versions {
		if err = m.Health(ctx, v); err != nil {
			fmt.Fprintln(out, "FAIL:", err)
			failed = true
		} else {
			fmt.Fprintln(out, "OK: node", v, m.Directory("node", v))
		}
	}
	if failed {
		return 3, errors.New("Node.js health checks failed; use the reinstall commands above")
	}
	fmt.Fprintln(out, "Node.js checks passed.")
	return 0, nil
}

func activationHint(out io.Writer) {
	executable, err := os.Executable()
	if err != nil {
		executable = "svm"
	}
	shell := filepath.Base(os.Getenv("SHELL"))
	if shell != "zsh" && shell != "bash" {
		if runtime.GOOS == "windows" {
			shell = "powershell"
		} else {
			shell = "bash"
		}
	}
	fmt.Fprintln(out, "\nThe default is saved. Activate this terminal to use node/npm directly:")
	if shell == "powershell" {
		quoted := "'" + strings.ReplaceAll(executable, "'", "''") + "'"
		fmt.Fprintf(out, "  & %s __env powershell | Out-String | Invoke-Expression\n", quoted)
		fmt.Fprintf(out, "\nAutomatic integration is configured when you set a global default.\n")
	} else {
		quoted := "'" + strings.ReplaceAll(executable, "'", `'\''`) + "'"
		fmt.Fprintf(out, "  eval \"$(%s __env %s)\"\n", quoted, shell)
		fmt.Fprintf(out, "\nAutomatic integration is configured when you set a global default.\n")
	}
	fmt.Fprintln(out, "Project bindings take priority over the global default.")
}

func currentShell() string {
	shell := filepath.Base(os.Getenv("SHELL"))
	if shell == "zsh" || shell == "bash" {
		return shell
	}
	if runtime.GOOS == "windows" {
		return "powershell"
	}
	if os.Getenv("SHELL") != "" {
		return shell
	}
	return "bash"
}
func setDefault(args []string, m *sdk.Manager, out io.Writer) (int, error) {
	const usage = "usage: svm default node [version|--clear] [--shell bash|zsh|powershell | --no-shell]"
	if len(args) == 0 || args[0] != "node" {
		return 2, errors.New(usage)
	}
	if len(args) == 1 {
		defaults, err := m.Defaults()
		if err != nil {
			return 1, err
		}
		v := defaults["node"]
		if v == "" {
			v = "(not set)"
		}
		fmt.Fprintln(out, "Global node:", v)
		return 0, nil
	}
	version := args[1]
	noShell := false
	shell := currentShell()
	explicitShell := false
	for i := 2; i < len(args); i++ {
		switch args[i] {
		case "--no-shell":
			if noShell || explicitShell {
				return 2, errors.New(usage)
			}
			noShell = true
		case "--shell":
			if explicitShell || noShell || i+1 >= len(args) {
				return 2, errors.New(usage)
			}
			i++
			shell = args[i]
			explicitShell = true
			if shell != "bash" && shell != "zsh" && shell != "powershell" {
				return 2, errors.New(usage)
			}
		default:
			return 2, errors.New(usage)
		}
	}
	if !noShell && version != "--clear" && shell != "bash" && shell != "zsh" && shell != "powershell" {
		return 2, errors.New("cannot detect a supported shell; specify --shell bash|zsh|powershell or --no-shell")
	}
	if version == "--clear" {
		version = ""
	}
	if err := m.SetDefault("node", version); err != nil {
		return 1, err
	}
	if version == "" {
		fmt.Fprintln(out, "Global node default cleared. Project bindings still take priority.")
		activationHint(out)
		return 0, nil
	}
	if noShell {
		fmt.Fprintln(out, "Global node:", version)
		fmt.Fprintln(out, "Shell setup skipped (--no-shell). Use svm node ... or explicitly activate the shell.")
		return 0, nil
	}
	profile, err := m.ShellHook(shell, true)
	if err != nil {
		return 1, fmt.Errorf("global default saved, but shell setup failed: %w; retry svm default node %s --shell %s", err, version, shell)
	}
	fmt.Fprintln(out, "Global node:", version)
	fmt.Fprintf(out, "Shell integration ready: %s (%s)\n", shell, profile)
	fmt.Fprintln(out, "Open a new terminal, or load the profile in this terminal:")
	if shell == "powershell" {
		fmt.Fprintf(out, "  . '%s'\n", strings.ReplaceAll(profile, "'", "''"))
	} else {
		fmt.Fprintf(out, "  source '%s'\n", strings.ReplaceAll(profile, "'", `'\''`))
	}
	fmt.Fprintln(out, "Then run: node --version")
	fmt.Fprintln(out, "Project bindings take priority over the global default.")
	return 0, nil
}
