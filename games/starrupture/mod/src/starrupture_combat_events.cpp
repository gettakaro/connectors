#include "starrupture_combat_events.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <set>
#include <utility>

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#include "SDK/Chimera_parameters.hpp"
#include "SDK/GameplayAbilities_parameters.hpp"
#include "SDK/MassAIPrototypeEnemyRuntime_parameters.hpp"
#include "modloader_process_events.h"
#include "plugin_interface.h"
#include "plugin_helpers.h"
#include "starrupture_sdk_bridge.h"
#endif

namespace
{
// Engine ability-system gameplay-cue multicasts, verified in the pinned Server SDK
// (GameplayAbilities_classes.hpp). Order matches TakaroCombatSdkDecoder::cueFunctions_.
constexpr const char* kTakaroCueFunctionNames[8] = {
    "NetMulticast_InvokeGameplayCueExecuted",
    "NetMulticast_InvokeGameplayCueExecuted_WithParams",
    "NetMulticast_InvokeGameplayCueExecuted_FromSpec",
    "NetMulticast_InvokeGameplayCueAdded",
    "NetMulticast_InvokeGameplayCueAdded_WithParams",
    "NetMulticast_InvokeGameplayCueAddedAndWhileActive_FromSpec",
    "NetMulticast_InvokeGameplayCueAddedAndWhileActive_WithParams",
    "NetMulticast_InvokeGameplayCuesAddedAndWhileActive_WithParams",
};

bool NonBlank(const std::string& value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char character) { return !std::isspace(character); });
}

bool ValidPlayer(const TakaroPlayerSnapshot& player)
{
    return NonBlank(player.gameId) && NonBlank(player.name) && NonBlank(player.platformId) && player.ping >= 0;
}

bool ValidPosition(const TakaroPositionDto& position)
{
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

template <typename Map>
void EraseOldest(Map& values)
{
    if (values.empty()) return;
    const auto oldest = std::min_element(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.second.observedAt < right.second.observedAt;
    });
    values.erase(oldest);
}

void EraseOldestDeath(std::map<std::string, TakaroCombatCorrelator::Clock::time_point>& values)
{
    if (values.empty()) return;
    const auto oldest = std::min_element(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });
    values.erase(oldest);
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
TakaroCombatSdkDecoder g_decoder;
TakaroCombatCorrelator g_combat;
TakaroCombatLogBudget g_combatLog;
bool g_decoderReady = false;
bool g_combatSubscribed = false;

// Diagnostic lines carry class and function names only, never player identifiers.
constexpr const char* kCombatLogPrefix = "[takaro-combat]";

std::string ClassNameOf(SDK::UObject* object)
{
    return object && object->Class ? object->Class->GetName() : std::string("<null>");
}

void CombatLog(const std::string& kind, const std::string& line)
{
    if (g_combatLog.Allow(kind))
    {
        TAKARO_LOG_INFO("%s %s", kCombatLogPrefix, line.c_str());
    }
    else if (g_combatLog.JustExhausted(kind))
    {
        TAKARO_LOG_INFO("%s %s diagnostics budget spent for this boot; further lines suppressed",
            kCombatLogPrefix, kind.c_str());
    }
}

std::string TagNameOf(const SDK::FGameplayTag& tag)
{
    return tag.TagName.IsNone() ? std::string{} : tag.TagName.ToString();
}

std::optional<TakaroPlayerSnapshot> PlayerFromEquippedWeapon(SDK::AActor* source)
{
    auto* world = SDK::UWorld::GetWorld();
    auto* gameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!source || !gameState) return std::nullopt;

    std::map<std::string, TakaroPlayerSnapshot> players;
    for (auto* basePlayerState : gameState->PlayerArray)
    {
        if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) continue;
        auto* controller = static_cast<SDK::ACrPlayerStateBase*>(basePlayerState)->GetPlayerController();
        auto* pawn = controller ? controller->K2_GetPawn() : nullptr;
        if (!controller || !pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass())) continue;
        auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);
        auto* weaponSystem = character->BP_GetWeaponComponent();
        if (!weaponSystem || weaponSystem->GetEquippedWeaponActor() != source) continue;
        if (const auto player = TakaroPlayerFromController(controller))
            players.insert_or_assign(player->gameId, *player);
    }
    if (players.size() != 1) return std::nullopt;
    return players.begin()->second;
}

std::optional<TakaroPlayerSnapshot> PlayerFromActor(SDK::AActor* source)
{
    if (!source) return std::nullopt;
    std::deque<SDK::AActor*> pending{source};
    std::set<SDK::AActor*> visited;
    std::map<std::string, TakaroPlayerSnapshot> players;
    while (!pending.empty() && visited.size() < 16)
    {
        auto* actor = pending.front();
        pending.pop_front();
        if (!actor || !visited.insert(actor).second) continue;
        SDK::APlayerController* playerController = nullptr;
        if (actor->IsA(SDK::APlayerController::StaticClass()))
            playerController = static_cast<SDK::APlayerController*>(actor);
        else if (actor->IsA(SDK::APawn::StaticClass()))
        {
            auto* controller = static_cast<SDK::APawn*>(actor)->GetController();
            if (controller && controller->IsA(SDK::APlayerController::StaticClass()))
                playerController = static_cast<SDK::APlayerController*>(controller);
        }
        if (!playerController)
        {
            auto* controller = actor->GetInstigatorController();
            if (controller && controller->IsA(SDK::APlayerController::StaticClass()))
                playerController = static_cast<SDK::APlayerController*>(controller);
        }
        if (playerController)
            if (const auto player = TakaroPlayerFromController(playerController))
                players.insert_or_assign(player->gameId, *player);
        if (auto* owner = actor->GetOwner()) pending.push_back(owner);
        if (auto* instigator = actor->GetInstigator()) pending.push_back(instigator);
    }
    if (!pending.empty() || players.size() > 1) return std::nullopt;
    if (players.empty()) return PlayerFromEquippedWeapon(source);
    return players.begin()->second;
}

