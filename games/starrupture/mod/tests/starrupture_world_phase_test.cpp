#include "starrupture_world_phase.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    try
    {
        TakaroWorldObservation lobby;
        lobby.worldName = "DedicatedServerStart";
        lobby.hasGameState = true;
        lobby.gameStateBegunPlay = true;
        Expect(ClassifyTakaroWorldPhase(lobby) == TakaroWorldPhase::Lobby,
            "lobby map with a begun-play game state was not classified as lobby");

        TakaroWorldObservation gameplay;
        gameplay.worldName = "ChimeraMain";
        gameplay.hasGameState = true;
        gameplay.gameStateBegunPlay = true;
        Expect(ClassifyTakaroWorldPhase(gameplay) == TakaroWorldPhase::Gameplay,
            "ChimeraMain with a begun-play game state was not classified as gameplay");

        TakaroWorldObservation loading = gameplay;
        loading.gameStateBegunPlay = false;
        Expect(ClassifyTakaroWorldPhase(loading) == TakaroWorldPhase::Loading,
            "ChimeraMain before begin play was reported as gameplay");
        loading.hasGameState = false;
        loading.gameStateBegunPlay = true;
        Expect(ClassifyTakaroWorldPhase(loading) == TakaroWorldPhase::Loading,
            "ChimeraMain without a game state was reported as gameplay");

        TakaroWorldObservation unknown;
        unknown.worldName = "Lobby";
        unknown.hasGameState = true;
        unknown.gameStateBegunPlay = true;
        Expect(ClassifyTakaroWorldPhase(unknown) == TakaroWorldPhase::Unknown,
            "an unpinned map name was classified as a known phase");
        Expect(ClassifyTakaroWorldPhase(TakaroWorldObservation{}) == TakaroWorldPhase::Unknown,
            "an empty observation was classified as a known phase");

        Expect(std::string(TakaroWorldPhaseName(TakaroWorldPhase::Lobby)) == "lobby", "lobby name");
        Expect(std::string(TakaroWorldPhaseName(TakaroWorldPhase::Loading)) == "loading", "loading name");
        Expect(std::string(TakaroWorldPhaseName(TakaroWorldPhase::Gameplay)) == "gameplay", "gameplay name");
        Expect(std::string(TakaroWorldPhaseName(TakaroWorldPhase::Unknown)) == "unknown", "unknown name");
        Expect(TakaroWorldPhaseNotReadyReason(TakaroWorldPhase::Gameplay) == nullptr,
            "gameplay phase carried a not-ready reason");
        Expect(std::string(TakaroWorldPhaseNotReadyReason(TakaroWorldPhase::Lobby)).find("Manage Server") != std::string::npos,
            "lobby reason does not tell the operator to start the world");
        Expect(TakaroWorldPhaseNotReadyReason(TakaroWorldPhase::Loading) != nullptr, "loading reason missing");
        Expect(TakaroWorldPhaseNotReadyReason(TakaroWorldPhase::Unknown) != nullptr, "unknown reason missing");
        std::cout << "starrupture_world_phase_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_world_phase_test: " << error.what() << "\n";
        return 1;
    }
}
