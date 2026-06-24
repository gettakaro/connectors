import { createRequire } from 'node:module';
import { seedConanItemCatalog, type ConanItemCatalog } from './itemCatalog.js';

const require = createRequire(import.meta.url);

interface StatementSync {
  get(...params: unknown[]): Record<string, unknown> | undefined;
  all(...params: unknown[]): Record<string, unknown>[];
}

interface DatabaseSync {
  prepare(sql: string): StatementSync;
  close(): void;
}

type DatabaseSyncConstructor = new (path: string, options?: { readOnly?: boolean }) => DatabaseSync;

function databaseConstructor(): DatabaseSyncConstructor {
  return (require('node:sqlite') as { DatabaseSync: DatabaseSyncConstructor }).DatabaseSync;
}

export interface TakaroPosition {
  x: number;
  y: number;
  z: number;
  dimension?: string;
}

export interface TakaroItem {
  code: string;
  name: string;
  amount?: number;
  quality?: string;
  position?: {
    x: number;
    y: number;
  };
}

export interface TakaroEntity {
  code: string;
  name: string;
}

export interface TakaroLocation extends TakaroPosition {
  code: string;
  name: string;
}

interface CharacterRow {
  id: number;
  playerId: string;
  charName: string;
  user: string | null;
  platformId: string | null;
}

interface LocationRow {
  id: number;
  charName: string;
  map: string | null;
  x: number;
  y: number;
  z: number;
}

interface InventoryRow {
  itemId: number;
  invType: number;
  templateId: number;
  data: Uint8Array | Buffer | null;
}

export class ConanSaveDbReader {
  constructor(
    private readonly databasePath: string | null,
    readonly itemCatalog: ConanItemCatalog = seedConanItemCatalog(),
  ) {}

  isConfigured(): boolean {
    return Boolean(this.databasePath);
  }

  getPlayerLocation(identifier: string): TakaroPosition | null {
    return this.withDb((db) => {
      const character = this.findCharacter(db, identifier);
      if (!character) return null;
      const row = db
        .prepare(
          `select map, x, y, z
             from actor_position
            where id = ?
            limit 1`,
        )
        .get(character.id) as { map?: string; x?: number; y?: number; z?: number } | undefined;
      if (!row) return null;
      return {
        x: Number(row.x ?? 0),
        y: Number(row.y ?? 0),
        z: Number(row.z ?? 0),
        ...(row.map ? { dimension: String(row.map) } : {}),
      };
    });
  }

  listPlayerLocations(): TakaroLocation[] {
    return this.withDb((db) => {
      const rows = db
        .prepare(
          `select c.id, c.char_name as charName, p.map, p.x, p.y, p.z
             from characters c
             join actor_position p on p.id = c.id
            order by lower(c.char_name)`,
        )
        .all() as unknown as LocationRow[];
      return rows.map((row) => ({
        code: `player:${row.id}`,
        name: row.charName || `Player ${row.id}`,
        x: Number(row.x ?? 0),
        y: Number(row.y ?? 0),
        z: Number(row.z ?? 0),
        ...(row.map ? { dimension: String(row.map) } : {}),
      }));
    });
  }

  getPlayerInventory(identifier: string): TakaroItem[] {
    return this.withDb((db) => {
      const character = this.findCharacter(db, identifier);
      if (!character) return [];
      const rows = db
        .prepare(
          `select item_id as itemId, inv_type as invType, template_id as templateId, data
             from item_inventory
            where owner_id = ?
              and template_id is not null
            order by inv_type, item_id`,
        )
        .all(character.id) as unknown as InventoryRow[];

      return aggregateInventoryRows(rows, this.itemCatalog);
    });
  }

  listItems(): TakaroItem[] {
    if (this.itemCatalog.hasExternalCatalog) return this.itemCatalog.listKnownItems();

    return this.withDb((db) => {
      const rows = db
        .prepare(
          `select template_id as templateId, data
             from item_inventory
            where template_id is not null
            order by template_id`,
        )
        .all() as unknown as InventoryRow[];
      const discovered = new Map<string, TakaroItem>();
      for (const row of rows.filter(isTangibleInventoryRow)) {
        const item = this.itemCatalog.itemForTemplate(String(row.templateId));
        discovered.set(item.code, item);
      }
      for (const item of this.itemCatalog.listKnownItems()) {
        discovered.set(item.code, item);
      }
      return [...discovered.values()].sort((a, b) => a.name.localeCompare(b.name));
    });
  }

