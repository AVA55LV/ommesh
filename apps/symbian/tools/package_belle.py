#!/usr/bin/env python3
"""Build a signed OMMesh Belle SIS using the historical Symbian SIS tools."""

import argparse
import shutil
import struct
import subprocess
import tempfile
import tomllib
from pathlib import Path

from symbian.packaging.registration import _LANGUAGES, _resource, compile_registration
from symbian.project.sdk import project_sdk

from prepare_belle_executable import (
    E32_EXE_UID,
    GUI_APPLICATION_UID,
    HEAP_MAX,
    HEAP_MIN,
    header_checksum,
    uid_checksum,
)

NOKIA_BELLE_PLATFORM_UID = 0x2003A678


def compile_bitmap_registration(
    project: Path, application: dict, executable_name: str, uid3: int
) -> list[tuple[str, bytes]]:
    captions = dict(application)
    icon_path = project / captions.pop("icon", "")
    if icon_path.suffix.lower() != ".mbm" or not icon_path.resolve().is_relative_to(project):
        raise ValueError("application.icon must point to a project-relative MBM file")

    icon = icon_path.read_bytes()
    uid1, uid2, uid3_bitmap, uid_crc, directory = struct.unpack_from("<5I", icon)
    if (uid1, uid2, uid3_bitmap) != (0x10000037, 0x10000042, 0) or uid_crc != uid_checksum(icon):
        raise ValueError("Invalid launcher MBM file store")
    count, color, mask = struct.unpack_from("<3I", icon, directory)
    if count != 2 or not 20 <= color < mask < directory or directory + 12 != len(icon):
        raise ValueError("Launcher MBM must contain one bitmap and its mask")
    image = struct.unpack_from("<10I", icon, color)
    transparency = struct.unpack_from("<10I", icon, mask)
    if (
        image[:4] != (40 + 64 * 64 * 3, 40, 64, 64)
        or transparency[:4] != (40 + 64 * 64, 40, 64, 64)
        or image[6:] != (24, 1, 0, 0) or transparency[6:] != (8, 0, 0, 0)
        or color + image[0] != mask or mask + transparency[0] != directory
    ):
        raise ValueError("Launcher MBM requires a 64px color bitmap and grayscale mask")
    # The Belle launcher treats an EGray256 soft mask as alpha.
    alpha = icon[mask + 40:directory]
    if any(alpha[corner] for corner in (0, 63, 63 * 64, 64 * 64 - 1)) or alpha[32 * 64 + 32] != 255:
        raise ValueError("Launcher mask requires transparent corners and an opaque center")

    # Reuse the SDK's registration UID and locale validation. Its standard icon
    # builder only emits vector MIFs, so compile CAPTION_AND_ICON_INFO for MBM.
    sdk = project_sdk(project)
    registration, _ = compile_registration(project, captions, executable_name, uid3, sdk=sdk)
    stem = Path(executable_name).stem
    icon_name = f"{stem}_icon.mbm"
    assets = [registration[0]]
    locales = {0: captions}
    locales.update({_LANGUAGES[tag]: text for tag, text in captions.get("localizations", {}).items()})
    with tempfile.TemporaryDirectory(prefix="ommesh-bitmap-registration-") as temporary:
        for number, text in sorted(locales.items()):
            source = (
                "CHARACTER_SET UTF8\n"
                "#include <AppInfo.rh>\n"
                "RESOURCE LOCALISABLE_APP_INFO\n"
                "{\n"
                f'  short_caption = "{text["short_caption"]}";\n'
                "  caption_and_icon = CAPTION_AND_ICON_INFO\n"
                "  {\n"
                f'    caption = "{text["caption"]}";\n'
                "    number_of_icons = 1;\n"
                f'    icon_file = "\\\\resource\\\\apps\\\\{icon_name}";\n'
                "  };\n"
                "}\n"
            )
            extension = "rsc" if number == 0 else f"r{number:02d}"
            target = f"!:\\resource\\apps\\{stem}_loc.{extension}"
            assets.append((target, _resource(sdk, Path(temporary), f"loc{number:02d}", source)))
    assets.append((f"!:\\resource\\apps\\{icon_name}", icon))
    return assets


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
    for name in ("omsk.bin", "omsk-detail.bin", "omsk-oblast.bin", "siberia.bin", "world.bin"):
        atlas = (project / "resources/maps" / name).read_bytes()
        if not atlas or atlas not in executable:
            raise ValueError(f"The OMMesh executable is missing the embedded map {name}")

    name, vendor = package["name"], package["vendor"]
    major, minor, build = package["version"]
    executable_name = package["executable_name"]
    assets = compile_bitmap_registration(project, manifest["application"], executable_name, uid3)
    files = [(f"!:\\sys\\bin\\{executable_name}", executable), *assets]

    with tempfile.TemporaryDirectory(prefix="ommesh-belle-") as temporary:
        stage = Path(temporary)
        description = [
            "&EN",
            f'#{{"{name}"}},({package["uid"]:#010x}),{major},{minor},{build}',
            f'%{{"{vendor}"}}',
            f':"{vendor}"',
            f'[{NOKIA_BELLE_PLATFORM_UID:#010x}],0,0,0,{{"S60ProductID"}}',
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
