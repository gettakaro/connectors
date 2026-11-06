#pragma once

#include "plugin_state.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class TakaroCombatCallbackKind
{
    PlayerDied,
    EntityDamaged,
    EntityDied,
    // Engine ability-system gameplay-cue multicasts. StarRupture applies damage through the
    // gameplay ability system, so these are the only reflected callbacks that carry the
    // instigator and effect causer of a hit on an enemy.
    EntityCue,
};

// Classifies only reflected functions verified in the pinned generated Server SDK.
std::optional<TakaroCombatCallbackKind> ClassifyTakaroCombatCallback(const std::string& functionFullName);

// Uses the game's stable identification code when available, falling back to the
// actor class for transient actors such as equipped weapon instances.
std::string SelectTakaroStableActorCode(
    const std::string& identificationCode, const std::string& actorClassName);

// Picks the weapon code for a damage observation. A projectile's source weapon wins,
// then the attacking pawn's equipped weapon, then the damage actor itself; blank when
// nothing is known so the correlator fails closed.
std::string SelectTakaroWeaponCode(
    const std::string& sourceWeaponCode, const std::string& equippedWeaponCode,
    const std::string& damageActorCode);

// The weapon class named by a killing damage tag such as
// "GameplayEffect.Damage.Instant.MiningTool" (-> "MiningTool"). Blank unless the tag is a
// damage tag with a trailing segment, so a non-damage tag never becomes a weapon.
std::string TakaroWeaponCodeFromDamageTag(const std::string& killingDamageTag);

// Bounded diagnostic logging: each kind gets a fixed number of lines per boot, then one
// suppression notice, so a busy fight cannot flood ModLoader.log.
class TakaroCombatLogBudget
{
public:
    explicit TakaroCombatLogBudget(size_t linesPerKind = 300);
    // True when the caller may log this kind now; false once the budget is spent.
    bool Allow(const std::string& kind);
    // True exactly once per kind, when the budget has just run out.
    bool JustExhausted(const std::string& kind) const;
    void Reset();

private:
    size_t linesPerKind_;
    std::map<std::string, size_t> used_;
};

struct IPluginHookUtils;

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
namespace SDK
{
class UObject;
class UFunction;
class AActor;
class ACrCharacterPlayerBase;
}

struct TakaroSdkPlayerDiedCallback
{
    SDK::ACrCharacterPlayerBase* player;
};

struct TakaroSdkEntityDamagedCallback
{
    SDK::UObject* victim;
    SDK::AActor* damageActor;
    double damage;
    const char* source;
};

struct TakaroSdkEntityDiedCallback
{
    SDK::UObject* victim;
    const char* source;
    std::string killingDamageTag;
};

struct TakaroSdkEntityCueCallback
{
    SDK::UObject* victim;
    SDK::AActor* instigator;
    SDK::AActor* effectCauser;
    SDK::UObject* sourceObject;
    double magnitude;
    std::string tag;
    const char* source;
    // True when the cue landed on a player character: logged as cue-on-player for
    // diagnostics only, never fed to the entity-killed correlator.
    bool playerVictim;
};

using TakaroSdkCombatCallback = std::variant<
    TakaroSdkPlayerDiedCallback, TakaroSdkEntityDamagedCallback, TakaroSdkEntityDiedCallback,
    TakaroSdkEntityCueCallback>;

// Decodes the authoritative server callbacks delivered by the shared ProcessEvent observer.
class TakaroCombatSdkDecoder
{
public:
    bool Initialize();
    std::optional<TakaroSdkCombatCallback> Decode(
        SDK::UObject* object, SDK::UFunction* function, const void* parameters) const;

private:
    SDK::UFunction* playerDied_ = nullptr;
    SDK::UFunction* serverDeathReaction_ = nullptr;
    SDK::UFunction* multicastDeathReaction_ = nullptr;
    SDK::UFunction* localDeathReaction_ = nullptr;
    SDK::UFunction* entityDamaged_ = nullptr;
    SDK::UFunction* entityDamageQueued_ = nullptr;
    SDK::UFunction* entityDied_ = nullptr;
    SDK::UFunction* entityDiedMulticast_ = nullptr;
    SDK::UFunction* entityDiedFromGas_ = nullptr;
    // Ability-system gameplay-cue multicasts, in the order of kCueFunctionNames.
    SDK::UFunction* cueFunctions_[8] = {};
    size_t cueFunctionsResolved_ = 0;
};

bool InitializeTakaroCombatEvents();
void HandleTakaroCombatProcessEvent(
    SDK::UObject* object, SDK::UFunction* function, const void* parameters);
bool InstallTakaroCombatHooks(IPluginHookUtils* hooks);
void RemoveTakaroCombatHooks(IPluginHookUtils* hooks);
void RecordTakaroPlayerDeathObservation(const struct TakaroPlayerDeathObservation& observation);
#endif

void ResetTakaroCombatEvents();

