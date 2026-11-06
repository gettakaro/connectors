using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Takaro;
using Takaro.Services;
using Takaro.WebSocket;

// players.characters: reported only on a Permanent Death server.
public static partial class ContractHarness
{
    private static void FamilyCharactersManifest()
    {
        Equal(
            0,
            ProtocolHandshake
                .BuildCapabilityManifest(RequestRouter.SupportedActions)
                .Count(c => (string)c["name"] == PlayerCharacter.Capability),
            "a server without Permanent Death declares no characters"
        );

        Dictionary<string, object> entry = ProtocolHandshake
            .BuildCapabilityManifest(
                RequestRouter.SupportedActions,
                null,
                PlayerCharacter.BuildCapability()
            )
            .Single(c => (string)c["name"] == PlayerCharacter.Capability);
        JObject parameters = JObject.Parse(JsonConvert.SerializeObject(entry["parameters"]));
        Equal("server", (string)parameters["held"], "the server keeps the character");
        Equal("death", (string)parameters["ends"][0], "a death ends the character");
    }

    private static void FamilyCharacterIdentity()
    {
        Equal("eos-1", PlayerCharacter.Id("eos", 0), "the first life");
        Equal("eos-3", PlayerCharacter.Id("eos", 2), "two lives ended");

        JObject plain = JObject.Parse(
            JsonConvert.SerializeObject(new TakaroPlayer { GameId = "eos", Name = "Limon" })
        );
        True(plain["character"] == null, "no character field without a character");

        JObject reported = JObject.Parse(
            JsonConvert.SerializeObject(
                new TakaroPlayer
                {
                    GameId = "eos",
                    Name = "Limon",
                    Character = new TakaroCharacter { Id = "eos-2", Name = "Limon" },
                }
            )
        );
        Equal("eos-2", (string)reported["character"]["id"], "character id on the wire");
        Equal("Limon", (string)reported["character"]["name"], "character name on the wire");
    }

    private static void FamilyCharacterOnEvents()
    {
        var client = new ClientInfo
        {
            entityId = 73,
            playerName = "Limon",
            CrossplatformId = new PlatformUserIdentifierAbs { CombinedString = "EOS_eos" },
        };
        True(
            Shared.TransformClientInfoToTakaroPlayer(client).Character == null,
            "no character on an event without Permanent Death"
        );

        StateMirror.Instance.PermanentDeath = true;
        try
        {
            TakaroPlayer player = Shared.TransformClientInfoToTakaroPlayerIdentity(client);
            Equal("eos-1", player.Character.Id, "events name the character");

            WebSocketTransport.Instance.TerminalMessages.Clear();
            GameEventPublisher.SendPlayerDeath(
                player,
                null,
                new UnityEngine.Vector3(1, 2, 3),
                characterEnded: true
            );
            AssertPublishedEvent("player-death", out JObject death);
            True((bool)death["characterEnded"], "a Permanent Death ends the character");
            Equal("eos-1", (string)death["player"]["character"]["id"], "the dying character");
        }
        finally
        {
            StateMirror.Instance.PermanentDeath = false;
        }
    }
}
