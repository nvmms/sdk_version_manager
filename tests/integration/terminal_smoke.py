"""Unix PTY integration test; run after `go build -o build/svm ./cmd/svm`."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

BINARY = str(Path("build/svm").resolve())
# Keep the session leader alive while checking the child's terminal restoration.
WRAPPER = """
import subprocess, sys, termios
before = termios.tcgetattr(0)
result = subprocess.run(sys.argv[1:])
after = termios.tcgetattr(0)
# Darwin may set PENDIN when canonical mode is re-enabled; it is kernel state.
before[3] &= ~getattr(termios, 'PENDIN', 0)
after[3] &= ~getattr(termios, 'PENDIN', 0)
assert before == after, 'terminal mode was not restored'
print('TERMINAL_RESTORED', flush=True)
sys.exit(result.returncode)
"""


def session_setup():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)
    os.tcsetpgrp(0, os.getpgrp())


def check_session(state, project, steps, exit_key, provider="node"):
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 90, 0, 0))
    process = subprocess.Popen(
        [sys.executable, "-c", WRAPPER, BINARY, "list", provider],
        stdin=slave, stdout=slave, stderr=slave, cwd=project,
        env=dict(os.environ, SVM_HOME=str(state), TERM="xterm-256color"),
        preexec_fn=session_setup,
    )
    output = bytearray()

    def drain():
        if select.select([master], [], [], 0.02)[0]:
            try:
                output.extend(os.read(master, 65536))
            except BlockingIOError:
                pass
            except OSError as error:
                # Linux returns EIO after a PTY session ends.
                if error.errno != 5:
                    raise

    def expect(fragment):
        deadline = time.monotonic() + 5
        while fragment not in output:
            drain()
            assert time.monotonic() < deadline, (fragment, output[-1600:])

    try:
        expect(b"1 / 40")
        assert process.poll() is None
        assert b"24.0.0 " not in output, "the entire list was rendered"
        for key, fragment in steps:
            output.clear()
            os.write(master, key)
            expect(fragment)
        if exit_key is not None:
            output.clear()
            os.write(master, exit_key)
        # Consume output while waiting: PTY close can wait for pending output.
        deadline = time.monotonic() + 5
        while process.poll() is None:
            drain()
            assert time.monotonic() < deadline, ("exit timed out", output[-1600:])
        for _ in range(5):
            drain()
        assert process.returncode == 0, output[-1600:]
        assert b"TERMINAL_RESTORED" in output
        assert b"\x1b[?1049l" in output
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        os.close(master)
        os.close(slave)


with tempfile.TemporaryDirectory(prefix="svm-terminal-") as root:
    state, project = Path(root) / "state", Path(root) / "project"
    project.mkdir()
    cache = state / "cache/node/versions.json"
    cache.parent.mkdir(parents=True)
    cache.write_text(json.dumps([
        {"version": f"v24.0.{i}", "date": "2025-05-06", "lts": False,
         "files": ["osx-x64-tar", "osx-arm64-tar", "linux-x64", "linux-arm64"]}
        for i in range(40)
    ]))
    executable = state / "installs/node/24.0.39/bin/node"
    executable.parent.mkdir(parents=True)
    executable.write_text("fixture; never executed")
    executable.chmod(0o755)
    check_session(state, project, [
        (b"\x1b[B", b"2 / 40"),
        (b"\x1b[6~", b"13 / 40"),
        (b"\x1b[F", b"40 / 40"),
        (b"\x1b[H", b"1 / 40"),
        (b"\r", b"svm use node 24.0.39"),
    ], None)
    assert not (project / ".svmrc").exists(), "download unexpectedly bound the project"
    check_session(state, project, [], b"q")
    check_session(state, project, [], b"\x1b")
    check_session(state, project, [], b"\x03")
    # Redirected output is deterministic plain text even when a terminal is available.
    result = subprocess.run(
        [BINARY, "list", "node", "--plain"], cwd=project,
        env=dict(os.environ, SVM_HOME=str(state)), capture_output=True, check=True,
    )
    assert b"\x1b" not in result.stdout and b"40 versions" in result.stdout
print("PASS: paging, selection, direct install, usage instructions, exit, restoration and plain output")

# Go uses the same picker and Enter-to-install workflow without Node-specific output.
import platform
with tempfile.TemporaryDirectory(prefix="svm-go-terminal-") as root:
    state, project = Path(root) / "state", Path(root) / "project"
    project.mkdir()
    host_os = "darwin" if sys.platform == "darwin" else "linux"
    host_arch = "arm64" if platform.machine() in ("arm64", "aarch64") else "amd64"
    cache = state / "cache/go/versions.json"
    cache.parent.mkdir(parents=True)
    cache.write_text(json.dumps([
        {"version": f"go1.27.{i}", "stable": True, "files": [
            {"filename": f"go1.27.{i}.{host_os}-{host_arch}.tar.gz", "os": host_os,
             "arch": host_arch, "kind": "archive", "sha256": "0" * 64, "size": 1}
        ]} for i in range(40)
    ]))
    executable = state / "installs/go/1.27.39/bin/go"
    executable.parent.mkdir(parents=True)
    executable.write_text("fixture; never executed")
    executable.chmod(0o755)
    check_session(state, project, [(b"\r", b"svm use go 1.27.39")], None, provider="go")
print("PASS: Go picker direct install, Go-specific usage and terminal restoration")
