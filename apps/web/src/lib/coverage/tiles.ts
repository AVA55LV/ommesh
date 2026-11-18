/**
 * The cells a coverage survey fills: the honeycomb the coverage map at
 * nw.ommesh.ru draws, worked out the same way, so a drive's cells land where
 * the community map puts them when the survey is sent there.
 *
 * A cell is a hexagon `cellM` across its flats, flat sides up and down and
 * corners left and right. The lattice it sits on is laid on the flat plane of
 * the place's UTM zone rather than on degrees: a degree of longitude shortens
 * with the latitude, and a lattice laid on them drifts half a cell apart over
 * a couple of kilometres. Two zones' lattices do not meet at their border; a
 * survey lives inside a city, and the border runs between cities.
 */

import { metresPerPixel } from "../geo.js";
import { pointTone, type PointTone, type SurveyPoint } from "../surveyData.js";

/** Metres in a degree of latitude, as the hexagon's corners are laid in degrees for the map. */
export const M_PER_DEG = 111320;
/** A cell's size across its flats: the coverage map's smallest. */
export const CELL_M = 200;
/**
 * The sizes the map falls back on as it draws further out: a cell smaller than
 * this on screen is worth nothing but a smear, so the next coarser one is drawn.
 */
export const CELL_LEVELS = [200, 400, 800, 1600, 3200, 6400];
export const MIN_CELL_PX = 24;

export interface Cell {
  /** The zone, row and column: a cell of this size is named by all three. */
  key: string;
  r: number;
  c: number;
  /** The middle of the hexagon, rounded to about a decimetre as the coverage map rounds it. */
  lat: number;
  lon: number;
}

export interface Tile extends Cell {
  tone: PointTone;
}

// ---- WGS 84 transverse Mercator, the zone of the place ----

const A = 6_378_137;
const F = 1 / 298.257223563;
const E2 = F * (2 - F);
const EP2 = E2 / (1 - E2);
/** The scale on the zone's central meridian, and the false origin of a zone's plane. */
const K0 = 0.9996;
const FALSE_EASTING = 500_000;
const FALSE_NORTHING = 10_000_000;

/** The zone a longitude falls in. */
function zoneOf(lon: number): number {
  return Math.floor((lon + 180) / 6) + 1;
}

/** A place on a zone's plane, metres east and north of the zone's false origin. */
function toPlane(lat: number, lon: number, zone = zoneOf(lon)): { x: number; y: number } {
  const lon0 = (zone - 1) * 6 - 180 + 3;
  const phi = (lat * Math.PI) / 180;
  const dLon = ((lon - lon0) * Math.PI) / 180;
  const sin = Math.sin(phi);
  const cos = Math.cos(phi);
  const tan = Math.tan(phi);
  const n = A / Math.sqrt(1 - E2 * sin * sin);
  const t = tan * tan;
  const c = EP2 * cos * cos;
  const ea = cos * dLon;
  const m =
    A *
    ((1 - E2 / 4 - (3 * E2 ** 2) / 64 - (5 * E2 ** 3) / 256) * phi -
      ((3 * E2) / 8 + (3 * E2 ** 2) / 32 + (45 * E2 ** 3) / 1024) * Math.sin(2 * phi) +
      ((15 * E2 ** 2) / 256 + (45 * E2 ** 3) / 1024) * Math.sin(4 * phi) -
      ((35 * E2 ** 3) / 3072) * Math.sin(6 * phi));
  const x = K0 * n * (ea + ((1 - t + c) * ea ** 3) / 6 + ((5 - 18 * t + t * t + 72 * c - 58 * EP2) * ea ** 5) / 120) + FALSE_EASTING;
  const y =
    K0 * (m + n * tan * ((ea * ea) / 2 + ((5 - t + 9 * c + 4 * c * c) * ea ** 4) / 24 + ((61 - 58 * t + t * t + 600 * c - 330 * EP2) * ea ** 6) / 720)) +
    (lat < 0 ? FALSE_NORTHING : 0);
  return { x, y };
}

