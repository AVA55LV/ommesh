import { test } from "node:test";
import assert from "node:assert/strict";
import { clampTextScale, TEXT_STEPS } from "./textSize.js";

test("text size steps run from 90% to 200% in order", () => {
  assert.equal(TEXT_STEPS[0], 0.9);
  assert.equal(TEXT_STEPS[TEXT_STEPS.length - 1], 2);
  for (let i = 1; i < TEXT_STEPS.length; i++) {
    assert.ok(TEXT_STEPS[i]! > TEXT_STEPS[i - 1]!, "steps must increase");
  }
});

test("clampTextScale keeps scales inside the offered range and falls back to 1", () => {
  assert.equal(clampTextScale(0.5), 0.9);
  assert.equal(clampTextScale(1.75), 1.75);
  assert.equal(clampTextScale(2), 2);
  assert.equal(clampTextScale(2.5), 2);
  assert.equal(clampTextScale(1.234), 1.23);
  assert.equal(clampTextScale(NaN), 1);
  assert.equal(clampTextScale(0), 1);
  assert.equal(clampTextScale(-1), 1);
});
