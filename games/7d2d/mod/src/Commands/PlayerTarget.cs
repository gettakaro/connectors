using Takaro.Persistence;
using Takaro.Services;

namespace Takaro.Commands
{
    /// <summary>
    /// The shared player-id resolver for console commands (main thread only). Online players
    /// resolve through the game's own console parsing (entity id, name, partial name, EOS_,
    /// Steam_, XBL_, PSN_ user ids); everyone else through the player registry. The decision
    /// and the failure lines are <see cref="PlayerIdResolver"/>.
    /// </summary>
    internal sealed class PlayerTarget
    {
        public ClientInfo Client;
        public EntityPlayer Entity;
        public PlayerRecord Registered;

        /// <summary>Online and spawned: the live entity can be changed.</summary>
        public bool IsOnline => Entity != null;

        public static PlayerTarget Resolve(string id, out string failureLine)
        {
            PlayerIdResolver.Resolution<ClientInfo> resolution =
                PlayerIdResolver.Resolve<ClientInfo>(
                    id,
                    FindOnline,
                    StateMirror.Instance.GetRegisteredPlayers
                );
            failureLine = resolution.FailureLine;
            if (!resolution.Found)
                return null;
            if (resolution.Online == null)
                return new PlayerTarget { Registered = resolution.Registered };

            GameManager.Instance.World.Players.dict.TryGetValue(
                resolution.Online.entityId,
                out EntityPlayer entity
            );
            return new PlayerTarget { Client = resolution.Online, Entity = entity };
        }

        private static int FindOnline(string id, out ClientInfo client) =>
            ConsoleHelper.ParseParamPartialNameOrId(
                id,
                out PlatformUserIdentifierAbs _,
                out client,
                false
            );
    }
}
