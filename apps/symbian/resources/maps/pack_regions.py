"""Build offline atlas layers from Natural Earth downloads and an OSM PBF."""

import argparse
import json
import math
import struct
import subprocess
import urllib.request
from collections import defaultdict
from pathlib import Path

from pack import chained

WORLD = (-180, 180, -85, 85)
SIBERIA = (18, 180, 40, 83)
OBLAST = (69.0, 77.0, 53.3, 59.25)
SCALE = 1000
MAX_POINTS_PER_LINE = 512
MAX_POINTS_PER_ATLAS = 48000
NE_ROOT = "https://naturalearth.s3.amazonaws.com"
WORLD_SOURCES = (
    ("110m_physical", "coastline", 6),
    ("110m_cultural", "admin_0_boundary_lines_land", 7),
    ("110m_physical", "lakes", 8),
    ("110m_physical", "rivers_lake_centerlines", 1),
)
SIBERIA_SOURCES = (
    ("50m_physical", "coastline", 6),
    ("50m_cultural", "admin_1_states_provinces_lines", 7),
    ("50m_physical", "lakes", 8),
    ("50m_physical", "rivers_lake_centerlines", 1),
    ("10m_cultural", "roads", 5),
)
CITY_NAMES = {
    "Moscow": "Москва", "Saint Petersburg": "Санкт-Петербург", "Omsk": "Омск",
    "Novosibirsk": "Новосибирск", "Yekaterinburg": "Екатеринбург", "Kazan": "Казань",
    "Chelyabinsk": "Челябинск", "Tara": "Тара", "Tyumen": "Тюмень",
    "Kurgan": "Курган", "Barnaul": "Барнаул", "Tomsk": "Томск",
    "Kemerovo": "Кемерово", "Novokuznetsk": "Новокузнецк", "Krasnoyarsk": "Красноярск",
    "Irkutsk": "Иркутск", "Yakutsk": "Якутск", "Vladivostok": "Владивосток",
    "Khabarovsk": "Хабаровск", "Ulan-Ude": "Улан-Удэ", "Chita": "Чита", "Salekhard": "Салехард",
    "Norilsk": "Норильск", "Tobolsk": "Тобольск", "Khanty-Mansiysk": "Ханты-Мансийск",
    "Surgut": "Сургут", "Nizhnevartovsk": "Нижневартовск", "Petropavlovsk-Kamchatsky": "Петропавловск-Камчатский",
    "Anadyr": "Анадырь", "Magadan": "Магадан", "Blagoveshchensk": "Благовещенск",
    "Kyzyl": "Кызыл", "Abakan": "Абакан", "Gorno-Altaysk": "Горно-Алтайск",
    "Astana": "Астана", "Pavlodar": "Павлодар", "Petropavlovsk": "Петропавловск",
    "Ulaanbaatar": "Улан-Батор", "Beijing": "Пекин", "Tokyo": "Токио", "London": "Лондон",
    "Paris": "Париж", "Berlin": "Берлин", "New York": "Нью-Йорк", "Sao Paulo": "Сан-Паулу",
}


def features(file):
    return json.loads(file.read_bytes())["features"]


def ne_archive(work, category, name, download):
    stem = f"ne_{category.split('_')[0]}_{name}"
    source = work / "ne" / f"{stem}.zip"
    if not source.exists() and download:
        source.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(f"{NE_ROOT}/{category}/{source.name}", timeout=45) as stream:
            source.write_bytes(stream.read())
    if not source.is_file():
        raise FileNotFoundError(f"Missing Natural Earth archive {source}; use --download-natural-earth")
    return f"/vsizip/{source}", stem


def extract(output, source, layer, *options):
    output.parent.mkdir(parents=True, exist_ok=True)
    output.unlink(missing_ok=True)
    subprocess.run(["ogr2ogr", "-f", "GeoJSON", str(output), str(source), layer,
                    *options], check=True)
    return features(output)


def points(geom):
    coordinates = geom.get("coordinates")
    kind = geom["type"]
    if kind == "LineString":
        yield coordinates
    elif kind in ("MultiLineString", "Polygon"):
        yield from coordinates
    elif kind == "MultiPolygon":
        for polygon in coordinates:
            yield from polygon
    elif kind == "GeometryCollection":
        for item in geom["geometries"]:
            yield from points(item)


def quantized(raw):
    previous = None
    line = []
    for x, y, *_ in raw:
        current = round(x * SCALE), round(y * SCALE)
        if current == previous:
            continue
        if previous and abs(current[0] - previous[0]) > 180 * SCALE:
            if len(line) > 1:
                yield line
            line = []
        line.append(current)
        previous = current
    if len(line) > 1:
        yield line


def add_shapes(shapes, category, items):
    for feature in items:
        geom = feature["geometry"]
        if geom is not None:
            for raw in points(geom):
                shapes[category].extend(quantized(raw))


def bridge(line):
    result = [line[0]]
    for x, y in line[1:]:
        px, py = result[-1]
        dx, dy = x - px, y - py
        steps = max(1, math.ceil(max(abs(dx), abs(dy)) / 32767))
        result.extend((px + round(dx * i / steps), py + round(dy * i / steps)) for i in range(1, steps + 1))
    return result