std::string StableActorCode(SDK::AActor* actor)
{
    if (!actor) return {};
    SDK::FName name;
    const auto identificationCode =
        SDK::UIdentificationLibrary::GetActorUniqueName(actor, &name) && !name.IsNone()
        ? name.ToString()
        : std::string{};
    const auto actorClassName = actor->Class ? actor->Class->GetName() : std::string{};
    return SelectTakaroStableActorCode(identificationCode, actorClassName);
}

// The handle wraps a TSharedPtr whose first word is the context pointer; the context is
// polymorphic (vtable at 0) with the reflected fields after it. Both layouts come from the
// pinned dump and are pinned here so an SDK move fails to compile instead of misreading.
static_assert(sizeof(SDK::FGameplayEffectContextHandle) == 0x18, "FGameplayEffectContextHandle layout moved");
static_assert(sizeof(SDK::FGameplayEffectContext) == 0x80, "FGameplayEffectContext layout moved");
static_assert(offsetof(SDK::FGameplayEffectContext, Instigator) == 0x08, "FGameplayEffectContext::Instigator moved");
static_assert(offsetof(SDK::FGameplayEffectContext, EffectCauser) == 0x10, "FGameplayEffectContext::EffectCauser moved");
static_assert(offsetof(SDK::FGameplayEffectContext, SourceObject) == 0x2C, "FGameplayEffectContext::SourceObject moved");
static_assert(sizeof(SDK::FGameplayCueParameters) == 0xD8, "FGameplayCueParameters layout moved");
static_assert(offsetof(SDK::FGameplayCueParameters, Instigator) == 0xA0, "FGameplayCueParameters::Instigator moved");
static_assert(sizeof(SDK::FGameplayEffectSpecForRPC) == 0x78, "FGameplayEffectSpecForRPC layout moved");

const SDK::FGameplayEffectContext* ContextFromHandle(const SDK::FGameplayEffectContextHandle& handle)
{
    const SDK::FGameplayEffectContext* context = nullptr;
    std::memcpy(&context, &handle, sizeof(context));
    return context;
}

struct TakaroCueAttribution
{
    SDK::AActor* instigator = nullptr;
    SDK::AActor* effectCauser = nullptr;
    SDK::UObject* sourceObject = nullptr;
};

void FillFromContext(const SDK::FGameplayEffectContext* context, TakaroCueAttribution& out)
{
    if (!context) return;
    if (!out.instigator) out.instigator = context->Instigator.Get();
    if (!out.effectCauser) out.effectCauser = context->EffectCauser.Get();
    if (!out.sourceObject) out.sourceObject = context->SourceObject.Get();
}

void FillFromCueParameters(const SDK::FGameplayCueParameters& parameters, TakaroCueAttribution& out)
{
    out.instigator = parameters.Instigator.Get();
    out.effectCauser = parameters.EffectCauser.Get();
    out.sourceObject = parameters.SourceObject.Get();
    FillFromContext(ContextFromHandle(parameters.EffectContext), out);
}

std::string CueTag(const SDK::FGameplayTag& primary, const SDK::FGameplayCueParameters* parameters)
{
    auto tag = TagNameOf(primary);
    if (tag.empty() && parameters) tag = TagNameOf(parameters->MatchedTagName);
    if (tag.empty() && parameters) tag = TagNameOf(parameters->OriginalTag);
    return tag;
}

