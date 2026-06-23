#!/usr/bin/env python3
import pathlib
import shutil
import sys
import tarfile
import tempfile
import urllib.request


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: setup_vcpkg.py <baseline-sha> <destination>", file=sys.stderr)
        return 2

    baseline = sys.argv[1]
    destination = pathlib.Path(sys.argv[2])
    url = f"https://github.com/microsoft/vcpkg/archive/{baseline}.tar.gz"

    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="blazerules-vcpkg-"))
    try:
        archive = tmpdir / "vcpkg.tar.gz"
        urllib.request.urlretrieve(url, archive)
        with tarfile.open(archive, "r:gz") as tf:
            tf.extractall(tmpdir)

        roots = [p for p in tmpdir.iterdir() if p.is_dir() and p.name.startswith("vcpkg-")]
        if not roots:
            raise RuntimeError("vcpkg archive did not contain a vcpkg-* root directory")

        shutil.rmtree(destination, ignore_errors=True)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(roots[0]), str(destination))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
