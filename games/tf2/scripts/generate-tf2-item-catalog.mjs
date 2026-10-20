#!/usr/bin/env node
import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const sourceUrl = process.env.TF2_ITEM_SCHEMA_URL
  || "https://raw.githubusercontent.com/tsuza/tf2-item-schema/master/files/localized/item_schema_json_en.txt";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "..");
const outputPath = resolve(projectRoot, "mod", "addons", "sourcemod", "scripting", "takaro_tf2_item_catalog.inc");

function sourcepawnString(value) {
  return String(value ?? "")
    .normalize("NFKD")
    .replace(/[\u0300-\u036f]/g, "")
    .replace(/[^\x20-\x7E]/g, "")
    .replace(/\\/g, "\\\\")
    .replace(/"/g, "\\\"")
    .replace(/\r?\n/g, " ");
}

function itemName(item) {
  const readable = String(item.item_name ?? "").trim();
  if (readable && !readable.startsWith("#")) return readable;

  const fallback = String(item.name ?? "").trim();
  if (fallback) return fallback.replace(/^Upgradeable /, "");

  return `TF2 item ${item.defindex}`;
}

function itemDescription(item) {
  const parts = ["TF2 Steam schema weapon"];
  if (item.item_class) parts.push(`class ${item.item_class}`);
  if (item.item_slot) parts.push(`slot ${item.item_slot}`);
  if (Array.isArray(item.used_by_classes) && item.used_by_classes.length) {
    parts.push(`used by ${item.used_by_classes.join("/")}`);
  }
  return parts.join("; ");
}

const response = await fetch(sourceUrl);
if (!response.ok) {
  throw new Error(`Failed to fetch TF2 item schema from ${sourceUrl}: HTTP ${response.status}`);
}

const schema = await response.json();
const rawItems = schema?.result?.items;
if (!Array.isArray(rawItems)) {
  throw new Error("TF2 item schema did not contain result.items[]");
}

const weapons = rawItems
  .filter((item) => Number.isInteger(item.defindex))
  .filter((item) => String(item.item_class ?? "").startsWith("tf_weapon"))
  .sort((a, b) => a.defindex - b.defindex);

if (weapons.length < 300) {
  throw new Error(`Expected at least 300 TF2 weapon schema items, received ${weapons.length}`);
}

const lines = [
  "// This file is generated. Do not edit by hand.",
  `// Generated from ${sourceUrl}`,
  `// Entries: ${weapons.length} TF2 Steam schema weapon items.`,
  "",
  "void PopulateGeneratedTf2ItemCatalog(JSONArray items)",
  "{"
];

for (const item of weapons) {
  const code = sourcepawnString(item.defindex);
  const name = sourcepawnString(itemName(item));
  const description = sourcepawnString(itemDescription(item));
  const quality = sourcepawnString(item.item_class);
  lines.push(`  PushItemDTO(items, "${code}", "${name}", "${description}", "${quality}", 1);`);
}

lines.push("}", "");
writeFileSync(outputPath, lines.join("\n"), "utf8");
console.log(`Generated ${weapons.length} TF2 item catalog entries at ${outputPath}`);
