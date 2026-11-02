"""Opt-in official Go SDK lifecycle test: SVM_TEST_GO_VERSION=1.27.3 python3 ..."""
import os
from pathlib import Path
import subprocess
import tempfile

version = os.environ.get("SVM_TEST_GO_VERSION")
if not version:
    print("SKIP: set SVM_TEST_GO_VERSION to opt into a real official SDK download")
    raise SystemExit(0)
binary = str(Path("build/svm.exe" if os.name == "nt" else "build/svm").resolve())
with tempfile.TemporaryDirectory(prefix="svm-go-official-") as root:
    root = Path(root)
    home, project, outside, state = [root / p for p in ("home", "project", "outside", "state")]
    for directory in (home, project, outside):
        directory.mkdir()
    env = dict(os.environ, SVM_HOME=str(state), HOME=str(home), USERPROFILE=str(home),
               XDG_CONFIG_HOME=str(home / "config"), GOPATH=str(root / "gopath"),
               GOCACHE=str(root / "gocache"), GOPROXY="off", GOROOT="stale-root", GOTOOLCHAIN="auto")

    def run(*args, cwd=project):
        result = subprocess.run([binary, *args], cwd=cwd, env=env, capture_output=True, text=True, timeout=240)
        print("$ svm", *args, flush=True)
        print((result.stdout + result.stderr)[-1200:], flush=True)
        assert result.returncode == 0, result.returncode
        return result.stdout

    assert version in run("list", "go", "--plain")
    run("install", "go", version)
    run("default", "go", version, "--no-shell")
    run("use", "go")
    assert "go" + version in run("go", "version")
    actual = Path(run("go", "env", "GOROOT").strip()).resolve()
    assert actual == (state / "installs/go" / version).resolve()
    assert run("go", "env", "GOTOOLCHAIN").strip() == "local"
    (project / "main.go").write_text('package main\nimport "fmt"\nfunc main() {fmt.Println("svm-go-ok")}\n')
    assert "svm-go-ok" in run("go", "run", "main.go")
    run("gofmt", "-w", "main.go")
    run("doctor")
    run("default", "go", "--clear", "--no-shell")
    run("uninstall", "go", version, "--yes", cwd=outside)
    assert not (state / "installs/go" / version).exists()
    # An unusable proxy proves reinstall needs neither an index nor archive request.
    run("proxy", "set", "http://127.0.0.1:1")
    run("use", "go", version)
    assert "go" + version in run("go", "version")
    print("PASS: official Go install, project/global selection, proxy, GOROOT, local toolchain, go run, gofmt, doctor, uninstall and offline use", flush=True)
