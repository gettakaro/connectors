#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';

const [itemTablePath, maybeNameMapPath, maybeOutputPath] = process.argv.slice(2);

if (!itemTablePath || !maybeNameMapPath) {
  console.error('Usage: node scripts/prepare-item-catalog.mjs <ItemTable.json> [ItemNameToTemplateID.json] <output.json>');
  process.exit(1);
}

const nameMapPath = maybeOutputPath ? maybeNameMapPath : null;
const outputPath = maybeOutputPath ?? maybeNameMapPath;

try {
  const itemRows = collectRows(readJson(itemTablePath));
  const nameRows = nameMapPath ? collectRows(readJson(nameMapPath)) : [];
  const codeByTemplateId = new Map();

  for (const row of nameRows) {
    const templateId = templateIdForRow(row);
    const code = stringValue(row.RowName);
    if (!templateId || !code) continue;
    codeByTemplateId.set(templateId, code);
  }

  const items = [];
  for (const row of itemRows) {
    const templateId = templateIdForRow(row);
    if (!templateId) continue;

    const rowName = stringValue(row.RowName) || templateId;
    const code = codeByTemplateId.get(templateId) || rowName;
    const name = displayNameForRow(row) || code;

    if (isInternalName(rowName) || isInternalName(code) || isInternalName(name)) continue;

    const entry = {
      templateId,
      code,
      name,
    };

    const itemClass = stringValue(row.ItemClass ?? row.itemClass ?? row.Class ?? row.ItemBlueprint);
    if (itemClass) entry.itemClass = itemClass;

    const category = stringValue(row.Category ?? row.category ?? row.ItemType);
    if (category) entry.category = category;

    const tags = arrayValue(row.Tags ?? row.tags);
    if (tags.length) entry.tags = tags;

    const aliases = unique([templateId, name].filter((alias) => alias && alias !== code));
    if (aliases.length) entry.aliases = aliases;

    items.push(entry);
  }

  items.sort((a, b) => a.name.localeCompare(b.name) || a.code.localeCompare(b.code));

  const source = nameMapPath
    ? `DevKit ${path.basename(itemTablePath)} + ${path.basename(nameMapPath)}`
    : `DevKit ${path.basename(itemTablePath)}`;
  fs.writeFileSync(outputPath, `${JSON.stringify({ version: 1, source, items }, null, 2)}\n`);
} catch (err) {
  console.error(err instanceof Error ? err.message : String(err));
  process.exit(1);
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function collectRows(value) {
  if (Array.isArray(value)) return value.map((row) => normalizeObjectRow(row)).filter(Boolean);
  if (!value || typeof value !== 'object') return [];

  for (const key of ['Rows', 'rows', 'Data', 'data']) {
    if (Array.isArray(value[key])) return value[key].map((row) => normalizeObjectRow(row)).filter(Boolean);
  }

  return Object.entries(value)
    .map(([rowName, row]) => {
      const normalized = normalizeObjectRow(row);
      return normalized ? { RowName: rowName, ...normalized } : null;
    })
    .filter(Boolean);
}

function normalizeObjectRow(row) {
  return row && typeof row === 'object' && !Array.isArray(row) ? row : null;
}

function templateIdForRow(row) {
  const value =
    row.TemplateID ??
    row.TemplateId ??
    row.templateId ??
    row.Template_ID ??
    row.ID ??
    row.Id ??
    row.id ??
    (isNumericString(row.RowName) ? row.RowName : null);
  const text = stringValue(value);
  return text && isNumericString(text) ? text : null;
}

function displayNameForRow(row) {
  for (const field of [row.DisplayName, row.Name, row.ItemName, row.Title, row.Description]) {
    const value = stringValue(field);
    if (!value) continue;
    return extractNsLocText(value) || value;
  }
  return null;
}

function extractNsLocText(value) {
  const match = /^NSLOCTEXT\(\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*"((?:\\"|[^"])*)"\s*\)$/i.exec(value.trim());
  return match ? match[1].replace(/\\"/g, '"') : null;
}

function stringValue(value) {
  if (value == null) return null;
  if (typeof value === 'string') return value.trim();
  if (typeof value === 'number' && Number.isFinite(value)) return String(value);
  return null;
}

function arrayValue(value) {
  if (Array.isArray(value)) return unique(value.map((entry) => stringValue(entry)).filter(Boolean));
  const text = stringValue(value);
  return text ? unique(text.split(',').map((entry) => entry.trim()).filter(Boolean)) : [];
}

function unique(values) {
  return [...new Set(values)];
}

function isNumericString(value) {
  return /^\d+$/.test(String(value ?? '').trim());
}

function isInternalName(value) {
  return /^(XX_|DEV_)/i.test(String(value ?? '').trim());
}
