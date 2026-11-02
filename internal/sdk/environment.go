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

// Environment emits activation or restoration for the calling shell. It never
// changes the parent environment itself and does not persist a machine PATH.
func (m *Manager) Environment(dir, shell string) (string, error) {
	if shell != "bash" && shell != "zsh" && shell != "powershell" {
		return "", errors.New("supported shells: bash, zsh, powershell")
	}
	version, _, err := m.Resolve(dir, "node", false)
	if err != nil {
		if !errors.Is(err, ErrNoVersion) {
			return "", err
		}
		if shell == "powershell" {
			return "if (Test-Path Env:SVM_ORIGINAL_PATH) { $env:PATH=$env:SVM_ORIGINAL_PATH; Remove-Item Env:SVM_ORIGINAL_PATH }; Remove-Item Env:SVM_ACTIVE_NODE -ErrorAction SilentlyContinue\n", nil
		}
		return "if [ \"${SVM_ORIGINAL_PATH+x}\" = x ]; then export PATH=\"$SVM_ORIGINAL_PATH\"; unset SVM_ORIGINAL_PATH; fi\nunset SVM_ACTIVE_NODE\n", nil
	}
	if !m.Installed("node", version) {
		return "", fmt.Errorf("node %s is not installed; run svm install node %s", version, version)
	}
	entry, _ := (Node{}).Entry("node")
	bin := filepath.Dir(filepath.Join(m.Directory("node", version), entry.Executable))
	if shell == "powershell" {
		return "if (!(Test-Path Env:SVM_ORIGINAL_PATH)) { $env:SVM_ORIGINAL_PATH=$env:PATH }\n$env:PATH=" + powershellQuote(bin+string(os.PathListSeparator)) + "+$env:SVM_ORIGINAL_PATH\n$env:SVM_ACTIVE_NODE=" + powershellQuote(version) + "\n", nil
	}
	return "if [ \"${SVM_ORIGINAL_PATH+x}\" != x ]; then export SVM_ORIGINAL_PATH=\"$PATH\"; fi\nexport PATH=" + shellQuote(bin+string(os.PathListSeparator)) + "\"$SVM_ORIGINAL_PATH\"\nexport SVM_ACTIVE_NODE=" + shellQuote(version) + "\n", nil
}