std::optional<TakaroSdkCombatCallback> DecodeTakaroCue(
    SDK::UAbilitySystemComponent* abilitySystem, size_t index, const void* parameters)
{
    SDK::AActor* victim = abilitySystem->AvatarActor ? abilitySystem->AvatarActor : abilitySystem->OwnerActor;
    if (!victim) return std::nullopt;
    const bool playerVictim = victim->IsA(SDK::ACrCharacterPlayerBase::StaticClass());
    if (!playerVictim && !victim->IsA(SDK::AMassEnemyCharacterBase::StaticClass())) return std::nullopt;
    TakaroSdkEntityCueCallback callback{victim, nullptr, nullptr, nullptr, 0.0, {}, kTakaroCueFunctionNames[index], playerVictim};
    TakaroCueAttribution attribution;
    switch (index)
    {
    case 0:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueExecuted*>(parameters);
        callback.tag = CueTag(p->GameplayCueTag, nullptr);
        FillFromContext(ContextFromHandle(p->EffectContext), attribution);
        break;
    }
    case 1:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueExecuted_WithParams*>(parameters);
        callback.tag = CueTag(p->GameplayCueTag, &p->GameplayCueParameters);
        callback.magnitude = p->GameplayCueParameters.RawMagnitude;
        FillFromCueParameters(p->GameplayCueParameters, attribution);
        break;
    }
    case 2:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueExecuted_FromSpec*>(parameters);
        callback.tag = p->Spec.Def ? p->Spec.Def->GetName() : std::string{};
        FillFromContext(ContextFromHandle(p->Spec.EffectContext), attribution);
        break;
    }
    case 3:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueAdded*>(parameters);
        callback.tag = CueTag(p->GameplayCueTag, nullptr);
        FillFromContext(ContextFromHandle(p->EffectContext), attribution);
        break;
    }
    case 4:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueAdded_WithParams*>(parameters);
        callback.tag = CueTag(p->GameplayCueTag, &p->Parameters);
        callback.magnitude = p->Parameters.RawMagnitude;
        FillFromCueParameters(p->Parameters, attribution);
        break;
    }
    case 5:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueAddedAndWhileActive_FromSpec*>(parameters);
        callback.tag = p->Spec.Def ? p->Spec.Def->GetName() : std::string{};
        FillFromContext(ContextFromHandle(p->Spec.EffectContext), attribution);
        break;
    }
    case 6:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCueAddedAndWhileActive_WithParams*>(parameters);
        callback.tag = CueTag(p->GameplayCueTag, &p->GameplayCueParameters);
        callback.magnitude = p->GameplayCueParameters.RawMagnitude;
        FillFromCueParameters(p->GameplayCueParameters, attribution);
        break;
    }
    case 7:
    {
        const auto* p = static_cast<const SDK::Params::AbilitySystemComponent_NetMulticast_InvokeGameplayCuesAddedAndWhileActive_WithParams*>(parameters);
        callback.tag = CueTag(p->GameplayCueTags.GameplayTags.Num() > 0 ? p->GameplayCueTags.GameplayTags[0] : SDK::FGameplayTag{},
            &p->GameplayCueParameters);
        callback.magnitude = p->GameplayCueParameters.RawMagnitude;
        FillFromCueParameters(p->GameplayCueParameters, attribution);
        break;
    }
    default: return std::nullopt;
    }
    callback.instigator = attribution.instigator;
    callback.effectCauser = attribution.effectCauser;
    callback.sourceObject = attribution.sourceObject;
    return callback;
}
#endif
}

std::optional<TakaroCombatCallbackKind> ClassifyTakaroCombatCallback(const std::string& functionFullName)
{
    if (functionFullName == "Function Chimera.CrCharacterPlayerBase.OnDied")
        return TakaroCombatCallbackKind::PlayerDied;
    if (functionFullName == "Function Chimera.CrDeathReactionComponent.ServerOnDeathEvent" ||
        functionFullName == "Function Chimera.CrDeathReactionComponent.MulticastOnDeathEvent" ||
        functionFullName == "Function Chimera.CrDeathReactionComponent.OnDeathEvent")
        return TakaroCombatCallbackKind::PlayerDied;
    if (functionFullName == "Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnDamage")
        return TakaroCombatCallbackKind::EntityDamaged;
    if (functionFullName ==
        "Function MassAIPrototypeEnemyRuntime.MassEnemyEventQueueSubsystem.DealDamageToAiActor")
        return TakaroCombatCallbackKind::EntityDamaged;
    if (functionFullName == "Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnAiDied" ||
        functionFullName == "Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.Multicast_OnAiDied" ||
        functionFullName == "Function MassAIPrototypeEnemyRuntime.MassEnemyCharacterBase.OnDiedFromGas")
        return TakaroCombatCallbackKind::EntityDied;
    for (const char* name : kTakaroCueFunctionNames)
        if (functionFullName == std::string("Function GameplayAbilities.AbilitySystemComponent.") + name)
            return TakaroCombatCallbackKind::EntityCue;
    return std::nullopt;
}

std::string TakaroWeaponCodeFromDamageTag(const std::string& killingDamageTag)
{
    if (killingDamageTag.find(".Damage.") == std::string::npos) return {};
    const auto dot = killingDamageTag.rfind('.');
    if (dot == std::string::npos || dot + 1 >= killingDamageTag.size()) return {};
    const auto segment = killingDamageTag.substr(dot + 1);
    return NonBlank(segment) ? segment : std::string{};
}

std::string SelectTakaroWeaponCode(
    const std::string& sourceWeaponCode, const std::string& equippedWeaponCode,
    const std::string& damageActorCode)
{
    if (NonBlank(sourceWeaponCode)) return sourceWeaponCode;
    if (NonBlank(equippedWeaponCode)) return equippedWeaponCode;
    return NonBlank(damageActorCode) ? damageActorCode : std::string{};
}

TakaroCombatLogBudget::TakaroCombatLogBudget(size_t linesPerKind)
    : linesPerKind_((std::max<size_t>)(linesPerKind, 1))
{
}

bool TakaroCombatLogBudget::Allow(const std::string& kind)
{
    auto& used = used_[kind];
    // Count one step past the budget so the exhaustion notice fires exactly once.
    if (used <= linesPerKind_ + 1) ++used;
    return used <= linesPerKind_;
}

bool TakaroCombatLogBudget::JustExhausted(const std::string& kind) const
{
    const auto found = used_.find(kind);
    return found != used_.end() && found->second == linesPerKind_ + 1;
}

void TakaroCombatLogBudget::Reset()
{
    used_.clear();
}

