#!/usr/bin/env python3
"""Build and package one portable CLI target using only Python and Go."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION_PATTERN = re.compile(r"(?:v?\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?|dev-[0-9a-f]{7,40})\Z")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--os", choices=("windows", "darwin", "linux"), required=True)
    parser.add_argument("--arch", choices=("amd64", "arm64"), required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    if not VERSION_PATTERN.fullmatch(args.version):
        parser.error("version must be SemVer (e.g. v0.1.0) or dev-<commit>")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    go = os.environ.get("SVM_BUILD_GO", "go")
    go_version = subprocess.check_output([go, "version"], text=True).strip()
    system = "macos" if args.os == "darwin" else args.os
    arch = "x64" if args.arch == "amd64" else "arm64"
    name = f"svm-{args.version}-{system}-{arch}"
    env = dict(os.environ, GOOS=args.os, GOARCH=args.arch, CGO_ENABLED="0")
    with tempfile.TemporaryDirectory(prefix="svm-package-") as temporary:
        stage = Path(temporary) / name
        stage.mkdir()
        binary = stage / ("svm.exe" if args.os == "windows" else "svm")
        subprocess.run(
            [go, "build", "-mod=readonly", "-trimpath", "-buildvcs=false",
             "-ldflags=-s -w -buildid=", "-o", str(binary), "./cmd/svm"],
            cwd=ROOT, env=env, check=True,
        )
        binary.chmod(0o755)
        host_os = {"Darwin": "darwin", "Windows": "windows", "Linux": "linux"}.get(platform.system())
        host_arch = {"x86_64": "amd64", "AMD64": "amd64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine())
        if (args.os, args.arch) == (host_os, host_arch):
            result = subprocess.run([str(binary), "--help"], cwd=stage, capture_output=True, text=True, check=True, timeout=20)
            if "SDK Version Manager" not in result.stdout:
                raise RuntimeError("packaged CLI help smoke test failed")
        for source in ("README.md", "README.en.md"):
            shutil.copy2(ROOT / source, stage / source)
        shutil.copytree(ROOT / "docs/third-party", stage / "licenses")
        (stage / "BUILD-INFO.json").write_text(json.dumps({
            "version": args.version, "commit": commit, "go": go_version,
            "os": args.os, "arch": args.arch, "cgo": False,
        }, indent=2) + "\n", encoding="utf-8")
        (stage / "INSTALL.txt").write_text(
            "SDK Version Manager (SVM) portable CLI\n\n"
            "Extract the entire archive and add the directory containing svm to your user PATH.\n"
            "Windows: run svm.exe --help. macOS/Linux: run ./svm --help.\n"
            "Example: svm list stable flutter\n"
            "A global default also configures supported shell integration:\n"
            "  svm default node <installed-version>\n"
            "Follow the printed activation step for the current terminal.\n"
            "No administrator privileges or Qt runtime are required.\n"
            "These packages are unsigned and are not notarized.\n",
            encoding="utf-8",
        )
        if args.os == "windows":
            archive = output / f"{name}.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
                for path in sorted(stage.rglob("*")):
                    if path.is_file():
                        bundle.write(path, path.relative_to(stage.parent).as_posix())
            with zipfile.ZipFile(archive) as bundle:
                if bundle.testzip() is not None:
                    raise RuntimeError("ZIP integrity check failed")
        else:
            archive = output / f"{name}.tar.gz"
            with tarfile.open(archive, "w:gz") as bundle:
                bundle.add(stage, arcname=name)
            with tarfile.open(archive, "r:gz") as bundle:
                if not bundle.getmember(f"{name}/svm").mode & 0o111:
                    raise RuntimeError("tar archive lost executable permissions")
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    checksum = digest.hexdigest()
    archive.with_name(archive.name + ".sha256").write_text(f"{checksum}  {archive.name}\n", encoding="ascii")
    print(archive)


if __name__ == "__main__":
    main()
