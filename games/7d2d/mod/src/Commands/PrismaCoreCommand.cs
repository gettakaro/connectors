using System;
using System.Collections.Generic;
using Takaro.Services;

namespace Takaro.Commands
{
    /// <summary>
    /// Base of every PrismaCore-compatible console command. Conventions (DEVELOPMENT.md,
    /// "Console commands"): plain PrismaCore names and aliases, the output contract in a
    /// game-free <c>Contract/</c> class, and all game-state access on the main thread.
    /// SdtdConsole runs Execute on the main thread for the console, telnet, the web dashboard
    /// and Takaro's executeConsoleCommand; a caller on another thread (ExecuteSync) is
    /// marshalled through the connector's MainThreadDispatcher and waited for. No command
    /// starts a thread or sleeps.
    /// </summary>
    internal abstract class PrismaCoreCommand : ConsoleCmdAbstract
    {
        private static readonly TimeSpan OffThreadTimeout = TimeSpan.FromSeconds(30);

        protected abstract string Description { get; }

        protected abstract string Usage { get; }

        /// <summary>Runs on the game main thread and returns the command's output lines.</summary>
        protected abstract List<string> Run(List<string> args);

        public override string getDescription() => $"[{API.ModPrefix}] - {Description}";

        public override string GetHelp() => Usage;

        public override void Execute(List<string> _params, CommandSenderInfo _senderInfo)
        {
            List<string> lines;
            if (ThreadManager.IsMainThread())
            {
                lines = Run(_params);
            }
            else
            {
                var pending = MainThreadDispatcher.Instance.Run(() => Run(_params));
                if (!pending.Wait(OffThreadTimeout))
                    throw new TimeoutException(
                        $"the main thread did not run '{GetCommands()[0]}' within {OffThreadTimeout.TotalSeconds:0} s"
                    );
                lines = pending.Result;
            }

            foreach (string line in lines)
                SdtdConsole.Instance.Output(line);
        }

        protected static List<string> Lines(string line) => new List<string> { line };
    }
}
