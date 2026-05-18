using System;
using System.Collections.Generic;
using Takaro.Services;

namespace Takaro.Commands
{
  internal class Debug : ConsoleCmdAbstract
  {
    public override string getDescription() => $"[{API.ModPrefix}] - Toggle Debug Mode";

    public override string GetHelp() => "Usage:\ntakaro-debug\n";

		public override string[] getCommands() => new string[1]
    {
      "takaro-debug"
    };

    public override void Execute(List<string> _params, CommandSenderInfo _senderInfo)
    {
      try
      {
        LogService.Instance.LogDebug = !LogService.Instance.LogDebug;
      }
      catch (Exception ex)
      {
        LogService.Instance.Error($"Error in debug.Execute: {ex.Message}");
      }
    }
  }
}
