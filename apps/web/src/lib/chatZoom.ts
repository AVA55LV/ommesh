/**
 * Temporary pinch-to-zoom for the messages in a chat (#130): two fingers on
 * the list scale its text live and keep the line between them under the
 * fingers as the bubbles reflow. Leaving the chat, pinching back to the
 * reader's usual size, or tapping the pill returns the chat to normal.
 */

import { useCallback, useEffect, useRef, type RefObject } from "react";
import { haptic } from "./haptic.js";

export const ZOOM_MIN = 0.9;
export const ZOOM_MAX = 3;
/** Releasing within this much of the reader's usual text size snaps back to it. */
export const ZOOM_SNAP = 0.06;
/** Ignore a two-finger touch with fingers nearly stacked, or barely moved. */
const MIN_SPAN = 10;
const SLOP = 6;

export function clampZoom(scale: number): number {
  if (!Number.isFinite(scale) || scale <= 0) return 1;
  return Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, Math.round(scale * 100) / 100));
}

/** The text scale for a pinch that started at `startScale` and `startSpan`, now at `span`. */
export function pinchScale(startScale: number, startSpan: number, span: number): number {
  if (!Number.isFinite(startSpan) || startSpan < MIN_SPAN || !Number.isFinite(span) || span <= 0) {
    return clampZoom(startScale);
  }
  return clampZoom(startScale * (span / startSpan));
}

/** Where a pinch lands when the fingers lift: null once back at the reader's usual size. */
export function settleZoom(scale: number, baseScale: number): number | null {
  const clamped = clampZoom(scale);
  return Math.abs(clamped - baseScale) <= ZOOM_SNAP ? null : clamped;
}

/**
 * How far to move `scrollTop` after a reflow so the point `ratio` of the way
 * down an anchored message stays at `targetY` on screen.
 */
export function anchorScrollDelta(top: number, height: number, ratio: number, targetY: number): number {
  return top + ratio * height - targetY;
}

interface Anchor {
  id: string;
  ratio: number;
  targetY: number;
}

function spanOf(touches: ArrayLike<{ clientX: number; clientY: number }>): number {
  const a = touches[0]!;
  const b = touches[1]!;
  return Math.hypot(b.clientX - a.clientX, b.clientY - a.clientY);
}

