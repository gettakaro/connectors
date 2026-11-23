/**
 * Types for `gen-catalogue.mjs`, so the sidecar's test suite can import the generator's real predicates instead of
 * re-implementing them. The generator itself stays dependency-free plain ESM — it has to run from a bare `node` on
 * an operator's box.
 */

export interface CatalogueItem {
  /** `items.template_id`, and what `AddItemToInventory.ItemName` takes. */
  code: string;
  /** The player-facing display name. Never an asset id. */
  name: string;
  description?: string;
}

export interface CatalogueEntity {
  code: string;
  name: string;
  type: 'hostile' | 'friendly' | 'neutral';
  description?: string;
}

export interface DroppedCounts {
  noCode: number;
  noName: number;
  devName: number;
  duplicate: number;
}

/** True when `name` looks like an asset id rather than something a player reads. */
export function isDevName(name: unknown, code?: string): boolean;

/** Flattens MediaWiki markup and collapses whitespace; returns undefined for nothing useful. */
export function fixMojibake<T>(value: T): T;
export function cleanText(value: unknown): string | undefined;

export function buildItems(raw: unknown[]): {
  rows: CatalogueItem[];
  dropped: DroppedCounts;
  droppedExamples: { code: string; name: string; why: string }[];
};

export function buildEntities(): CatalogueEntity[];

/** Returns a human-readable problem per offending row; an empty array means the catalogue may be written. */
export function validate(rows: { code: string; name: string }[], label: string): string[];
