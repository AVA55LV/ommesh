"""Pack clipped OSM GeoJSON geographic strokes for the MIDP renderer."""

import json
import math
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path.cwd()
OUT = ROOT / "omsk.bin"
BOUNDS = (72700000, 74050000, 54680000, 55320000)
QUANTUM = 100000
MAX_SHAPE_POINTS = 512


def read_features(filename):
    return json.loads((ROOT / filename).read_bytes())["features"]


def layer(tags):
    if tags.get("waterway"):
        return 0 if tags["waterway"] == "stream" else 1
    if tags.get("railway") == "rail":
        return 2
    highway = tags["highway"].split("_", 1)[0]
    if highway in ("tertiary", "unclassified"):
        return 3
    if highway == "secondary":
        return 4
    return 5


def quantize(points):
    answer = []
    for x, y, *_ in points:
        point = round(x * QUANTUM), round(y * QUANTUM)
        if not answer or point != answer[-1]:
            answer.append(point)
    return answer


def chained(segments):
    ends = defaultdict(list)
    for number, segment in enumerate(segments):
        ends[segment[0]].append((number, False))
        ends[segment[-1]].append((number, True))
    used = set()

    def extend(line):
        while len(ends[line[-1]]) == 2:
            next_ = [entry for entry in ends[line[-1]] if entry[0] not in used]
            if len(next_) != 1:
                break
            index, reverse = next_[0]
            used.add(index)
            addition = segments[index]
            line.extend(addition[-2::-1] if reverse else addition[1:])
        return line

    for i, line in enumerate(segments):
        if i not in used:
            used.add(i)
            yield extend(extend(list(line))[::-1])[::-1]


def bridge_large_deltas(line):
    result = [line[0]]
    for x, y in line[1:]:
        previous_x, previous_y = result[-1]
        dx, dy = x - previous_x, y - previous_y
        steps = max(1, math.ceil(max(abs(dx), abs(dy)) / 32767))
        for i in range(1, steps + 1):
            fraction = i / steps
            result.append((previous_x + round(dx * fraction), previous_y + round(dy * fraction)))
    return result


def pack():
    road_layers = defaultdict(list)
    for feature in read_features("roads-water.geojson"):
        geom = feature["geometry"]
        lines = geom["coordinates"]
        if geom["type"] == "LineString":
            lines = [lines]
        elif geom["type"] != "MultiLineString":
            continue
        for points in lines:
            line = quantize(points)
            if len(line) > 1:
                road_layers[layer(feature["properties"])].append(line)

    shapes = []
    for kind, segments in sorted(road_layers.items()):
        for line in chained(segments):
            line = bridge_large_deltas(line)
            for start in range(0, len(line) - 1, MAX_SHAPE_POINTS - 1):
                shapes.append((kind, line[start : start + MAX_SHAPE_POINTS]))

    places = []
    for feature in read_features("places.geojson"):
        props = feature["properties"]
        place_kind = {"village": 0, "town": 1, "city": 2}[props["place"]]
        name = props["name"].encode("utf-8")
        if 0 < len(name) <= 96:
            (x, y), = quantize([feature["geometry"]["coordinates"]])
            places.append((place_kind, x, y, name))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as output:
        output.write(struct.pack(">4s4iH", b"OMM1", *BOUNDS, len(shapes)))
        for kind, line in shapes:
            x, y = line[0]
            output.write(struct.pack(">BHii", kind, len(line), x, y))
            for next_x, next_y in line[1:]:
                output.write(struct.pack(">hh", next_x - x, next_y - y))
                x, y = next_x, next_y
        output.write(struct.pack(">H", len(places)))
        for kind, x, y, name in sorted(places, reverse=True):
            output.write(struct.pack(">BiiB", kind, x, y, len(name)))
            output.write(name)
    print(OUT, OUT.stat().st_size, "bytes")
    print(len(shapes), "lines", sum(len(line) for _, line in shapes), "vertices",
          len(places), "places", "lines by layer:", dict(Counter(kind for kind, _ in shapes)))


if __name__ == "__main__":
    pack()
