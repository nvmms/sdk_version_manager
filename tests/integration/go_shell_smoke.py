"""Offline multi-Provider shell activation and Go variable restoration test."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

binary = str(Path("build/svm").resolve())
for shell in ("bash", "zsh"):
    if not shutil.which(shell):
        continue
    with tempfile.TemporaryDirectory(prefix="svm-go-shell-") as root:
        root = Path(root)
        home, state, project, outside = [root / p for p in ("home", "state", "project", "outside")]
        for directory in (home, project, outside):
            directory.mkdir()
        for version in ("1.27.2", "1.27.3"):
            exe = state / "installs/go" / version / "bin/go"
            exe.parent.mkdir(parents=True)
            exe.write_text("#!/bin/sh\nprintf '%s\\n' \"$GOROOT\"\n")
            exe.chmod(0o755)
        node = state / "installs/node/24.0.0/bin/node"
        node.parent.mkdir(parents=True)
        node.write_text("#!/bin/sh\nprintf 'v24.0.0\\n'\n")
        node.chmod(0o755)
        (project / ".svmrc").write_text(json.dumps({"schemaVersion": 1, "sdks": {"node": "24.0.0", "go": "1.27.2"}}))
        env = dict(os.environ, HOME=str(home), SVM_HOME=str(state), SHELL=shutil.which(shell),
                   GOROOT="original-root", GOTOOLCHAIN="auto", TERM="xterm-256color",
                   SVM_TEST_BINARY=binary, SVM_TEST_PROJECT=str(project), SVM_TEST_OUTSIDE=str(outside), SVM_TEST_STATE=str(state))
        for key in list(env):
            if key.startswith("SVM_ACTIVE_") or key.startswith("SVM_ORIGINAL_"):
                env.pop(key)
        env.pop("ZDOTDIR", None)
        subprocess.run([binary, "default", "node", "24.0.0", "--shell", shell], env=env, cwd=outside, check=True, capture_output=True)
        subprocess.run([binary, "default", "go", "1.27.3", "--shell", shell], env=env, cwd=outside, check=True, capture_output=True)
        script = '''
[ "$(go)" = "$SVM_TEST_STATE/installs/go/1.27.3" ] || exit 21
[ "$GOTOOLCHAIN" = local ] || exit 22
[ "$(node --version)" = v24.0.0 ] || exit 23
cd "$SVM_TEST_PROJECT"
_svm_prompt
[ "$(go)" = "$SVM_TEST_STATE/installs/go/1.27.2" ] || exit 24
"$SVM_TEST_BINARY" default go --clear --no-shell >/dev/null
cd "$SVM_TEST_OUTSIDE"
_svm_prompt
[ "$GOROOT" = original-root ] || exit 25
[ "$GOTOOLCHAIN" = auto ] || exit 26
[ "$(node --version)" = v24.0.0 ] || exit 27
[ "${SVM_ACTIVE_GO+x}" != x ] || exit 28
'''
        result = subprocess.run([shell, "-ic", script], env=env, cwd=outside, text=True, capture_output=True)
        assert result.returncode == 0, (shell, result.returncode, result.stdout, result.stderr)
        print("PASS:", shell, "Node+Go coexistence, project override, Go default activation, GOROOT/GOTOOLCHAIN restoration")
