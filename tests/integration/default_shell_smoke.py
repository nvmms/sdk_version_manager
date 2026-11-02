"""Fresh-home default -> shell setup -> new terminal acceptance test."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

binary = str(Path("build/svm").resolve())
for shell, profile in (("bash", ".bashrc"), ("zsh", ".zshrc")):
    if not shutil.which(shell):
        print("SKIP:", shell, "not installed")
        continue
    with tempfile.TemporaryDirectory(prefix="svm-first-use-") as root:
        home, state = Path(root) / "home", Path(root) / "state"
        home.mkdir()
        exe = state / "installs/node/24.0.0/bin/node"
        exe.parent.mkdir(parents=True)
        exe.write_text("#!/bin/sh\nprintf 'v24.0.0\\n'\n")
        exe.chmod(0o755)
        env = dict(os.environ, HOME=str(home), SVM_HOME=str(state), SHELL=shutil.which(shell), TERM="xterm-256color")
        env.pop("SVM_ORIGINAL_PATH", None)
        env.pop("SVM_ACTIVE_NODE", None)
        if shell == "zsh":
            env["ZDOTDIR"] = str(home / "dotfiles")
            config_path = home / "dotfiles" / profile
        else:
            env.pop("ZDOTDIR", None)
            config_path = home / profile
        for _ in range(2):
            result = subprocess.run([binary, "default", "node", "24.0.0"], cwd=home, env=env, capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert "Shell integration ready" in result.stdout
        assert config_path.read_text().count("# >>> SVM managed environment >>>") == 1
        # No manual source, shell install, or environment evaluation step.
        result = subprocess.run([shell, "-ic", "node --version; command -v svm"], cwd=home, env=env, capture_output=True, text=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert "v24.0.0" in result.stdout and str(Path(binary).parent) in result.stdout
        if shell=="bash":
            result=subprocess.run([shell,"-lic","node --version; command -v svm"],cwd=home,env=env,capture_output=True,text=True)
            assert result.returncode==0 and "v24.0.0" in result.stdout,(result.stdout,result.stderr)
        inherited=dict(env,SVM_ORIGINAL_PATH=env["PATH"],SVM_ACTIVE_NODE="old")
        result=subprocess.run([shell,"-ic","node --version; command -v svm"],cwd=home,env=inherited,capture_output=True,text=True)
        assert result.returncode==0 and "v24.0.0" in result.stdout and str(Path(binary).parent) in result.stdout,(result.stdout,result.stderr)
        print("PASS:", shell, "fresh default setup, login/interactive shell, inherited environment, ZDOTDIR and idempotency")
