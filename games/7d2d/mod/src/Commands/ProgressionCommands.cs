using System.Collections.Generic;
using Takaro.Commands.Contract;

namespace Takaro.Commands
{
    /// <summary>
    /// resetlevel/rl and resetskillpoints/rs. The value is set on the live player entity and
    /// pushed to every client at once with the game's own NetPackagePlayerStats, which carries
    /// the whole Progression when it is marked changed; the player's own client replaces its
    /// local progression with it, so the HUD and the skills window update without a relog,
    /// and the client's next save writes the new value.
    /// </summary>
    internal abstract class ProgressionCommand : PrismaCoreCommand
    {
        protected abstract ProgressionContract.Spec Spec { get; }

        protected abstract void Apply(Progression progression, int value);

        public override string[] getCommands() => Spec.Names;

        protected override string Usage =>
            $"Usage:\n  {Spec.Names[0]} <player> <{Spec.ValueName}>\n"
            + "<player> is an entity id, a player name or a platform id (EOS_, Steam_, XBL_, PSN_).\n"
            + "The player must be online.";

        protected override List<string> Run(List<string> args)
        {
            string error = ProgressionContract.Validate(Spec, args, out string id, out int value);
            if (error != null)
                return Lines(error);

            PlayerTarget target = PlayerTarget.Resolve(id, out string failure);
            if (target == null)
                return Lines(failure);
            if (!target.IsOnline)
                return Lines(Spec.OfflineLine);

            EntityPlayer player = target.Entity;
            Apply(player.Progression, value);
            player.Progression.bProgressionStatsChanged = true;
            ConnectionManager.Instance.SendPackage(
                NetPackageManager.GetPackage<NetPackagePlayerStats>().Setup(player)
            );
            return Lines(Spec.SuccessLine(value));
        }
    }

    internal sealed class ResetLevel : ProgressionCommand
    {
        protected override ProgressionContract.Spec Spec => ProgressionContract.ResetLevel;

        protected override string Description => "Set an online player's level";

        protected override void Apply(Progression progression, int value)
        {
            progression.Level = value;
            progression.ExpToNextLevel = progression.GetExpForNextLevel();
        }
    }

    internal sealed class ResetSkillPoints : ProgressionCommand
    {
        protected override ProgressionContract.Spec Spec => ProgressionContract.ResetSkillPoints;

        protected override string Description => "Set an online player's unspent skill points";

        protected override void Apply(Progression progression, int value)
        {
            progression.SkillPoints = value;
        }
    }
}
