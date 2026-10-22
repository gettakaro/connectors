using System.Collections.Generic;
using Takaro.Commands.Contract;

namespace Takaro.Commands
{
    /// <summary>
    /// isbloodmoon: true from dusk on the blood moon day until dawn the day after, by the
    /// game's own rule (GameUtils.IsBloodMoonTime) over the game's blood moon day and the
    /// dusk/dawn hours of the configured day length.
    /// </summary>
    internal sealed class IsBloodMoon : PrismaCoreCommand
    {
        public override string[] getCommands() => BloodMoonContract.Names;

        protected override string Description => "Print whether the blood moon is active";

        protected override string Usage => "Usage:\n  isbloodmoon\nPrints true or false.";

        protected override List<string> Run(List<string> args)
        {
            World world = GameManager.Instance.World;
            bool active = GameUtils.IsBloodMoonTime(
                world.worldTime,
                GameUtils.CalcDuskDawnHours(GameStats.GetInt(EnumGameStats.DayLightLength)),
                GameStats.GetInt(EnumGameStats.BloodMoonDay)
            );
            return Lines(BloodMoonContract.Line(active));
        }
    }
}