struct TakaroPlayerDeathObservation
{
    std::string victimInstanceKey;
    TakaroPlayerSnapshot player;
    std::optional<TakaroPlayerSnapshot> attacker;
    std::optional<TakaroPositionDto> position;
};

struct TakaroPlayerDeathEvent
{
    TakaroPlayerSnapshot player;
    std::optional<TakaroPlayerSnapshot> attacker;
    std::optional<TakaroPositionDto> position;
};

// Reconciles the replicated, server-owned pawn bDead state. The first sample
// establishes a baseline; only later alive-to-dead transitions emit.
class TakaroPlayerDeathStateTracker
{
public:
    std::vector<TakaroPlayerDeathObservation> Reconcile(
        const std::vector<TakaroPlayerSnapshot>& players,
        const std::map<std::string, bool>& deadStates,
        const std::map<std::string, TakaroPositionDto>& lastDeathPositions = {});
    void Reset();

private:
    std::map<std::string, bool> previous_;
    std::map<std::string, TakaroPositionDto> lastLivePositions_;
    std::map<std::string, TakaroPositionDto> previousDeathPositions_;
};

struct TakaroEntityDamageObservation
{
    std::string victimInstanceKey;
    std::string entityCode;
    std::optional<TakaroPlayerSnapshot> player;
    std::string weapon;
    double damage = 0;
};

struct TakaroEntityKilledEvent
{
    TakaroPlayerSnapshot player;
    std::string entity;
    std::string weapon;
};

// Why an entity death did or did not become an entity-killed event.
enum class TakaroEntityDeathOutcome
{
    Invalid,
    Duplicate,
    NoDamageRecord,
    Expired,
    EntityMismatch,
    // A player was credited but neither the damage record nor the killing tag named a weapon.
    NoWeapon,
    Matched,
};

const char* TakaroEntityDeathOutcomeName(TakaroEntityDeathOutcome outcome);

struct TakaroEntityDeathResult
{
    std::optional<TakaroEntityKilledEvent> event;
    TakaroEntityDeathOutcome outcome = TakaroEntityDeathOutcome::Invalid;
};

struct TakaroDamageSourceNode
{
    std::string actorKey;
    std::string ownerKey;
    std::string instigatorKey;
    std::optional<TakaroPlayerSnapshot> player;
    std::optional<std::string> weapon;
};

struct TakaroDamageSource
{
    TakaroPlayerSnapshot player;
    std::string weapon;
};

using TakaroDamageSourceLookup =
    std::function<std::optional<TakaroDamageSourceNode>(const std::string& actorKey)>;

// Traverses an observed damage actor's owner/instigator graph. Conflicting or incomplete
// attribution fails closed instead of guessing a player or weapon.
std::optional<TakaroDamageSource> ResolveTakaroDamageSource(
    const std::string& damageActorKey, const TakaroDamageSourceLookup& lookup, size_t maxNodes = 16);

class TakaroCombatCorrelator
{
public:
    using Clock = std::chrono::steady_clock;

    explicit TakaroCombatCorrelator(
        std::chrono::milliseconds finalDamagerWindow = std::chrono::seconds(5),
        std::chrono::milliseconds deathDedupeWindow = std::chrono::seconds(2),
        size_t maxRecords = 512);

    std::optional<TakaroPlayerDeathEvent> ObservePlayerDeath(
        const TakaroPlayerDeathObservation& observation, Clock::time_point now = Clock::now());
    void ObserveEntityDamage(
        const TakaroEntityDamageObservation& observation, Clock::time_point now = Clock::now());
    std::optional<TakaroEntityKilledEvent> ObserveEntityDeath(
        const std::string& victimInstanceKey, const std::string& entityCode,
        Clock::time_point now = Clock::now());
    // Same as ObserveEntityDeath, and says why nothing was emitted. A second death
    // callback for the same victim inside the dedupe window is reported as Duplicate,
    // which is what Multicast_OnAiDied following OnAiDied looks like.
    // fallbackWeapon (the killing damage tag's weapon) is used when the damage record
    // resolved a player but no weapon actor; without either, nothing is emitted.
    TakaroEntityDeathResult ObserveEntityDeathDetailed(
        const std::string& victimInstanceKey, const std::string& entityCode,
        Clock::time_point now = Clock::now(), const std::string& fallbackWeapon = {});
    void Reset();

private:
    struct DamageRecord
    {
        std::string entityCode;
        TakaroPlayerSnapshot player;
        std::string weapon;
        Clock::time_point observedAt;
    };

    void Prune(Clock::time_point now);

    std::chrono::milliseconds finalDamagerWindow_;
    std::chrono::milliseconds deathDedupeWindow_;
    size_t maxRecords_;
    std::map<std::string, DamageRecord> damageByVictim_;
    std::map<std::string, Clock::time_point> playerDeaths_;
    std::map<std::string, Clock::time_point> entityDeaths_;
};
