"""Build independently compressed offline Omsk building sectors from an OSM PBF."""

import argparse
import json
import math
import re
import struct
import subprocess
import zlib
from collections import defaultdict
from pathlib import Path

BOUNDS = (72.70, 54.68, 74.05, 55.32)
SECTOR_ZOOM = 15
SUBDIVISIONS = 4096
WORLD_UNITS = (1 << SECTOR_ZOOM) * SUBDIVISIONS
MAX_RINGS = 24
MAX_RING_POINTS = 512
MAX_SHAPES = 4096
MAX_LABELS = 4096
MAX_SECTOR_BYTES = 256 * 1024
NUMBER = re.compile(r'"addr:housenumber"\s*=>\s*"((?:[^"\\]|\\.)*)"')
STREET = re.compile(r'"addr:street"\s*=>\s*"((?:[^"\\]|\\.)*)"')


def extract(pbf, work):
    bbox = [str(value) for value in BOUNDS]
    layers = (
        ("buildings", "multipolygons", "building,other_tags",
         "building IS NOT NULL AND building <> 'no' AND building <> 'proposed'",
         ["-clipsrc", *bbox, "-simplify", "0.000005"]),
        ("addresses", "points", "other_tags", "other_tags LIKE '%addr:housenumber%'", []),
    )
    for name, layer, fields, where, extra in layers:
        output = work / (name + ".geojson")
        output.unlink(missing_ok=True)
        subprocess.run([
            "ogr2ogr", "-f", "GeoJSON", str(output), str(pbf), layer,
            "-spat", *bbox, "-select", fields, "-where", where, *extra,
        ], check=True)


def features(source):
    return json.loads(source.read_bytes())["features"]


def project(longitude, latitude, *_):
    x = (longitude + 180) / 360
    radians = math.radians(latitude)
    y = 0.5 - math.log(math.tan(math.pi / 4 + radians / 2)) / (2 * math.pi)
    return round(WORLD_UNITS * x), round(WORLD_UNITS * y)


def ring(raw):
    points = []
    for coord in raw:
        point = project(*coord)
        if not points or point != points[-1]:
            points.append(point)
    if points and points[0] == points[-1]:
        points.pop()
    return points


def multiple(geometry):
    if geometry and geometry["type"] == "Polygon":
        yield geometry["coordinates"]
    elif geometry and geometry["type"] == "MultiPolygon":
        yield from geometry["coordinates"]


def tag_value(tags, tag):
    match = tag.search(tags) if tags else None
    return re.sub(r'\\(["\\])', r'\1', match.group(1)).strip() if match else ""


def address(tags):
    text = tag_value(tags, NUMBER)
    encoded = text.encode("utf-8")
    return encoded if 0 < len(encoded) <= 48 and text.isprintable() else None


def bounds(points):
    return min(x for x, _ in points), min(y for _, y in points), max(x for x, _ in points), max(y for _, y in points)


def sector(x, y):
    return x // SUBDIVISIONS, y // SUBDIVISIONS


def inside(rings):
    _, top, _, bottom = bounds(rings[0])
    y = (top + bottom) / 2
    crossing = []
    for polygon in rings:
        for a, b in zip(polygon, polygon[1:] + polygon[:1]):
            if (a[1] <= y < b[1]) or (b[1] <= y < a[1]):
                crossing.append(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]))
    crossing.sort()
    if len(crossing) >= 2:
        left, right = max(zip(crossing[::2], crossing[1::2]), key=lambda pair: pair[1] - pair[0])
        return round((left + right) / 2), round(y)
    return rings[0][0]


def near_building(x, y, rings):
    inside_building = False
    for polygon in rings:
        for a, b in zip(polygon, polygon[1:] + polygon[:1]):
            dx, dy = b[0] - a[0], b[1] - a[1]
            px, py = x - a[0], y - a[1]
            length = dx * dx + dy * dy
            projection = px * dx + py * dy
            cross = px * dy - py * dx
            if 0 <= projection <= length and cross * cross <= 36 * length:
                return True
            if (a[1] <= y < b[1]) or (b[1] <= y < a[1]):
                if x < a[0] + (y - a[1]) * dx / dy:
                    inside_building = not inside_building
    return inside_building


def put_varuint(target, value):
    while value >= 128:
        target.append((value & 127) | 128)
        value >>= 7
    target.append(value)


def put_delta(target, value):
    # Interleave signs so small positive and negative changes both take one byte.
    put_varuint(target, 2 * value if value >= 0 else -2 * value - 1)


