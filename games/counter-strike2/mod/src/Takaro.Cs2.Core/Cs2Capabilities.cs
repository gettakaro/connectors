namespace Takaro.Cs2.Core;

public static class Cs2Capabilities
{
    private static readonly string[] Always =
    [
        "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
        "items.list", "items.give", "entities.list", "chat.send", "console.execute",
        "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans",
        "events.log", "events.player-connected", "events.player-disconnected", "events.chat-message",
        "events.player-death", "events.entity-killed"
    ];

    public static IReadOnlyList<object> Build(bool allowShutdown, object? mapDimension, bool hasLocations, string? gameVersion)
    {
        var manifest = Always.Select(name => (object)new { name }).ToList();
        manifest.Add(new { name = "players.progression", parameters = new { source = "client-reported" } });
        if (allowShutdown) manifest.Add(new { name = "server.shutdown" });
        if (hasLocations) manifest.Add(new { name = "locations.list" });
        if (mapDimension is not null)
            manifest.Add(new { name = "map", parameters = new { dimensions = new[] { mapDimension } } });
        manifest.Add(string.IsNullOrWhiteSpace(gameVersion)
            ? new { name = "game.profile", parameters = (object)new { name = "Counter-Strike 2" } }
            : new { name = "game.profile", parameters = (object)new { name = "Counter-Strike 2", version = gameVersion } });
        return manifest;
    }
}
