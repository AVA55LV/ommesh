import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { CLI_GROUPS, CLI_GROUP_TITLE, COMMANDS, RADIO_COMMANDS, suggest } from "./cli.js";

const labels = (draft: string) => suggest(draft).chips.map((c) => c.label);

/** The words of the reference, as the app ships them. */
const words = (code: string) => JSON.parse(readFileSync(new URL(`../i18n/${code}/cli.json`, import.meta.url), "utf8")) as Record<string, string>;

test("part of a command anywhere in it finds it: duty finds both dutycycles", () => {
  assert.deepEqual(labels("duty"), ["get dutycycle", "set dutycycle"]);
  assert.deepEqual(labels("cycle"), ["get dutycycle", "set dutycycle"]);
  assert.ok(labels("wifi").includes("set wifi.ssid"));
});

test("commands that start with what is typed come before those that only contain it", () => {
  const found = labels("set flood");
  assert.deepEqual(found.slice(0, 4), ["set flood.max", "set flood.max.advert", "set flood.max.unscoped", "set flood.advert.interval"]);
});

test("a command that takes a value says what the value is, and offers the values it takes", () => {
  assert.equal(suggest("set dutycycle ").hint, "1–100, % of airtime it may use");
  assert.equal(suggest("set dutycycle 5").hint, "1–100, % of airtime it may use");
  assert.deepEqual(suggest("set repeat ").chips, [
    { label: "on", fill: "set repeat on" },
    { label: "off", fill: "set repeat off" },
  ]);
  assert.deepEqual(labels("set repeat"), ["on", "off"]);
  assert.deepEqual(labels("set loop.detect m"), ["minimal", "moderate"]);
  assert.deepEqual(labels("set repeat on"), []);
  // "set flood.max " is its own command, not the start of "set flood.max.advert".
  assert.equal(suggest("set flood.max ").hint, "hops, 0–64");
});

test("a word with subcommands offers them, and each goes on to its own value", () => {
  assert.ok(labels("region ").includes("home"));
  assert.deepEqual(labels("gps advert "), ["none", "share", "prefs"]);
  assert.equal(suggest("region put ").hint, "region, then its parent");
});

test("nothing typed offers the everyday ones; serial-only commands are not offered at all", () => {
  assert.deepEqual(labels(""), ["ver", "clock", "neighbors", "get radio", "get dutycycle", "advert"]);
  const all = COMMANDS.map((c) => c.text.trim());
  for (const local of ["erase", "log", "stats-packets", "set freq", "get prv.key", "get acl"]) assert.ok(!all.includes(local), local);
  // "gps" asks, "gps " goes on to a value: the same word twice, but each once.
  assert.equal(new Set(COMMANDS.map((c) => c.text)).size, COMMANDS.length);
});

test("the radio's own console offers the companion's commands, not a repeater's", () => {
  const radio = (draft: string) => suggest(draft, "radio").chips.map((c) => c.label);
  assert.deepEqual(radio(""), ["ver", "get radio", "get tx", "get name", "board"]);
  assert.deepEqual(radio("set p"), ["set path.hash.mode", "set pin"]);
  assert.ok(!radio("nei").length, "no neighbours on a companion");
  assert.equal(suggest("set tz.offset ", "radio").hint, "hours from UTC, −12 to 14");
  assert.equal(new Set(RADIO_COMMANDS.map((c) => c.text)).size, RADIO_COMMANDS.length);
});

test("a suggestion says what the command does, and how many there are", () => {
  const adc = suggest("adc");
  assert.deepEqual(adc.chips.map((c) => c.label), ["get adc.multiplier", "set adc.multiplier"]);
  assert.ok(adc.chips.every((c) => c.about), "both say what they do");
  assert.equal(adc.total, 2);
  // A value says nothing about itself: it is not a command.
  assert.ok(suggest("set repeat ").chips.every((c) => !c.about));
  // More than the chips show: the field lists a few, the reference has the rest.
  const many = suggest("set");
  assert.equal(many.chips.length, 12);
  assert.ok(many.total > many.chips.length);
});

test("every command has words in both languages", () => {
  const en = words("en");
  const ru = words("ru");
  const missing: string[] = [];
  for (const command of [...COMMANDS, ...RADIO_COMMANDS]) {
    const key = command.about.replace(/^cli\./, "");
    if (!en[key]) missing.push(`en ${key}`);
    if (!ru[key]) missing.push(`ru ${key}`);
  }
  assert.deepEqual(missing, [], "a command with no words reads as its key");
});

test("no words are left over for a command that no longer exists", () => {
  const used = new Set([...COMMANDS, ...RADIO_COMMANDS].map((command) => command.about.replace(/^cli\./, "")));
  const dead = Object.keys(words("en")).filter((key) => key.startsWith("about.") && !used.has(key));
  assert.deepEqual(dead, [], "the reference would never show these");
});

test("every command is in a group the reference knows, and each group is named in both languages", () => {
  const en = words("en");
  const ru = words("ru");
  for (const command of [...COMMANDS, ...RADIO_COMMANDS]) {
    assert.ok(CLI_GROUPS.includes(command.group), `${command.text.trim()} is in no group`);
    const key = CLI_GROUP_TITLE[command.group].replace(/^cli\./, "");
    assert.ok(en[key], `en ${key}`);
    assert.ok(ru[key], `ru ${key}`);
  }
});
