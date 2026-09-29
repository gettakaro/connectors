// The name of an entity template, as a player reads it.
//
// The game client names a template only where it shows a name: a boss or mini-boss health
// bar ("Fell Thunderbrute") and an NPC ("Oswald Anders"). kEntityLabels holds those En_Us
// names, baked by tools/gen_entity_labels.py (the dedicated server ships no localisation).
// A regular creature has no display name anywhere in the client; it gets the code-derived
// name from names.cpp (Enemy_Wildbeast_Rat_hook -> "Rat"), and the raw code is the last
// resort.
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
    const char* source;  // "client": the template's own name; "sibling": a named variant's;
                         // "fishItem": a fish, named as its raw-fish item
};

extern const EntityLabel kEntityLabels[];  // sorted by guid
extern const size_t kEntityLabelCount;

// The client's name for a template, or nullptr when it has none.
const EntityLabel* EntityLabelByGuid(const uint8_t* guid);

struct EntityName {
    std::string name;    // never empty
    const char* source;  // "client" | "sibling" | "fishItem" | "code" | "raw"
};

// guid may be nullptr; code is the template code (nullptr or "" when the template is unknown,
// in which case `raw` is what the caller has, e.g. "entity#1234").
EntityName ResolveEntityName(const uint8_t* guid, const char* code, const std::string& raw = "");

// Whether a template code is a creature or NPC that belongs in listEntities.
bool IsCatalogueEntity(const char* code);

// What listEntities answers with: the catalogue templates in table order, each with exactly the
// name an entity-killed event for it carries (ResolveEntityName). Names repeat where the
// templates are the same creature (four Balthazar templates, one per upgrade; a Heavy and the
// Heavy a summoning stone calls up).
void CatalogueEntities(std::vector<const EntityDef*>& rows, std::vector<std::string>& names);
