// Research probe for gettakaro/connectors#197: can catalog-provided Takaro commands be
// native, silent chat commands on Carbon and Oxide? Not part of the connector, not
// released, not deployed by any script. It does nothing until StubUrl is set in its config.
//
// It polls a local stub (stub/stub.mjs) for a small command catalog, registers each name
// as a direct chat root when that root is free, always offers the namespaced fallback
// `/takaro <name>`, and routes every invocation through one callback that takes the
// caller from the framework's BasePlayer. Replies go to the caller only.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Oxide.Core.Libraries;

namespace Oxide.Plugins
{
    [Info("TakaroCommandProbe", "Takaro", "0.0.1")]
    [Description("Research probe for native Takaro chat commands (issue #197). Off unless StubUrl is set.")]
    public class TakaroCommandProbe : RustPlugin
    {
        private const string NamespaceRoot = "takaro";
        private const string Tag = "[Takaro probe]";

        private class ProbeConfig
        {
            public string StubUrl = "";
            public float PollSeconds = 2f;
            public float InvokeTimeoutSeconds = 5f;
            // Research only: register catalog roots without the foreign-owner preflight, to
            // record what each framework does with a duplicate name.
            public bool ForceRegisterConflicts = false;
        }

        private ProbeConfig _config;
        private int _mainThreadId;
        private bool _backendOnline;
        private long _catalogRevision = -1;
        private readonly HashSet<string> _catalog = new HashSet<string>();
        private readonly HashSet<string> _ownedRoots = new HashSet<string>();
        private bool _namespaceRootOwned;
        private bool _pollInFlight;
        // Names a player can type as one chat command token.
        private static readonly System.Text.RegularExpressions.Regex ValidName =
            new System.Text.RegularExpressions.Regex("^[a-z0-9_]{1,32}$");

        protected override void LoadDefaultConfig() => Config.WriteObject(new ProbeConfig(), true);

        private void Init()
        {
            _mainThreadId = System.Threading.Thread.CurrentThread.ManagedThreadId;
            _config = Config.ReadObject<ProbeConfig>() ?? new ProbeConfig();
            if (string.IsNullOrEmpty(_config.StubUrl))
            {
                Puts($"{Tag} StubUrl is empty: probe disabled");
                return;
            }
            Puts($"{Tag} enabled framework={Framework()} stub={_config.StubUrl} force={_config.ForceRegisterConflicts}");
            _namespaceRootOwned = TryClaimRoot(NamespaceRoot);
            timer.Every(_config.PollSeconds, PollCatalog);
            PollCatalog();
        }

        private void Unload()
        {
            // Both frameworks also drop a plugin's commands on unload; this logs what was held.
            foreach (var root in _ownedRoots.ToList())
                cmd.RemoveChatCommand(root, this);
            if (_namespaceRootOwned)
                cmd.RemoveChatCommand(NamespaceRoot, this);
            Puts($"{Tag} unloaded, released roots=[{string.Join(",", _ownedRoots)}] namespace={_namespaceRootOwned}");
        }

        private static string Framework()
        {
#if CARBON
            return "carbon";
#else
            return "oxide";
#endif
        }

        private string ThreadNote() =>
            System.Threading.Thread.CurrentThread.ManagedThreadId == _mainThreadId ? "main" : "OFF-MAIN";

        // ---- catalog -------------------------------------------------------------------

        private void PollCatalog()
        {
            // One poll at a time, so a late answer can never roll the catalog back.
            if (_pollInFlight) return;
            _pollInFlight = true;
            webrequest.Enqueue($"{_config.StubUrl}/catalog", null, (code, body) =>
            {
                _pollInFlight = false;
                long revision = 0;
                List<string> names = null;
                if (code == 200)
                {
                    try
                    {
                        var doc = JObject.Parse(body);
                        revision = doc.Value<long>("revision");
                        var raw = (doc["commands"] as JArray ?? new JArray())
                            .Select(c => (c.Value<string>("name") ?? "").Trim().ToLowerInvariant())
                            .Where(n => n != NamespaceRoot)
                            .ToList();
                        foreach (var bad in raw.Where(n => !ValidName.IsMatch(n)))
                            PrintWarning($"{Tag} catalog name '{bad}' is not a typable command, ignored");
                        names = raw.Where(n => ValidName.IsMatch(n)).ToList();
                    }
                    catch (Exception ex)
                    {
                        PrintWarning($"{Tag} bad catalog: {ex.Message}");
                    }
                }
                // Online only when a valid catalog arrived; otherwise commands fail privately.
                SetBackendOnline(names != null, code);
                if (names == null) return;
                if (revision > _catalogRevision) Reconcile(revision, names);
                else if (revision < _catalogRevision) Puts($"{Tag} ignored older catalog rev={revision} (have {_catalogRevision})");
                VerifyOwnership();
            }, this, RequestMethod.GET, null, _config.InvokeTimeoutSeconds);
        }

