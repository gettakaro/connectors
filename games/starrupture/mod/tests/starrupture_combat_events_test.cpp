#include "starrupture_combat_events.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <string>

namespace
{
TakaroPlayerSnapshot Player(const std::string& gameId)
{
    TakaroPlayerSnapshot player;
    player.gameId = gameId;
    player.name = "Observed " + gameId;
    player.platformId = "starrupture:" + gameId;
    return player;
}
}

int main()
{
    assert(ClassifyTakaroCombatCallback("Function Chimera.CrCharacterPlayerBase.OnDied") ==
           TakaroCombatCallbackKind::PlayerDied);
    assert(ClassifyTakaroCombatCallback("Function Chimera.CrDeathReactionComponent.ServerOnDeathEvent") ==
           TakaroCombatCallbackKind::PlayerDied);
    assert(ClassifyTakaroCombatCallback("Function Chimera.CrDeathReactionComponent.MulticastOnDeathEvent") ==
           TakaroCombatCallbackKind::PlayerDied);
    assert(ClassifyTakaroCombatCallback("Function Chimera.CrDeathReactionComponent.OnDeathEvent") ==
           TakaroCombatCallbackKind::PlayerDied);
    assert(!ClassifyTakaroCombatCallback("Function Chimera.CrPlayerControllerBase.ClientOnDied"));
    assert(ClassifyTakaroCombatCallback("Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnDamage") ==
        TakaroCombatCallbackKind::EntityDamaged);
    assert(ClassifyTakaroCombatCallback(
               "Function MassAIPrototypeEnemyRuntime.MassEnemyEventQueueSubsystem.DealDamageToAiActor") ==
        TakaroCombatCallbackKind::EntityDamaged);
    assert(ClassifyTakaroCombatCallback("Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnAiDied") ==
        TakaroCombatCallbackKind::EntityDied);
    assert(ClassifyTakaroCombatCallback(
               "Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.Multicast_OnAiDied") ==
        TakaroCombatCallbackKind::EntityDied);
    assert(ClassifyTakaroCombatCallback("Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnDiedFromGas") ==
        TakaroCombatCallbackKind::EntityDied);
    assert(!ClassifyTakaroCombatCallback("Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnStartDeathSequence"));
    // Ability-system gameplay-cue multicasts carry the instigator of a GAS hit.
    assert(ClassifyTakaroCombatCallback(
               "Function GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCueExecuted_WithParams") ==
        TakaroCombatCallbackKind::EntityCue);
    assert(ClassifyTakaroCombatCallback(
               "Function GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCueExecuted_FromSpec") ==
        TakaroCombatCallbackKind::EntityCue);
    assert(ClassifyTakaroCombatCallback(
               "Function GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCuesAddedAndWhileActive_WithParams") ==
        TakaroCombatCallbackKind::EntityCue);
    assert(!ClassifyTakaroCombatCallback("Function GameplayAbilities.AbilitySystemComponent.ExecuteGameplayCueLocal"));
    assert(!ClassifyTakaroCombatCallback("Function GameplayAbilities.AbilitySystemComponent.NetMulticast_InvokeGameplayCueExecuted_Bogus"));

    // The killing damage tag names the weapon class of the final blow.
    assert(TakaroWeaponCodeFromDamageTag("GameplayEffect.Damage.Instant.MiningTool") == "MiningTool");
    assert(TakaroWeaponCodeFromDamageTag("GameplayEffect.Damage.Explosion") == "Explosion");
    assert(TakaroWeaponCodeFromDamageTag("GameplayEffect.Damage.Instant.").empty());
    assert(TakaroWeaponCodeFromDamageTag("GameplayCue.Enemy.Hit").empty());
    assert(TakaroWeaponCodeFromDamageTag("").empty());
    assert(std::string(TakaroEntityDeathOutcomeName(TakaroEntityDeathOutcome::NoWeapon)) == "no-weapon");

    // Weapon precedence: projectile source weapon, then the pawn's equipped weapon, then the actor itself.
    assert(SelectTakaroWeaponCode("weapon.rocket", "weapon.rifle", "BP_Rocket_C") == "weapon.rocket");
    assert(SelectTakaroWeaponCode("", "weapon.rifle", "BP_Player_C") == "weapon.rifle");
    assert(SelectTakaroWeaponCode("", "", "BP_Grenade_C") == "BP_Grenade_C");
    assert(SelectTakaroWeaponCode("  ", "", "   ").empty());

    // Diagnostics budget: a fixed number of lines per kind, then exactly one exhaustion notice.
    TakaroCombatLogBudget budget(2);
    assert(budget.Allow("damage"));
    assert(budget.Allow("damage"));
    assert(!budget.JustExhausted("damage"));
    assert(!budget.Allow("damage"));
    assert(budget.JustExhausted("damage"));
    assert(!budget.Allow("damage"));
    assert(!budget.JustExhausted("damage"));
    assert(budget.Allow("death"));
    budget.Reset();
    assert(budget.Allow("damage"));
    assert(std::string(TakaroEntityDeathOutcomeName(TakaroEntityDeathOutcome::NoDamageRecord)) == "no-damage-record");
    assert(!ClassifyTakaroCombatCallback("Function Chimera.CrCharacterPlayerBase.OnRespawned"));
    assert(SelectTakaroStableActorCode("weapon.rifle.basic", "BP_Rifle_C") == "weapon.rifle.basic");
    assert(SelectTakaroStableActorCode("", "BP_Rifle_C") == "BP_Rifle_C");
    assert(SelectTakaroStableActorCode("   ", "BP_Rifle_C") == "BP_Rifle_C");
    assert(SelectTakaroStableActorCode("", "").empty());

    using Clock = TakaroCombatCorrelator::Clock;
    const auto start = Clock::time_point{};

    TakaroPlayerDeathStateTracker deathStates;
    auto alive = Player("state-player");
    alive.hasLocation = true;
    alive.x = 10;
    alive.y = 20;
    alive.z = 30;
    assert(deathStates.Reconcile({alive}, {{alive.gameId, false}}).empty());
    auto deadWithoutLocation = alive;
    deadWithoutLocation.hasLocation = false;
    const auto stateDeath = deathStates.Reconcile({deadWithoutLocation}, {{alive.gameId, true}});
    assert(stateDeath.size() == 1);
    assert(stateDeath[0].player.gameId == alive.gameId);
    assert(stateDeath[0].position && stateDeath[0].position->x == 10 && stateDeath[0].position->z == 30);
    assert(deathStates.Reconcile({deadWithoutLocation}, {{alive.gameId, true}}).empty());
    assert(deathStates.Reconcile({alive}, {{alive.gameId, false}}).empty());
    assert(deathStates.Reconcile({deadWithoutLocation}, {{alive.gameId, true}}).size() == 1);
    deathStates.Reset();
    assert(deathStates.Reconcile({deadWithoutLocation}, {{alive.gameId, true}}).empty());
    const TakaroPositionDto firstDeathPosition{100, 200, 300, std::nullopt};
    const TakaroPositionDto nextDeathPosition{110, 200, 300, std::nullopt};
    deathStates.Reset();
    assert(deathStates.Reconcile({alive}, {{alive.gameId, false}}, {{alive.gameId, firstDeathPosition}}).empty());
    const auto transformedDeath = deathStates.Reconcile(
        {alive}, {{alive.gameId, false}}, {{alive.gameId, nextDeathPosition}});
    assert(transformedDeath.size() == 1);
    assert(transformedDeath[0].position && transformedDeath[0].position->x == 110);
    assert(deathStates.Reconcile(
        {alive}, {{alive.gameId, false}}, {{alive.gameId, nextDeathPosition}}).empty());

    // One death trips the dead flag and the last-death position on different ticks: one event.
    deathStates.Reset();
    assert(deathStates.Reconcile({alive}, {{alive.gameId, false}}, {{alive.gameId, firstDeathPosition}}).empty());
    assert(deathStates.Reconcile({alive}, {{alive.gameId, true}}, {{alive.gameId, firstDeathPosition}}).empty());
    assert(deathStates.Reconcile({alive}, {{alive.gameId, true}}, {{alive.gameId, nextDeathPosition}}).size() == 1);
    assert(deathStates.Reconcile({alive}, {{alive.gameId, false}}, {{alive.gameId, nextDeathPosition}}).empty());
    assert(deathStates.Reconcile({alive}, {{alive.gameId, true}}, {{alive.gameId, nextDeathPosition}}).empty());
    // A save loaded at join is a baseline, not a death.
    deathStates.Reset();
    assert(deathStates.Reconcile({alive}, {{alive.gameId, true}}, {{alive.gameId, firstDeathPosition}}).empty());

    TakaroCombatCorrelator combat(std::chrono::seconds(5), std::chrono::seconds(2), 4);
    TakaroPlayerDeathObservation fall;
    fall.victimInstanceKey = "player-pawn-1";
    fall.player = Player("player-1");
    fall.position = TakaroPositionDto{101.25, -22.5, 903.0, std::string("world")};

    const auto firstDeath = combat.ObservePlayerDeath(fall, start);
    assert(firstDeath);
    assert(firstDeath->player.gameId == "player-1");
    assert(!firstDeath->attacker);
    assert(firstDeath->position);
    assert(firstDeath->position->x == 101.25);
    assert(firstDeath->position->y == -22.5);
    assert(firstDeath->position->z == 903.0);
    assert(firstDeath->position->dimension == "world");

    assert(!combat.ObservePlayerDeath(fall, start + std::chrono::milliseconds(50)));
    assert(combat.ObservePlayerDeath(fall, start + std::chrono::seconds(3)));

    TakaroPlayerDeathObservation pvp = fall;
    pvp.victimInstanceKey = "player-pawn-2";
    pvp.attacker = Player("player-2");
    const auto pvpDeath = combat.ObservePlayerDeath(pvp, start);
    assert(pvpDeath && pvpDeath->attacker);
    assert(pvpDeath->attacker->gameId == "player-2");

    TakaroPlayerDeathObservation incompleteDeath = fall;
    incompleteDeath.victimInstanceKey.clear();
    assert(!combat.ObservePlayerDeath(incompleteDeath, start));
    incompleteDeath = fall;
    incompleteDeath.victimInstanceKey = "player-pawn-incomplete";
    incompleteDeath.player.name = "   ";
    assert(!combat.ObservePlayerDeath(incompleteDeath, start));
    incompleteDeath = fall;
    incompleteDeath.position->x = std::nan("");
    assert(!combat.ObservePlayerDeath(incompleteDeath, start));

    TakaroEntityDamageObservation direct;
    direct.victimInstanceKey = "enemy-instance-1";
    direct.entityCode = "enemy.melee.tier1";
    direct.player = Player("player-1");
    direct.weapon = "weapon.rifle.basic";
    direct.damage = 25.0;
    combat.ObserveEntityDamage(direct, start);

    const auto directKill = combat.ObserveEntityDeath(
        "enemy-instance-1", "enemy.melee.tier1", start + std::chrono::seconds(1));
    assert(directKill);
    assert(directKill->player.gameId == "player-1");
    assert(directKill->entity == "enemy.melee.tier1");
    assert(directKill->weapon == "weapon.rifle.basic");
    assert(!combat.ObserveEntityDeath(
        "enemy-instance-1", "enemy.melee.tier1", start + std::chrono::seconds(1)));

    direct.victimInstanceKey = "enemy-instance-2";
    combat.ObserveEntityDamage(direct, start);
    assert(!combat.ObserveEntityDeath(
        "enemy-instance-2", "enemy.melee.tier1", start + std::chrono::seconds(6)));

    direct.victimInstanceKey = "enemy-instance-3";
    combat.ObserveEntityDamage(direct, start);
    assert(!combat.ObserveEntityDeath(
        "enemy-instance-3", "enemy.ranged.tier1", start + std::chrono::seconds(1)));

    direct.victimInstanceKey = "enemy-instance-4";
    combat.ObserveEntityDamage(direct, start);
    auto unresolvedFinalDamage = direct;
    unresolvedFinalDamage.player.reset();
    unresolvedFinalDamage.weapon.clear();
    combat.ObserveEntityDamage(unresolvedFinalDamage, start + std::chrono::milliseconds(500));
    assert(!combat.ObserveEntityDeath(
        "enemy-instance-4", "enemy.melee.tier1", start + std::chrono::seconds(1)));

    // Detailed outcomes name why a death did not become a kill.
    TakaroCombatCorrelator detailed(std::chrono::seconds(5), std::chrono::seconds(2), 8);
    assert(detailed.ObserveEntityDeathDetailed("", "enemy.crab", start).outcome == TakaroEntityDeathOutcome::Invalid);
    assert(detailed.ObserveEntityDeathDetailed("crab-1", "enemy.crab", start).outcome ==
        TakaroEntityDeathOutcome::NoDamageRecord);
    // The same victim again inside the dedupe window is a duplicate (Multicast_OnAiDied after OnAiDied).
    assert(detailed.ObserveEntityDeathDetailed("crab-1", "enemy.crab", start + std::chrono::milliseconds(100)).outcome ==
        TakaroEntityDeathOutcome::Duplicate);
    TakaroEntityDamageObservation crabHit = direct;
    crabHit.victimInstanceKey = "crab-2";
    crabHit.entityCode = "enemy.crab";
    detailed.ObserveEntityDamage(crabHit, start);
    const auto matched = detailed.ObserveEntityDeathDetailed("crab-2", "enemy.crab", start + std::chrono::seconds(1));
    assert(matched.outcome == TakaroEntityDeathOutcome::Matched);
    assert(matched.event && matched.event->player.gameId == "player-1" && matched.event->weapon == "weapon.rifle.basic");
    assert(detailed.ObserveEntityDeathDetailed("crab-2", "enemy.crab", start + std::chrono::seconds(1)).outcome ==
        TakaroEntityDeathOutcome::Duplicate);
    // Outside the dedupe window the same key is a fresh death with no record.
    assert(detailed.ObserveEntityDeathDetailed("crab-2", "enemy.crab", start + std::chrono::seconds(4)).outcome ==
        TakaroEntityDeathOutcome::NoDamageRecord);
    crabHit.victimInstanceKey = "crab-3";
    detailed.ObserveEntityDamage(crabHit, start);
    assert(detailed.ObserveEntityDeathDetailed("crab-3", "enemy.crab", start + std::chrono::seconds(6)).outcome ==
        TakaroEntityDeathOutcome::Expired);
    crabHit.victimInstanceKey = "crab-4";
    detailed.ObserveEntityDamage(crabHit, start);
    assert(detailed.ObserveEntityDeathDetailed("crab-4", "enemy.panzerhund", start + std::chrono::seconds(1)).outcome ==
        TakaroEntityDeathOutcome::EntityMismatch);
    // Unresolved attribution leaves no record, so an environmental or turret kill reports no-damage-record.
    TakaroEntityDamageObservation turretHit = crabHit;
    turretHit.victimInstanceKey = "crab-5";
    turretHit.player.reset();
    turretHit.weapon = "BP_RifleTurret_C";
    detailed.ObserveEntityDamage(turretHit, start);
    assert(detailed.ObserveEntityDeathDetailed("crab-5", "enemy.crab", start + std::chrono::seconds(1)).outcome ==
        TakaroEntityDeathOutcome::NoDamageRecord);

    // A cue that resolved the player but no weapon actor still emits when the killing tag names the weapon.
    TakaroCombatCorrelator cueKills(std::chrono::seconds(5), std::chrono::seconds(2), 8);
    TakaroEntityDamageObservation cueHit;
    cueHit.victimInstanceKey = "melee-1";
    cueHit.entityCode = "enemy.melee.tier1";
    cueHit.player = Player("player-1");
    cueHit.weapon.clear();
    cueHit.damage = 1.0;
    cueKills.ObserveEntityDamage(cueHit, start);
    const auto tagKill = cueKills.ObserveEntityDeathDetailed(
        "melee-1", "enemy.melee.tier1", start + std::chrono::seconds(1), "MiningTool");
    assert(tagKill.outcome == TakaroEntityDeathOutcome::Matched);
    assert(tagKill.event && tagKill.event->player.gameId == "player-1" && tagKill.event->weapon == "MiningTool");
    // Without a weapon from either source nothing is emitted, and the reason says so.
    cueHit.victimInstanceKey = "melee-2";
    cueKills.ObserveEntityDamage(cueHit, start);
    assert(cueKills.ObserveEntityDeathDetailed("melee-2", "enemy.melee.tier1", start + std::chrono::seconds(1), "").outcome ==
        TakaroEntityDeathOutcome::NoWeapon);
    // A weapon actor on the record beats the tag fallback.
    cueHit.victimInstanceKey = "melee-3";
    cueHit.weapon = "weapon.rifle.basic";
    cueKills.ObserveEntityDamage(cueHit, start);
    const auto actorKill = cueKills.ObserveEntityDeathDetailed(
        "melee-3", "enemy.melee.tier1", start + std::chrono::seconds(1), "StandardAmmo");
    assert(actorKill.event && actorKill.event->weapon == "weapon.rifle.basic");
    // A cue with no resolvable player still leaves no record.
    cueHit.victimInstanceKey = "melee-4";
    cueHit.player.reset();
    cueKills.ObserveEntityDamage(cueHit, start);
    assert(cueKills.ObserveEntityDeathDetailed("melee-4", "enemy.melee.tier1", start + std::chrono::seconds(1), "MiningTool").outcome ==
        TakaroEntityDeathOutcome::NoDamageRecord);

    std::map<std::string, TakaroDamageSourceNode> graph;
    graph["projectile"] = TakaroDamageSourceNode{"projectile", "weapon-actor", "player-pawn", std::nullopt,
        std::string("weapon.rocket.basic")};
    graph["weapon-actor"] = TakaroDamageSourceNode{"weapon-actor", {}, "player-pawn", std::nullopt,
        std::string("weapon.rocket.basic")};
    graph["player-pawn"] = TakaroDamageSourceNode{"player-pawn", {}, {}, Player("player-1"), std::nullopt};
    const auto projectile = ResolveTakaroDamageSource("projectile", [&](const std::string& key) {
        const auto found = graph.find(key);
        return found == graph.end() ? std::optional<TakaroDamageSourceNode>{} : found->second;
    });
    assert(projectile);
    assert(projectile->player.gameId == "player-1");
    assert(projectile->weapon == "weapon.rocket.basic");

    graph["player-pawn"].ownerKey = "projectile";
    assert(ResolveTakaroDamageSource("projectile", [&](const std::string& key) {
        const auto found = graph.find(key);
        return found == graph.end() ? std::optional<TakaroDamageSourceNode>{} : found->second;
    }));

    graph["other-player"] = TakaroDamageSourceNode{"other-player", {}, {}, Player("player-2"), std::nullopt};
    graph["weapon-actor"].ownerKey = "other-player";
    assert(!ResolveTakaroDamageSource("projectile", [&](const std::string& key) {
        const auto found = graph.find(key);
        return found == graph.end() ? std::optional<TakaroDamageSourceNode>{} : found->second;
    }));

    graph["weapon-actor"].ownerKey.clear();
    graph["projectile"].weapon.reset();
    graph["weapon-actor"].weapon.reset();
    assert(!ResolveTakaroDamageSource("projectile", [&](const std::string& key) {
        const auto found = graph.find(key);
        return found == graph.end() ? std::optional<TakaroDamageSourceNode>{} : found->second;
    }));

    std::cout << "starrupture_combat_events_test: passed\n";
    return 0;
}
