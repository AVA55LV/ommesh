"""Pack an OpenStreetMap vector detail overlay for Omsk."""

import argparse
import json
import math
from pathlib import Path
import struct
import subprocess
import zlib
from collections import defaultdict


BOUNDS = (73.12, 54.83, 73.66, 55.18)
# Reuse the extent and simplification of the original building extract.
BUILDING_EXTRACT = (72.70, 54.68, 74.05, 55.32)
SECTOR_ZOOM = 14
LOCAL_BITS = 11
SIDE = 1 << LOCAL_BITS
WORLD = (1 << SECTOR_ZOOM) * SIDE
MAX_POINTS = 512
MAX_FEATURES = 4096
MAX_SECTORS = 4096
MAX_RAW = 128 * 1024
STREETS = ('residential', 'living_street', 'unclassified', 'pedestrian')
PATHS = ('steps', 'cycleway')


def extract(pbf, work, buildings):
    def ogr(output, layer, bounds, fields, where, tolerance):
        extent = tuple(map(str, bounds))
        subprocess.run(['ogr2ogr', '-overwrite', '-f', 'GeoJSON', str(output), str(pbf), layer,
                        '-spat', *extent, '-clipsrc', *extent, '-select', fields,
                        '-where', where, '-simplify', tolerance], check=True)

    if buildings is None:
        buildings = work / 'buildings.geojson'
        ogr(buildings, 'multipolygons', BUILDING_EXTRACT, 'building',
            "building IS NOT NULL AND building <> 'no' AND building <> 'proposed'", '0.000005')
    roads = work / 'detail-roads.geojson'
    highway = ','.join("'" + tag + "'" for tag in (*STREETS, *PATHS))
    ogr(roads, 'lines', BOUNDS, 'highway,name',
        f"highway IN ({highway}) OR (highway IN ('footway','path') AND name IS NOT NULL) OR "
        "(highway='service' AND (name IS NOT NULL OR other_tags IS NULL OR "
        "(other_tags NOT LIKE '%\"service\"=>\"driveway\"%' AND "
        "other_tags NOT LIKE '%\"service\"=>\"parking_aisle\"%')))", '0.000009')
    return buildings, roads


def features(path):
    # GDAL emits one GeoJSON feature per line, including large collections.
    with path.open(encoding='utf-8') as file:
        for line in file:
            if line.startswith('{ "type": "Feature",'):
                yield json.loads(line.rstrip().rstrip(','))


def project(longitude, latitude, *_):
    x = (longitude + 180) / 360
    y = 0.5 - math.asinh(math.tan(math.radians(latitude))) / (2 * math.pi)
    return round(WORLD * x), round(WORLD * y)


def points(coordinates):
    result = []
    for coordinate in coordinates:
        point = project(*coordinate)
        if not result or point != result[-1]:
            if len(result) >= 2 and straight(result[-2], result[-1], point):
                result.pop()
            result.append(point)
    return result


def polygon_area(coordinates):
    lng, lat = coordinates[0][:2]
    double_area = sum((a[0] - lng) * (b[1] - lat) - (b[0] - lng) * (a[1] - lat)
                      for a, b in zip(coordinates, coordinates[1:]))
    return abs(double_area) * math.cos(math.radians(lat)) * 111195 ** 2 / 2


def straight(a, b, c):
    first_x, first_y = b[0] - a[0], b[1] - a[1]
    next_x, next_y = c[0] - b[0], c[1] - b[1]
    return first_x * next_y == first_y * next_x and first_x * next_x + first_y * next_y > 0


def sectors(points):
    left = min(x for x, _ in points) // SIDE
    top = min(y for _, y in points) // SIDE
    right = max(x for x, _ in points) // SIDE
    bottom = max(y for _, y in points) // SIDE
    for x in range(left, right + 1):
        for y in range(top, bottom + 1):
            yield x, y


def add_buildings(path, tiles, minimum_area):
    west, south, east, north = BOUNDS
    footprints = 0
    for feature in features(path):
        geometry = feature.get('geometry')
        if not geometry:
            continue
        polygons = [geometry['coordinates']] if geometry['type'] == 'Polygon' else geometry['coordinates']
        for shape in polygons:
            if not shape or len(shape[0]) < 4:
                continue
            longitude, latitude = shape[0][0][:2]
            if not (west <= longitude <= east and south <= latitude <= north):
                continue
            if polygon_area(shape[0]) < minimum_area:
                continue
            for index, coordinates in enumerate(shape):
                ring = points(coordinates)
                if ring and ring[0] == ring[-1]:
                    ring.pop()
                if not 3 <= len(ring) <= MAX_POINTS:
                    if index == 0:
                        break
                    continue
                first = min(range(len(ring)), key=ring.__getitem__)
                ring = ring[first:] + ring[:first]
                ring_tiles = list(sectors(ring))
                if len(ring_tiles) > 16:
                    if index == 0:
                        break
                    continue
                for tile in ring_tiles:
                    tiles[tile].append((0, ring))
                footprints += index == 0
    return footprints


