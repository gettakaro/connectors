// The name of an entity template, as a player reads it.
//
// The dedicated server ships no localisation; kEntityLabels holds the game client's En_Us names,
// baked by tools/gen_entity_labels.py: a template's own name (a boss health bar, an NPC), the
// Logic Spawner entry that places it ("Rat", "Scavenger Scab"), the item that is it (a farm
// animal, a fish by its catch), or the name of the named templates that share its model, its code
// once spawn suffixes are dropped, or its arsenal. A template with none of these has no in-game
// name: listEntities leaves it out, and a kill or death names it by its faction's in-game word
// (kFactionLabels: Fell, Hollow, Scavenger, ...) or "Unknown". A name is never built from the code;
// the code travels beside it (entityCode, killerEntity).
//
// IsCatalogueEntity decides which templates listEntities answers with: creatures and NPCs,
// not the colliders, weak spots, traps, projectiles, props and dev templates that share the
// actor table. Kill attribution still resolves every template.
//
// Pure lookups over static tables: no Windows, no game memory. tests/entity_names_test.cpp
// links this on the host.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct EntityDef;

struct EntityLabel {
    uint8_t guid[16];    // templateGuid, RFC-4122 byte order (as EntityDef.guid)
    const char* name;    // En_Us display name from the game client
    const char* source;  // "client" | "logicSpawn" | "item" | "model" | "sibling" | "arsenal"
};

struct FactionLabel {
    const char* faction;  // EntityDef.faction
    const char* name;     // the client's word for it
};

extern const EntityLabel kEntityLabels[];  // sorted by guid
extern const size_t kEntityLabelCount;
extern const FactionLabel kFactionLabels[];
extern const size_t kFactionLabelCount;

// The client's name for a template, or nullptr when it has none.
const EntityLabel* EntityLabelByGuid(const uint8_t* guid);

struct EntityName {
    std::string name;    // never empty, never the code
    const char* source;  // a label source, or "faction" | "unknown"
};

// def may be nullptr (a template outside the table): "Unknown".
EntityName ResolveEntityName(const EntityDef* def);

// Whether a template code is a creature or NPC that belongs in listEntities.
bool IsCatalogueEntity(const char* code);

// What listEntities answers with: the catalogue templates that have an in-game name, in table
// order, each with exactly the name an entity-killed event for it carries (ResolveEntityName).
// Names repeat where the templates are the same creature (five Balthazar templates, one per
// upgrade; a Thunderbrute and the one a summoning stone calls up; nine looks of a Scavenger Scalper).
// Codes never repeat: Takaro keys entities by code, so a code shared by several templates is
// listed once (see CatalogueEntities in entity.cpp for which template's name it gets).
void CatalogueEntities(std::vector<const EntityDef*>& rows, std::vector<std::string>& names);
