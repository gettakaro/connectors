using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using System.Text;
using UnityEngine;

namespace Takaro
{

    public class CommandResult : IConsoleConnection
    {
        private readonly TaskCompletionSource<string> _tcs;
        private readonly string command;

        public CommandResult(string command, TaskCompletionSource<string> tcs)
        {
            this.command = command;
            _tcs = tcs;
        }

        public void SendLines(List<string> output)
        {
            StringBuilder sb = new StringBuilder();
            foreach (string line in output)
            {
                sb.AppendLine(line);
            }
            string result = sb.ToString();
            _tcs.SetResult(result);
        }

        public void SendLine(string _text)
        {
            // Empty implementation
        }

        public void SendLog(string formattedMessage, string plainMessage, string trace, LogType type, DateTime timestamp, long uptime)
        {
            // Empty implementation
        }

        public void EnableLogLevel(LogType type, bool enable)
        {
            // Empty implementation
        }

        public string GetDescription()
        {
            return $"WebCommandResult_for_{command}";
        }
    }
}