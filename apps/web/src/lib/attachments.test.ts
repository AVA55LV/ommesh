import { test } from "node:test";
import assert from "node:assert/strict";
import { t } from "../i18n/index.js";
import { ATTACH_DEFAULTS, attachmentsReady, autoPathStyle, clipboardFiles, keyFor, kindOf, linkFor, nameOfPaste, presignPut, publicBase, sizeText, type AttachmentsSettings } from "./attachments.js";

const SETTINGS: AttachmentsSettings = {
  ...ATTACH_DEFAULTS,
  enabled: true,
  endpoint: "https://s3.example.ru",
  bucket: "ommesh",
  region: "ru-1",
  accessKey: "OMSHK7Q2XW4",
  secretKey: "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
};

const AT = new Date("2026-11-17T05:10:00Z");

test("attachments stay off until the bucket is filled in", () => {
  assert.equal(attachmentsReady(), false);
  assert.equal(attachmentsReady({ ...ATTACH_DEFAULTS, enabled: true }), false);
  assert.equal(attachmentsReady(SETTINGS), true);
  // The switch alone is not enough: without a bucket there is nowhere to put a photo.
  assert.equal(attachmentsReady({ ...SETTINGS, secretKey: "" }), false);
});

test("the link is the public address and a two-part key, short enough for a message", () => {
  assert.equal(publicBase(SETTINGS), "https://s3.example.ru/ommesh");
  assert.equal(publicBase({ ...SETTINGS, publicBase: "https://i.example.ru/" }), "https://i.example.ru");
  assert.equal(linkFor("p/6f3a91c2.jpg", { ...SETTINGS, publicBase: "https://i.example.ru" }), "https://i.example.ru/p/6f3a91c2.jpg");
  // 158 bytes is what a direct message may carry: the link leaves room for a word or two.
  assert.ok(linkFor("p/6f3a91c2.jpg", { ...SETTINGS, publicBase: "https://i.example.ru" }).length < 60);
});

test("a key says what kind of thing it is and carries no name of the reader's", () => {
  assert.match(keyFor("photo", "тропа.jpg", "image/jpeg"), /^p\/[0-9a-f]{8}\.jpg$/);
  assert.match(keyFor("photo", "снимок.png", "image/png"), /^p\/[0-9a-f]{8}\.png$/);
  assert.match(keyFor("file", "Договор аренды.pdf", "application/pdf"), /^f\/[0-9a-f]{8}\.pdf$/);
  assert.match(keyFor("file", "archive", ""), /^f\/[0-9a-f]{8}\.bin$/);
  // Two uploads of the same name never share a key.
  assert.notEqual(keyFor("photo", "тропа.jpg"), keyFor("photo", "тропа.jpg"));
});

test("a presigned put names the bucket, the host and the signature, and never the secret", async () => {
  const url = await presignPut(SETTINGS, "p/6f3a91c2.jpg", AT);
  assert.match(url, /^https:\/\/ommesh\.s3\.example\.ru\/p\/6f3a91c2\.jpg\?/);
  assert.match(url, /X-Amz-Algorithm=AWS4-HMAC-SHA256/);
  assert.match(url, /X-Amz-Credential=OMSHK7Q2XW4%2F20261117%2Fru-1%2Fs3%2Faws4_request/);
  assert.match(url, /X-Amz-Date=20261117T051000Z/);
  assert.match(url, /X-Amz-Expires=900/);
  assert.match(url, /X-Amz-SignedHeaders=host/);
  assert.match(url, /X-Amz-Signature=[0-9a-f]{64}$/);
  // The secret signs and stays home: nothing in the address says it.
  assert.equal(url.includes(SETTINGS.secretKey), false);
  assert.equal(url.includes("secret"), false);
});

test("the same moment and key sign the same, another key signs differently", async () => {
  const one = await presignPut(SETTINGS, "p/1.jpg", AT);
  const again = await presignPut(SETTINGS, "p/1.jpg", AT);
  const other = await presignPut(SETTINGS, "p/2.jpg", AT);
  assert.equal(one, again);
  assert.notEqual(one, other);
});

test("a bucket on an address and a port is reached by path, not by its name under the host", async () => {
  const local = { ...SETTINGS, endpoint: "http://localhost:9000" };
  const url = await presignPut(local, "f/6f3a91c2.pdf", AT);
  assert.match(url, /^http:\/\/localhost:9000\/ommesh\/f\/6f3a91c2\.pdf\?/);
});

test("a bucket behind a proxy on the web's own port is put in the path only when said so", async () => {
  const behind = { ...SETTINGS, endpoint: "https://preview.example.ru" };
  assert.match(await presignPut(behind, "p/6f3a91c2.jpg", AT), /^https:\/\/ommesh\.preview\.example\.ru\/p\//);
  const path = await presignPut({ ...behind, pathStyle: true }, "p/6f3a91c2.jpg", AT);
  assert.match(path, /^https:\/\/preview\.example\.ru\/ommesh\/p\/6f3a91c2\.jpg\?/);
  // A port or a bare address cannot be a subdomain: those are a path whatever the switch says.
  assert.equal(autoPathStyle({ ...SETTINGS, endpoint: "http://localhost:9000" }), true);
  assert.equal(autoPathStyle(behind), false);
});

test("a pasted picture goes the photo's way and anything else as a file", () => {
  assert.equal(kindOf({ type: "image/png" }), "photo");
  assert.equal(kindOf({ type: "IMAGE/JPEG" }), "photo");
  assert.equal(kindOf({ type: "application/pdf" }), "file");
  // A file copied in the file manager may have no type at all.
  assert.equal(kindOf({ type: "" }), "file");
});

test("a file the clipboard gave without a name still has one", () => {
  assert.equal(nameOfPaste(new File(["x"], "тропа.jpg", { type: "image/jpeg" })), "тропа.jpg");
  assert.equal(nameOfPaste(new File(["x"], "", { type: "image/png" })), "clipboard.png");
  assert.equal(nameOfPaste(new File(["x"], "", { type: "" })), "clipboard.bin");
});

test("a clipboard is read as files, in the order they were copied, and text is none", () => {
  const photo = new File(["png"], "image.png", { type: "image/png" });
  const doc = new File(["pdf"], "договор.pdf", { type: "application/pdf" });
  // A screenshot comes as an item of kind file, whatever else the clipboard holds.
  assert.deepEqual(clipboardFiles({ items: [{ kind: "file", getAsFile: () => photo }, { kind: "string", getAsFile: () => null }], files: [] }), [photo]);
  // A file copied in the file manager may be listed among the items too; the items are the order.
  assert.deepEqual(clipboardFiles({ items: [{ kind: "file", getAsFile: () => doc }, { kind: "file", getAsFile: () => photo }] }), [doc, photo]);
  // A browser that fills only the plain list still gives its files.
  assert.deepEqual(clipboardFiles({ items: [], files: [photo, doc] }), [photo, doc]);
  // Copied words are words, not an attachment; so is an empty clipboard.
  assert.deepEqual(clipboardFiles({ items: [{ kind: "string", getAsFile: () => null }], files: [] }), []);
  assert.deepEqual(clipboardFiles({}), []);
});

test("a size follows the app's language and the reader's decimal mark", () => {
  // Both halves are read while drawing: the number ours, the words i18n's.
  assert.equal(sizeText(900), t("chats.preview.kb", { size: "1" }));
  assert.equal(sizeText(2_400_000), t("chats.preview.mb", { size: "2.3" }));
});