  listEntities(): TakaroEntity[] {
    return this.withDb((db) => {
      const rows = db
        .prepare(
          `select class as code
             from actor_position
            where class is not null
              and class != ''
            group by class
            order by class`,
        )
        .all() as Array<{ code: string }>;
      return rows.map((row) => ({
        code: row.code,
        name: displayEntityName(row.code),
      }));
    });
  }

  private withDb<T>(read: (db: DatabaseSync) => T): T {
    if (!this.databasePath) throw new Error('Conan save database path is not configured');
    const Db = databaseConstructor();
    const db = new Db(this.databasePath, { readOnly: true });
    try {
      return read(db);
    } finally {
      db.close();
    }
  }

  private findCharacter(db: DatabaseSync, identifier: string): CharacterRow | null {
    const normalized = normalizeIdentifier(identifier);
    const rows = db
      .prepare(
        `select c.id,
                c.playerId,
                c.char_name as charName,
                a.user,
                a.platformId
           from characters c
           left join account a on a.id = cast(c.playerId as integer)
          order by c.id`,
      )
      .all() as unknown as CharacterRow[];

    return (
      rows.find((row) =>
        [row.id, row.playerId, row.charName, row.user, row.platformId]
          .filter((value) => value != null && value !== '')
          .some((value) => normalizeIdentifier(String(value)) === normalized),
      ) ?? null
    );
  }
}

function normalizeIdentifier(value: string): string {
  const parts = value.trim().split(':');
  return (parts.length > 1 ? parts.slice(1).join(':') : value).trim().toLowerCase();
}

function displayEntityName(code: string): string {
  const lastPath = code.split('/').pop() || code;
  const lastObject = lastPath.split('.').pop() || lastPath;
  return lastObject.replace(/_C$/i, '').replace(/_/g, ' ') || code;
}

function aggregateInventoryRows(rows: InventoryRow[], itemCatalog: ConanItemCatalog): TakaroItem[] {
  const byCode = new Map<string, TakaroItem>();
  for (const row of rows.filter(isTangibleInventoryRow)) {
    const item = itemCatalog.itemForTemplate(String(row.templateId));
    const existing = byCode.get(item.code);
    if (existing) {
      existing.amount = (existing.amount ?? 0) + 1;
      continue;
    }
    byCode.set(item.code, {
      code: item.code,
      name: item.name,
      amount: 1,
      quality: '1',
      position: {
        x: Number(row.itemId ?? 0),
        y: Number(row.invType ?? 0),
      },
    });
  }
  return [...byCode.values()];
}

function isTangibleInventoryRow(row: InventoryRow): boolean {
  if (isInternalEquipmentTemplate(String(row.templateId))) return false;
  const text = blobText(row.data);
  if (/FeatItem|EmoteItem/i.test(text)) return false;
  if (/XX_Unarmed|Unarmed/i.test(text)) return false;
  return /GameItem/i.test(text);
}

function isInternalEquipmentTemplate(code: string): boolean {
  return code === '51204' || code === '51205';
}

function blobText(value: Uint8Array | Buffer | null): string {
  if (!value) return '';
  return Buffer.from(value).toString('latin1').replace(/[^\x20-\x7e]+/g, ' ');
}

export function conanItemName(code: string): string {
  const catalog = seedConanItemCatalog();
  const templateCode = catalog.resolveTemplateId(code);
  return catalog.nameForTemplate(templateCode);
}

export function conanItemCode(input: string): string {
  return seedConanItemCatalog().resolveTemplateId(input);
}

export function conanItemPublicCode(input: string): string {
  const catalog = seedConanItemCatalog();
  return catalog.publicCodeForTemplate(catalog.resolveTemplateId(input));
}
