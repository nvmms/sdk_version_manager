package sdk

import (
	"bytes"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/nvmms/sdk_version_manager/internal/config"
	"github.com/nvmms/sdk_version_manager/internal/filelock"
)

const hookStart = "# >>> SVM managed environment >>>"
const hookEnd = "# <<< SVM managed environment <<<"

func (m *Manager) ShellHook(shell string, install bool) (string, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return "", err
	}
	var path string
	switch shell {
	case "bash":
		path = filepath.Join(home, ".bashrc")
	case "zsh":
		dotdir := os.Getenv("ZDOTDIR")
		if dotdir == "" {
			dotdir = home
		}
		if !filepath.IsAbs(dotdir) {
			return "", errors.New("ZDOTDIR must be an absolute directory")
		}
		path = filepath.Join(dotdir, ".zshrc")
	case "powershell":
		program, err := exec.LookPath("pwsh")
		if err != nil {
			program, err = exec.LookPath("powershell")
		}
		if err != nil {
			return "", errors.New("PowerShell not found; install PowerShell before configuring its hook")
		}
		command := exec.Command(program, "-NoProfile", "-NonInteractive", "-Command", "[Console]::Write($PROFILE.CurrentUserAllHosts)")
		b, err := command.Output()
		if err != nil {
			return "", err
		}
		path = strings.TrimSpace(strings.TrimPrefix(string(b), "\ufeff"))
		if !filepath.IsAbs(path) {
			return "", errors.New("PowerShell returned an invalid profile path")
		}
	default:
		return "", errors.New("supported shells: bash, zsh, powershell")
	}
	executable, err := os.Executable()
	if err != nil {
		return "", err
	}
	if err = writeShellHook(path, shell, executable, install); err != nil {
		return "", err
	}
	if shell == "bash" {
		candidates := []string{filepath.Join(home, ".bash_profile"), filepath.Join(home, ".bash_login"), filepath.Join(home, ".profile")}
		if install {
			login := candidates[0]
			for _, candidate := range candidates {
				if _, e := os.Stat(candidate); e == nil {
					login = candidate
					break
				} else if !errors.Is(e, os.ErrNotExist) {
					return "", e
				}
			}
			if err = writeShellHook(login, shell, executable, true); err != nil {
				return "", err
			}
		} else {
			for _, candidate := range candidates {
				if _, e := os.Stat(candidate); e == nil {
					if err = writeShellHook(candidate, shell, executable, false); err != nil {
						return "", err
					}
				} else if !errors.Is(e, os.ErrNotExist) {
					return "", e
				}
			}
		}
	}
	return path, nil
}
func writeShellHook(path, shell, executable string, install bool) error {
	if info, err := os.Lstat(path); err == nil && info.Mode()&os.ModeSymlink != 0 {
		target, err := filepath.EvalSymlinks(path)
		if err != nil {
			return err
		}
		path = target
	}
	lock, err := filelock.Acquire(path+".svm.lock", false)
	if err != nil {
		return err
	}
	defer lock.Close()
	original, err := os.ReadFile(path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	text := string(original)
	start, end := strings.Index(text, hookStart), strings.Index(text, hookEnd)
	if (start < 0) != (end < 0) || start >= 0 && (end < start || strings.Count(text, hookStart) != 1 || strings.Count(text, hookEnd) != 1) {
		return errors.New("malformed SVM profile block; repair its markers manually")
	}
	if start >= 0 {
		end += len(hookEnd)
		if end < len(text) && text[end] == '\r' {
			end++
		}
		if end < len(text) && text[end] == '\n' {
			end++
		}
		text = text[:start] + text[end:]
	}
	if install {
		body := ""
		switch shell {
		case "bash":
			body = "_svm_prompt() { local svm_script; if svm_script=$(" + shellQuote(executable) + " __env bash); then eval \"$svm_script\"; fi; }\n" +
				"if [[ $(declare -p PROMPT_COMMAND 2>/dev/null) == 'declare -a'* ]]; then\n  [[ \" ${PROMPT_COMMAND[*]} \" == *' _svm_prompt '* ]] || PROMPT_COMMAND+=(_svm_prompt)\nelse\n  [[ \";${PROMPT_COMMAND-};\" == *';_svm_prompt;'* ]] || PROMPT_COMMAND=\"_svm_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}\"\nfi\n"
		case "zsh":
			body = "_svm_prompt() { local svm_script; if svm_script=$(" + shellQuote(executable) + " __env zsh); then eval \"$svm_script\"; fi; }\ntypeset -ga precmd_functions\n(( ${precmd_functions[(Ie)_svm_prompt]} )) || precmd_functions+=(_svm_prompt)\n"
		case "powershell":
			body = "if (!(Test-Path Function:global:_svm_original_prompt)) { Copy-Item Function:prompt Function:global:_svm_original_prompt }\nfunction global:prompt {\n  $svmScript = @(& " + powershellQuote(executable) + " __env powershell)\n  if ($LASTEXITCODE -eq 0) { Invoke-Expression ($svmScript -join [Environment]::NewLine) }\n  & _svm_original_prompt\n}\n"
		default:
			return errors.New("unsupported shell")
		}

		bin := filepath.Dir(executable)
		if shell == "powershell" {
			setup := "if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains " + powershellQuote(bin) + ") { $env:PATH=" + powershellQuote(bin+string(os.PathListSeparator)) + "+$env:PATH }\n"
			setup += "if ((Test-Path Env:SVM_ORIGINAL_PATH) -and (($env:SVM_ORIGINAL_PATH -split [IO.Path]::PathSeparator) -notcontains " + powershellQuote(bin) + ")) { $env:SVM_ORIGINAL_PATH=" + powershellQuote(bin+string(os.PathListSeparator)) + "+$env:SVM_ORIGINAL_PATH }\n"
			body = setup + body
		} else {
			setup := "case \":$PATH:\" in *" + shellQuote(":"+bin+":") + "*) ;; *) export PATH=" + shellQuote(bin+string(os.PathListSeparator)) + "\"$PATH\" ;; esac\n"
			setup += "if [ \"${SVM_ORIGINAL_PATH+x}\" = x ]; then case \":$SVM_ORIGINAL_PATH:\" in *" + shellQuote(":"+bin+":") + "*) ;; *) export SVM_ORIGINAL_PATH=" + shellQuote(bin+string(os.PathListSeparator)) + "\"$SVM_ORIGINAL_PATH\" ;; esac; fi\n"
			body = setup + body
		}
		if shell == "bash" || shell == "zsh" {
			body += "_svm_prompt\n"
		}
		if text != "" && !strings.HasSuffix(text, "\n") {
			text += "\n"
		}
		text += hookStart + "\n" + body + hookEnd + "\n"
	}
	if !install && start < 0 {
		return nil
	}
	current, err := os.ReadFile(path)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	if !bytes.Equal(current, original) {
		return errors.New("shell profile changed; retry")
	}
	if text == string(original) {
		return nil
	}
	mode := os.FileMode(0600)
	if info, err := os.Stat(path); err == nil {
		mode = info.Mode().Perm()
	}
	if err := config.Atomic(path, []byte(text)); err != nil {
		return err
	}
	return os.Chmod(path, mode)
}
