import { test } from "node:test";
import assert from "node:assert/strict";
import { anchorScrollDelta, bindChatZoom, clampZoom, pinchScale, settleZoom, ZOOM_MAX, ZOOM_MIN } from "./chatZoom.js";

test("pinchScale scales proportionally and clamps to the readable bounds", () => {
  assert.equal(pinchScale(1, 100, 150), 1.5);
  assert.equal(pinchScale(1.5, 100, 150), 2.25);
  assert.equal(pinchScale(1, 100, 500), ZOOM_MAX);
  assert.equal(pinchScale(1, 100, 20), ZOOM_MIN);
  assert.equal(pinchScale(1.5, 5, 200), 1.5);
  assert.equal(clampZoom(NaN), 1);
});

test("settleZoom snaps back to normal near the reader's usual text size", () => {
  assert.equal(settleZoom(1.04, 1), null);
  assert.equal(settleZoom(0.95, 1), null);
  assert.equal(settleZoom(1.53, 1.5), null);
  assert.equal(settleZoom(1.8, 1), 1.8);
  assert.equal(settleZoom(1.2, 1.5), 1.2);
  assert.equal(settleZoom(2.6, 2), 2.6);
});

test("anchorScrollDelta keeps the pinched line under the fingers after reflow", () => {
  // Message grew from 60px to 120px; midpoint was half-way down (ratio 0.5) at y = 300.
  // Its top is still at 270, so its midpoint moved to 330 -> scroll down by 30px.
  assert.equal(anchorScrollDelta(270, 120, 0.5, 300), 30);
  // Message shrank back to 60px; top is 270, midpoint is 300 -> no movement needed.
  assert.equal(anchorScrollDelta(270, 60, 0.5, 300), 0);
});

function fakeClassList() {
  const set = new Set<string>();
  return {
    add: (c: string) => set.add(c),
    remove: (c: string) => set.delete(c),
    has: (c: string) => set.has(c),
  };
}

function fakeChatDom() {
  const listeners = new Map<string, (e: unknown) => void>();
  const styleProps = new Map<string, string>();
  const bodyClasses = fakeClassList();
  const pillClasses = fakeClassList();
  const pillValue = { textContent: "" };
  const pill = {
    classList: pillClasses,
    querySelector: (sel: string) => (sel === ".chat-zoom-value" ? pillValue : null),
  } as unknown as HTMLButtonElement;
  const body = {
    classList: bodyClasses,
    style: {
      setProperty: (k: string, v: string) => styleProps.set(k, v),
      removeProperty: (k: string) => styleProps.delete(k),
    },
  } as unknown as HTMLElement;

  let scrollTop = 100;
  // Message height grows proportionally with --chat-scale to simulate text reflow.
  const rowRect = () => {
    const scale = Number(styleProps.get("--chat-scale") ?? 1);
    const height = 60 * scale;
    const top = 300 - scrollTop;
    return { top, bottom: top + height, height };
  };
  const row = {
    dataset: { id: "m1" },
    getBoundingClientRect: rowRect,
  } as unknown as HTMLElement;

  const scroller = {
    get scrollTop() {
      return scrollTop;
    },
    set scrollTop(v: number) {
      scrollTop = v;
    },
    getBoundingClientRect: () => ({ top: 50, bottom: 650, height: 600 }),
    querySelectorAll: () => [row],
    querySelector: (sel: string) => (sel.includes("m1") ? row : null),
    addEventListener: (name: string, fn: (e: unknown) => void) => listeners.set(name, fn),
    removeEventListener: (name: string) => listeners.delete(name),
  } as unknown as HTMLElement;

  const touch = (type: string, points: Array<[number, number]>) => {
    let prevented = false;
    const event = {
      touches: points.map(([clientX, clientY]) => ({ clientX, clientY })),
      preventDefault: () => {
        prevented = true;
      },
    };
    listeners.get(type)?.(event);
    return prevented;
  };

  return { scroller, body, pill, pillValue, styleProps, bodyClasses, pillClasses, touch, rowRect };
}

test("bindChatZoom scales on a two-finger pinch, anchors scroll, and resets cleanly", () => {
  const dom = fakeChatDom();
  let changes = 0;
  let snaps = 0;
  const bound = bindChatZoom(dom.scroller, dom.body, () => dom.pill, () => 1, {
    onChange: () => changes++,
    onSnap: () => snaps++,
    schedule: (fn) => {
      fn();
      return 1;
    },
    cancel: () => undefined,
  });

  // Start pinch centered at y = 230 (middle of m1: top 200..260), initial span 80px.
  dom.touch("touchstart", [
    [100, 190],
    [100, 270],
  ]);
  // Spread fingers to 160px -> 2x scale (200%).
  const prevented = dom.touch("touchmove", [
    [100, 150],
    [100, 310],
  ]);
  assert.equal(prevented, true);
  assert.equal(dom.bodyClasses.has("zoomed"), true);
  assert.equal(dom.styleProps.get("--chat-scale"), "2");
  assert.equal(dom.pillClasses.has("on"), true);
  assert.equal(dom.pillValue.textContent, "200%");
  // Reflow doubled m1's height from 60 to 120; scrollTop should shift by +30 so y = 230 stays its midpoint.
  assert.equal(dom.scroller.scrollTop, 130);

  // Lift fingers -> stays zoomed at 200%.
  dom.touch("touchend", []);
  assert.equal(dom.bodyClasses.has("zoomed"), true);
  assert.equal(dom.styleProps.get("--chat-scale"), "2");
  assert.equal(changes, 1);
  assert.equal(snaps, 0);

  // Tapping the pill resets zoom back to normal.
  bound.reset();
  assert.equal(dom.bodyClasses.has("zoomed"), false);
  assert.equal(dom.styleProps.has("--chat-scale"), false);
  assert.equal(dom.pillClasses.has("on"), false);
  assert.equal(changes, 2);

  // Pinching out and pinching back near 100% snaps back to normal with haptic feedback.
  dom.touch("touchstart", [
    [100, 190],
    [100, 270],
  ]);
  dom.touch("touchmove", [
    [100, 150],
    [100, 310],
  ]);
  dom.touch("touchmove", [
    [100, 189],
    [100, 271],
  ]);
  dom.touch("touchend", []);
  assert.equal(dom.bodyClasses.has("zoomed"), false);
  assert.equal(dom.pillClasses.has("on"), false);
  assert.equal(snaps, 1);

  bound.dispose();
});
