/**
 * The tiles a coverage survey fills: the squares of 200 m the «Замеры» map on
 * nw.ommesh.ru draws, worked out the same way, so a drive's tiles land where
 * the community map puts them when the survey is sent there.
 *
 * A tile is fixed by its row: the latitude step is 200 m over the metres in a
 * degree of latitude, and the longitude step is that step over the cosine of
 * the row's own latitude, so a tile is square in metres and neighbouring
 * tiles meet. Its centre sits at half a step on both axes.
 */

import { metresPerPixel } from "../geo.js";
import { pointTone, type PointTone, type SurveyPoint } from "../surveyData.js";

/** Metres in a degree of latitude, the constant the coverage map works in. */
export const M_PER_DEG = 111320;
/** A tile of the coverage map, metres. */
export const CELL_M = 200;
/**
 * The grids a map falls back to as it draws further out: a tile smaller than
 * this on screen is worth nothing but a smear, so the next coarser one is drawn.
 */
export const CELL_LEVELS = [200, 400, 800, 1600, 3200, 6400];
export const MIN_CELL_PX = 24;

export interface Cell {
  /** The row and column, which name a tile as long as its size is known. */
  key: string;
  lat: number;
  lon: number;
  /** The width of a tile of this row, in degrees: the height over the cosine of its latitude. */
  lonStep: number;
}

export interface Tile extends Cell {
  tone: PointTone;
}

/** The tile a point falls in, its centre at half a step as the coverage map draws it. */
export function cellOf(lat: number, lon: number, size = CELL_M): Cell {
  const latStep = size / M_PER_DEG;
  const i = Math.floor(lat / latStep);
  const centreLat = (i + 0.5) * latStep;
  const lonStep = size / (M_PER_DEG * Math.cos((centreLat * Math.PI) / 180));
  const j = Math.floor(lon / lonStep);
  return {
    key: `${i}:${j}`,
    lat: centreLat,
    lon: (j + 0.5) * lonStep,
    lonStep,
  };
}

/** The tile's corners, the first again last, as a map takes a ring: longitude, latitude. */
export function cellPolygon(cell: Cell, size = CELL_M): [number, number][] {
  const halfLat = size / 2 / M_PER_DEG;
  const halfLon = cell.lonStep / 2;
  return [
    [cell.lon - halfLon, cell.lat - halfLat],
    [cell.lon + halfLon, cell.lat - halfLat],
    [cell.lon + halfLon, cell.lat + halfLat],
    [cell.lon - halfLon, cell.lat + halfLat],
    [cell.lon - halfLon, cell.lat - halfLat],
  ];
}

const TONE_RANK: Record<PointTone, number> = { none: 0, weak: 1, fair: 2, good: 3 };

/**
 * The tiles the survey's points fall in, each with the best tone among them:
 * a tile is green where any ask in it was heard well, whichever others were
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

/** The size the map draws at this zoom: the finest whose tile is not smaller than `MIN_CELL_PX`. */
export function levelAt(zoom: number, lat: number): number {
  const pxPerMetre = 1 / metresPerPixel(lat, zoom);
  for (const size of CELL_LEVELS) {
    if (size * pxPerMetre >= MIN_CELL_PX) return size;
  }
  return CELL_LEVELS[CELL_LEVELS.length - 1]!;
}
