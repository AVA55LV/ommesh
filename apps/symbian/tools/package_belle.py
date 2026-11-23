#!/usr/bin/env python3
"""Build a signed OMMesh Belle SIS using the historical Symbian SIS tools."""

import argparse
import shutil
import struct
import subprocess
import tempfile
import tomllib
from pathlib import Path

from symbian.packaging.registration import compile_registration

from prepare_belle_executable import (
    E32_EXE_UID,
    GUI_APPLICATION_UID,
    HEAP_MAX,
    HEAP_MIN,
    header_checksum,
    uid_checksum,
)


def create_package(args: argparse.Namespace) -> None:
    project = Path(__file__).resolve().parents[1]
    manifest = tomllib.loads((project / "symbian.toml").read_text(encoding="utf-8"))
    package = manifest["package"]
    executable = args.executable.read_bytes()
    uid1, uid2, uid3, uid_crc = struct.unpack_from("<4I", executable)
    (header_crc,) = struct.unpack_from("<I", executable, 20)
    if (
        executable[16:20] != b"EPOC"
        or (uid1, uid2, uid3) != (E32_EXE_UID, GUI_APPLICATION_UID, manifest["project"]["uid3"])
        or (uid_crc, header_crc) != (uid_checksum(executable), header_checksum(executable))
        or struct.unpack_from("<2I", executable, 56) != (HEAP_MIN, HEAP_MAX)
    ):
        raise ValueError("Expected the OMMesh executable from CMake target ommesh_belle_compat")
    for name in ("omsk.bin", "omsk-buildings.bin", "omsk-oblast.bin", "siberia.bin", "world.bin"):
        atlas = (project / "resources/maps" / name).read_bytes()
        if not atlas or atlas not in executable:
            raise ValueError(f"The OMMesh executable is missing the embedded map {name}")

    name, vendor = package["name"], package["vendor"]
    major, minor, build = package["version"]
    executable_name = package["executable_name"]
    assets, _ = compile_registration(project, manifest["application"], executable_name, uid3)
    files = [(f"!:\\sys\\bin\\{executable_name}", executable), *assets]

    with tempfile.TemporaryDirectory(prefix="ommesh-belle-") as temporary:
        stage = Path(temporary)
        description = [
            "&EN",
            f'#{{"{name}"}},({package["uid"]:#010x}),{major},{minor},{build}',
            f'%{{"{vendor}"}}',
            f':"{vendor}"',
        ]
        for index, (target, payload) in enumerate(files):
            source = f"payload-{index}"
            (stage / source).write_bytes(payload)
            description.append(f'"{source}"-"{target}"')
        (stage / "ommesh.pkg").write_text("\n".join(description) + "\n", encoding="utf-8")

        subprocess.run([args.makesis, "ommesh.pkg", "unsigned.sis"], cwd=stage, check=True)
        subprocess.run(
            [args.signsis, "-cr", "-s", "unsigned.sis", "signed.sis",
             str(args.cert.resolve()), str(args.key.resolve())],
            cwd=stage,
            check=True,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(stage / "signed.sis", args.output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--makesis", default="makesis")
    parser.add_argument("--signsis", default="signsis")
    args = parser.parse_args()
    if args.output.resolve() in (args.executable.resolve(), args.cert.resolve(), args.key.resolve()):
        parser.error("The output must be distinct from the inputs")
    try:
        create_package(args)
    except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
