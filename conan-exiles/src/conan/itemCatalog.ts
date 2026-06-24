import fs from 'node:fs';

export interface ConanItemCatalogEntry {
  templateId: string;
  code: string;
  name: string;
  aliases?: string[];
  category?: string;
  itemClass?: string;
  tags?: string[];
}

export interface ConanItemCatalogDocument {
  version: 1;
  source: string;
  items: ConanItemCatalogEntry[];
}

interface CatalogOptions {
  hasExternalCatalog?: boolean;
}

const SEED_CATALOG_DOCUMENT: ConanItemCatalogDocument = {
  version: 1,
  source: 'built-in seed',
  items: [
    { templateId: '10001', code: 'Stone', name: 'Stone' },
    { templateId: '12001', code: 'PlantFiber', name: 'Plant Fiber', aliases: ['plant-fiber', 'plant_fiber'] },
  ],
};

export class ConanItemCatalog {
  readonly hasExternalCatalog: boolean;

  private readonly entriesByTemplateId = new Map<string, ConanItemCatalogEntry>();
  private readonly templateIdByAlias = new Map<string, string>();
  private readonly ambiguousDisplayNames: Set<string>;

  constructor(document: ConanItemCatalogDocument = SEED_CATALOG_DOCUMENT, options: CatalogOptions = {}) {
    this.hasExternalCatalog = Boolean(options.hasExternalCatalog);
    const entries = [...SEED_CATALOG_DOCUMENT.items, ...document.items].map(normalizeEntry);
    const nameCounts = displayNameCounts(entries);
    const uniqueNames = new Set([...nameCounts.entries()].filter(([, count]) => count === 1).map(([name]) => name));
    this.ambiguousDisplayNames = new Set([...nameCounts.entries()].filter(([, count]) => count > 1).map(([name]) => name));
    for (const entry of entries) this.addEntry(entry, uniqueNames);
  }

  resolveTemplateId(input: string): string {
    const value = String(input).trim();
    const fallbackLabelMatch = /^Conan item\s+(\d+)$/i.exec(value);
    if (fallbackLabelMatch) return fallbackLabelMatch[1]!;
    return this.templateIdByAlias.get(normalizeCatalogAlias(value)) ?? value;
  }

  publicCodeForTemplate(id: string): string {
    const templateId = String(id).trim();
    return this.entriesByTemplateId.get(templateId)?.code ?? templateId;
  }

  nameForTemplate(id: string): string {
    const templateId = String(id).trim();
    return this.entriesByTemplateId.get(templateId)?.name ?? `Conan item ${templateId}`;
  }

  itemForTemplate(id: string): { code: string; name: string } {
    const templateId = String(id).trim();
    const known = this.entriesByTemplateId.get(templateId);
    return known ? { code: known.code, name: known.name } : { code: templateId, name: this.nameForTemplate(templateId) };
  }

  listKnownItems(): { code: string; name: string }[] {
    return [...this.entriesByTemplateId.values()]
      .filter((entry) => !isInternalCatalogEntry(entry))
      .map((entry) => ({ code: entry.code, name: this.listNameForEntry(entry) }))
      .sort((a, b) => a.name.localeCompare(b.name) || a.code.localeCompare(b.code));
  }

  private listNameForEntry(entry: ConanItemCatalogEntry): string {
    return this.ambiguousDisplayNames.has(normalizeCatalogAlias(entry.name)) ? `${entry.name} (${entry.code})` : entry.name;
  }

  private addEntry(entry: ConanItemCatalogEntry, uniqueNames: Set<string>): void {
    this.entriesByTemplateId.set(entry.templateId, entry);
    for (const alias of aliasesForEntry(entry, uniqueNames)) {
      this.addAlias(alias, entry.templateId);
    }
  }

  private addAlias(alias: string, templateId: string): void {
    const normalized = normalizeCatalogAlias(alias);
    if (!normalized) return;
    const existing = this.templateIdByAlias.get(normalized);
    if (existing && existing !== templateId) {
      throw new Error(`Conan item catalog alias collision for '${alias}' (${existing} vs ${templateId})`);
    }
    this.templateIdByAlias.set(normalized, templateId);
  }
}

export function seedConanItemCatalog(): ConanItemCatalog {
  return new ConanItemCatalog(SEED_CATALOG_DOCUMENT);
}

export function loadConanItemCatalog(catalogPath: string | null | undefined): ConanItemCatalog {
  if (!catalogPath) return seedConanItemCatalog();
  const document = JSON.parse(fs.readFileSync(catalogPath, 'utf8')) as ConanItemCatalogDocument;
  if (document.version !== 1 || !Array.isArray(document.items)) {
    throw new Error(`Invalid Conan item catalog at ${catalogPath}`);
  }
  return new ConanItemCatalog(document, { hasExternalCatalog: true });
}

export function normalizeCatalogAlias(input: string): string {
  return input.trim().toLowerCase().replace(/[^a-z0-9]+/g, '');
}

function normalizeEntry(entry: ConanItemCatalogEntry): ConanItemCatalogEntry {
  return {
    ...entry,
    templateId: String(entry.templateId).trim(),
    code: String(entry.code).trim(),
    name: String(entry.name).trim(),
    aliases: entry.aliases?.map((alias) => String(alias).trim()).filter(Boolean),
    tags: entry.tags?.map((tag) => String(tag).trim()).filter(Boolean),
  };
}

function aliasesForEntry(entry: ConanItemCatalogEntry, uniqueNames: Set<string>): string[] {
  const aliases = [entry.templateId, entry.code, ...(entry.aliases ?? [])];
  if (uniqueNames.has(normalizeCatalogAlias(entry.name))) {
    aliases.push(entry.name);
  } else {
    aliases.push(`${entry.name} (${entry.code})`);
    if (entry.code !== entry.templateId) aliases.push(`${entry.name} (${entry.templateId})`);
  }
  return aliases;
}

function displayNameCounts(entries: ConanItemCatalogEntry[]): Map<string, number> {
  const templateIdsByName = new Map<string, Set<string>>();
  for (const entry of entries) {
    const normalized = normalizeCatalogAlias(entry.name);
    if (!normalized) continue;
    let templateIds = templateIdsByName.get(normalized);
    if (!templateIds) {
      templateIds = new Set<string>();
      templateIdsByName.set(normalized, templateIds);
    }
    templateIds.add(entry.templateId);
  }
  return new Map([...templateIdsByName.entries()].map(([name, templateIds]) => [name, templateIds.size]));
}

function isInternalCatalogEntry(entry: ConanItemCatalogEntry): boolean {
  return [entry.name, entry.code].some((value) => /^(XX_|DEV_)/i.test(value.trim()));
}