        private void SetBackendOnline(bool online, int code)
        {
            if (online == _backendOnline) return;
            _backendOnline = online;
            Puts($"{Tag} backend {(online ? "online" : "offline")} (code {code}) thread={ThreadNote()}");
        }

        private void Reconcile(long revision, List<string> names)
        {
            var desired = new HashSet<string>(names);
            var removed = _catalog.Where(n => !desired.Contains(n)).ToList();
            var added = desired.Where(n => !_catalog.Contains(n)).ToList();

            foreach (var name in removed)
            {
                _catalog.Remove(name);
                if (_ownedRoots.Remove(name)) cmd.RemoveChatCommand(name, this);
            }
            foreach (var name in added)
            {
                _catalog.Add(name);
                _ownedRoots.Remove(name);
                if (TryClaimRoot(name)) _ownedRoots.Add(name);
            }
            _catalogRevision = revision;
            Puts($"{Tag} catalog rev={revision} thread={ThreadNote()} added=[{string.Join(",", added)}] removed=[{string.Join(",", removed)}] direct=[{string.Join(",", _ownedRoots.OrderBy(n => n))}] fallback={(_namespaceRootOwned ? $"/{NamespaceRoot} <name>" : "NONE")}");
        }

        // A root another plugin owns is never displaced: it stays theirs, and the command
        // stays reachable through the namespaced fallback.
        private bool TryClaimRoot(string name)
        {
            string owner = ForeignOwner(name);
            if (owner != null && !_config.ForceRegisterConflicts)
            {
                string route = name == NamespaceRoot ? "no fallback root" : _namespaceRootOwned ? $"use /{NamespaceRoot} {name}" : "NO ROUTE (namespace root not held)";
                Puts($"{Tag} conflict root=/{name} owner={owner}: skipped, {route}");
                return false;
            }
            cmd.AddChatCommand(name, this, (player, command, args) => OnInvoke(player, command, args));
            string after = ForeignOwner(name);
            bool owned = after == null;
            Puts($"{Tag} register root=/{name} preflightOwner={owner ?? "none"} ownerAfter={after ?? "probe"} owned={owned}");
            return owned;
        }

        // Every poll: notice roots another plugin took over, and claim roots that became free.
        private void VerifyOwnership()
        {
            if (_namespaceRootOwned && ForeignOwner(NamespaceRoot) != null)
            {
                _namespaceRootOwned = false;
                Puts($"{Tag} namespace root /{NamespaceRoot} was taken over by {ForeignOwner(NamespaceRoot)}; no fallback route");
            }
            else if (!_namespaceRootOwned && RegistryOwner(NamespaceRoot) == null)
            {
                _namespaceRootOwned = TryClaimRoot(NamespaceRoot);
            }
            foreach (var name in _ownedRoots.ToList())
            {
                string owner = ForeignOwner(name);
                if (owner == null) continue;
                _ownedRoots.Remove(name);
                Puts($"{Tag} root=/{name} was taken over by {owner}; fallback {(_namespaceRootOwned ? $"/{NamespaceRoot} {name}" : "NONE")}");
            }
            foreach (var name in _catalog.Where(n => !_ownedRoots.Contains(n)).ToList())
            {
                if (RegistryOwner(name) != null) continue;
                if (TryClaimRoot(name)) _ownedRoots.Add(name);
            }
        }

        // RCON: takaroprobe.status <name> [name ...] prints who the framework says owns each root.
        [ConsoleCommand("takaroprobe.status")]
        private void StatusCommand(ConsoleSystem.Arg arg)
        {
            if (arg.Connection != null) return;
            var names = arg.FullString.ToString().Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            var lines = names.Select(n => $"/{n.ToLowerInvariant()} owner={RegistryOwner(n.ToLowerInvariant()) ?? "free"}");
            arg.ReplyWith($"{Tag} rev={_catalogRevision} backendOnline={_backendOnline} catalog=[{string.Join(",", _catalog)}] direct=[{string.Join(",", _ownedRoots)}]\n" + string.Join("\n", lines));
        }

        private string ForeignOwner(string name)
        {
            string owner = RegistryOwner(name);
            return owner == "probe" ? null : owner;
        }

