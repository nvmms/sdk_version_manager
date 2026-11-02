"""Offline Bash/Zsh activation test using an isolated home and a fake Node.js."""
import json
import os
from pathlib import Path
import shutil
import signal
import time
import select
import subprocess
import tempfile

binary = str(Path("build/svm").resolve())
with tempfile.TemporaryDirectory(prefix="svm-shell-") as root:
    home, state, project, outside = [Path(root) / name for name in ("home", "state", "project", "outside")]
    for directory in (home, project, outside):
        directory.mkdir()
    exe = state / "installs/node/24.0.0/bin/node"
    exe.parent.mkdir(parents=True)
    exe.write_text("#!/bin/sh\nprintf 'v24.0.0\\n'\n")
    exe.chmod(0o755)
    (project / ".svmrc").write_text(json.dumps({"schemaVersion": 1, "sdks": {"node": "24.0.0"}}))
    env = dict(os.environ, HOME=str(home), SVM_HOME=str(state))
    env.pop("SVM_ORIGINAL_PATH", None)
    env.pop("SVM_ACTIVE_NODE", None)
    for shell, profile in (("bash", ".bashrc"), ("zsh", ".zshrc")):
        if not shutil.which(shell):
            print("SKIP:", shell, "not available")
            continue
        path = home / profile
        original = "# existing profile\nexport SVM_TEST_KEEP=yes\n"
        path.write_text(original)
        for _ in range(2):
            subprocess.run([binary, "default", "node", "24.0.0", "--shell", shell], env=env, check=True, capture_output=True)
        assert path.read_text().count("# >>> SVM managed environment >>>") == 1
        script = '''
source "$HOME/PROFILE"
base_path="${SVM_ORIGINAL_PATH:-$PATH}"
cd "$SVM_TEST_PROJECT"
_svm_prompt
[ "$SVM_TEST_KEEP" = yes ] || exit 8
[ "$(node --version)" = v24.0.0 ] || exit 9
first_path="$PATH"
_svm_prompt
[ "$PATH" = "$first_path" ] || exit 10
"$SVM_TEST_BINARY" default node --clear --no-shell >/dev/null
cd "$SVM_TEST_OUTSIDE"
_svm_prompt
[ "$PATH" = "$base_path" ] || exit 11
[ "${SVM_ACTIVE_NODE+x}" != x ] || exit 12
printf 'activation and restoration passed\\n'
'''.replace("PROFILE", profile)
        result = subprocess.run([shell, "-c", script], env=dict(env, SVM_TEST_PROJECT=str(project), SVM_TEST_OUTSIDE=str(outside), SVM_TEST_BINARY=binary), capture_output=True, text=True)
        assert result.returncode == 0, (shell, result.stdout, result.stderr)
        assert path.read_text().startswith(original)
        print("PASS:", shell, "activation, repeated prompt, directory exit, profile preservation")

    # A signal sent specifically to SVM is forwarded to the selected SDK process.
    exe.write_text("#!/bin/sh\ntrap 'exit 23' TERM\nprintf 'READY\\n'\nwhile :; do sleep 0.1; done\n")
    proc = subprocess.Popen([binary, "node"], cwd=project, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        assert select.select([proc.stdout], [], [], 5)[0], "SDK proxy did not start"
        assert proc.stdout.readline().strip() == "READY"
        os.kill(proc.pid, signal.SIGTERM)
        assert proc.wait(timeout=5) == 23
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    print("PASS: SDK proxy forwards SIGTERM and preserves the child exit code")
