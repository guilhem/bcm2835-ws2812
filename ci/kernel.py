#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare matching headers from locked Pi sources/config and REAL vendor symbols."""
import argparse
import hashlib
import gzip
import json
import lzma
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def fetch(url, path, sha):
    if not path.exists():
        tmp = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(url, timeout=60) as response, tmp.open("wb") as out:
            shutil.copyfileobj(response, out)
        tmp.replace(path)
    with path.open("rb") as stream:
        actual = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual != sha:
        raise SystemExit(f"SHA256 mismatch: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=["armv6", "arm64"])
    args = parser.parse_args()
    lock = json.loads((ROOT / "kernels.lock.json").read_text())
    target = lock["targets"][args.target]
    downloads = ROOT / "build" / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    source = ROOT / "build" / "kernel-source"
    archive = downloads / "linux.tar.gz"
    fetch(f'https://codeload.github.com/raspberrypi/linux/tar.gz/{lock["linux"]}',
          archive, lock["source_sha256"])
    marker = source / ".ws2812-source"
    if not marker.exists() or marker.read_text().strip() != lock["linux"]:
        if source.exists():
            shutil.rmtree(source)
        source.mkdir()
        with tarfile.open(archive) as tar:
            prefix = tar.getmembers()[0].name + "/"
            for member in tar.getmembers():
                if member.name.startswith(prefix):
                    member.name = member.name[len(prefix):]
                    if member.name:
                        tar.extract(member, source, filter="data")
        marker.write_text(lock["linux"] + "\n")
    base = f'https://raw.githubusercontent.com/raspberrypi/firmware/{lock["firmware"]}'
    for key, directory in [("image", "boot"), ("symvers", "extra"), ("dtb", "boot")]:
        fetch(f'{base}/{directory}/{target[key]}', downloads / target[key], target[key + "_sha256"])
    headers = ROOT / "build" / args.target
    headers.mkdir(exist_ok=True)
    # IKCONFIG=m: the exact vendor config lives in configs.ko, not the image.
    config_module = downloads / f"{args.target}-configs.ko.xz"
    fetch(f'{base}/modules/{target["release"]}/kernel/kernel/configs.ko.xz',
          config_module, target["config_sha256"])
    packed = lzma.decompress(config_module.read_bytes())
    start = packed.index(b"IKCFG_ST") + 8
    end = packed.index(b"IKCFG_ED", start)
    config = gzip.decompress(packed[start:end])
    (headers / ".config").write_bytes(config)
    # Firmware builds add '+' to their releases, independently of git metadata.
    env = os.environ | {"ARCH": target["arch"], "CROSS_COMPILE": target["cross_compile"],
                        "LOCALVERSION": "+"}
    command = ["make", "-C", str(source), f"O={headers}"]
    subprocess.run(command + ["olddefconfig", "modules_prepare"], env=env, check=True)
    actual = (headers / "include/config/kernel.release").read_text().strip()
    if actual != target["release"]:
        raise SystemExit(f'kernel release mismatch: {actual} != {target["release"]}')
    if "CONFIG_LEDS_CLASS_MULTICOLOR=m\n" not in (headers / ".config").read_text():
        raise SystemExit("vendor config lacks multicolor LED module")
    # modules_prepare deliberately does NOT generate this file.
    shutil.copyfile(downloads / target["symvers"], headers / "Module.symvers")
    print(f"headers: {headers}\nvendor DTB: {downloads / target['dtb']}")


if __name__ == "__main__":
    main()
