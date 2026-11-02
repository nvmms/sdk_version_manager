package sdk

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

func shellQuote(s string) string      { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }
func powershellQuote(s string) string { return "'" + strings.ReplaceAll(s, "'", "''") + "'" }

// Environment activates all resolved Providers in one PATH and restores variables
// owned by SVM when a Provider is no longer selected. It never changes GOPATH.
func (m *Manager) Environment(dir, shell string) (string, error) {
	if shell != "bash" && shell != "zsh" && shell != "powershell" {
		return "", errors.New("supported shells: bash, zsh, powershell")
	}
	selected := map[string]string{}
	var bins []string
	for _, p := range m.Providers {
		v, _, err := m.Resolve(dir, p.ID(), false)
		if errors.Is(err, ErrNoVersion) {
			continue
		}
		if err != nil {
			return "", err
		}
		if !m.Installed(p.ID(), v) {
			return "", fmt.Errorf("%s %s is not installed; run svm install %s %s", p.ID(), v, p.ID(), v)
		}
		entry, _ := p.Entry(p.ID())
		bins = append(bins, sdkBins(p.ID(), m.Directory(p.ID(), v), filepath.Join(m.Directory(p.ID(), v), entry.Executable))...)
		selected[p.ID()] = v
	}
	var b strings.Builder
	if shell == "powershell" {
		if len(bins) > 0 {
			b.WriteString("if (!(Test-Path Env:SVM_ORIGINAL_PATH)) { $env:SVM_ORIGINAL_PATH=$env:PATH }\n")
			b.WriteString("$env:PATH=" + powershellQuote(strings.Join(bins, string(os.PathListSeparator))+string(os.PathListSeparator)) + "+$env:SVM_ORIGINAL_PATH\n")
		} else {
			b.WriteString("if (Test-Path Env:SVM_ORIGINAL_PATH) { $env:PATH=$env:SVM_ORIGINAL_PATH; Remove-Item Env:SVM_ORIGINAL_PATH }\n")
		}
		if v := selected["node"]; v != "" {
			b.WriteString("$env:SVM_ACTIVE_NODE=" + powershellQuote(v) + "\n")
		} else {
			b.WriteString("Remove-Item Env:SVM_ACTIVE_NODE -ErrorAction SilentlyContinue\n")
		}
		if v := selected["go"]; v != "" {
			for _, key := range []string{"GOROOT", "GOTOOLCHAIN"} {
				fmt.Fprintf(&b, "if (!(Test-Path Env:SVM_ACTIVE_GO)) { if (Test-Path Env:%s) { $env:SVM_ORIGINAL_%s=$env:%s; $env:SVM_ORIGINAL_%s_SET='1' } else { $env:SVM_ORIGINAL_%s_SET='0' } }\n", key, key, key, key, key)
			}
			b.WriteString("$env:GOROOT=" + powershellQuote(m.Directory("go", v)) + "\n$env:GOTOOLCHAIN='local'\n$env:SVM_ACTIVE_GO=" + powershellQuote(v) + "\n")
		} else {
			for _, key := range []string{"GOROOT", "GOTOOLCHAIN"} {
				fmt.Fprintf(&b, "if (Test-Path Env:SVM_ACTIVE_GO) { if ($env:SVM_ORIGINAL_%s_SET -eq '1') { $env:%s=$env:SVM_ORIGINAL_%s } else { Remove-Item Env:%s -ErrorAction SilentlyContinue }; Remove-Item Env:SVM_ORIGINAL_%s,Env:SVM_ORIGINAL_%s_SET -ErrorAction SilentlyContinue }\n", key, key, key, key, key, key)
			}
			b.WriteString("Remove-Item Env:SVM_ACTIVE_GO -ErrorAction SilentlyContinue\n")
		}
		flutterEnvironment(&b, m, selected["flutter"], true)
		return b.String(), nil
	}
	if len(bins) > 0 {
		b.WriteString("if [ \"${SVM_ORIGINAL_PATH+x}\" != x ]; then export SVM_ORIGINAL_PATH=\"$PATH\"; fi\n")
		b.WriteString("export PATH=" + shellQuote(strings.Join(bins, string(os.PathListSeparator))+string(os.PathListSeparator)) + "\"$SVM_ORIGINAL_PATH\"\n")
	} else {
		b.WriteString("if [ \"${SVM_ORIGINAL_PATH+x}\" = x ]; then export PATH=\"$SVM_ORIGINAL_PATH\"; unset SVM_ORIGINAL_PATH; fi\n")
	}
	if v := selected["node"]; v != "" {
		b.WriteString("export SVM_ACTIVE_NODE=" + shellQuote(v) + "\n")
	} else {
		b.WriteString("unset SVM_ACTIVE_NODE\n")
	}
	if v := selected["go"]; v != "" {
		for _, key := range []string{"GOROOT", "GOTOOLCHAIN"} {
			fmt.Fprintf(&b, "if [ \"${SVM_ACTIVE_GO+x}\" != x ]; then if [ \"${%s+x}\" = x ]; then export SVM_ORIGINAL_%s=\"$%s\" SVM_ORIGINAL_%s_SET=1; else export SVM_ORIGINAL_%s_SET=0; fi; fi\n", key, key, key, key, key)
		}
		b.WriteString("export GOROOT=" + shellQuote(m.Directory("go", v)) + "\nexport GOTOOLCHAIN=local\nexport SVM_ACTIVE_GO=" + shellQuote(v) + "\n")
	} else {
		for _, key := range []string{"GOROOT", "GOTOOLCHAIN"} {
			fmt.Fprintf(&b, "if [ \"${SVM_ACTIVE_GO+x}\" = x ]; then if [ \"${SVM_ORIGINAL_%s_SET-0}\" = 1 ]; then export %s=\"$SVM_ORIGINAL_%s\"; else unset %s; fi; unset SVM_ORIGINAL_%s SVM_ORIGINAL_%s_SET; fi\n", key, key, key, key, key, key)
		}
		b.WriteString("unset SVM_ACTIVE_GO\n")
	}
	flutterEnvironment(&b, m, selected["flutter"], false)
	return b.String(), nil
}