        // The plugin that owns `name` as a chat command: "probe" for this plugin, null when
        // free, "preflight-unavailable" when the framework hides it.
        private string RegistryOwner(string name)
        {
#if CARBON
            if (Carbon.Community.Runtime.CommandManager.Contains(Carbon.Community.Runtime.CommandManager.Chat, name, out var existing))
                return existing.Reference == this ? "probe" : (existing.Reference?.ToString() ?? "unknown");
            return null;
#else
            try
            {
                var chatCommands = cmd.GetType().GetField("chatCommands", BindingFlags.Instance | BindingFlags.NonPublic)?.GetValue(cmd) as IDictionary;
                string chatOwner = null;
                if (chatCommands != null && chatCommands.Contains(name))
                {
                    var owner = chatCommands[name].GetType().GetField("Plugin")?.GetValue(chatCommands[name]) as Oxide.Core.Plugins.Plugin;
                    chatOwner = owner == this ? "probe" : owner?.Name ?? "unknown";
                    if (chatOwner != "probe") return chatOwner;
                }
                // Covalence commands are dispatched before chat commands, so they shadow ours.
                var covalence = typeof(Oxide.Game.Rust.RustCore).GetField("Covalence", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public)?.GetValue(null);
                var system = covalence?.GetType().GetProperty("CommandSystem", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)?.GetValue(covalence);
                var registered = system?.GetType().GetField("registeredCommands", BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public)?.GetValue(system) as IDictionary;
                if (registered != null && registered.Contains(name))
                {
                    var source = registered[name].GetType().GetField("Source")?.GetValue(registered[name]) as Oxide.Core.Plugins.Plugin;
                    return "covalence:" + (source?.Name ?? "unknown");
                }
                if (chatCommands == null || registered == null) return "preflight-unavailable";
                return chatOwner;
            }
            catch (Exception ex)
            {
                PrintWarning($"{Tag} preflight failed: {ex.Message}");
                return "preflight-unavailable";
            }
#endif
        }

        // ---- invocation ----------------------------------------------------------------

        private void OnInvoke(BasePlayer player, string command, string[] args)
        {
            if (player == null) return;
            string root = (command ?? "").ToLowerInvariant();
            string name = root;
            var rest = args ?? new string[0];
            if (root == NamespaceRoot)
            {
                if (rest.Length == 0) { Reply(player, $"usage: /{NamespaceRoot} <command> [args]"); return; }
                name = rest[0].ToLowerInvariant();
                rest = rest.Skip(1).ToArray();
            }

            // The caller is always the framework's player. Nothing the player types and
            // nothing the backend answers can change it.
            string callerId = player.UserIDString;
            string invocationId = Guid.NewGuid().ToString("N");
            Puts($"{Tag} invoke id={invocationId} root=/{root} command={name} args=[{string.Join("|", rest)}] caller={callerId} thread={ThreadNote()}");

            if (!_catalog.Contains(name))
            {
                Reply(player, $"Unknown Takaro command '{name}'.");
                Puts($"{Tag} outcome id={invocationId} unknown");
                return;
            }
            if (!_backendOnline)
            {
                Reply(player, "Takaro is unavailable right now. Your command was not run.");
                Puts($"{Tag} outcome id={invocationId} offline-not-sent");
                return;
            }

            var body = new JObject
            {
                ["invocationId"] = invocationId,
                ["command"] = name,
                ["args"] = new JArray(rest),
                ["root"] = root,
                ["catalogRevision"] = _catalogRevision,
                ["caller"] = new JObject { ["steamId"] = callerId, ["name"] = player.displayName },
            }.ToString(Formatting.None);

            webrequest.Enqueue($"{_config.StubUrl}/invoke", body, (code, response) =>
            {
                // Runs later: the caller may have left. Never fall back to broadcast.
                bool connected = player != null && player.IsConnected;
                if (code != 200)
                {
                    // The request was sent, so Takaro may have run it before the answer was lost.
                    Puts($"{Tag} outcome id={invocationId} backend-no-answer code={code} callerConnected={connected} thread={ThreadNote()}");
                    if (connected) Reply(player, "Takaro did not answer. Your command may or may not have run; check before trying again.");
                    return;
                }
                string status, reply;
                try
                {
                    var doc = JObject.Parse(response);
                    status = doc.Value<string>("status") ?? "error";
                    reply = doc.Value<string>("reply") ?? "";
                }
                catch (Exception ex)
                {
                    Puts($"{Tag} outcome id={invocationId} bad-response {ex.Message}");
                    if (connected) Reply(player, "Takaro returned an unreadable answer.");
                    return;
                }
                Puts($"{Tag} outcome id={invocationId} status={status} callerConnected={connected} replyTo={callerId} thread={ThreadNote()}");
                if (connected) Reply(player, reply);
                else Puts($"{Tag} outcome id={invocationId} reply dropped: caller {callerId} disconnected");
            }, this, RequestMethod.POST, new Dictionary<string, string> { ["Content-Type"] = "application/json" }, _config.InvokeTimeoutSeconds);
        }

        private void Reply(BasePlayer player, string text) => player.ChatMessage($"{Tag} {text}");
    }
}
