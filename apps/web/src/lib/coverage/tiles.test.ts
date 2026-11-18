import { test } from "node:test";
import assert from "node:assert/strict";
import { CELL_LEVELS, CELL_M, MIN_CELL_PX, M_PER_DEG, cellOf, cellPolygon, hexSide, levelAt, tilesAt } from "./tiles.js";
import { metresPerPixel } from "../geo.js";
import type { SurveyPoint } from "../surveyData.js";

const reply = (key: string, us: number, them: number) => ({ key, us, them, rssi: -90 });
const point = (lat: number, lon: number, replies = [reply("aa", 2, 3)]): SurveyPoint => ({ at: 0, lat, lon, accuracy: 5, replies });

/** Metres between two places near the first one, the long way round the cosine. */
const away = (lat: number, lon: number, atLat: number, atLon: number) =>
  Math.hypot((atLat - lat) * M_PER_DEG, (atLon - lon) * M_PER_DEG * Math.cos((lat * Math.PI) / 180));

test("a cell's middle is its own, and a place is never further from it than a hexagon's side", () => {
  for (const [lat, lon] of [
    [55.03, 73.37],
    [57.15, 65.53],
    [-33.87, 18.42],
    [60.17, -100.5],
  ] as [number, number][]) {
    const cell = cellOf(lat, lon);
    const again = cellOf(cell.lat, cell.lon);
    assert.deepEqual([again.r, again.c], [cell.r, cell.c], `${lat}, ${lon}`);
    assert.ok(away(lat, lon, cell.lat, cell.lon) <= hexSide(CELL_M) + 1, `${lat}, ${lon}: ${away(lat, lon, cell.lat, cell.lon).toFixed(1)} m`);
  }
});

test("a hexagon is as tall as a cell and a cell and a fifth wide across its corners", () => {
  const cell = cellOf(55, 73);
  const ring = cellPolygon(cell.lat, cell.lon);
  const lats = ring.map((p) => p[1]);
  const lons = ring.map((p) => p[0]);
  const height = (Math.max(...lats) - Math.min(...lats)) * M_PER_DEG;
  const width = (Math.max(...lons) - Math.min(...lons)) * M_PER_DEG * Math.cos((cell.lat * Math.PI) / 180);
  assert.ok(Math.abs(height - CELL_M) < 1, `height ${height}`);
  assert.ok(Math.abs(width - (2 * CELL_M) / Math.sqrt(3)) < 1, `width ${width}`);
  assert.deepEqual(ring[0], ring[ring.length - 1], "the ring closes");
});

test("a place and its neighbour 250 m off are in different cells, and 40 m off in the same", () => {
  const here = cellOf(55, 73);
  const near = cellOf(55.0003, 73.0002);
  assert.equal(near.key, here.key);
  const east = cellOf(55, 73 + 250 / (M_PER_DEG * Math.cos((55 * Math.PI) / 180)));
  assert.notEqual(east.key, here.key);
  // The middle of the cell next door is a column and a half of sides away, not a hair.
  assert.ok(away(here.lat, here.lon, east.lat, east.lon) > 150);
});

test("a cell takes the best answer in it, and is grey only where nobody answered", () => {
  const good = point(55, 73, [reply("aa", 4, 5)]);
  const weak = point(55.0002, 73.0002, [reply("aa", -9, -8)]);
  const none = point(55, 73, []);
  assert.deepEqual(tilesAt([good, weak]).map((t) => t.tone), ["good"]);
  assert.deepEqual(tilesAt([none]).map((t) => t.tone), ["none"]);
  assert.deepEqual(tilesAt([none, weak]).map((t) => t.tone), ["weak"], "one answer lifts the cell off grey");
  assert.equal(tilesAt([none, point(55.0001, 73.0001, [])]).length, 1, "two asks in one cell are one cell");
});

test("the map draws the finest grid whose cell is not a smear", () => {
  // 200 m at zoom 13 is 36 px; at zoom 12 it is 18 px, so the 400 m grid takes over.
  assert.equal(levelAt(13, 55), 200);
  assert.equal(levelAt(12, 55), 400);
  assert.equal(levelAt(9, 55), 3200);
  for (const zoom of [9, 10, 11, 12, 13, 14, 15, 16]) {
    const size = levelAt(zoom, 55);
    assert.ok(CELL_LEVELS.includes(size));
    assert.ok(size / metresPerPixel(55, zoom) >= MIN_CELL_PX || size === CELL_LEVELS[CELL_LEVELS.length - 1]!, `${size} at ${zoom}`);
  }
});