func flutterEnvironment(b *strings.Builder, m *Manager, version string, powershell bool) {
	if powershell {
		if version != "" {
			b.WriteString("if (!(Test-Path Env:SVM_ACTIVE_FLUTTER)) { if (Test-Path Env:FLUTTER_ROOT) { $env:SVM_ORIGINAL_FLUTTER_ROOT=$env:FLUTTER_ROOT; $env:SVM_ORIGINAL_FLUTTER_ROOT_SET='1' } else { $env:SVM_ORIGINAL_FLUTTER_ROOT_SET='0' } }\n")
			b.WriteString("$env:FLUTTER_ROOT=" + powershellQuote(m.Directory("flutter", version)) + "\n$env:SVM_ACTIVE_FLUTTER=" + powershellQuote(version) + "\n")
		} else {
			b.WriteString("if (Test-Path Env:SVM_ACTIVE_FLUTTER) { if ($env:SVM_ORIGINAL_FLUTTER_ROOT_SET -eq '1') { $env:FLUTTER_ROOT=$env:SVM_ORIGINAL_FLUTTER_ROOT } else { Remove-Item Env:FLUTTER_ROOT -ErrorAction SilentlyContinue }; Remove-Item Env:SVM_ACTIVE_FLUTTER,Env:SVM_ORIGINAL_FLUTTER_ROOT,Env:SVM_ORIGINAL_FLUTTER_ROOT_SET -ErrorAction SilentlyContinue }\n")
		}
		return
	}
	if version != "" {
		b.WriteString("if [ \"${SVM_ACTIVE_FLUTTER+x}\" != x ]; then if [ \"${FLUTTER_ROOT+x}\" = x ]; then export SVM_ORIGINAL_FLUTTER_ROOT=\"$FLUTTER_ROOT\" SVM_ORIGINAL_FLUTTER_ROOT_SET=1; else export SVM_ORIGINAL_FLUTTER_ROOT_SET=0; fi; fi\n")
		b.WriteString("export FLUTTER_ROOT=" + shellQuote(m.Directory("flutter", version)) + "\nexport SVM_ACTIVE_FLUTTER=" + shellQuote(version) + "\n")
	} else {
		b.WriteString("if [ \"${SVM_ACTIVE_FLUTTER+x}\" = x ]; then if [ \"${SVM_ORIGINAL_FLUTTER_ROOT_SET-0}\" = 1 ]; then export FLUTTER_ROOT=\"$SVM_ORIGINAL_FLUTTER_ROOT\"; else unset FLUTTER_ROOT; fi; unset SVM_ACTIVE_FLUTTER SVM_ORIGINAL_FLUTTER_ROOT SVM_ORIGINAL_FLUTTER_ROOT_SET; fi\n")
	}
}
