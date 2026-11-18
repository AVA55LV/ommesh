import { test } from "node:test";
import assert from "node:assert/strict";
import { CELL_M, CELL_LEVELS, MIN_CELL_PX, M_PER_DEG, cellOf, cellPolygon, levelAt, tilesAt } from "./tiles.js";
import { metresPerPixel } from "../geo.js";
import type { SurveyPoint } from "../surveyData.js";

const reply = (key: string, us: number, them: number) => ({ key, us, them, rssi: -90 });
const point = (lat: number, lon: number, replies = [reply("aa", 2, 3)]): SurveyPoint => ({ at: 0, lat, lon, accuracy: 5, replies });

test("a tile is a square of 200 m, its centre half a step from the grid's lines", () => {
  const cell = cellOf(55, 73);
  const latStep = CELL_M / M_PER_DEG;
  assert.ok(Math.abs(cell.lat / latStep - 0.5 - Math.floor(cell.lat / latStep)) < 1e-9, "half a step down its row");
  const lonStep = CELL_M / (M_PER_DEG * Math.cos((cell.lat * Math.PI) / 180));
  assert.ok(Math.abs(cell.lon / lonStep - 0.5 - Math.floor(cell.lon / lonStep)) < 1e-9, "half a step along it");
  assert.ok(Math.abs(cell.lonStep - lonStep) < 1e-12);
});

test("a point falls in the tile its centre belongs to, and neighbours share an edge", () => {
  const a = cellOf(55, 73);
  // 0,0001° of latitude is 11 m: the same tile.
  assert.equal(cellOf(55.0001, 73).key, a.key);
  // About 250 m north: the next row.
  assert.notEqual(cellOf(55.0025, 73).key, a.key);
  const north = cellOf(55.0025, 73);
  const aBox = cellPolygon(a);
  const nBox = cellPolygon(north);
  assert.ok(Math.abs(aBox[2]![1] - nBox[0]![1]) < 1e-9, "the top of one tile is the bottom of the next");

  const east = cellOf(55, 73 + a.lonStep);
  assert.notEqual(east.key, a.key);
  const eBox = cellPolygon(east);
  assert.ok(Math.abs(aBox[1]![0] - eBox[0]![0]) < 1e-9, "the right edge of one tile is the left of the next");
});

test("a tile is 200 m by 200 m in metres, not in degrees", () => {
  const cell = cellOf(55, 73);
  const box = cellPolygon(cell);
  const height = (box[2]![1] - box[1]![1]) * M_PER_DEG;
  const width = (box[1]![0] - box[0]![0]) * M_PER_DEG * Math.cos((cell.lat * Math.PI) / 180);
  assert.ok(Math.abs(height - CELL_M) < 0.01, `height ${height}`);
  assert.ok(Math.abs(width - CELL_M) < 0.01, `width ${width}`);
});

test("a tile takes the best answer in it, and is grey only where nobody answered", () => {
  const good = point(55, 73, [reply("aa", 4, 5)]);
  const weak = point(55.0002, 73, [reply("aa", -9, -8)]);
  const none = point(55, 73, []);
  const near = point(55.0001, 73.0001, []);

  assert.deepEqual(tilesAt([good, weak]).map((t) => t.tone), ["good"]);
  assert.deepEqual(tilesAt([none]).map((t) => t.tone), ["none"]);
  assert.deepEqual(tilesAt([none, weak]).map((t) => t.tone), ["weak"], "one answer lifts the tile off grey");
  assert.equal(tilesAt([near, none]).length, 1, "two asks in one tile are one tile");
});

test("the map draws the finest grid whose tile is not a smear", () => {
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