/** The place at a point of a zone's plane; `south` says which side of the equator the point is. */
function fromPlane(x: number, y: number, zone: number, south: boolean): { lat: number; lon: number } {
  const lon0 = ((zone - 1) * 6 - 180 + 3) * (Math.PI / 180);
  const m = (y - (south ? FALSE_NORTHING : 0)) / K0;
  const e1 = (1 - Math.sqrt(1 - E2)) / (1 + Math.sqrt(1 - E2));
  const mu = m / (A * (1 - E2 / 4 - (3 * E2 ** 2) / 64 - (5 * E2 ** 3) / 256));
  const phi1 =
    mu +
    ((3 * e1) / 2 - (27 * e1 ** 3) / 32) * Math.sin(2 * mu) +
    ((21 * e1 ** 2) / 16 - (55 * e1 ** 4) / 32) * Math.sin(4 * mu) +
    ((151 * e1 ** 3) / 96) * Math.sin(6 * mu) +
    ((1097 * e1 ** 4) / 512) * Math.sin(8 * mu);
  const sin = Math.sin(phi1);
  const cos = Math.cos(phi1);
  const tan = Math.tan(phi1);
  const n1 = A / Math.sqrt(1 - E2 * sin * sin);
  const t1 = tan * tan;
  const c1 = EP2 * cos * cos;
  const r1 = (A * (1 - E2)) / (1 - E2 * sin * sin) ** 1.5;
  const d = (x - FALSE_EASTING) / (n1 * K0);
  const phi =
    phi1 -
    ((n1 * tan) / r1) *
      ((d * d) / 2 -
        ((5 + 3 * t1 + 10 * c1 - 4 * c1 * c1 - 9 * EP2) * d ** 4) / 24 +
        ((61 + 90 * t1 + 298 * c1 + 45 * t1 * t1 - 252 * EP2 - 3 * c1 * c1) * d ** 6) / 720);
  const lon = lon0 + (d - ((1 + 2 * t1 + c1) * d ** 3) / 6 + ((5 - 2 * c1 + 28 * t1 - 3 * c1 * c1 + 8 * EP2 + 24 * t1 * t1) * d ** 5) / 120) / cos;
  return { lat: (phi * 180) / Math.PI, lon: (lon * 180) / Math.PI };
}

// ---- the lattice ----

/** A cell's corners are this far from its middle: the side of the hexagon `cellM` across its flats. */
export function hexSide(cellM: number): number {
  return cellM / Math.sqrt(3);
}

/**
 * The cell a place falls in: the nearest middle of the honeycomb on its
 * zone's plane. Columns lie a cell and a half wide apart, the middles in one
 * column a cell apart up the plane, and every other column is shifted by half
 * of that. The same place always falls in the same cell.
 */
export function cellOf(lat: number, lon: number, size = CELL_M): Cell {
  const side = hexSide(size);
  const stepX = 1.5 * side;
  const stepY = size;
  const zone = zoneOf(lon);
  const { x, y } = toPlane(lat, lon, zone);
  const c0 = Math.round(x / stepX - 0.5);
  let best = { r: 0, c: c0, x: 0, y: 0 };
  let bestD = Infinity;
  for (let c = c0 - 1; c <= c0 + 1; c++) {
    const cx = (c + 0.5) * stepX;
    const offset = Math.abs(c % 2) * (stepY / 2);
    const r0 = Math.round((y - offset) / stepY - 0.5);
    for (let r = r0 - 1; r <= r0 + 1; r++) {
      const cy = offset + (r + 0.5) * stepY;
      const d = (x - cx) ** 2 + (y - cy) ** 2;
      if (d < bestD) {
        bestD = d;
        best = { r, c, x: cx, y: cy };
      }
    }
  }
  const at = fromPlane(best.x, best.y, zone, lat < 0);
  return {
    key: `${zone}:${best.r}:${best.c}`,
    r: best.r,
    c: best.c,
    lat: Math.round(at.lat * 1e6) / 1e6,
    lon: Math.round(at.lon * 1e6) / 1e6,
  };
}

/**
 * A cell's hexagon: flat sides up and down, corners left and right, as the
 * coverage map draws it — its corners laid around the middle in degrees, a
 * degree of longitude shorter where the latitude is higher.
 */
export function cellPolygon(lat: number, lon: number, size = CELL_M): [number, number][] {
  const side = hexSide(size) / M_PER_DEG;
  const dLon = side / Math.cos((lat * Math.PI) / 180);
  const ring: [number, number][] = [];
  for (let k = 0; k < 6; k++) {
    const a = (k * Math.PI) / 3;
    ring.push([lon + dLon * Math.cos(a), lat + side * Math.sin(a)]);
  }
  ring.push(ring[0]!);
  return ring;
}

const TONE_RANK: Record<PointTone, number> = { none: 0, weak: 1, fair: 2, good: 3 };

/**
 * The cells the survey's points fall in, each with the best tone among them:
 * a cell is green where any ask in it was heard well, whichever others were
 * not, and grey only where every ask went unanswered.
 */
export function tilesAt(points: readonly SurveyPoint[], size = CELL_M): Tile[] {
  const tiles = new Map<string, Tile>();
  for (const point of points) {
    const cell = cellOf(point.lat, point.lon, size);
    const tone = pointTone(point);
    const seen = tiles.get(cell.key);
    if (!seen) {
      tiles.set(cell.key, { ...cell, tone });
    } else if (TONE_RANK[tone] > TONE_RANK[seen.tone]) {
      seen.tone = tone;
    }
  }
  return [...tiles.values()];
}

/** The size the map draws at this zoom: the finest whose cell is not smaller than `MIN_CELL_PX`. */
export function levelAt(zoom: number, lat: number): number {
  const pxPerMetre = 1 / metresPerPixel(lat, zoom);
  for (const size of CELL_LEVELS) {
    if (size * pxPerMetre >= MIN_CELL_PX) return size;
  }
  return CELL_LEVELS[CELL_LEVELS.length - 1]!;
}
