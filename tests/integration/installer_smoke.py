"""Offline installer lifecycle/security tests using mocked GitHub downloads."""
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile

INSTALLER = Path("install.sh").resolve()
SYSTEM = "macos" if platform.system() == "Darwin" else "linux"
CPU = "arm64" if platform.machine() in ("arm64", "aarch64") else "x64"

with tempfile.TemporaryDirectory(prefix="svm-installer-test-") as temporary:
    root = Path(temporary)
    home, tools, fixtures = [root / name for name in ("home", "tools", "releases")]
    for directory in (home, tools, fixtures):
        directory.mkdir()
    curl = tools / "curl"
    curl.write_text('''#!/usr/bin/env python3
import os, pathlib, shutil, sys
args=sys.argv[1:]
output=args[args.index('--output')+1]
url=args[-1]
with open(os.environ['SVM_TEST_LOG'],'a') as log: log.write(url+'\\n')
source=pathlib.Path(os.environ['SVM_TEST_RELEASE'])/url.rsplit('/',1)[-1]
if not source.is_file(): sys.exit(22)
shutil.copyfile(source,output)
''')
    curl.chmod(0o755)
    uname = tools / "uname"
    uname.write_text('''#!/bin/sh
case "$1" in -s) printf '%s\\n' "$SVM_TEST_UNAME" ;; -m) printf '%s\\n' "$SVM_TEST_CPU" ;; *) exit 1 ;; esac
''')
    uname.chmod(0o755)
    log = root / "downloads.log"
    env = dict(os.environ, HOME=str(home), SHELL="/bin/zsh", ZDOTDIR=str(home / "dotfiles"),
               PATH=str(tools) + os.pathsep + os.environ["PATH"], SVM_TEST_LOG=str(log),
               SVM_TEST_RELEASE=str(fixtures), SVM_TEST_UNAME=platform.system(), SVM_TEST_CPU=platform.machine())
    for key in ("SVM_INSTALL_DIR", "SVM_PROFILE", "SVM_VERSION", "SVM_NO_PROFILE"):
        env.pop(key, None)

    def release(version="v0.1.0", system=SYSTEM, cpu=CPU, bad_link=False, extra=False):
        package = f"svm-{version}-{system}-{cpu}"
        archive = fixtures / f"{package}.tar.gz"
        files = {"svm": f"#!/bin/sh\nprintf 'SDK Version Manager {version}\\n'\n".encode(),
                 "INSTALL.txt": b"install", "BUILD-INFO.json": b"{}",
                 "licenses/go-LICENSE.txt": b"Go license", "licenses/xz-LICENSE.txt": b"XZ license"}
        with tarfile.open(archive, "w:gz") as bundle:
            for name, data in files.items():
                info = tarfile.TarInfo(f"{package}/{name}")
                info.mode = 0o755 if name == "svm" else 0o644
                if name == "svm" and bad_link:
                    info.type = tarfile.SYMTYPE
                    info.linkname = "/tmp/untrusted-svm"
                    bundle.addfile(info)
                else:
                    info.size = len(data)
                    bundle.addfile(info, io.BytesIO(data))
            if extra:
                info = tarfile.TarInfo("../../escape")
                info.size = 4
                bundle.addfile(info, io.BytesIO(b"evil"))
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        (fixtures / "SHA256SUMS").write_text(f"{digest}  {archive.name}\n")
        return archive

    def run(args=(), overrides=None, success=True):
        result = subprocess.run(["sh", str(INSTALLER), *args], env=dict(env, **(overrides or {})), capture_output=True, text=True)
        assert (result.returncode == 0) == success, (result.returncode, result.stdout, result.stderr)
        return result

    release()
    run()
    installed = home / ".svm/bin/svm"
    assert installed.is_file() and os.access(installed, os.X_OK)
    profile = home / "dotfiles/.zshrc"
    first = profile.read_text()
    run()
    assert profile.read_text() == first, "repeat install changed the PATH block"
    assert "/download/v0.1.0/" in log.read_text(), "latest archive was not pinned to resolved version"
    assert "/latest/download/SHA256SUMS" in log.read_text()
    previous = installed.read_bytes()
    release("v0.2.0")
    run(("--version", "0.2.0"))
    assert "v0.2.0" in installed.read_text()
    assert (home / ".svm/bin/svm.previous").read_bytes() == previous
    previous = installed.read_bytes()
    before_profile = profile.read_bytes()
    # Failed downloads or verification never replace a working binary/profile.
    (fixtures / "SHA256SUMS").write_text("0" * 64 + f"  svm-v0.2.0-{SYSTEM}-{CPU}.tar.gz\n")
    run(("--version", "v0.2.0"), success=False)
    assert installed.read_bytes() == previous and profile.read_bytes() == before_profile
    assert not (home / ".svm/.installer-lock").exists()
    release("v0.2.0", bad_link=True)
    run(("--version", "v0.2.0"), success=False)
    assert installed.read_bytes() == previous
    release("v0.2.0", extra=True)
    run(("--version", "v0.2.0"))
    assert not (root / "escape").exists(), "installer unpacked unexpected archive members"
    # Custom locations with spaces/quotes and symlinked profiles remain usable.
    custom = root / "custom space's"
    real_profile = home / "real-profile"
    real_profile.write_text("export KEEP=yes\n")
    real_profile.chmod(0o640)
    linked_profile = home / "profile-link"
    linked_profile.symlink_to(real_profile.name)
    run(("--version", "v0.2.0"), {"SVM_INSTALL_DIR": str(custom), "SVM_PROFILE": str(linked_profile)})
    assert linked_profile.is_symlink() and real_profile.read_text().startswith("export KEEP=yes\n")
    assert real_profile.stat().st_mode & 0o777 == 0o640
    resolved = subprocess.check_output(["sh", "-c", '. "$1"; command -v svm', "sh", str(linked_profile)], env=env, text=True).strip()
    assert Path(resolved).resolve() == (custom / "bin/svm").resolve(), resolved
    baseline=subprocess.check_output(["sh","-c",'. "$1"; printf "%s" "$SVM_ORIGINAL_PATH"',"sh",str(linked_profile)],env=dict(env,SVM_ORIGINAL_PATH="/usr/bin"),text=True)
    assert baseline.startswith(str(custom.resolve()/"bin")+":"), baseline

    # All supported OS/CPU names resolve to the matching release asset.
    for system, kernel in (("macos", "Darwin"), ("linux", "Linux")):
        for cpu, machine in (("x64", "x86_64"), ("arm64", "aarch64")):
            release(system=system, cpu=cpu)
            location = root / f"{system}-{cpu}"
            run(("--no-profile",), {"SVM_INSTALL_DIR": str(location), "SVM_TEST_UNAME": kernel, "SVM_TEST_CPU": machine})
            assert (location / "bin/svm").exists()
    # Explicit opt-out doesn't create a shell profile.
    empty_home = root / "empty-home"
    empty_home.mkdir()
    release()
    run(("--no-profile",), {"HOME": str(empty_home), "ZDOTDIR": str(empty_home)})
    assert not (empty_home / ".zshrc").exists()
    foreign = root / "unowned"
    (foreign / "bin").mkdir(parents=True)
    (foreign / "bin/svm").write_text("user binary")
    run(("--no-profile",), {"SVM_INSTALL_DIR": str(foreign)}, success=False)
    assert (foreign / "bin/svm").read_text() == "user binary"
    print("PASS: latest/version install, upgrade, checksum failure, link rejection, safe extraction, profiles, quoting, permissions, platforms, opt-out and ownership")
