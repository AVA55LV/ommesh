#!/usr/bin/env python3
"""Add the application UID type and Qt heap bounds to the Symbian E32 image."""

import argparse
import binascii
import struct
from pathlib import Path


E32_EXE_UID = 0x1000007A
GUI_APPLICATION_UID = 0x100039CE
E32_CRC_PLACEHOLDER = 0xC90FDAA2
OMMESH_UID = 0xE78AF613
HEAP_MIN = 0x20000
HEAP_MAX = 0x1800000


def uid_checksum(image: bytes | bytearray) -> int:
    low = binascii.crc_hqx(image[0:12:2], 0)
    high = binascii.crc_hqx(image[1:12:2], 0)
    return (high << 16) | low


def header_checksum(image: bytes | bytearray) -> int:
    (header_size,) = struct.unpack_from("<I", image, 100)
    if not 156 <= header_size <= min(len(image), 8348):
        raise ValueError("Invalid E32 V header size")
    header = bytearray(image[:header_size])
    struct.pack_into("<I", header, 20, E32_CRC_PLACEHOLDER)
    return binascii.crc32(header, 0xFFFFFFFF) ^ 0xFFFFFFFF


def prepare_image(image: bytes) -> bytes:
    if len(image) < 156 or image[16:20] != b"EPOC":
        raise ValueError("Expected an executable with an E32 V header")
    uid1, uid2, uid3, uid_crc = struct.unpack_from("<4I", image)
    (stored_header_crc,) = struct.unpack_from("<I", image, 20)
    (image_flags,) = struct.unpack_from("<I", image, 44)
    if (uid1, uid2, uid3, image_flags) != (E32_EXE_UID, 0, OMMESH_UID, 0x12000028):
        raise ValueError("Expected an unmodified OMMesh Belle SDK 0.3.0 executable")
    if uid_crc != uid_checksum(image) or stored_header_crc != header_checksum(image):
        raise ValueError("Source E32 checksum mismatch")

    result = bytearray(image)
    struct.pack_into("<I", result, 4, GUI_APPLICATION_UID)
    struct.pack_into("<I", result, 12, uid_checksum(result))
    struct.pack_into("<2I", result, 56, HEAP_MIN, HEAP_MAX)
    struct.pack_into("<I", result, 20, header_checksum(result))
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        parser.error("The source and destination must be different files")
    try:
        payload = prepare_image(args.source.read_bytes())
        args.destination.parent.mkdir(parents=True, exist_ok=True)
        args.destination.write_bytes(payload)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
