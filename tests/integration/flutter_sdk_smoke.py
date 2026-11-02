"""Opt-in real Flutter SDK download/lifecycle test; archives can exceed 2 GiB."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

version = os.environ.get("SVM_TEST_FLUTTER_VERSION")
if not version:
    print("SKIP: set SVM_TEST_FLUTTER_VERSION to opt into a real official Flutter download")
    raise SystemExit(0)
binary = str(Path("build/svm.exe" if os.name == "nt" else "build/svm").resolve())
with tempfile.TemporaryDirectory(prefix="svm-flutter-official-") as root:
    root = Path(root)
    home, project, outside, state = [root / p for p in ("home", "project", "outside", "state")]
    for directory in (home, project, outside):
        directory.mkdir()
    env = dict(os.environ, SVM_HOME=str(state), HOME=str(home), USERPROFILE=str(home),
               XDG_CONFIG_HOME=str(home / "config"), PUB_CACHE=str(root / "pub-cache"),
               CI="true", FLUTTER_SUPPRESS_ANALYTICS="true", DART_SUPPRESS_ANALYTICS="true",
               FLUTTER_ROOT="stale-root")

    def run(*args, cwd=project):
        print("$ svm", *args, flush=True)
        result = subprocess.run([binary, *args], cwd=cwd, env=env, capture_output=True, text=True, timeout=1800)
        print((result.stdout + result.stderr)[-2200:], flush=True)
        assert result.returncode == 0, result.returncode
        return result.stdout

    assert version in run("list", "stable", "flutter", "--plain")
    run("install", "flutter", version)
    run("default", "flutter", version, "--no-shell")
    run("use", "flutter")
    reported = json.loads(run("flutter", "--version", "--machine"))
    assert reported["frameworkVersion"] == version, reported
    run("dart", "--version")
    run("doctor")
    run("default", "flutter", "--clear", "--no-shell")
    run("uninstall", "flutter", version, "--yes", cwd=outside)
    assert not (state / "installs/flutter" / version).exists()
    run("proxy", "set", "http://127.0.0.1:1")
    run("use", "flutter", version)
    assert (state / "installs/flutter" / version / "bin").is_dir()
    print("PASS: official Flutter install, channel list, selection, Flutter/Dart proxies, health, uninstall and cached offline use", flush=True)
