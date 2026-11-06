#include "starrupture_sdk_bridge.h"
#include "starrupture_player_pawn.h"
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct FakePawn {};
struct FakeController
{
    FakePawn* pawn = nullptr;
    FakePawn* K2_GetPawn() const { return pawn; }
};
struct FakePlayerState
{
    FakePawn* pawn = nullptr;
    FakeController* controller = nullptr;
    FakePawn* GetPawn() const { return pawn; }
    FakeController* GetPlayerController() const { return controller; }
};
}

int main()
{
    try
    {
        FakePawn directPawn;
        FakePawn controllerPawn;
        FakeController controller{&controllerPawn};
        FakePlayerState directState{&directPawn, &controller};
        FakePlayerState controllerState{nullptr, &controller};
        FakePlayerState unavailableState{nullptr, nullptr};
        Expect(ResolveTakaroPlayerPawn(&directState) == &directPawn,
            "direct PlayerState pawn was not preferred");
        Expect(ResolveTakaroPlayerPawn(&controllerState) == &controllerPawn,
            "controller-owned pawn was not used when PlayerState pawn was absent");
        Expect(ResolveTakaroPlayerPawn(&unavailableState) == nullptr,
            "missing controller fabricated a pawn");
        Expect(ResolveTakaroPlayerPawn<FakePlayerState>(nullptr) == nullptr,
            "null PlayerState fabricated a pawn");

        TakaroRawPlayerSnapshot raw;
        raw.gameId = "stable-server-id";
        raw.name = "Player";
        raw.ping = 17;
        raw.hasLocation = true;
        raw.x = 100.0;
        raw.y = -200.0;
        raw.z = 300.0;
        const auto player = BuildTakaroPlayerSnapshot(raw);
        Expect(player.has_value(), "valid pinned-SDK values rejected");
        Expect(player->gameId == raw.gameId, "gameId transformed");
        Expect(player->platformId == "starrupture:737461626c652d7365727665722d6964",
            "stable identity was not encoded as a Takaro platformId");
        Expect(player->hasLocation && player->x == 100.0, "raw centimetre location transformed");

        Expect(player->steamId.empty(), "a non-Steam identity was reported as a steamId");
        Expect(!player->skills.has_value(), "skills were fabricated before the server loaded them");

        TakaroRawPlayerSnapshot steam = raw;
        steam.gameId = "76561198000000000";
        const auto steamPlayer = BuildTakaroPlayerSnapshot(steam);
        Expect(steamPlayer->steamId == steam.gameId, "SteamID64 was not reported as steamId");
        Expect(steamPlayer->platformId == "steam:76561198000000000", "SteamID64 platformId was not steam:<id>");

        TakaroRawPlayerSnapshot skilled = raw;
        skilled.skills = std::vector<TakaroRawSkill>{
            {0, 3, 120.5f}, {1, 0, 0.0f}, {2, 1, 7.0f}, {3, 9, 9.0f}, {1, -1, 1.0f}};
        const auto skilledPlayer = BuildTakaroPlayerSnapshot(skilled);
        Expect(skilledPlayer->skills.has_value() && skilledPlayer->skills->size() == 3,
            "skills were not filtered to the real, valid ones");
        Expect((*skilledPlayer->skills)[0].skill == "movement" && (*skilledPlayer->skills)[0].level == 3 &&
            (*skilledPlayer->skills)[0].experience == 120.5, "movement reading changed");
        Expect((*skilledPlayer->skills)[1].skill == "combat", "combat reading lost");
        Expect((*skilledPlayer->skills)[2].skill == "survival", "survival reading lost");

        raw.hasLocation = false;
        raw.x = 999.0;
        raw.y = 999.0;
        raw.z = 999.0;
        const auto playerWithoutPawn = BuildTakaroPlayerSnapshot(raw);
        Expect(playerWithoutPawn.has_value(), "valid player without a pawn was rejected");
        Expect(!playerWithoutPawn->hasLocation, "player without a pawn received a fabricated location");

        raw.gameId.clear();
        Expect(!BuildTakaroPlayerSnapshot(raw), "missing stable identity accepted");
        raw.gameId = "display-name";
        raw.name.clear();
        Expect(!BuildTakaroPlayerSnapshot(raw), "missing display name accepted as identity fallback");

        auto& operations = TakaroGameOperations();
        for (const auto type : AllTakaroOperationTypes())
        {
            TakaroOperationRequest request{type, TakaroOperationName(type), "{}", 1, 4102444800000LL};
            const auto result = operations.Execute(request);
            const auto* failure = std::get_if<TakaroOperationFailure>(&result);
            Expect(failure != nullptr, "unproved operation reported success");
            Expect(failure->error.find(request.action) != std::string::npos, "unsupported error omitted the operation name");
            if (type == TakaroOperationType::ListEntities || type == TakaroOperationType::ListLocations)
                Expect(failure->error.find("pinned StarRupture Game SDK") != std::string::npos,
                    "catalog operation did not dispatch through its pinned SDK boundary");
        }

        std::cout << "starrupture_sdk_bridge_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_sdk_bridge_test: " << error.what() << "\n";
        return 1;
    }
}