function escapeId(id: string): string {
  return typeof CSS !== "undefined" && typeof CSS.escape === "function" ? CSS.escape(id) : id.replace(/["\\]/g, "\\$&");
}

function findAnchor(el: HTMLElement, targetY: number): Anchor | null {
  const rows = el.querySelectorAll<HTMLElement>("[data-id]");
  let best: HTMLElement | null = null;
  let bestDist = Infinity;
  for (const row of rows) {
    const rect = row.getBoundingClientRect();
    if (rect.height <= 0) continue;
    if (rect.top <= targetY && rect.bottom >= targetY) {
      best = row;
      break;
    }
    const dist = targetY < rect.top ? rect.top - targetY : targetY - rect.bottom;
    if (dist < bestDist) {
      bestDist = dist;
      best = row;
    }
  }
  if (!best?.dataset.id) return null;
  const rect = best.getBoundingClientRect();
  const ratio = Math.min(1, Math.max(0, (targetY - rect.top) / Math.max(1, rect.height)));
  return { id: best.dataset.id, ratio, targetY };
}

function holdAnchor(el: HTMLElement, anchor: Anchor | null): void {
  if (!anchor) return;
  const row = el.querySelector<HTMLElement>(`[data-id="${escapeId(anchor.id)}"]`);
  if (!row) return;
  const rect = row.getBoundingClientRect();
  const delta = anchorScrollDelta(rect.top, rect.height, anchor.ratio, anchor.targetY);
  if (Math.abs(delta) > 0.5) el.scrollTop += delta;
}

function showPill(button: HTMLButtonElement | null, scale: number | null): void {
  if (!button) return;
  if (scale === null) {
    button.classList.remove("on");
    return;
  }
  const value = button.querySelector(".chat-zoom-value");
  const label = `${Math.round(scale * 100)}%`;
  if (value) value.textContent = label;
  button.classList.add("on");
}

function applyScale(body: HTMLElement | null, button: HTMLButtonElement | null, scale: number | null): void {
  if (!body) return;
  if (scale === null) {
    body.classList.remove("zoomed");
    body.style.removeProperty("--chat-scale");
  } else {
    body.style.setProperty("--chat-scale", String(scale));
    body.classList.add("zoomed");
  }
  showPill(button, scale);
}

export interface ChatZoomBinding {
  reset(): void;
  clear(): void;
  dispose(): void;
}

/**
 * Attaches two-finger pinch-to-zoom to a chat list's DOM elements. The scale
 * is written straight to `body` (and the pill's label) so messages do not
 * re-render on every frame of the gesture.
 */
export function bindChatZoom(
  el: HTMLElement,
  body: HTMLElement,
  getPill: () => HTMLButtonElement | null,
  getBaseScale: () => number,
  options: {
    onChange?: () => void;
    onSnap?: () => void;
    schedule?: (fn: () => void) => number;
    cancel?: (id: number) => void;
  } = {},
): ChatZoomBinding {
  const schedule = options.schedule ?? ((fn) => requestAnimationFrame(fn));
  const cancel = options.cancel ?? ((id) => cancelAnimationFrame(id));

  let current: number | null = null;
  let tracking = false;
  let pinching = false;
  let startSpan = 0;
  let startScale = getBaseScale();
  let live = startScale;
  let anchor: Anchor | null = null;
  let frame = 0;
  let pending: number | null = null;

  const flush = () => {
    frame = 0;
    if (pending === null) return;
    const next = pending;
    pending = null;
    live = next;
    current = next;
    applyScale(body, getPill(), next);
    holdAnchor(el, anchor);
  };

  const down = (e: TouchEvent) => {
    if (!e.touches || e.touches.length !== 2) return;
    const span = spanOf(e.touches);
    if (span < MIN_SPAN) return;
    const midY = (e.touches[0]!.clientY + e.touches[1]!.clientY) / 2;
    tracking = true;
    pinching = false;
    startSpan = span;
    startScale = current ?? getBaseScale();
    live = startScale;
    anchor = findAnchor(el, midY);
  };

  const move = (e: TouchEvent) => {
    if (!tracking || !e.touches || e.touches.length !== 2) return;
    const span = spanOf(e.touches);
    if (!pinching) {
      if (Math.abs(span - startSpan) < SLOP) return;
      pinching = true;
    }
    e.preventDefault();
    pending = pinchScale(startScale, startSpan, span);
    if (!frame) frame = schedule(flush);
  };

  const end = (e: TouchEvent) => {
    if (!tracking || (e.touches && e.touches.length >= 2)) return;
    if (frame) {
      cancel(frame);
      frame = 0;
    }
    if (pending !== null) {
      live = pending;
      pending = null;
    }
    const wasPinching = pinching;
    const savedAnchor = anchor;
    tracking = false;
    pinching = false;
    anchor = null;
    if (!wasPinching) return;
    const settled = settleZoom(live, getBaseScale());
    const snappedBack = settled === null && current !== null;
    current = settled;
    applyScale(body, getPill(), settled);
    holdAnchor(el, savedAnchor);
    if (snappedBack) (options.onSnap ?? haptic)();
    options.onChange?.();
  };

  const reset = () => {
    if (current === null) return;
    const box = el.getBoundingClientRect();
    const mid = findAnchor(el, box.top + box.height / 2);
    current = null;
    applyScale(body, getPill(), null);
    holdAnchor(el, mid);
    options.onChange?.();
  };

  const clear = () => {
    if (frame) {
      cancel(frame);
      frame = 0;
    }
    pending = null;
    tracking = false;
    pinching = false;
    anchor = null;
    current = null;
    applyScale(body, getPill(), null);
  };

  el.addEventListener("touchstart", down, { passive: true });
  el.addEventListener("touchmove", move, { passive: false });
  el.addEventListener("touchend", end);
  el.addEventListener("touchcancel", end);

  return {
    reset,
    clear,
    dispose() {
      el.removeEventListener("touchstart", down);
      el.removeEventListener("touchmove", move);
      el.removeEventListener("touchend", end);
      el.removeEventListener("touchcancel", end);
      clear();
    },
  };
}

/**
 * Pinch-to-zoom on a chat's list. Leaving the chat or changing the app's
 * text size drops any temporary zoom.
 */
export function useChatZoom(
  scroller: RefObject<HTMLElement | null>,
  inner: RefObject<HTMLElement | null>,
  baseScale: number,
  conversation: string,
  onChange?: () => void,
) {
  const pill = useRef<HTMLButtonElement>(null);
  const binding = useRef<ChatZoomBinding | null>(null);
  const baseRef = useRef(baseScale);
  baseRef.current = baseScale;
  const changeRef = useRef(onChange);
  changeRef.current = onChange;

  useEffect(() => {
    const el = scroller.current;
    const body = inner.current;
    if (!el || !body) return;
    const bound = bindChatZoom(el, body, () => pill.current, () => baseRef.current, {
      onChange: () => changeRef.current?.(),
    });
    binding.current = bound;
    return () => {
      binding.current = null;
      bound.dispose();
    };
  }, [scroller, inner]);

  // Leaving the chat or changing the app's text size drops any temporary zoom.
  useEffect(() => {
    binding.current?.clear();
  }, [conversation, baseScale]);

  const reset = useCallback(() => {
    binding.current?.reset();
  }, []);

  return { pill, reset };
}