def payload(shapes, labels, key):
    if len(shapes) > MAX_SHAPES or len(labels) > MAX_LABELS:
        raise ValueError(f"Too many features in sector {key}")
    raw = bytearray(struct.pack(">HH", len(shapes), len(labels)))
    x0, y0 = (component * SUBDIVISIONS for component in key)
    # Each ring starts relative to the preceding ring's first vertex. The
    # remaining vertices use deltas from their preceding vertex in the ring.
    anchor_x = anchor_y = 0
    for rings in shapes:
        raw.append(len(rings))
        for points in rings:
            put_varuint(raw, len(points))
            previous_x, previous_y = anchor_x, anchor_y
            anchor_x, anchor_y = points[0][0] - x0, points[0][1] - y0
            for x, y in points:
                local_x, local_y = x - x0, y - y0
                if not (-32768 <= local_x <= 32767 and -32768 <= local_y <= 32767):
                    raise ValueError(f"Building coordinate exceeds sector budget: {key}")
                put_delta(raw, local_x - previous_x)
                put_delta(raw, local_y - previous_y)
                previous_x, previous_y = local_x, local_y
    for x, y, number in labels:
        raw.extend(struct.pack(">hhB", x - x0, y - y0, len(number)))
        raw.extend(number)
    if len(raw) > MAX_SECTOR_BYTES:
        raise ValueError(f"Sector {key} exceeds decompression budget")
    return struct.pack(">I", len(raw)) + zlib.compress(raw, 9)


def pack(work, output):
    shapes = defaultdict(list)
    labels = defaultdict(set)
    numbered_shapes = defaultdict(list)
    building_count = 0
    skipped = 0
    for feature in features(work / "buildings.geojson"):
        tags = feature["properties"].get("other_tags")
        number = address(tags)
        street = tag_value(tags, STREET).casefold() if number else ""
        best = None
        best_area = 0
        for polygon in multiple(feature["geometry"]):
            rings = [ring(raw) for raw in polygon]
            if not rings or len(rings[0]) < 3:
                skipped += 1
                continue
            rings = [points for points in rings if len(points) >= 3]
            if len(rings) > MAX_RINGS or any(len(points) > MAX_RING_POINTS for points in rings):
                skipped += 1
                continue
            left, top, right, bottom = bounds(rings[0])
            first_x, first_y = sector(left, top)
            last_x, last_y = sector(right, bottom)
            if last_x - first_x > 6 or last_y - first_y > 6:
                skipped += 1
                continue
            for sx in range(first_x, last_x + 1):
                for sy in range(first_y, last_y + 1):
                    shapes[sx, sy].append(rings)
                    if number:
                        numbered_shapes[sx, sy, number].append((street, (left, top, right, bottom), rings))
            building_count += 1
            area = (right - left) * (bottom - top)
            if area > best_area:
                best = rings
                best_area = area
        if number and best:
            x, y = inside(best)
            labels[sector(x, y)].add((x, y, number))
    for feature in features(work / "addresses.geojson"):
        tags = feature["properties"].get("other_tags")
        number = address(tags)
        if not number or not feature["geometry"] or feature["geometry"]["type"] != "Point":
            continue
        x, y = project(*feature["geometry"]["coordinates"])
        sx, sy = sector(x, y)
        street = tag_value(tags, STREET).casefold()
        candidates = numbered_shapes.get((sx, sy, number), ())
        duplicate = any(
            (not street or not building_street or street == building_street)
            and box[0] - 6 <= x <= box[2] + 6 and box[1] - 6 <= y <= box[3] + 6
            and near_building(x, y, rings)
            for building_street, box, rings in candidates
        )
        if not duplicate:
            labels[sx, sy].add((x, y, number))
    ordered = sorted(set(shapes) | set(labels))
    if len(ordered) > 8192:
        raise ValueError("Building index exceeds renderer budget")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as dest:
        dest.write(struct.pack(">4sBH", b"OMH2", SECTOR_ZOOM, len(ordered)))
        dest.write(bytes(12 * len(ordered)))
        index = []
        for sx, sy in ordered:
            key = sx, sy
            data = payload(shapes[key], sorted(labels[key]), key)
            index.append((sx, sy, dest.tell(), len(data)))
            dest.write(data)
        dest.seek(7)
        for entry in index:
            dest.write(struct.pack(">HHII", *entry))
    print(f"{output.name}: {output.stat().st_size:,} bytes, {building_count:,} polygon footprints, "
          f"{sum(map(len, labels.values())):,} address anchors, {len(index):,} sectors, {skipped} omitted geometries")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--osm-pbf", type=Path, help="refresh the working GeoJSON files from this extract")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "omsk-buildings.bin")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    if args.osm_pbf:
        extract(args.osm_pbf, args.work_dir)
    pack(args.work_dir, args.output)


if __name__ == "__main__":
    main()
