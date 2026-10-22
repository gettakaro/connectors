using System;
using System.Collections.Generic;
using Takaro.Persistence;

namespace Takaro.Services
{
    /// <summary>
    /// The game-free half of the shared player-id resolver used by console commands. A
    /// command argument is an entity id, a player name or a platform id (EOS_, Steam_, XBL_,
    /// PSN_). Online players are resolved by the game's own console parsing
    /// (<c>Commands/PlayerTarget.cs</c>); this class resolves the rest against the player
    /// registry (the durable players collection) and owns the game's failure lines, so the
    /// output stays the one StaffCommands and other tools match on.
    /// </summary>
    public static class PlayerIdResolver
    {
        public enum IdForm
        {
            EntityId,
            PlatformId,
            Name,
        }

        public enum Outcome
        {
            Found,
            NotFound,
            Ambiguous,
        }

        public static readonly string[] PlatformPrefixes = { "EOS_", "Steam_", "XBL_", "PSN_" };

        /// <summary>
        /// The game's console lookup of online players: returns how many online players
        /// match and sets <paramref name="client"/> when exactly one does.
        /// </summary>
        public delegate int OnlineLookup<TClient>(string id, out TClient client)
            where TClient : class;

        public sealed class Resolution<TClient>
            where TClient : class
        {
            public TClient Online;
            public PlayerRecord Registered;
            public string FailureLine;

            public bool Found => FailureLine == null;
        }

        /// <summary>
        /// Online first, through the game's lookup (entity id, name, partial name, user id);
        /// then the registry. A failure carries the game's own line.
        /// </summary>
        public static Resolution<TClient> Resolve<TClient>(
            string id,
            OnlineLookup<TClient> online,
            Func<IEnumerable<PlayerRecord>> registry
        )
            where TClient : class
        {
            int matches = online(id, out TClient client);
            if (client != null)
                return new Resolution<TClient> { Online = client };
            if (matches > 1)
                return new Resolution<TClient> { FailureLine = AmbiguousLine(id) };

            switch (ResolveRegistered(id, registry(), out PlayerRecord record))
            {
                case Outcome.Found:
                    return new Resolution<TClient> { Registered = record };
                case Outcome.Ambiguous:
                    return new Resolution<TClient> { FailureLine = AmbiguousLine(id) };
                default:
                    return new Resolution<TClient> { FailureLine = NotFoundLine(id) };
            }
        }

        public static IdForm Classify(string id)
        {
            if (int.TryParse(id, out _))
                return IdForm.EntityId;
            foreach (string prefix in PlatformPrefixes)
            {
                if (
                    id.Length > prefix.Length
                    && id.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
                )
                    return IdForm.PlatformId;
            }
            return IdForm.Name;
        }

        /// <summary>
        /// Finds a registered player by any id form. Platform ids match the stored EOS game
        /// id, Steam id, Xbox id or native platform id; names match case-insensitively and
        /// must be unique.
        /// </summary>
        public static Outcome ResolveRegistered(
            string id,
            IEnumerable<PlayerRecord> registry,
            out PlayerRecord match
        )
        {
            match = null;
            if (string.IsNullOrEmpty(id))
                return Outcome.NotFound;

            switch (Classify(id))
            {
                case IdForm.EntityId:
                    int entityId = int.Parse(id);
                    foreach (PlayerRecord record in registry)
                    {
                        if (record.EntityId == entityId)
                        {
                            match = record;
                            return Outcome.Found;
                        }
                    }
                    return Outcome.NotFound;

                case IdForm.PlatformId:
                    foreach (PlayerRecord record in registry)
                    {
                        if (MatchesPlatformId(record, id))
                        {
                            match = record;
                            return Outcome.Found;
                        }
                    }
                    return Outcome.NotFound;

                default:
                    int count = 0;
                    foreach (PlayerRecord record in registry)
                    {
                        if (!string.Equals(record.Name, id, StringComparison.OrdinalIgnoreCase))
                            continue;
                        count++;
                        match = record;
                    }
                    if (count == 1)
                        return Outcome.Found;
                    match = null;
                    return count == 0 ? Outcome.NotFound : Outcome.Ambiguous;
            }
        }

        public static bool MatchesPlatformId(PlayerRecord record, string id)
        {
            int separator = id.IndexOf('_');
            string prefix = id.Substring(0, separator + 1);
            string value = id.Substring(separator + 1);
            if (string.Equals(prefix, "EOS_", StringComparison.OrdinalIgnoreCase))
                return string.Equals(record.GameId, value, StringComparison.OrdinalIgnoreCase);
            if (string.Equals(prefix, "Steam_", StringComparison.OrdinalIgnoreCase))
                return record.SteamId != null && record.SteamId == value;
            if (string.Equals(prefix, "XBL_", StringComparison.OrdinalIgnoreCase))
                return record.XboxLiveId != null
                    && string.Equals(record.XboxLiveId, value, StringComparison.OrdinalIgnoreCase);
            return record.PlatformId != null
                && string.Equals(record.PlatformId, id, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>The game's own line (ConsoleHelper.ParseParamPartialNameOrId).</summary>
        public static string NotFoundLine(string id) =>
            "\"" + id + "\" is not a valid entity id, player name or user id.";

        /// <summary>The game's own line for a name that matches several players.</summary>
        public static string AmbiguousLine(string id) =>
            "\"" + id + "\" matches multiple player names.";
    }
}
