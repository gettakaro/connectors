#include "starrupture_player_actions.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    try
    {
        const auto teleport = ValidateTakaroTeleport("stable-id", 100.0, -200.5, 300.0);
        Expect(teleport.ok, "valid teleport rejected");
        Expect(teleport.gameId == "stable-id" && teleport.y == -200.5, "teleport values changed");
        Expect(!ValidateTakaroTeleport("", 1, 2, 3).ok, "empty player identity accepted");
        Expect(!ValidateTakaroTeleport("stable-id", std::nan(""), 2, 3).ok, "NaN coordinate accepted");
        Expect(!ValidateTakaroTeleport("stable-id", 20'000'001, 2, 3).ok, "unbounded coordinate accepted");
        Expect(IsTakaroTeleportPostcondition(10, 20, 30, 10.5, 20, 30),
            "sub-centimetre teleport readback was rejected");
        Expect(!IsTakaroTeleportPostcondition(10, 20, 30, 16, 20, 30),
            "six-centimetre teleport no-op was accepted");
        // K2_TeleportTo sweeps: a target above terrain settles on the ground, so the
        // vertical band accepts the settle while the horizontal placement stays tight.
        Expect(IsTakaroTeleportPostcondition(10, 20, 9600, 10, 20, 9420),
            "a character that settled 180 cm onto the ground was rejected");
        Expect(IsTakaroTeleportPostcondition(10, 20, 9600, 10, 20, 9300),
            "a character that settled exactly 300 cm onto the ground was rejected");
        Expect(!IsTakaroTeleportPostcondition(10, 20, 9600, 10, 20, 9299),
            "a character 301 cm below the request was accepted");
        Expect(!IsTakaroTeleportPostcondition(10, 20, 9600, 10, 20, 9700),
            "a character 100 cm above the request was accepted");
        Expect(!IsTakaroTeleportPostcondition(10, 20, 9600, 40, 20, 9420),
            "a settled character 30 cm off horizontally was accepted");

        const auto kick = ValidateTakaroKick("stable-id", "Rule violation");
        Expect(kick.ok && kick.reason == "Rule violation", "valid kick rejected");
        Expect(!ValidateTakaroKick("", "reason").ok, "kick without identity accepted");
        Expect(!ValidateTakaroKick("stable-id", "").ok, "kick without reason accepted");
        Expect(!ValidateTakaroKick("stable-id", std::string(513, 'x')).ok, "unbounded kick reason accepted");

        const auto message = ValidateTakaroMessage("Server restart soon", std::nullopt, "Takaro");
        Expect(message.ok && message.rendered == "[Takaro] Server restart soon", "server message normalization failed");
        const auto direct = ValidateTakaroMessage("Hello", std::optional<std::string>{"stable-id"}, "Coach", "Limon");
        Expect(direct.ok && direct.recipientGameId == std::optional<std::string>{"stable-id"},
            "direct message recipient changed");
        Expect(direct.rendered == "[Coach] [to Limon] Hello",
            "direct message must name the recipient in the multicast text");
        const auto unnamed = ValidateTakaroMessage("Hello", std::optional<std::string>{"stable-id"}, "Takaro", "  ");
        Expect(unnamed.ok && unnamed.rendered == "[Takaro] [to player] Hello",
            "direct message without a display name must not leak the gameId");
        const auto leaked = ValidateTakaroMessage("Hello", std::optional<std::string>{"stable-id"}, "Takaro", "stable-id");
        Expect(leaked.ok && leaked.rendered == "[Takaro] [to player] Hello",
            "direct message must never render the gameId as the name");
        Expect(!ValidateTakaroMessage("Hello", std::optional<std::string>{"   "}, "Takaro", "Limon").ok,
            "blank direct message recipient accepted");
        const auto global = ValidateTakaroMessage("Hello", std::nullopt, "Coach", "Limon");
        Expect(global.ok && global.rendered == "[Coach] Hello",
            "global message must ignore a stray display name");
        Expect(!ValidateTakaroMessage("", std::nullopt, "Takaro").ok, "empty message accepted");
        Expect(!ValidateTakaroMessage(std::string(2001, 'x'), std::nullopt, "Takaro").ok,
            "unbounded message accepted");

        std::cout << "starrupture_player_actions_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_player_actions_test: " << error.what() << "\n";
        return 1;
    }
}