def write_atlas(output, bounds, shapes, labels):
    packed = []
    for kind, category in sorted(shapes.items()):
        for line in chained(category):
            line = bridge(line)
            for start in range(0, len(line) - 1, MAX_POINTS_PER_LINE - 1):
                packed.append((kind, line[start : start + MAX_POINTS_PER_LINE]))
    count = sum(len(line) for _, line in packed)
    labels = sorted(labels, key=lambda label: (-label[0], -label[1], label[4]))
    if len(packed) > 7000 or count > MAX_POINTS_PER_ATLAS or len(labels) > 1024:
        raise ValueError(f"Atlas exceeds renderer budget: {len(packed)} lines, {count} points, {len(labels)} labels")
    with output.open("wb") as file:
        file.write(struct.pack(">4s4iH", b"OMM2", *(round(value * 1_000_000) for value in bounds), len(packed)))
        for kind, line in packed:
            x, y = line[0]
            file.write(struct.pack(">BHii", kind, len(line), x, y))
            for nx, ny in line[1:]:
                file.write(struct.pack(">hh", nx - x, ny - y))
                x, y = nx, ny
        file.write(struct.pack(">H", len(labels)))
        for kind, _, longitude, latitude, name in labels:
            text = name.encode("utf-8")
            file.write(struct.pack(">BiiB", kind, longitude, latitude, len(text)))
            file.write(text)
    print(f"{output.name}: {output.stat().st_size:,} bytes, {len(packed):,} lines, {count:,} vertices, {len(labels):,} labels")


def region_ne(work, out, name, sources, bounds, download):
    shapes = defaultdict(list)
    west, east, south, north = bounds
    clip = tuple(map(str, (west, south, east, north)))
    selected = ["-spat", *clip, "-clipsrc", *clip] if name == "siberia" else []
    tolerance = "0.11" if name == "world" else "0.038"
    for category, dataset, feature_type in sources:
        url, layer = ne_archive(work, category, dataset, download)
        filter_ = ["-where", "min_zoom <= 7.0 AND featurecla = 'Road'"] if dataset == "roads" else []
        output = work / "vectors" / f"{name}-{dataset}.geojson"
        items = extract(output, url, layer, *selected, *filter_, "-simplify", tolerance, "-select", "featurecla")
        add_shapes(shapes, feature_type, items)
    dataset = "110m_cultural" if name == "world" else "50m_cultural"
    url, layer = ne_archive(work, dataset, "populated_places_simple", download)
    items = extract(work / "vectors" / f"{name}-places.geojson", url, layer,
                    *selected, "-select", "name,adm0cap,scalerank,pop_max")
    labels = []
    for feature in items:
        fields = feature["properties"]
        if name == "siberia" and fields["pop_max"] < 40000 and not fields["adm0cap"]:
            continue
        x, y = feature["geometry"]["coordinates"]
        name_ = CITY_NAMES.get(fields["name"], fields["name"])
        if not name_ or len(name_.encode("utf-8")) > 96:
            continue
        rank = 2 if fields["adm0cap"] or fields["pop_max"] > 1_000_000 else 1 if fields["pop_max"] > 160_000 else 0
        labels.append((rank, fields["pop_max"], round(x * SCALE), round(y * SCALE), name_))
    write_atlas(out / f"{name}.bin", bounds, shapes, labels)


def oblast(work, pbf, out):
    selected = ["-spat", "69.0", "53.3", "77.0", "59.25"]
    clip = ["-clipsrc", *selected[1:]]
    condition = "highway IN ('motorway','trunk','primary','secondary','tertiary') OR waterway IN ('river','canal') OR railway = 'rail'"
    segments = extract(work / "vectors/oblast-lines.geojson", pbf, "lines", *selected, *clip,
                       "-select", "highway,waterway,railway", "-simplify", "0.0012", "-where", condition)
    shapes = defaultdict(list)
    for item in segments:
        tags = item["properties"]
        category = 1 if tags.get("waterway") else 2 if tags.get("railway") else {"secondary": 4, "tertiary": 3}.get(tags.get("highway"), 5)
        add_shapes(shapes, category, [item])
    boundaries = extract(work / "vectors/oblast-border.geojson", pbf, "multipolygons", "-where",
                         "name = 'Омская область'", "-select", "name", "-simplify", "0.003", "-nlt", "MULTIPOLYGON")
    add_shapes(shapes, 7, boundaries)
    places = extract(work / "vectors/oblast-places.geojson", pbf, "points", *selected, "-select", "name,place",
                     "-where", "place IN ('city','town','village') AND name IS NOT NULL")
    labels = []
    for feature in places:
        fields = feature["properties"]
        x, y = feature["geometry"]["coordinates"]
        name_ = fields["name"]
        if 0 < len(name_.encode("utf-8")) <= 96:
            rank = {"city": 2, "town": 1, "village": 0}[fields["place"]]
            labels.append((rank, 0, round(x * SCALE), round(y * SCALE), name_))
    write_atlas(out / "omsk-oblast.bin", OBLAST, shapes, labels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--osm-pbf", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--download-natural-earth", action="store_true")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    region_ne(work, output, "world", WORLD_SOURCES, WORLD, args.download_natural_earth)
    region_ne(work, output, "siberia", SIBERIA_SOURCES, SIBERIA, args.download_natural_earth)
    oblast(work, args.osm_pbf.resolve(), output)


if __name__ == "__main__":
    main()