def clipped_segment(first, last, tile):
    """Clip one segment to the inclusive Web Mercator tile extent."""
    start, stop = 0.0, 1.0
    for axis in (0, 1):
        delta = last[axis] - first[axis]
        low, high = tile[axis] * SIDE, (tile[axis] + 1) * SIDE
        if delta == 0:
            if not low <= first[axis] <= high:
                return None
            continue
        enter, leave = sorted(((low - first[axis]) / delta, (high - first[axis]) / delta))
        start, stop = max(start, enter), min(stop, leave)
        if start >= stop:
            return None
    a, b = (tuple(round(first[axis] + (last[axis] - first[axis]) * phase) for axis in (0, 1))
            for phase in (start, stop))
    return (a, b) if a != b else None


def add_roads(path, tiles):
    roads = defaultdict(int)
    for feature in features(path):
        geometry = feature.get('geometry')
        if not geometry:
            continue
        kind = 1 if feature['properties']['highway'] in STREETS else 2
        paths = [geometry['coordinates']] if geometry['type'] == 'LineString' else geometry['coordinates']
        for path in paths:
            line = points(path)
            if len(line) < 2:
                continue
            if feature['properties']['highway'] == 'service' and not feature['properties'].get('name'):
                length = sum(math.hypot(a[0] - b[0], a[1] - b[1]) for a, b in zip(line, line[1:]))
                if length < 175:  # approximately 120 m at Omsk's latitude
                    continue
            roads[kind] += 1
            chunks = defaultdict(list)
            for a, b in zip(line, line[1:]):
                for tile in sectors((a, b)):
                    segment = clipped_segment(a, b, tile)
                    if not segment:
                        continue
                    first, last = segment
                    paths_in_tile = chunks[tile]
                    if paths_in_tile and paths_in_tile[-1][-1] == first and len(paths_in_tile[-1]) < MAX_POINTS:
                        paths_in_tile[-1].append(last)
                    else:
                        paths_in_tile.append([first, last])
            for tile, paths_in_tile in chunks.items():
                tiles[tile].extend((kind, points) for points in paths_in_tile)
    return roads


def write_uint(output, number):
    while number >= 128:
        output.append((number & 127) | 128)
        number >>= 7
    output.append(number)


def zigzag(value):
    return 2 * value if value >= 0 else -2 * value - 1


def payload(tile, geometry):
    if len(geometry) > MAX_FEATURES:
        raise ValueError(f'{tile}: too many features ({len(geometry)})')
    origin = tuple(part * SIDE for part in tile)
    raw = bytearray(struct.pack('>H', len(geometry)))
    anchor_x = anchor_y = 0
    for kind, line in sorted(geometry, key=lambda item: (item[0], item[1][0], len(item[1]))):
        raw.append(kind)
        write_uint(raw, len(line))
        prior_x, prior_y = anchor_x, anchor_y
        anchor_x, anchor_y = (line[0][axis] - origin[axis] for axis in (0, 1))
        for x, y in line:
            x, y = x - origin[0], y - origin[1]
            if not (-32768 <= x <= 32767 and -32768 <= y <= 32767):
                raise ValueError(f'{tile}: coordinate outside 16-bit tile range')
            write_uint(raw, zigzag(x - prior_x))
            write_uint(raw, zigzag(y - prior_y))
            prior_x, prior_y = x, y
    if len(raw) > MAX_RAW:
        raise ValueError(f'{tile}: sector has {len(raw)} decoded bytes')
    return struct.pack('>I', len(raw)) + zlib.compress(raw, 9), len(raw)


def pack(buildings, roads, minimum_area):
    tiles = defaultdict(list)
    footprint_count = add_buildings(buildings, tiles, minimum_area)
    road_counts = add_roads(roads, tiles)
    if not footprint_count or not road_counts[1] or len(tiles) > MAX_SECTORS:
        raise ValueError('Unexpected number of features; check the Omsk OSM extract')
    header = bytearray(struct.pack('>4sBBH', b'OMD1', SECTOR_ZOOM, LOCAL_BITS, len(tiles)))
    data = bytearray()
    first = len(header) + 12 * len(tiles)
    peak = 0
    for tile, geometry in sorted(tiles.items()):
        blob, decoded = payload(tile, geometry)
        header.extend(struct.pack('>HHII', *tile, first + len(data), len(blob)))
        data.extend(blob)
        peak = max(peak, decoded)
    header.extend(data)
    print(f'{len(header):,} bytes; {len(tiles):,} sectors; peak {peak:,} decoded bytes; '
          f'{footprint_count:,} buildings, {road_counts[1]:,} street lines, {road_counts[2]:,} secondary paths')
    return header


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--osm-pbf', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--buildings', type=Path, help='reuse a GDAL building GeoJSON extracted with the same bounds')
    parser.add_argument('--minimum-building-area', type=float, default=140, help='include footprints of at least this many square metres')
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('omsk-detail.bin'))
    args = parser.parse_args()
    if not math.isfinite(args.minimum_building_area) or args.minimum_building_area <= 0:
        parser.error('--minimum-building-area must be positive')
    args.work_dir.mkdir(parents=True, exist_ok=True)
    buildings, roads = extract(args.osm_pbf, args.work_dir, args.buildings)
    args.output.write_bytes(pack(buildings, roads, args.minimum_building_area))


if __name__ == '__main__':
    main()