const char* TakaroEntityDeathOutcomeName(TakaroEntityDeathOutcome outcome)
{
    switch (outcome)
    {
    case TakaroEntityDeathOutcome::Invalid: return "invalid";
    case TakaroEntityDeathOutcome::Duplicate: return "duplicate";
    case TakaroEntityDeathOutcome::NoDamageRecord: return "no-damage-record";
    case TakaroEntityDeathOutcome::Expired: return "expired";
    case TakaroEntityDeathOutcome::EntityMismatch: return "entity-mismatch";
    case TakaroEntityDeathOutcome::NoWeapon: return "no-weapon";
    case TakaroEntityDeathOutcome::Matched: return "matched";
    }
    return "unknown";
}

std::string SelectTakaroStableActorCode(
    const std::string& identificationCode, const std::string& actorClassName)
{
    if (NonBlank(identificationCode)) return identificationCode;
    return NonBlank(actorClassName) ? actorClassName : std::string{};
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
bool TakaroCombatSdkDecoder::Initialize()
{
    auto* playerClass = SDK::ACrCharacterPlayerBase::StaticClass();
    auto* deathReactionClass = SDK::UCrDeathReactionComponent::StaticClass();
    auto* enemyClass = SDK::AMassEnemyCharacterBase::StaticClass();
    auto* enemyEventQueueClass = SDK::UMassEnemyEventQueueSubsystem::StaticClass();
    playerDied_ = playerClass ? playerClass->GetFunction("CrCharacterPlayerBase", "OnDied") : nullptr;
    serverDeathReaction_ = deathReactionClass ? deathReactionClass->GetFunction("CrDeathReactionComponent", "ServerOnDeathEvent") : nullptr;
    multicastDeathReaction_ = deathReactionClass ? deathReactionClass->GetFunction("CrDeathReactionComponent", "MulticastOnDeathEvent") : nullptr;
    localDeathReaction_ = deathReactionClass ? deathReactionClass->GetFunction("CrDeathReactionComponent", "OnDeathEvent") : nullptr;
    entityDamaged_ = enemyClass ? enemyClass->GetFunction("MassEnemyCharacterBase", "OnDamage") : nullptr;
    entityDamageQueued_ = enemyEventQueueClass
        ? enemyEventQueueClass->GetFunction("MassEnemyEventQueueSubsystem", "DealDamageToAiActor")
        : nullptr;
    entityDied_ = enemyClass ? enemyClass->GetFunction("MassEnemyCharacterBase", "OnAiDied") : nullptr;
    entityDiedMulticast_ = enemyClass ? enemyClass->GetFunction("MassEnemyCharacterBase", "Multicast_OnAiDied") : nullptr;
    entityDiedFromGas_ = enemyClass ? enemyClass->GetFunction("MassEnemyCharacterBase", "OnDiedFromGas") : nullptr;
    auto* abilitySystemClass = SDK::UAbilitySystemComponent::StaticClass();
    cueFunctionsResolved_ = 0;
    for (size_t index = 0; index < 8; ++index)
    {
        cueFunctions_[index] = abilitySystemClass
            ? abilitySystemClass->GetFunction("AbilitySystemComponent", kTakaroCueFunctionNames[index])
            : nullptr;
        if (cueFunctions_[index]) ++cueFunctionsResolved_;
    }
    TAKARO_LOG_INFO("%s init OnDied=%d ServerOnDeathEvent=%d MulticastOnDeathEvent=%d OnDeathEvent=%d "
        "OnDamage=%d DealDamageToAiActor=%d OnAiDied=%d Multicast_OnAiDied=%d OnDiedFromGas=%d GameplayCue=%u/8",
        kCombatLogPrefix, playerDied_ != nullptr, serverDeathReaction_ != nullptr,
        multicastDeathReaction_ != nullptr, localDeathReaction_ != nullptr, entityDamaged_ != nullptr,
        entityDamageQueued_ != nullptr, entityDied_ != nullptr, entityDiedMulticast_ != nullptr,
        entityDiedFromGas_ != nullptr, static_cast<unsigned>(cueFunctionsResolved_));
    // The two extra death sources are optional: OnAiDied alone was the July contract.
    return playerDied_ && serverDeathReaction_ && multicastDeathReaction_ && localDeathReaction_ &&
        entityDamaged_ && entityDamageQueued_ && entityDied_;
}

std::optional<TakaroSdkCombatCallback> TakaroCombatSdkDecoder::Decode(
    SDK::UObject* object, SDK::UFunction* function, const void* parameters) const
{
    if (!object || !function) return std::nullopt;
    if (function == playerDied_ && object->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
    {
        return TakaroSdkPlayerDiedCallback{static_cast<SDK::ACrCharacterPlayerBase*>(object)};
    }
    if ((function == serverDeathReaction_ || function == multicastDeathReaction_ || function == localDeathReaction_) &&
        object->IsA(SDK::UCrDeathReactionComponent::StaticClass()))
    {
        auto* owner = static_cast<SDK::UCrDeathReactionComponent*>(object)->GetOwner();
        if (!owner || !owner->IsA(SDK::ACrCharacterPlayerBase::StaticClass())) return std::nullopt;
        return TakaroSdkPlayerDiedCallback{static_cast<SDK::ACrCharacterPlayerBase*>(owner)};
    }
    const bool enemyVictim = object->IsA(SDK::AMassEnemyCharacterBase::StaticClass());
    if (enemyVictim && (function == entityDied_ || (entityDiedMulticast_ && function == entityDiedMulticast_) ||
        (entityDiedFromGas_ && function == entityDiedFromGas_)))
    {
        // OnAiDied, Multicast_OnAiDied, and OnDiedFromGas share the (FHitResult, FGameplayTag) shape.
        std::string tag;
        if (parameters)
            tag = TagNameOf(static_cast<const SDK::Params::MassEnemyCharacterBase_OnAiDied*>(parameters)->KillingDamageTag);
        const char* source = function == entityDied_ ? "OnAiDied"
            : function == entityDiedMulticast_ ? "Multicast_OnAiDied" : "OnDiedFromGas";
        return TakaroSdkEntityDiedCallback{object, source, std::move(tag)};
    }
    if (!parameters) return std::nullopt;
    if (cueFunctionsResolved_ && object->IsA(SDK::UAbilitySystemComponent::StaticClass()))
    {
        for (size_t index = 0; index < 8; ++index)
        {
            if (!cueFunctions_[index] || function != cueFunctions_[index]) continue;
            return DecodeTakaroCue(static_cast<SDK::UAbilitySystemComponent*>(object), index, parameters);
        }
        return std::nullopt;
    }
    if (function == entityDamaged_ && enemyVictim)
    {
        const auto* decoded = static_cast<const SDK::Params::MassEnemyCharacterBase_OnDamage*>(parameters);
        return TakaroSdkEntityDamagedCallback{object, decoded->Actor, decoded->InDamage, "OnDamage"};
    }
    if (function == entityDamageQueued_ &&
        object->IsA(SDK::UMassEnemyEventQueueSubsystem::StaticClass()))
    {
        const auto* decoded =
            static_cast<const SDK::Params::MassEnemyEventQueueSubsystem_DealDamageToAiActor*>(parameters);
        if (!decoded->TargetActor ||
            !decoded->TargetActor->IsA(SDK::AMassEnemyCharacterBase::StaticClass()))
            return std::nullopt;
        return TakaroSdkEntityDamagedCallback{decoded->TargetActor, decoded->AttackerActor, 1.0, "DealDamageToAiActor"};
    }
    return std::nullopt;
}

bool InitializeTakaroCombatEvents()
{
    g_combat.Reset();
    g_combatLog.Reset();
    g_decoderReady = g_decoder.Initialize();
    return g_decoderReady;
}

// The weapon that a damage actor stands for: a projectile names its source weapon, a pawn
// names its equipped weapon, anything else names itself. Blank when unknown.
std::string WeaponCodeForDamageActor(SDK::AActor* damageActor, std::string* weaponClassOut)
{
    if (!damageActor) return {};
    SDK::AActor* weaponActor = nullptr;
    if (damageActor->IsA(SDK::ACrAbilityProjectile::StaticClass()))
        weaponActor = static_cast<SDK::ACrAbilityProjectile*>(damageActor)->SourceWeaponActor;
    std::string equipped;
    if (damageActor->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
    {
        auto* weaponSystem = static_cast<SDK::ACrCharacterPlayerBase*>(damageActor)->BP_GetWeaponComponent();
        auto* equippedActor = weaponSystem ? weaponSystem->GetEquippedWeaponActor() : nullptr;
        if (equippedActor)
        {
            equipped = StableActorCode(equippedActor);
            if (weaponClassOut && weaponClassOut->empty()) *weaponClassOut = ClassNameOf(equippedActor);
        }
    }
    if (weaponActor && weaponClassOut) *weaponClassOut = ClassNameOf(weaponActor);
    if (!weaponActor && equipped.empty() && weaponClassOut) *weaponClassOut = ClassNameOf(damageActor);
    return SelectTakaroWeaponCode(
        weaponActor ? StableActorCode(weaponActor) : std::string{}, equipped, StableActorCode(damageActor));
}

void HandleTakaroCombatProcessEvent(
    SDK::UObject* object, SDK::UFunction* function, const void* parameters)
{
    if (!g_decoderReady) return;
    const auto callback = g_decoder.Decode(object, function, parameters);
    if (!callback) return;
    if (const auto* death = std::get_if<TakaroSdkPlayerDiedCallback>(&*callback))
    {
        TAKARO_LOG_INFO("Authoritative player-death callback observed.");
        const auto player = PlayerFromActor(death->player);
        if (!player) return;
        TakaroPlayerDeathObservation observation;
        observation.victimInstanceKey = player->gameId;
        observation.player = *player;
        observation.attacker = std::nullopt;
        if (death->player)
        {
            const auto location = death->player->K2_GetActorLocation();
            observation.position = TakaroPositionDto{location.X, location.Y, location.Z, std::nullopt};
        }
        RecordTakaroPlayerDeathObservation(observation);
        return;
    }
    if (const auto* damage = std::get_if<TakaroSdkEntityDamagedCallback>(&*callback))
    {
        auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(damage->victim);
        const auto entityName = enemy->GetCharacterUniqueName();
        std::string weaponClass;
        TakaroEntityDamageObservation observation;
        observation.victimInstanceKey = enemy->GetFullName();
        observation.entityCode = entityName.IsNone() ? std::string{} : entityName.ToString();
        observation.player = PlayerFromActor(damage->damageActor);
        observation.weapon = WeaponCodeForDamageActor(damage->damageActor, &weaponClass);
        observation.damage = damage->damage;
        g_combat.ObserveEntityDamage(observation);
        CombatLog("damage", "damage via=" + std::string(damage->source) +
            " victim=" + ClassNameOf(damage->victim) +
            " source=" + ClassNameOf(damage->damageActor) +
            " player=" + (observation.player ? "resolved" : "unresolved") +
            " weapon=" + (NonBlank(observation.weapon) ? "set" : "blank") +
            " weaponClass=" + (weaponClass.empty() ? "<none>" : weaponClass) +
            " entity=" + (NonBlank(observation.entityCode) ? "set" : "blank") +
            " damage=" + std::to_string(observation.damage));
        return;
    }
    if (const auto* cue = std::get_if<TakaroSdkEntityCueCallback>(&*callback))
    {
        if (cue->playerVictim)
        {
            // Diagnostics only: proves whether the game raises gameplay cues on hits at all.
            CombatLog("cue-on-player", "cue-on-player via=" + std::string(cue->source) +
                " victim=" + ClassNameOf(cue->victim) +
                " tag=" + (cue->tag.empty() ? "<none>" : cue->tag) +
                " instigator=" + (cue->instigator ? ClassNameOf(cue->instigator) : "none") +
                " causer=" + (cue->effectCauser ? ClassNameOf(cue->effectCauser) : "none") +
                " source=" + (cue->sourceObject ? ClassNameOf(cue->sourceObject) : "none") +
                " magnitude=" + std::to_string(cue->magnitude));
            return;
        }
        auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(cue->victim);
        const auto entityName = enemy->GetCharacterUniqueName();
        std::string weaponClass;
        TakaroEntityDamageObservation observation;
        observation.victimInstanceKey = enemy->GetFullName();
        observation.entityCode = entityName.IsNone() ? std::string{} : entityName.ToString();
        observation.player = PlayerFromActor(cue->instigator);
        if (!observation.player) observation.player = PlayerFromActor(cue->effectCauser);
        auto* sourceActor = cue->sourceObject && cue->sourceObject->IsA(SDK::AActor::StaticClass())
            ? static_cast<SDK::AActor*>(cue->sourceObject) : nullptr;
        observation.weapon = WeaponCodeForDamageActor(cue->effectCauser, &weaponClass);
        if (!NonBlank(observation.weapon) && sourceActor)
            observation.weapon = WeaponCodeForDamageActor(sourceActor, &weaponClass);
        // A cue with no magnitude is still positive evidence of a hit on this victim.
        observation.damage = std::isfinite(cue->magnitude) && cue->magnitude > 0 ? cue->magnitude : 1.0;
        g_combat.ObserveEntityDamage(observation);
        CombatLog("cue", "cue via=" + std::string(cue->source) +
            " victim=" + ClassNameOf(cue->victim) +
            " tag=" + (cue->tag.empty() ? "<none>" : cue->tag) +
            " instigator=" + (cue->instigator ? ClassNameOf(cue->instigator) : "none") +
            " causer=" + (cue->effectCauser ? ClassNameOf(cue->effectCauser) : "none") +
            " source=" + (cue->sourceObject ? ClassNameOf(cue->sourceObject) : "none") +
            " magnitude=" + std::to_string(cue->magnitude) +
            " player=" + (observation.player ? "resolved" : "unresolved") +
            " weapon=" + (NonBlank(observation.weapon) ? "set" : "blank") +
            " weaponClass=" + (weaponClass.empty() ? "<none>" : weaponClass));
        return;
    }
    if (const auto* death = std::get_if<TakaroSdkEntityDiedCallback>(&*callback))
    {
        auto* enemy = static_cast<SDK::AMassEnemyCharacterBase*>(death->victim);
        const auto entityName = enemy->GetCharacterUniqueName();
        const auto entity = entityName.IsNone() ? std::string{} : entityName.ToString();
        const auto result = g_combat.ObserveEntityDeathDetailed(enemy->GetFullName(), entity,
            TakaroCombatCorrelator::Clock::now(), TakaroWeaponCodeFromDamageTag(death->killingDamageTag));
        if (result.event)
            TakaroState().AddEntityKilled(result.event->player, result.event->entity, result.event->weapon);
        CombatLog("death", "death via=" + std::string(death->source) +
            " victim=" + ClassNameOf(death->victim) +
            " tag=" + (death->killingDamageTag.empty() ? "<none>" : death->killingDamageTag) +
            " record=" + TakaroEntityDeathOutcomeName(result.outcome) +
            " emitted=" + (result.event ? "yes" : "no"));
    }
}

namespace
{
void TakaroCombatProcessEvent(void* object, void* function, void* parameters)
{
    HandleTakaroCombatProcessEvent(
        static_cast<SDK::UObject*>(object),
        static_cast<SDK::UFunction*>(function),
        parameters);
}
}

bool InstallTakaroCombatHooks(IPluginHookUtils* hooks)
{
    (void)hooks;
    if (g_combatSubscribed) return true;
    if (!InitializeTakaroCombatEvents() ||
        !SubscribeTakaroProcessEvent(&TakaroCombatProcessEvent))
        return false;
    g_combatSubscribed = true;
    return true;
}

void RemoveTakaroCombatHooks(IPluginHookUtils* hooks)
{
    (void)hooks;
    if (g_combatSubscribed) UnsubscribeTakaroProcessEvent(&TakaroCombatProcessEvent);
    g_combatSubscribed = false;
    g_decoderReady = false;
    g_combat.Reset();
}
#endif

void ResetTakaroCombatEvents()
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    g_combat.Reset();
#endif
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
void RecordTakaroPlayerDeathObservation(const TakaroPlayerDeathObservation& observation)
{
    if (const auto event = g_combat.ObservePlayerDeath(observation))
        TakaroState().AddPlayerDeath(event->player, event->attacker, event->position);
}
#endif

std::vector<TakaroPlayerDeathObservation> TakaroPlayerDeathStateTracker::Reconcile(
    const std::vector<TakaroPlayerSnapshot>& players,
    const std::map<std::string, bool>& deadStates,
    const std::map<std::string, TakaroPositionDto>& lastDeathPositions)
{
    std::vector<TakaroPlayerDeathObservation> deaths;
    std::set<std::string> current;
    for (const auto& player : players)
    {
        if (!ValidPlayer(player)) continue;
        current.insert(player.gameId);
        const auto dead = deadStates.find(player.gameId);
        const auto prior = previous_.find(player.gameId);
        const bool deadTransition = dead != deadStates.end() &&
            prior != previous_.end() && !prior->second && dead->second;
        if (dead != deadStates.end() && !dead->second && player.hasLocation)
            lastLivePositions_[player.gameId] = TakaroPositionDto{player.x, player.y, player.z, std::nullopt};

        // The last-death position changes exactly once per death and survives in the save, so
        // when the game gives one it alone decides. The first value seen is only a baseline.
        const auto deathPosition = lastDeathPositions.find(player.gameId);
        const bool deathPositionKnown = deathPosition != lastDeathPositions.end() &&
            ValidPosition(deathPosition->second);
        bool deathTransformChanged = false;
        if (deathPositionKnown)
        {
            const auto [previousDeathPosition, firstSeen] =
                previousDeathPositions_.try_emplace(player.gameId, deathPosition->second);
            deathTransformChanged = !firstSeen &&
                (std::abs(deathPosition->second.x - previousDeathPosition->second.x) > 0.001 ||
                 std::abs(deathPosition->second.y - previousDeathPosition->second.y) > 0.001 ||
                 std::abs(deathPosition->second.z - previousDeathPosition->second.z) > 0.001);
        }
        if (deathPositionKnown ? deathTransformChanged : deadTransition)
        {
            TakaroPlayerDeathObservation observation;
            observation.victimInstanceKey = player.gameId;
            observation.player = player;
            observation.attacker = std::nullopt;
            if (deathTransformChanged)
                observation.position = deathPosition->second;
            else if (player.hasLocation)
                observation.position = TakaroPositionDto{player.x, player.y, player.z, std::nullopt};
            else if (const auto last = lastLivePositions_.find(player.gameId); last != lastLivePositions_.end())
                observation.position = last->second;
            deaths.push_back(std::move(observation));
        }
        if (dead != deadStates.end()) previous_[player.gameId] = dead->second;
        if (deathPositionKnown) previousDeathPositions_[player.gameId] = deathPosition->second;
    }
    for (auto it = previous_.begin(); it != previous_.end();)
    {
        if (!current.contains(it->first))
        {
            lastLivePositions_.erase(it->first);
            previousDeathPositions_.erase(it->first);
            it = previous_.erase(it);
        }
        else ++it;
    }
    return deaths;
}

void TakaroPlayerDeathStateTracker::Reset()
{
    previous_.clear();
    lastLivePositions_.clear();
    previousDeathPositions_.clear();
}

std::optional<TakaroDamageSource> ResolveTakaroDamageSource(
    const std::string& damageActorKey, const TakaroDamageSourceLookup& lookup, size_t maxNodes)
{
    if (!NonBlank(damageActorKey) || !lookup || maxNodes == 0) return std::nullopt;

    std::deque<std::string> pending{damageActorKey};
    std::set<std::string> visited;
    std::map<std::string, TakaroPlayerSnapshot> players;
    std::set<std::string> weapons;
    while (!pending.empty() && visited.size() < maxNodes)
    {
        auto key = std::move(pending.front());
        pending.pop_front();
        if (!NonBlank(key) || !visited.insert(key).second) continue;
        const auto node = lookup(key);
        if (!node || node->actorKey != key) return std::nullopt;
        if (node->player)
        {
            if (!ValidPlayer(*node->player)) return std::nullopt;
            players.insert_or_assign(node->player->gameId, *node->player);
        }
        if (node->weapon)
        {
            if (!NonBlank(*node->weapon)) return std::nullopt;
            weapons.insert(*node->weapon);
        }
        if (NonBlank(node->ownerKey)) pending.push_back(node->ownerKey);
        if (NonBlank(node->instigatorKey)) pending.push_back(node->instigatorKey);
    }
    if (!pending.empty() || players.size() != 1 || weapons.size() != 1) return std::nullopt;
    return TakaroDamageSource{players.begin()->second, *weapons.begin()};
}

TakaroCombatCorrelator::TakaroCombatCorrelator(
    std::chrono::milliseconds finalDamagerWindow,
    std::chrono::milliseconds deathDedupeWindow,
    size_t maxRecords)
    : finalDamagerWindow_((std::max)(finalDamagerWindow, std::chrono::milliseconds(1))),
      deathDedupeWindow_((std::max)(deathDedupeWindow, std::chrono::milliseconds(1))),
      maxRecords_((std::max<size_t>)(maxRecords, 1))
{
}

std::optional<TakaroPlayerDeathEvent> TakaroCombatCorrelator::ObservePlayerDeath(
    const TakaroPlayerDeathObservation& observation, Clock::time_point now)
{
    Prune(now);
    if (!NonBlank(observation.victimInstanceKey) || !ValidPlayer(observation.player) ||
        (observation.attacker && !ValidPlayer(*observation.attacker)) ||
        (observation.position && !ValidPosition(*observation.position))) return std::nullopt;

    const auto duplicate = playerDeaths_.find(observation.victimInstanceKey);
    if (duplicate != playerDeaths_.end() && now - duplicate->second <= deathDedupeWindow_) return std::nullopt;
    if (playerDeaths_.size() >= maxRecords_) EraseOldestDeath(playerDeaths_);
    playerDeaths_.insert_or_assign(observation.victimInstanceKey, now);
    return TakaroPlayerDeathEvent{observation.player, observation.attacker, observation.position};
}

void TakaroCombatCorrelator::ObserveEntityDamage(
    const TakaroEntityDamageObservation& observation, Clock::time_point now)
{
    Prune(now);
    if (!NonBlank(observation.victimInstanceKey)) return;

    // Every positive damage callback supersedes the prior source. If its attribution is
    // incomplete, erase the old record so an earlier player cannot receive false credit.
    if (!std::isfinite(observation.damage) || observation.damage <= 0) return;
    damageByVictim_.erase(observation.victimInstanceKey);
    if (!NonBlank(observation.entityCode) || !observation.player || !ValidPlayer(*observation.player)) return;
    // A blank weapon is kept: the killing damage tag can still name the weapon at death time.
    if (damageByVictim_.size() >= maxRecords_) EraseOldest(damageByVictim_);
    damageByVictim_.insert_or_assign(observation.victimInstanceKey,
        DamageRecord{observation.entityCode, *observation.player, NonBlank(observation.weapon) ? observation.weapon : std::string{}, now});
}

std::optional<TakaroEntityKilledEvent> TakaroCombatCorrelator::ObserveEntityDeath(
    const std::string& victimInstanceKey, const std::string& entityCode, Clock::time_point now)
{
    return ObserveEntityDeathDetailed(victimInstanceKey, entityCode, now).event;
}

TakaroEntityDeathResult TakaroCombatCorrelator::ObserveEntityDeathDetailed(
    const std::string& victimInstanceKey, const std::string& entityCode, Clock::time_point now,
    const std::string& fallbackWeapon)
{
    // Prune after the lookup so an aged-out record is reported as Expired, not as absent.
    if (!NonBlank(victimInstanceKey)) return {std::nullopt, TakaroEntityDeathOutcome::Invalid};
    const auto duplicate = entityDeaths_.find(victimInstanceKey);
    if (duplicate != entityDeaths_.end() && now - duplicate->second <= deathDedupeWindow_)
        return {std::nullopt, TakaroEntityDeathOutcome::Duplicate};
    if (entityDeaths_.size() >= maxRecords_) EraseOldestDeath(entityDeaths_);
    entityDeaths_.insert_or_assign(victimInstanceKey, now);
    if (!NonBlank(entityCode)) return {std::nullopt, TakaroEntityDeathOutcome::Invalid};
    const auto found = damageByVictim_.find(victimInstanceKey);
    if (found == damageByVictim_.end())
    {
        Prune(now);
        return {std::nullopt, TakaroEntityDeathOutcome::NoDamageRecord};
    }
    const auto damage = found->second;
    damageByVictim_.erase(found);
    Prune(now);
    if (now - damage.observedAt > finalDamagerWindow_) return {std::nullopt, TakaroEntityDeathOutcome::Expired};
    if (damage.entityCode != entityCode) return {std::nullopt, TakaroEntityDeathOutcome::EntityMismatch};
    const auto weapon = NonBlank(damage.weapon) ? damage.weapon : fallbackWeapon;
    if (!NonBlank(weapon)) return {std::nullopt, TakaroEntityDeathOutcome::NoWeapon};
    return {TakaroEntityKilledEvent{damage.player, entityCode, weapon}, TakaroEntityDeathOutcome::Matched};
}

void TakaroCombatCorrelator::Reset()
{
    damageByVictim_.clear();
    playerDeaths_.clear();
    entityDeaths_.clear();
}

void TakaroCombatCorrelator::Prune(Clock::time_point now)
{
    for (auto item = damageByVictim_.begin(); item != damageByVictim_.end();)
    {
        if (now - item->second.observedAt > finalDamagerWindow_) item = damageByVictim_.erase(item);
        else ++item;
    }
    for (auto item = playerDeaths_.begin(); item != playerDeaths_.end();)
    {
        if (now - item->second > deathDedupeWindow_) item = playerDeaths_.erase(item);
        else ++item;
    }
    for (auto item = entityDeaths_.begin(); item != entityDeaths_.end();)
    {
        if (now - item->second > deathDedupeWindow_) item = entityDeaths_.erase(item);
        else ++item;
    }
}
