export interface TerrariaItemCatalogEntry {
  code: string;
  name: string;
  amount: number;
  quality: string;
  aliases: string[];
}

export const TERRARIA_ITEM_CATALOG: readonly TerrariaItemCatalogEntry[] = [
  {
    "code": "1",
    "name": "Synth Plain",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthPlain"
    ]
  },
  {
    "code": "2",
    "name": "Synth Strip",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthStrip"
    ]
  },
  {
    "code": "3",
    "name": "Synth Snack",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthSnack"
    ]
  },
  {
    "code": "4",
    "name": "Synth Twin A",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthTwinA"
    ]
  },
  {
    "code": "5",
    "name": "Synth Twin B",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthTwinB"
    ]
  },
  {
    "code": "10",
    "name": "Alpha Chest",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "AlphaChest"
    ]
  },
  {
    "code": "11",
    "name": "Fake Alpha Chest",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "FakeAlphaChest"
    ]
  },
  {
    "code": "12",
    "name": "Fake Beta Chest",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "FakeBetaChest"
    ]
  },
  {
    "code": "13",
    "name": "Beta Chest",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "BetaChest"
    ]
  },
  {
    "code": "14",
    "name": "Fake Chest",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "FakeChest"
    ]
  },
  {
    "code": "15",
    "name": "Fake newchest1",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "Fakenewchest1"
    ]
  },
  {
    "code": "16",
    "name": "Synth Odd Format",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthOddFormat"
    ]
  },
  {
    "code": "17",
    "name": "Synth Glass",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthGlass"
    ]
  },
  {
    "code": "5644",
    "name": "Synth Orb",
    "amount": 1,
    "quality": "0",
    "aliases": [
      "SynthOrb"
    ]
  }
] as const;

export function listTerrariaItems(): TerrariaItemCatalogEntry[] {
  return TERRARIA_ITEM_CATALOG.map((item) => ({ ...item, aliases: [...item.aliases] }));
}
