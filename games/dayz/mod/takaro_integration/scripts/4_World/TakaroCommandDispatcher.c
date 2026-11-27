// Inbound command pipeline.
// Takaro hands the bridge a list of pending operations via:
//   POST {api}/gameserver/{id}/poll  -> returns array of TakaroOperation
// We dispatch each operation to a handler, run it, and POST the result to:
//   POST {api}/gameserver/{id}/operation/{operationId}/result

class TakaroOperation
{
    string operationId;
    string action;          // testReachability, listPlayers, sendMessage, kickPlayer, ...
    string argsJson;        // raw JSON args; each handler parses what it needs
}

class TakaroOperationsResponse
{
    ref array<ref TakaroOperation> operations;

    void TakaroOperationsResponse()
    {
        operations = new array<ref TakaroOperation>;
    }
}

class TakaroOperationResult
{
    string operationId;
    bool ok;
    string resultJson;       // success payload as JSON
    string errorMessage;
}

// ---- Action arg shapes -------------------------------------------------

// Takaro's wire shape for sendMessage args is `{message, opts:{recipient:{gameId},
// senderNameOverride}}` (lib-gameserver IMessageOptsDTO + IPlayerReferenceDTO).
// The DLL forwards this argsJson verbatim, so the DTOs here have to mirror the
// nested shape — Enforce JsonSerializer.ReadFromString fails parsing when the
// payload contains a nested object that doesn't map to a `ref ClassName` field
// (which silently broke /link: PM was never delivered, op came back ok=false).
class ArgsSendMessageRecipient
{
    string gameId;
}

class ArgsSendMessageOpts
{
    ref ArgsSendMessageRecipient recipient;
    string senderNameOverride;
}

class ArgsSendMessage
{
    string message;
    ref ArgsSendMessageOpts opts;
}

class ArgsKickPlayer
{
    string gameId;
    string reason;
}

class ArgsBanPlayer
{
    string gameId;
    string reason;
    int durationSeconds;     // 0 = permanent (legacy)
    string expiresAt;        // Takaro ISO date, empty = permanent
}

class ArgsUnbanPlayer
{
    string gameId;
}

class ArgsTeleportPlayer
{
    string gameId;
    float x;
    float y;
    float z;
}

class ArgsGiveItem
{
    string gameId;
    string item;             // classname
    int amount;
    string quality;
}

class ArgsExecuteCommand
{
    string command;          // raw RCON-style command
}

class ArgsGetPlayer
{
    string gameId;
}

// ---- Result payload shapes --------------------------------------------

class ResultPlayerInfo
{
    string gameId;
    string name;
    string steamId;
    string platformId;
    string ip;
    int ping;
    bool online;
}

class ResultPlayersList
{
    ref array<ref ResultPlayerInfo> players;

    void ResultPlayersList()
    {
        players = new array<ref ResultPlayerInfo>;
    }
}

class ResultReachability
{
    bool connectable;
    string reason;
}

// ---- F5: config-driven map locations -----------------------------------
//
// DayZ has no stable script API for enumerating named world locations, so the
// list is CONFIG-DRIVEN: it is read from $profile:TakaroIntegration/locations.json
// and can be edited per server (Chernarus, Livonia, Sakhal, a modded map...)
// without touching the mod. The file is written with Chernarus defaults on
// first use.
//
// File shape (JsonFileLoader needs a class, so the array is wrapped):
//   { "locations": [ { "name": "Chernogorsk", "x": 6700, "y": 0, "z": 2600 }, ... ] }
//
// y = 0 means "ground level": DayZ world coordinates are (x, y=height, z), and
// 0 is a deliberate "unknown height / use terrain" marker, not sea level. Takaro
// only uses locations as named markers, so the height is not load-bearing.
class TakaroLocationEntry
{
    string name;
    float x;
    float y;
    float z;
}

class TakaroLocationsData
{
    ref array<ref TakaroLocationEntry> locations;

    void TakaroLocationsData()
    {
        locations = new array<ref TakaroLocationEntry>;
    }
}

// ---- Dispatcher --------------------------------------------------------

class TakaroCommandDispatcher
{
    TakaroHttpClient m_Http;

    void TakaroCommandDispatcher(TakaroHttpClient http)
    {
        m_Http = http;
    }

    void Dispatch(TakaroOperation op)
    {
        if (!op || op.action == "")
            return;

        TakaroLog.Debug("Dispatch: " + op.action + " (op " + op.operationId + ")");

        switch (op.action)
        {
            case "testReachability":
                HandleReachability(op);
                break;
            case "listPlayers":
            case "getPlayers":
                HandleListPlayers(op);
                break;
            case "getPlayer":
                HandleGetPlayer(op);
                break;
            case "getPlayerLocation":
                HandleGetPlayerLocation(op);
                break;
            case "getPlayerInventory":
                HandleGetPlayerInventory(op);
                break;
            case "sendMessage":
                HandleSendMessage(op);
                break;
            case "kickPlayer":
                HandleKick(op);
                break;
            case "banPlayer":
                HandleBan(op);
                break;
            case "unbanPlayer":
                HandleUnban(op);
                break;
            case "teleportPlayer":
                HandleTeleport(op);
                break;
            case "giveItem":
                HandleGiveItem(op);
                break;
            case "executeConsoleCommand":
            case "executeCommand":
                HandleExecCommand(op);
                break;
            case "shutdown":
                HandleShutdown(op);
                break;
            case "listItems":
                HandleListItems(op);
                break;
            case "listEntities":
                HandleListEntities(op);
                break;
            case "listLocations":
                HandleListLocations(op);
                break;
            case "listBans":
                HandleListBans(op);
                break;
            default:
                ReplyError(op, "Unknown action: " + op.action);
                break;
        }
    }

    // ---- handlers ------------------------------------------------------

    void HandleReachability(TakaroOperation op)
    {
        ResultReachability r = new ResultReachability();
        r.connectable = true;
        r.reason = "";
        ReplyOk(op, SerializeReachability(r));
    }

    void HandleListPlayers(TakaroOperation op)
    {
        ResultPlayersList list = new ResultPlayersList();
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (!id) continue;

            ResultPlayerInfo info = new ResultPlayerInfo();
            info.gameId = id.GetPlainId();
            info.name = TakaroNameCache.Resolve(id.GetPlainId(), id.GetName());
            if (TakaroEventFactory.IsSteamId64(id.GetPlainId())) info.steamId = id.GetPlainId();
            info.platformId = BuildPlatformId(id);
            info.ping = id.GetPingAct();
            info.online = true;
            list.players.Insert(info);
        }
        ReplyOk(op, SerializePlayersList(list));
    }

    void HandleGetPlayer(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (!id)
        {
            // F6: an offline player is a state, not an error. Takaro's
            // IGamePlayer requires gameId+name, so fall back to the persistent
            // name cache and finally to the gameId itself.
            ResultPlayerInfo offline = new ResultPlayerInfo();
            offline.gameId = args.gameId;
            string cachedName = TakaroNameCache.Resolve(args.gameId, "");
            if (cachedName == "") cachedName = args.gameId;
            offline.name = cachedName;
            if (TakaroEventFactory.IsSteamId64(args.gameId)) offline.steamId = args.gameId;
            offline.platformId = TakaroEventFactory.PlatformIdFor(args.gameId, "");
            offline.ping = 0;
            offline.online = false;
            ReplyOk(op, SerializePlayerInfo(offline));
            return;
        }
        ResultPlayerInfo info = new ResultPlayerInfo();
        info.gameId = id.GetPlainId();
        info.name = TakaroNameCache.Resolve(id.GetPlainId(), id.GetName());
        if (TakaroEventFactory.IsSteamId64(id.GetPlainId())) info.steamId = id.GetPlainId();
        info.platformId = BuildPlatformId(id);
        info.ping = id.GetPingAct();
        info.online = true;
        ReplyOk(op, SerializePlayerInfo(info));
    }

    void HandleGetPlayerInventory(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            // Empty inventory for offline player.
            ReplyOk(op, "[]");
            return;
        }
        // Walk the player's inventory and produce a Takaro IItemDTO array:
        //   [{name: "<classname>", code: "<classname>", amount: <int>, quality: <string>}]
        array<EntityAI> items = new array<EntityAI>;
        pb.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, items);
        string json = "[";
        bool first = true;
        int n = items.Count();
        for (int i = 0; i < n; i++)
        {
            EntityAI it = items[i];
            if (!it) continue;
            // M1: EnumerateInventory includes the root (the player body itself,
            // e.g. SurvivorF_Linda) — skip it.
            if (it == pb) continue;
            string code = it.GetType();
            if (code == "") continue;
            int amount = 1;
            // M1: only stackable items carry a count in GetQuantity(). For
            // food/liquids/batteries it is grams/ml/energy (Apple=125,
            // Chemlight=100), which is not an item count.
            ItemBase ib = ItemBase.Cast(it);
            Magazine mag = Magazine.Cast(it);
            if (mag && mag.IsAmmoPile()) amount = mag.GetAmmoCount();
            else if (ib && ib.ConfigGetBool("canBeSplit")) amount = (int)ib.GetQuantity();
            if (amount <= 0) amount = 1;
            if (!first) json += ",";
            first = false;
            string itemEntry = "{" + Quote("name") + ":" + Quote(code);
            itemEntry += "," + Quote("code") + ":" + Quote(code);
            itemEntry += "," + Quote("amount") + ":" + amount.ToString();
            itemEntry += "," + Quote("quality") + ":" + Quote("") + "}";
            json += itemEntry;
        }
        json += "]";
        ReplyOk(op, json);
    }

    void HandleGetPlayerLocation(TakaroOperation op)
    {
        ArgsGetPlayer args = new ArgsGetPlayer();
        if (!ParseGetPlayer(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            // Player is offline. We can't return null here — Takaro's generic
            // emitter does `if (!res) return null` after the connector strips
            // the response wrapper, but in practice the connector returns the
            // wrapper itself, and `new IPosition(wrapper)` fails isNumber on x.
            // Returning a zeroed IPosition keeps validation happy and matches
            // the "no known location" semantic well enough for downstream
            // consumers (eventWorker only uses it to record last-seen pos).
            ReplyOk(op, "{\"x\":0,\"y\":0,\"z\":0}");
            return;
        }
        vector pos = pb.GetPosition();
        // Build IPosition manually so we don't risk float-as-int rendering.
        string json = "{\"x\":" + pos[0].ToString() + ",\"y\":" + pos[1].ToString() + ",\"z\":" + pos[2].ToString() + "}";
        ReplyOk(op, json);
    }

    void HandleSendMessage(TakaroOperation op)
    {
        // Bypass JsonSerializer for this argsJson — Enforce's JSON parser
        // doesn't recurse into nested `ref ClassName` fields reliably, so
        // a flat-DTO read silently fails (returns false, prints
        // `JSON ERROR …takarohttpclient.c:38`) on Takaro's actual shape
        // `{message, opts:{recipient:{gameId},senderNameOverride}}`.
        // Manual extraction is the safe path: there's only one `gameId`
        // anywhere in this argsJson, so a top-level scan finds the
        // recipient regardless of nesting depth.
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return; }
        string message = ExtractJsonStringField(op.argsJson, "message");
        if (message == "") { ReplyError(op, "sendMessage: empty message"); return; }
        string recipientGameId = ExtractJsonStringField(op.argsJson, "gameId");
        // M2 fix: vanilla toast title was hard-coded "Discord". Use Takaro's
        // opts.senderNameOverride when present, else "Server".
        string senderTitle = ExtractJsonStringField(op.argsJson, "senderNameOverride");
        if (senderTitle == "") senderTitle = "Server";

        if (recipientGameId != "")
        {
            PlayerBase pb = FindPlayerByGameId(recipientGameId);
            if (!pb)
            {
                ReplyError(op, "Recipient not online: " + recipientGameId);
                return;
            }
            BroadcastSystemMessage(message, pb, senderTitle);
        }
        else
        {
            BroadcastSystemMessage(message, null, senderTitle);
        }
        ReplyOk(op, "{}");
    }

    void HandleKick(TakaroOperation op)
    {
        ArgsKickPlayer args = new ArgsKickPlayer();
        if (!ParseKick(op, args)) return;

        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (!id)
        {
            // F6: structured, machine-greppable message.
            ReplyError(op, "player " + args.gameId + " is not online");
            return;
        }
        KickWithReason(id, "Kicked", args.reason);
        ReplyOk(op, "{}");
    }

    // M3 fix: DisconnectPlayer bypasses MissionServer.PlayerDisconnected, so
    // no player-disconnected event reached Takaro on kick/ban, and the client
    // only saw the main menu with no reason. Emit the event, show the reason
    // as a notification, then disconnect after a short delay.

    void KickWithReason(PlayerIdentity id, string title, string reason)
    {
        if (!id) return;
        TakaroKicker.Get().Kick(FindPlayerByGameId(id.GetPlainId()), id, title, reason);
    }

    void HandleBan(TakaroOperation op)
    {
        ArgsBanPlayer args = new ArgsBanPlayer();
        if (!ParseBan(op, args)) return;

        // Bans are stored/enforced by TakaroBanStore (see TakaroBanStore.c).
        PlayerIdentity id = FindIdentityByGameId(args.gameId);
        if (id)
            KickWithReason(id, "Banned", args.reason);

        TakaroBanStore.Add(args.gameId, args.expiresAt, args.reason);
        TakaroLog.Info("banPlayer applied: " + args.gameId + " duration=" + args.durationSeconds.ToString() + "s reason=" + args.reason);
        ReplyOk(op, "{}");
    }

    void HandleUnban(TakaroOperation op)
    {
        ArgsUnbanPlayer args = new ArgsUnbanPlayer();
        if (!ParseUnban(op, args)) return;
        bool removed = TakaroBanStore.Remove(args.gameId);
        TakaroLog.Info("unbanPlayer applied: " + args.gameId + " (was banned: " + removed.ToString() + ")");
        ReplyOk(op, "{}");
    }

    void HandleTeleport(TakaroOperation op)
    {
        ArgsTeleportPlayer args = new ArgsTeleportPlayer();
        if (!ParseTeleport(op, args)) return;

        PlayerBase pb = FindPlayerByGameId(args.gameId);
        if (!pb)
        {
            // F6: structured, machine-greppable message.
            ReplyError(op, "player " + args.gameId + " is not online");
            return;
        }
        // Takaro x/y/z maps 1:1 to DayZ world x/y(height)/z. A y below the terrain
        // (e.g. y=0 from a 2D map click) is clamped to the surface so the player
        // never ends up underground; a y above ground is kept (fall damage).
        float groundY = GetGame().SurfaceY(args.x, args.z);
        float destY = args.y;
        if (destY < groundY) destY = groundY;
        vector destination = Vector(args.x, destY, args.z);
        pb.SetPosition(destination);
        ReplyOk(op, "{}");
    }

    void HandleGiveItem(TakaroOperation op)
    {
        // Bypass JsonSerializer — Takaro's wire shape for giveItem wraps the
        // recipient in `player: {gameId}`, so a flat-DTO read leaves
        // args.gameId empty and FindPlayerByGameId fails. Same pattern as
        // sendMessage (v0.1.7). Manual extraction is the safe path.
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return; }
        string gameId = ExtractJsonStringField(op.argsJson, "gameId");
        string itemClass = ExtractJsonStringField(op.argsJson, "item");
        string quality = ExtractJsonStringField(op.argsJson, "quality");
        int amount = ExtractJsonIntField(op.argsJson, "amount");
        if (amount <= 0) amount = 1;

        if (gameId == "") { ReplyError(op, "giveItem: missing gameId"); return; }
        if (itemClass == "") { ReplyError(op, "giveItem: missing item"); return; }

        PlayerBase pb = FindPlayerByGameId(gameId);
        if (!pb)
        {
            // F6: structured, machine-greppable message.
            ReplyError(op, "player " + gameId + " is not online");
            return;
        }
        for (int i = 0; i < amount; i++)
        {
            EntityAI item = pb.GetInventory().CreateInInventory(itemClass);
            if (!item)
            {
                // Drop on ground at player's feet as a fallback.
                GetGame().CreateObject(itemClass, pb.GetPosition());
            }
        }
        ReplyOk(op, "{}");
    }

    void HandleExecCommand(TakaroOperation op)
    {
        ArgsExecuteCommand args = new ArgsExecuteCommand();
        if (!ParseExec(op, args)) return;

        string trimmed = args.command;
        trimmed.TrimInPlace();

        string rawResult = "";
        bool success = true;

        // Tokenize on spaces — verb is the first token, rest is the argument.
        int firstSp = trimmed.IndexOf(" ");
        string verb = trimmed;
        string rest = "";
        if (firstSp >= 0)
        {
            verb = trimmed.Substring(0, firstSp);
            rest = trimmed.Substring(firstSp + 1, trimmed.Length() - firstSp - 1);
            rest.TrimInPlace();
        }
        // Verbs are case-insensitive — Help == help == HELP.
        verb.ToLower();

        if (verb == "say")
        {
            BroadcastSystemMessage(rest, null);
            rawResult = "Broadcast: " + rest;
        }
        else if (verb == "help")
        {
            rawResult = BuildHelpString();
        }
        else if (verb == "shutdown" || verb == "#shutdown")
        {
            rawResult = "Shutdown initiated";
            ReplyOk(op, BuildCommandOutput(rawResult, true));
            DisconnectAllAndExit();
            return;
        }
        else if (verb == "kick")
        {
            // kick <gameId> [reason]
            string kgid;
            string kreason;
            SplitFirstToken(rest, kgid, kreason);
            if (kgid == "") { rawResult = "Usage: kick <gameId> [reason]"; success = false; }
            else
            {
                PlayerIdentity kid = FindIdentityByGameId(kgid);
                if (!kid) { rawResult = "Player not online: " + kgid; success = false; }
                else { KickWithReason(kid, "Kicked", kreason); rawResult = "Kicked " + kgid; }
            }
        }
        else if (verb == "ban" || verb == "addban")
        {
            // ban <gameId> [reason]
            string bgid;
            string breason;
            SplitFirstToken(rest, bgid, breason);
            if (bgid == "") { rawResult = "Usage: ban <gameId> [reason]"; success = false; }
            else
            {
                PlayerIdentity bid = FindIdentityByGameId(bgid);
                if (bid) KickWithReason(bid, "Banned", breason);
                TakaroBanStore.Add(bgid, "", breason);
                rawResult = "Banned " + bgid;
            }
        }
        else if (verb == "unban" || verb == "removeban")
        {
            // unban <gameId>
            if (rest == "") { rawResult = "Usage: unban <gameId>"; success = false; }
            else { TakaroBanStore.Remove(rest); rawResult = "Unbanned " + rest; }
        }
        else if (verb == "tp" || verb == "teleport")
        {
            // tp <gameId> <x> <y> <z>
            array<string> tparts = new array<string>;
            SplitTokens(rest, tparts);
            if (tparts.Count() < 4) { rawResult = "Usage: tp <gameId> <x> <y> <z>"; success = false; }
            else
            {
                PlayerBase tpb = FindPlayerByGameId(tparts[0]);
                if (!tpb) { rawResult = "Player not online: " + tparts[0]; success = false; }
                else
                {
                    float tx = tparts[1].ToFloat();
                    float ty = tparts[2].ToFloat();
                    float tz = tparts[3].ToFloat();
                    tpb.SetPosition(Vector(tx, ty, tz));
                    rawResult = "Teleported " + tparts[0] + " to " + tx.ToString() + "," + ty.ToString() + "," + tz.ToString();
                }
            }
        }
        else if (verb == "visit")
        {
            // visit <requesterId> <targetId> [--force]
            // Teleports requester to target's current world position.
            // Requires both players to be in the same Expansion party
            // unless --force is supplied.
            array<string> vparts = new array<string>;
            SplitTokens(rest, vparts);
            bool vforce = false;
            for (int vi = 0; vi < vparts.Count(); vi++)
            {
                if (vparts[vi] == "--force") { vparts.Remove(vi); vforce = true; vi--; }
            }
            if (vparts.Count() < 2) { rawResult = "Usage: visit <requesterId> <targetId> [--force]"; success = false; }
            else
            {
                PlayerBase reqp = FindPlayerByGameId(vparts[0]);
                PlayerBase tgtp = FindPlayerByGameId(vparts[1]);
                if (!reqp) { rawResult = "Requester not online: " + vparts[0]; success = false; }
                else if (!tgtp) { rawResult = "Target not online: " + vparts[1]; success = false; }
                else
                {
                    bool vsameParty = false;
#ifdef EXPANSIONMOD
                    ExpansionPartyModule pmod;
                    if (CF_Modules<ExpansionPartyModule>.Get(pmod))
                    {
                        int reqPartyId = pmod.GetPartyID(reqp);
                        int tgtPartyId = pmod.GetPartyID(tgtp);
                        vsameParty = (reqPartyId != -1 && reqPartyId == tgtPartyId);
                    }
#endif
                    if (!vsameParty && !vforce)
                    {
                        rawResult = "Players are not in the same party. Use --force to bypass.";
                        success = false;
                    }
                    else
                    {
                        vector vpos = tgtp.GetPosition();
                        reqp.SetPosition(vpos);
                        rawResult = "Teleported " + vparts[0] + " to " + vparts[1] + " at " + vpos[0].ToString() + "," + vpos[1].ToString() + "," + vpos[2].ToString();
                        if (vforce && !vsameParty) rawResult += " (forced, not in same party)";
                    }
                }
            }
        }
        else if (verb == "give" || verb == "giveitem")
        {
            // give <gameId> <classname> [amount]
            array<string> gparts = new array<string>;
            SplitTokens(rest, gparts);
            if (gparts.Count() < 2) { rawResult = "Usage: give <gameId> <classname> [amount]"; success = false; }
            else
            {
                PlayerBase gpb = FindPlayerByGameId(gparts[0]);
                if (!gpb) { rawResult = "Player not online: " + gparts[0]; success = false; }
                else
                {
                    int gamount = 1;
                    if (gparts.Count() >= 3) { gamount = gparts[2].ToInt(); if (gamount <= 0) gamount = 1; }
                    int gcreated = 0;
                    for (int gi = 0; gi < gamount; gi++)
                    {
                        EntityAI gent = gpb.GetInventory().CreateInInventory(gparts[1]);
                        if (!gent) GetGame().CreateObject(gparts[1], gpb.GetPosition());
                        else gcreated++;
                    }
                    rawResult = "Gave " + gamount.ToString() + "x " + gparts[1] + " to " + gparts[0] + " (" + gcreated.ToString() + " in inventory, rest dropped)";
                }
            }
        }
        else if (verb == "parties" || verb == "listparties")
        {
            // List active Expansion parties and their online members.
#ifdef EXPANSIONMOD
            ExpansionPartyModule lpartyMod;
            if (!CF_Modules<ExpansionPartyModule>.Get(lpartyMod))
            {
                rawResult = "Expansion party module not loaded.";
                success = false;
            }
            else
            {
                array<Man> partyPpl = new array<Man>;
                GetGame().GetPlayers(partyPpl);
                map<int, ref array<string>> partyMap = new map<int, ref array<string>>;
                for (int ppi = 0; ppi < partyPpl.Count(); ppi++)
                {
                    PlayerBase ppartyPb = PlayerBase.Cast(partyPpl[ppi]);
                    if (!ppartyPb) continue;
                    PlayerIdentity ppartyId = ppartyPb.GetIdentity();
                    if (!ppartyId) continue;
                    int ppartyPid = lpartyMod.GetPartyID(ppartyPb);
                    if (ppartyPid == -1) continue;
                    array<string> ppartyList;
                    if (!partyMap.Find(ppartyPid, ppartyList))
                    {
                        ppartyList = new array<string>;
                        partyMap.Insert(ppartyPid, ppartyList);
                    }
                    ppartyList.Insert(ppartyId.GetName() + " (" + ppartyId.GetPlainId() + ")");
                }
                string partyNl = "\n";
                if (partyMap.Count() == 0) rawResult = "No active parties with online members.";
                else
                {
                    rawResult = "Active parties (" + partyMap.Count().ToString() + "):" + partyNl;
                    array<int> partyKeys = partyMap.GetKeyArray();
                    for (int pki = 0; pki < partyKeys.Count(); pki++)
                    {
                        int pkey = partyKeys[pki];
                        array<string> pmembers = partyMap.Get(pkey);
                        rawResult += "  party=" + pkey.ToString() + " (" + pmembers.Count().ToString() + ")" + partyNl;
                        for (int pmi = 0; pmi < pmembers.Count(); pmi++)
                            rawResult += "    " + pmembers[pmi] + partyNl;
                    }
                }
            }
#else
            rawResult = "Built without EXPANSIONMOD support.";
            success = false;
#endif
        }
        else if (verb == "party")
        {
            // party — list every online player with their party id (or "solo").
#ifdef EXPANSIONMOD
            ExpansionPartyModule sPartyMod;
            if (!CF_Modules<ExpansionPartyModule>.Get(sPartyMod))
            {
                rawResult = "Expansion party module not loaded.";
                success = false;
            }
            else
            {
                array<Man> spParty = new array<Man>;
                GetGame().GetPlayers(spParty);
                string spNl = "\n";
                rawResult = "Players (" + spParty.Count().ToString() + "):" + spNl;
                for (int spi = 0; spi < spParty.Count(); spi++)
                {
                    PlayerBase spPb = PlayerBase.Cast(spParty[spi]);
                    if (!spPb) continue;
                    PlayerIdentity spId = spPb.GetIdentity();
                    if (!spId) continue;
                    int spPid = sPartyMod.GetPartyID(spPb);
                    string spTag;
                    if (spPid == -1) spTag = "solo";
                    else spTag = "party " + spPid.ToString();
                    rawResult += "  " + spId.GetName() + " (" + spId.GetPlainId() + ") - " + spTag + spNl;
                }
            }
#else
            rawResult = "Built without EXPANSIONMOD support.";
            success = false;
#endif
        }
        else if (verb == "players" || verb == "listplayers")
        {
            rawResult = BuildPlayerListing();
        }
        else if (verb == "givehands")
        {
            // M3 test/admin verb: givehands <gameId> <classname>
            // Creates the item directly in the player's hands (e.g. FirefighterAxe),
            // so a melee test needs no inventory drag-and-drop.
            array<string> hparts = new array<string>;
            SplitTokens(rest, hparts);
            if (hparts.Count() < 2) { rawResult = "Usage: givehands <gameId> <classname>"; success = false; }
            else
            {
                PlayerBase hpb = FindPlayerByGameId(hparts[0]);
                if (!hpb) { rawResult = "Player not online: " + hparts[0]; success = false; }
                else
                {
                    EntityAI hent = hpb.GetHumanInventory().CreateInHands(hparts[1]);
                    if (!hent) { rawResult = "Could not create " + hparts[1] + " in hands (hands full?)"; success = false; }
                    else rawResult = "In hands: " + hparts[1];
                }
            }
        }
        else if (verb == "hitnearest")
        {
            // M3 test/admin verb: hitnearest <gameId> [radius] [ammo]
            // SYNTHETIC: applies lethal engine damage (ProcessDirectDamage) to the nearest
            // infected/animal, with the player's item in hands (or the player) as the
            // damage SOURCE. EEHitBy/EEKilled and the entity-killed emitter run for real;
            // the swing itself does not. Exists because injected attack input never
            // reaches DayZ on the test client. Evidence using it must say "synthetic".
            array<string> nparts = new array<string>;
            SplitTokens(rest, nparts);
            if (nparts.Count() < 1) { rawResult = "Usage: hitnearest <gameId> [radius] [ammo]"; success = false; }
            else
            {
                PlayerBase npb = FindPlayerByGameId(nparts[0]);
                if (!npb) { rawResult = "Player not online: " + nparts[0]; success = false; }
                else
                {
                    float nrad = 10.0;
                    if (nparts.Count() >= 2) nrad = nparts[1].ToFloat();
                    string nammo = "Bullet_762x54";
                    if (nparts.Count() >= 3) nammo = nparts[2];
                    array<Object> nobjs = new array<Object>;
                    GetGame().GetObjectsAtPosition3D(npb.GetPosition(), nrad, nobjs, null);
                    EntityAI ntarget = null;
                    float nbest = 999999.0;
                    for (int ni = 0; ni < nobjs.Count(); ni++)
                    {
                        EntityAI ne = EntityAI.Cast(nobjs[ni]);
                        if (!ne || !ne.IsAlive()) continue;
                        if (!ne.IsInherited(ZombieBase) && !ne.IsInherited(AnimalBase)) continue;
                        float nd = vector.Distance(ne.GetPosition(), npb.GetPosition());
                        if (nd < nbest) { nbest = nd; ntarget = ne; }
                    }
                    if (!ntarget) { rawResult = "No living infected/animal within " + nrad.ToString() + " m"; success = false; }
                    else
                    {
                        EntityAI nsource = npb.GetHumanInventory().GetEntityInHands();
                        if (!nsource) nsource = npb;
                        // No SetHealth fallback: that kills with killer=null and no EEHitBy, so
                        // nothing can be attributed. ProcessDirectDamage may apply next frame.
                        ntarget.ProcessDirectDamage(DamageType.CLOSE_COMBAT, nsource, "Head", nammo, "0 0 0", 100.0);
                        rawResult = "Hit " + ntarget.GetType() + " at " + nbest.ToString() + " m with source " + nsource.GetType() + " ammo " + nammo;
                    }
                }
            }
        }
        else if (verb == "spawnentity")
        {
            // M3 test/admin verb: spawnentity <classname> <gameId> [distanceMeters]
            // Spawns a creature (e.g. ZmbM_HermitSkinny_Beige, Animal_GallusGallusDomesticus)
            // on the surface in front of the player, with AI initialised. Used to
            // prove entity-killed / infected player-death without hunting for spawns.
            array<string> sparts = new array<string>;
            SplitTokens(rest, sparts);
            if (sparts.Count() < 2) { rawResult = "Usage: spawnentity <classname> <gameId> [distance]"; success = false; }
            else
            {
                PlayerBase spb = FindPlayerByGameId(sparts[1]);
                if (!spb) { rawResult = "Player not online: " + sparts[1]; success = false; }
                else if (!GetGame().ConfigIsExisting("CfgVehicles " + sparts[0])) { rawResult = "Unknown class: " + sparts[0]; success = false; }
                else
                {
                    float sdist = 3.0;
                    if (sparts.Count() >= 3) { sdist = sparts[2].ToFloat(); if (sdist <= 0) sdist = 3.0; }
                    vector sdir = spb.GetDirection();
                    sdir[1] = 0;
                    sdir.Normalize();
                    vector spos = spb.GetPosition() + (sdir * sdist);
                    spos[1] = GetGame().SurfaceY(spos[0], spos[2]);
                    Object sobj = GetGame().CreateObjectEx(sparts[0], spos, ECE_PLACE_ON_SURFACE | ECE_INITAI | ECE_EQUIP_ATTACHMENTS);
                    if (!sobj) { rawResult = "Spawn failed: " + sparts[0]; success = false; }
                    else rawResult = "Spawned " + sparts[0] + " at " + spos[0].ToString() + "," + spos[1].ToString() + "," + spos[2].ToString();
                }
            }
        }
        else if (verb == "announce" || verb == "banner")
        {
            // Loud center-screen banner via Expansion's BAGUETTE notification.
            // Format: `announce <title> | <body>`. If no `|` present, the
            // whole text becomes the body and title defaults to "Server".
            if (rest == "") { rawResult = "Usage: announce <title> | <message>  (or: announce <message>)"; success = false; }
            else
            {
                string atitle = "Server";
                string abody = rest;
                int barIdx = rest.IndexOf("|");
                if (barIdx > 0)
                {
                    atitle = rest.Substring(0, barIdx);
                    atitle.TrimInPlace();
                    abody = rest.Substring(barIdx + 1, rest.Length() - barIdx - 1);
                    abody.TrimInPlace();
                }
                BroadcastBanner(atitle, abody);
                rawResult = "Banner sent: [" + atitle + "] " + abody;
            }
        }
        else
        {
            rawResult = "Unknown command: " + verb + ". Type 'help' for the command list.";
            success = false;
        }
        ReplyOk(op, BuildCommandOutput(rawResult, success));
    }

    // Build the rich `players` / `listplayers` console output. Multi-line,
    // one player per row. Adds ping, hp, blood, position, and party tag
    // (when Expansion is loaded). Kept in a helper to avoid #ifdef inside
    // the dispatcher's switch-style chain (which Enforce's preprocessor
    // handles inconsistently when the conditional branch declares vars).
    string BuildPlayerListing()
    {
        array<Man> ppl = new array<Man>;
        GetGame().GetPlayers(ppl);
        // Hoist the Expansion module lookup out of the per-player loop —
        // CF_Modules<>.Get walks a registered-module list, no point doing
        // it once per row.
        bool haveParty = false;
#ifdef EXPANSIONMOD
        ExpansionPartyModule pmod;
        haveParty = CF_Modules<ExpansionPartyModule>.Get(pmod);
#endif
        string nl = "\n";
        int pCount = ppl.Count();
        string out_s = "Online (" + pCount.ToString() + "):" + nl;
        for (int pi = 0; pi < pCount; pi++)
        {
            PlayerBase ppb = PlayerBase.Cast(ppl[pi]);
            if (!ppb) continue;
            PlayerIdentity pid2 = ppb.GetIdentity();
            if (!pid2) continue;
            vector pos = ppb.GetPosition();
            int health = (int)ppb.GetHealth("", "Health");
            int blood = (int)ppb.GetHealth("", "Blood");
            int ping = pid2.GetPingAct();
            string partyTag = "";
#ifdef EXPANSIONMOD
            if (haveParty)
            {
                int rowPid = pmod.GetPartyID(ppb);
                if (rowPid != -1) partyTag = " party=" + rowPid.ToString();
            }
#endif
            out_s += "  " + pid2.GetName() + " (" + pid2.GetPlainId() + ")";
            out_s += " ping=" + ping.ToString() + "ms";
            out_s += " hp=" + health.ToString() + " blood=" + blood.ToString();
            out_s += " pos=(" + pos[0].ToString() + "," + pos[1].ToString() + "," + pos[2].ToString() + ")";
            out_s += partyTag;
            out_s += nl;
        }
        return out_s;
    }

    // Build the help string. One command per line with description. Kept as
    // many += statements rather than one big concat to dodge Enforce's
    // "Formula too complex" parser limit. Cached after first build — content
    // is static, so re-concatenating the ~14 segments per `help` invocation
    // is wasted work.
    static string s_HelpCache = "";
    string BuildHelpString()
    {
        if (s_HelpCache != "") return s_HelpCache;
        string nl = "\n";
        string h = "Available commands:" + nl;
        h += "  say <text>                          - broadcast a system message to all players" + nl;
        h += "  help                                - show this command list" + nl;
        h += "  shutdown                            - gracefully stop the server (kicks all, RequestExit)" + nl;
        h += "  kick <gameId> [reason]              - disconnect a player (gameId = Steam64)" + nl;
        h += "  ban <gameId> [reason]               - add to $profile:TakaroIntegration/bans.txt (enforced on join) and kick" + nl;
        h += "  unban <gameId>                      - remove the gameId from both ban files" + nl;
        h += "  tp <gameId> <x> <y> <z>             - teleport a player to world coordinates" + nl;
        h += "  visit <requesterId> <targetId> [--force]  - teleport requester to target (must be partied unless --force)" + nl;
        h += "  give <gameId> <classname> [amount]  - spawn item(s) into the player's inventory" + nl;
        h += "  players                             - list online players (name, id, ping, hp, blood, pos, party)" + nl;
        h += "  givehands <gameId> <classname>      - (test/admin) create an item in the player's hands" + nl;
        h += "  hitnearest <gameId> [radius] [ammo] - (test/admin, SYNTHETIC) lethal engine damage to nearest creature, player as source" + nl;
        h += "  spawnentity <class> <gameId> [dist] - (test/admin) spawn a creature in front of a player" + nl;
        h += "  parties                             - list active Expansion parties and their online members" + nl;
        h += "  party                               - list every online player with their party id (or 'solo')" + nl;
        h += "  announce <message>                  - center-screen banner to all players (Expansion BAGUETTE)" + nl;
        h += "  announce <title> | <message>       -   ...with a custom title (default 'Server')" + nl;
        h += nl;
        h += "Aliases: addBan/removeBan/teleport/giveItem/listPlayers/listParties/banner." + nl;
        h += "Other RCON commands route through BattlEye when the BE RCON connection is up.";
        s_HelpCache = h;
        return s_HelpCache;
    }

    // Split 'rest' into [first-token, remainder-after-first-space].
    void SplitFirstToken(string s, out string first, out string remainder)
    {
        first = "";
        remainder = "";
        if (s == "") return;
        int sp = s.IndexOf(" ");
        if (sp < 0) { first = s; return; }
        first = s.Substring(0, sp);
        remainder = s.Substring(sp + 1, s.Length() - sp - 1);
        remainder.TrimInPlace();
    }

    // Whitespace-split into an array. Walks via IndexOf instead of a
    // per-character Substring scan — for typical inputs (2–5 tokens) this is
    // O(tokens) substring allocations instead of O(string.Length).
    void SplitTokens(string s, out array<string> parts)
    {
        if (!parts) return;
        parts.Clear();
        if (s == "") return;
        string cur = s;
        cur.TrimInPlace();
        while (cur != "")
        {
            int sp = cur.IndexOf(" ");
            int tab = cur.IndexOf("\t");
            int end;
            if (sp < 0 && tab < 0) { parts.Insert(cur); return; }
            if (sp < 0) end = tab;
            else if (tab < 0) end = sp;
            else if (sp < tab) end = sp;
            else end = tab;
            string tok = cur.Substring(0, end);
            if (tok != "") parts.Insert(tok);
            cur = cur.Substring(end + 1, cur.Length() - end - 1);
            cur.TrimInPlace();
        }
    }

    string BuildCommandOutput(string rawResult, bool success)
    {
        string ok;
        if (success) ok = "true"; else ok = "false";
        return "{\"rawResult\":\"" + JsonSafeString(rawResult) + "\",\"success\":" + ok + "}";
    }

    // JSON-escape rawResult content so it survives Takaro's JSON parser
    // and renders multi-line strings as actual newlines on the dashboard.
    // Raw control chars in a JSON string value are invalid (Takaro rejects
    // the whole response, which surfaces as a 10s request timeout). We
    // escape newlines/CRs/tabs as the two-char JSON escape sequences and
    // drop double-quotes.
    //
    // CParser gotcha: `"\\"` literals break Enforce's parser, but `"\\n"`
    // parses fine because the lexer sees `\\` as one escaped backslash
    // followed by a regular `n` — net result is the 2-char string `\n`.
    // F7: delegates to TakaroEventFactory.Safe, which escapes backslash and
    // double-quote properly (see the CParser note there) and then folds
    // newline/CR/tab into their two-character JSON escapes.
    string JsonSafeString(string s)
    {
        return TakaroEventFactory.Safe(s);
    }

    // Drain delay between disconnecting all players and asking the engine
    // to exit. DisconnectPlayer enqueues the player-save tasks but they're
    // processed asynchronously — exiting too fast can lose the last
    // seconds of state (last item crafted, last base part placed). 5s is
    // generous; persistence finishes well within that window in practice.
    static const int SHUTDOWN_EXIT_DELAY_MS = 5000;

    // F5 — config-driven location list (see TakaroLocationsData).
    static const string LOCATIONS_FILE = "$profile:TakaroIntegration/locations.json";

    void TakaroFinalExit()
    {
        TakaroLog.Info("Engine exit after disconnect drain");
        GetGame().RequestExit(1);
    }

    void DisconnectAllAndExit()
    {
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (!id) continue;
            // M3: DisconnectPlayer skips MissionServer.PlayerDisconnected, so emit
            // player-disconnected ourselves (same path as kick/ban).
            TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
            if (bridge) bridge.OnPlayerKickedByTakaro(pb, id);
            GetGame().DisconnectPlayer(id);
        }
        // Schedule the engine exit on the system call queue so the
        // disconnect-driven persistence saves get a chance to flush.
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.TakaroFinalExit, SHUTDOWN_EXIT_DELAY_MS, false);
    }

    void HandleShutdown(TakaroOperation op)
    {
        TakaroLog.Warn("Shutdown requested by Takaro");
        ReplyOk(op, "{}");
        DisconnectAllAndExit();
    }

    // Walks CfgVehicles + CfgMagazines and returns spawnable items as
    // IItemDTO[]. Filters by scope (>=1 = inventory item or world-spawnable).
    // Cap at MAX_LIST_ITEMS so we don't ship a 30k-entry payload over WS.
    void HandleListItems(TakaroOperation op)
    {
        // Modded DayZ servers (Expansion + BBP + DDA + PvZmoD + …) push
        // CfgVehicles past 30k entries. The previous 5000 cap silently
        // truncated the alphabet before reaching mid-letters like 'H' —
        // that's why vanilla Hatchet never made it into Takaro's items
        // list while early-letter mod props (`bldr_prop_*`) did.
        const int MAX_LIST_ITEMS = 50000;
        string json = "[";
        bool first = true;
        int emitted = 0;

        // Pass 1: CfgVehicles — weapons, gear, tools, food, ammo boxes.
        const string ROOT_V = "CfgVehicles";
        int countV = GetGame().ConfigGetChildrenCount(ROOT_V);
        for (int i = 0; i < countV && emitted < MAX_LIST_ITEMS; i++)
        {
            string cls;
            GetGame().ConfigGetChildName(ROOT_V, i, cls);
            if (cls == "") continue;
            // Skip mod base-building props (`bldr_prop_*`). These are
            // un-interactable decoration entities — some still inherit
            // Inventory_Base so the IsKindOf filter alone doesn't catch them.
            if (cls.IndexOf("bldr_prop_") == 0) continue;
            string baseV = ROOT_V + " " + cls + " ";
            int scopeV = GetGame().ConfigGetInt(baseV + "scope");
            if (scopeV < 1) continue;
            // Player-pickupable items inherit from Inventory_Base. Skip
            // everything else: vehicles (Car/Truck), buildings/houses, AI
            // (ZombieBase/AnimalBase).
            if (!GetGame().IsKindOf(cls, "Inventory_Base")) continue;
            string displayV = "";
            GetGame().ConfigGetText(baseV + "displayName", displayV);
            if (displayV == "") displayV = cls;
            displayV = JsonSafeString(displayV);
            if (!first) json += ",";
            first = false;
            string entryV = "{" + Quote("name") + ":" + Quote(displayV);
            entryV += "," + Quote("code") + ":" + Quote(cls);
            entryV += "," + Quote("amount") + ":1";
            entryV += "," + Quote("quality") + ":" + Quote("") + "}";
            json += entryV;
            emitted++;
        }

        // Pass 2: CfgMagazines — ammo piles (`Ammo_556x45`) and magazines
        // (`Mag_STANAG_30Rnd`). These live in a SEPARATE config root from
        // CfgVehicles, which is why no real ammo ever showed up in Takaro's
        // item list — only the modded `bldr_prop_AmmoBox_*` props did.
        const string ROOT_M = "CfgMagazines";
        int countM = GetGame().ConfigGetChildrenCount(ROOT_M);
        for (int m = 0; m < countM && emitted < MAX_LIST_ITEMS; m++)
        {
            string mcls;
            GetGame().ConfigGetChildName(ROOT_M, m, mcls);
            if (mcls == "") continue;
            if (mcls.IndexOf("bldr_prop_") == 0) continue;
            string baseM = ROOT_M + " " + mcls + " ";
            int scopeM = GetGame().ConfigGetInt(baseM + "scope");
            if (scopeM < 1) continue;
            string displayM = "";
            GetGame().ConfigGetText(baseM + "displayName", displayM);
            if (displayM == "") displayM = mcls;
            displayM = JsonSafeString(displayM);
            if (!first) json += ",";
            first = false;
            string entryM = "{" + Quote("name") + ":" + Quote(displayM);
            entryM += "," + Quote("code") + ":" + Quote(mcls);
            entryM += "," + Quote("amount") + ":1";
            entryM += "," + Quote("quality") + ":" + Quote("") + "}";
            json += entryM;
            emitted++;
        }

        json += "]";
        TakaroLog.Info("listItems: " + emitted.ToString() + " items returned");
        ReplyOk(op, json);
    }

    // Same idea but for AI/animal/zombie entity types — anything in CfgVehicles
    // whose base class chain includes 'DZ_LightAI_Base', 'DayZAnimal', etc.
    void HandleListEntities(TakaroOperation op)
    {
        const int MAX = 2000;
        const string ROOT = "CfgVehicles";
        int count = GetGame().ConfigGetChildrenCount(ROOT);
        string json = "[";
        bool first = true;
        int emitted = 0;
        for (int i = 0; i < count && emitted < MAX; i++)
        {
            string cls;
            GetGame().ConfigGetChildName(ROOT, i, cls);
            if (cls == "") continue;
            // Filter to entity types: animals, infected (zombies), AI.
            if (!GetGame().IsKindOf(cls, "DayZAnimal") && !GetGame().IsKindOf(cls, "DayZInfected") && !GetGame().IsKindOf(cls, "DayZCreature")) continue;
            string display = "";
            GetGame().ConfigGetText(ROOT + " " + cls + " displayName", display);
            // M1: unresolved stringtable keys ("$STR_DN_MAN" arrives as
            // "STR_DN_MAN") are useless as a name — fall back to the classname.
            if (display == "" || display.IndexOf("STR_") == 0 || display.IndexOf("$STR_") == 0 || display.IndexOf("#STR_") == 0) display = cls;
            display = JsonSafeString(display);
            // M1: Takaro IEntityDTO.type is an enum hostile|friendly|neutral —
            // "entity" failed validation and nothing was stored.
            string etype = "neutral";
            if (GetGame().IsKindOf(cls, "DayZInfected") || GetGame().IsKindOf(cls, "Animal_CanisLupus") || GetGame().IsKindOf(cls, "Animal_UrsusArctos")) etype = "hostile";
            string entry = "{" + Quote("name") + ":" + Quote(display) + "," + Quote("code") + ":" + Quote(cls) + "," + Quote("type") + ":" + Quote(etype) + "}";
            if (!first) json += ",";
            first = false;
            json += entry;
            emitted++;
        }
        json += "]";
        TakaroLog.Info("listEntities: " + emitted.ToString() + " entities returned");
        ReplyOk(op, json);
    }

    // Map locations from the world's location config. DayZ exposes these via
    // GetGame().GetWorld().GetMapLoctList — typed result is array<MapLocation>.
    // We surface name + position as IMapLocationDTO[].
    // F5 — config-driven. See TakaroLocationsData above for the file shape.
    // Returns Takaro's IMapLocationDTO[]: {code, name, position:{x,y,z}}.
    void HandleListLocations(TakaroOperation op)
    {
        TakaroLocationsData data = LoadLocations();
        string json = "[";
        bool first = true;
        int emitted = 0;
        int n = 0;
        if (data && data.locations) n = data.locations.Count();
        for (int i = 0; i < n; i++)
        {
            TakaroLocationEntry e = data.locations[i];
            if (!e) continue;
            if (e.name == "") continue;
            if (!first) json += ",";
            first = false;
            json += BuildLocationEntry(e);
            emitted++;
        }
        json += "]";
        TakaroLog.Info("listLocations: " + emitted.ToString() + " locations from " + LOCATIONS_FILE);
        ReplyOk(op, json);
    }

    // Built via accumulator: a single long concat chain trips Enforce's
    // "formula too complex" (same reason as BuildBanEntry).
    string BuildLocationEntry(TakaroLocationEntry e)
    {
        // Takaro wants a stable `code`; derive it from the name with spaces
        // turned into underscores so it stays identifier-safe.
        string code = e.name;
        code.Replace(" ", "_");
        string s = "{" + Quote("code") + ":" + Quote(JsonSafeString(code));
        s += "," + Quote("name") + ":" + Quote(JsonSafeString(e.name));
        s += "," + Quote("position") + ":{";
        s += Quote("x") + ":" + e.x.ToString();
        s += "," + Quote("y") + ":" + e.y.ToString();
        s += "," + Quote("z") + ":" + e.z.ToString();
        s += "}}";
        return s;
    }

    // Load locations.json, writing Chernarus defaults if the file is absent.
    TakaroLocationsData LoadLocations()
    {
        if (!FileExist(TakaroConfig.CONFIG_DIR))
            MakeDirectory(TakaroConfig.CONFIG_DIR);

        TakaroLocationsData data = new TakaroLocationsData();
        if (FileExist(LOCATIONS_FILE))
        {
            JsonFileLoader<TakaroLocationsData>.JsonLoadFile(LOCATIONS_FILE, data);
            return data;
        }

        data = DefaultLocations();
        JsonFileLoader<TakaroLocationsData>.JsonSaveFile(LOCATIONS_FILE, data);
        TakaroLog.Info("listLocations: wrote Chernarus defaults to " + LOCATIONS_FILE + " — edit it for your map");
        return data;
    }

    void AddLocation(TakaroLocationsData data, string name, float x, float z)
    {
        TakaroLocationEntry e = new TakaroLocationEntry();
        e.name = name;
        e.x = x;
        e.y = 0;    // ground level, see the note on TakaroLocationsData
        e.z = z;
        data.locations.Insert(e);
    }

    // Chernarus (dayzOffline.chernarusplus) town centres, approximate.
    TakaroLocationsData DefaultLocations()
    {
        TakaroLocationsData data = new TakaroLocationsData();
        AddLocation(data, "Chernogorsk", 6700, 2600);
        AddLocation(data, "Elektrozavodsk", 10400, 2300);
        AddLocation(data, "Berezino", 12100, 9200);
        AddLocation(data, "Svetlojarsk", 13900, 13200);
        AddLocation(data, "Novodmitrovsk", 11600, 14500);
        AddLocation(data, "Stary Sobor", 6100, 7700);
        AddLocation(data, "Zelenogorsk", 2600, 5000);
        AddLocation(data, "Vybor", 3800, 8900);
        AddLocation(data, "Severograd", 8100, 12700);
        AddLocation(data, "Tisy", 1600, 14000);
        AddLocation(data, "Balota", 4500, 2400);
        AddLocation(data, "Kamyshovo", 12000, 3500);
        return data;
    }

    // Single ban entry builder. Built via accumulator to dodge two Enforce
    // gotchas at once: (1) string.Format with many \" escapes is unreliable
    // (silently mangles Takaro DTO fields, see TakaroEventQueue.Chat fix),
    // and (2) a single concat chain past ~10 segments hits "Formula too
    // complex". Splitting `+=` into stages stays under both limits.
    string BuildBanEntry(string gameId, string reason)
    {
        string s = "{" + Quote("player") + ":{" + Quote("gameId") + ":" + Quote(gameId) + "}";
        s += "," + Quote("reason") + ":" + Quote(reason);
        s += "," + Quote("expiresAt") + ":null}";
        return s;
    }

    // Reads vanilla DayZ ban.txt (Steam64-per-line) AND BattlEye bans.txt
    // (GUID DURATION REASON). Merges, dedups, returns IBanDTO[].
    void HandleListBans(TakaroOperation op)
    {
        array<string> ids = new array<string>;
        array<string> ex = new array<string>;
        array<string> rs = new array<string>;
        TakaroBanStore.Load(ids, ex, rs);
        string json = "[";
        int emitted = 0;
        for (int i = 0; i < ids.Count(); i++)
        {
            if (TakaroBanStore.IsExpired(ex[i])) continue;
            if (emitted > 0) json += ",";
            string entry = "{" + Quote("player") + ":{" + Quote("gameId") + ":" + Quote(ids[i]) + "}";
            entry += "," + Quote("reason") + ":" + Quote(JsonSafeString(rs[i]));
            if (ex[i] == "" || ex[i] == "null") entry += "," + Quote("expiresAt") + ":null}";
            else entry += "," + Quote("expiresAt") + ":" + Quote(ex[i]) + "}";
            json += entry;
            emitted++;
        }
        json += "]";
        TakaroLog.Info("listBans: " + emitted.ToString() + " bans returned");
        ReplyOk(op, json);
    }

    // ---- helpers -------------------------------------------------------

    // Liberal ID matcher — accepts any form Takaro or a copy-paste might
    // produce so visit/tp/kick/give Just Work:
    //   - bare Steam64                         "76561198148612622"
    //   - Takaro-style platformId              "steam:76561198148612622"
    //   - bare BE GUID                         "wI_qp9oOtv91...HLZedX_sMxDA"
    //   - BE GUID with "=" suffix              "wI_qp9oOtv91...HLZedX_sMxDA="
    //   - DLL-emitted platformId               "dayz:wI_qp9oOtv91..."
    bool IdMatches(PlayerIdentity id, string needle)
    {
        if (!id || needle == "") return false;
        string n = needle;
        // Strip known prefixes ("steam:", "dayz:")
        if (n.IndexOf("steam:") == 0) n = n.Substring(6, n.Length() - 6);
        else if (n.IndexOf("dayz:") == 0) n = n.Substring(5, n.Length() - 5);
        // Strip trailing "="
        int neqIdx = n.IndexOf("=");
        if (neqIdx >= 0) n = n.Substring(0, neqIdx);

        if (id.GetPlainId() == n) return true;

        string bisid = id.GetId();
        int eqIdx = bisid.IndexOf("=");
        if (eqIdx >= 0) bisid = bisid.Substring(0, eqIdx);
        return bisid == n;
    }

    PlayerIdentity FindIdentityByGameId(string gameId)
    {
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (IdMatches(id, gameId))
                return id;
        }
        return null;
    }

    // Compose Takaro's required platformId field. DayZ's GetId() returns a
    // base64-ish BIS hash with possible '=' padding; Takaro's regex
    // ^[A-Za-z0-9_-]+:[A-Za-z0-9_-]+$ rejects '=', so strip it.
    string BuildPlatformId(PlayerIdentity id)
    {
        if (!id) return "";
        return TakaroEventFactory.PlatformIdFor(id.GetPlainId(), id.GetId());
    }

    PlayerBase FindPlayerByGameId(string gameId)
    {
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            PlayerIdentity id = pb.GetIdentity();
            if (IdMatches(id, gameId))
                return pb;
        }
        return null;
    }

    void BroadcastSystemMessage(string msg, PlayerBase onlyTo, string title = "Server")
    {
        // With Expansion loaded we use the global chat channel (CCGlobal),
        // which renders the entire message body in GlobalChatColor (88,195,255
        // — sky blue) — closest available preset to Discord blurple. Note:
        // ExpansionChatLine.FormatText replaces every "<" / ">" with the
        // unicode look-alikes "‹" / "›", which kills inline markup, and
        // ExpansionChatLine.SetColorByName does not parse hex codes — only
        // named presets — so an exact #5865F2 isn't reachable server-side.
#ifdef EXPANSIONMOD
        ExpansionGlobalChatModule mod;
        if (CF_Modules<ExpansionGlobalChatModule>.Get(mod))
        {
            string body = msg;
            ExpansionChatMessageEventParams data = new ExpansionChatMessageEventParams(
                ExpansionChatChannels.CCGlobal, "", body, "", "");
            auto rpc = mod.Expansion_CreateRPC("RPC_AddChatMessage");
            rpc.Write(data);
            if (onlyTo && onlyTo.GetIdentity())
                rpc.Expansion_Send(true, onlyTo.GetIdentity());
            else
                rpc.Expansion_Send(true);
            TakaroLog.Info("BROADCAST (chat): " + msg);
            return;
        }
#endif
        if (onlyTo)
        {
            NotificationSystem.SendNotificationToPlayerExtended(onlyTo, 8.0, title, msg, "");
        }
        else
        {
            array<Man> players = new array<Man>;
            GetGame().GetPlayers(players);
            for (int i = 0; i < players.Count(); i++)
            {
                PlayerBase pb = PlayerBase.Cast(players[i]);
                if (!pb) continue;
                NotificationSystem.SendNotificationToPlayerExtended(pb, 8.0, title, msg, "");
            }
        }
        TakaroLog.Info("BROADCAST (popup): " + msg);
    }

    // Loud center-screen banner shown to every connected player. Uses
    // Expansion's BAGUETTE notification when Expansion is loaded; falls
    // back to popup notifications otherwise.
    void BroadcastBanner(string title, string msg)
    {
#ifdef EXPANSIONMOD
        // Use Expansion's built-in "Info" icon so the banner doesn't render
        // with an empty white box on the left edge.
        ExpansionNotification(title, msg, EXPANSION_NOTIFICATION_ICON_INFO, COLOR_EXPANSION_NOTIFICATION_INFO, 8, ExpansionNotificationType.BAGUETTE).Create(NULL);
        TakaroLog.Info("ANNOUNCE (banner): " + title + " — " + msg);
        return;
#endif
        array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        for (int i = 0; i < players.Count(); i++)
        {
            PlayerBase pb = PlayerBase.Cast(players[i]);
            if (!pb) continue;
            NotificationSystem.SendNotificationToPlayerExtended(pb, 10.0, title, msg, "");
        }
        TakaroLog.Info("ANNOUNCE (popup): " + title + " — " + msg);
    }

    // Per-arg-type parsers. We can't use a generic Class-typed parser because
    // JsonSerializer in Enforce Script needs the static type at the call site
    // for proper field introspection.
    bool ParseGetPlayer(TakaroOperation op, out ArgsGetPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseSendMessage(TakaroOperation op, out ArgsSendMessage args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    // M3 fix: Takaro sends the target as a nested `player` object (for
    // teleportPlayer the FULL PlayerOnGameserver row incl. inventory/roles),
    // e.g. {"player":{...,"gameId":"7656..."},"x":1,"y":2,"z":3}. The flat
    // JsonSerializer DTO read failed with "JSON ERROR", so every one of these
    // actions timed out in Takaro. Manual extraction, same as sendMessage/giveItem.
    bool ParseKick(TakaroOperation op, out ArgsKickPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        args.gameId = ExtractJsonStringField(op.argsJson, "gameId");
        args.reason = ExtractJsonStringFieldLast(op.argsJson, "reason");
        if (args.gameId == "") { ReplyError(op, "kickPlayer: missing gameId"); return false; }
        return true;
    }
    bool ParseBan(TakaroOperation op, out ArgsBanPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        args.gameId = ExtractJsonStringField(op.argsJson, "gameId");
        args.reason = ExtractJsonStringFieldLast(op.argsJson, "reason");
        args.durationSeconds = ExtractJsonIntField(op.argsJson, "durationSeconds");
        args.expiresAt = ExtractJsonStringFieldLast(op.argsJson, "expiresAt");
        if (args.gameId == "") { ReplyError(op, "banPlayer: missing gameId"); return false; }
        return true;
    }
    bool ParseUnban(TakaroOperation op, out ArgsUnbanPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        args.gameId = ExtractJsonStringField(op.argsJson, "gameId");
        if (args.gameId == "") { ReplyError(op, "unbanPlayer: missing gameId"); return false; }
        return true;
    }
    bool ParseTeleport(TakaroOperation op, out ArgsTeleportPlayer args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        args.gameId = ExtractJsonStringField(op.argsJson, "gameId");
        if (args.gameId == "") { ReplyError(op, "teleportPlayer: missing gameId"); return false; }
        // Locals, not `out args.x`: Enforce does not reliably write out-params into class members.
        float fx = 0;
        float fy = 0;
        float fz = 0;
        bool hx = ExtractJsonFloatFieldLast(op.argsJson, "x", fx);
        bool hy = ExtractJsonFloatFieldLast(op.argsJson, "y", fy);
        bool hz = ExtractJsonFloatFieldLast(op.argsJson, "z", fz);
        if (!hx || !hy || !hz)
        {
            ReplyError(op, "teleportPlayer: missing x/y/z");
            return false;
        }
        args.x = fx;
        args.y = fy;
        args.z = fz;
        return true;
    }

    // Last occurrence of "field":"value" (top-level fields come after the nested player object).
    string ExtractJsonStringFieldLast(string json, string field)
    {
        string needle = "\"" + field + "\":\"";
        int p = -1;
        int from = 0;
        while (true)
        {
            int q = json.IndexOfFrom(from, needle);
            if (q < 0) break;
            p = q;
            from = q + 1;
        }
        if (p < 0) return "";
        p += needle.Length();
        string rest = json.Substring(p, json.Length() - p);
        int e = rest.IndexOf("\"");
        if (e < 0) return "";
        return rest.Substring(0, e);
    }

    // Last occurrence of an unquoted numeric "field":<number>. Returns false if absent.
    bool ExtractJsonFloatFieldLast(string json, string field, out float value)
    {
        string needle = "\"" + field + "\":";
        int p = -1;
        int from = 0;
        while (true)
        {
            int q = json.IndexOfFrom(from, needle);
            if (q < 0) break;
            p = q;
            from = q + 1;
        }
        if (p < 0) return false;
        p += needle.Length();
        while (p < json.Length() && json.Get(p) == " ") p++;
        int start = p;
        while (p < json.Length())
        {
            string ch = json.Get(p);
            if (ch == "," || ch == "}" || ch == " " || ch == "]") break;
            p++;
        }
        if (p == start) return false;
        string num = json.Substring(start, p - start);
        if (num == "null") return false;
        value = num.ToFloat();
        return true;
    }
    bool ParseGiveItem(TakaroOperation op, out ArgsGiveItem args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }
    bool ParseExec(TakaroOperation op, out ArgsExecuteCommand args)
    {
        if (!op || op.argsJson == "") { ReplyError(op, "Missing args"); return false; }
        string err; JsonSerializer js = new JsonSerializer;
        if (!js.ReadFromString(args, op.argsJson, err)) { ReplyError(op, "Bad args: " + err); return false; }
        return true;
    }

    // Generic serialization helpers — the JsonSerializer works correctly when
    // given the concrete type, so we provide one helper per result class
    // rather than passing through a `Class`-typed wrapper which serializes
    // to an empty object.
    string SerializeReachability(ResultReachability r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }
    string SerializePlayersList(ResultPlayersList r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }
    string SerializePlayerInfo(ResultPlayerInfo r)
    {
        string s; JsonSerializer js = new JsonSerializer; js.WriteToString(r, false, s); return s;
    }

    // Build the operation-result body manually. We deliberately keep
    // `result` as an inline JSON object (not a quoted string) so we don't
    // have to escape its content — Enforce Script's parser handles escape
    // sequences awkwardly. operationId and errorMessage are simple values
    // that callers ensure don't contain quotes or control chars.
    string BuildResultJson(string operationId, bool ok, string resultJson, string errorMessage)
    {
        string okLit;
        if (ok) okLit = "true"; else okLit = "false";
        // Static keys inlined — Quote() helper used to build a fresh string
        // for each of `"operationId"` / `"ok"` / `"result"` / `"error"` on
        // every result. operationId/errorMessage are expected to be simple
        // (no quotes/control chars in the inputs we generate).
        string body = "{\"operationId\":\"" + operationId + "\",\"ok\":" + okLit;
        if (resultJson != "")
            body += ",\"result\":" + resultJson;
        if (errorMessage != "")
            body += ",\"error\":\"" + TakaroEventFactory.Safe(errorMessage) + "\"";  // M3 fix: JSON ERROR text has raw newlines
        body += "}";
        return body;
    }

    void ReplyOk(TakaroOperation op, string resultJson)
    {
        SendResultRaw(op.operationId, true, resultJson, "");
    }

    void ReplyError(TakaroOperation op, string msg)
    {
        SendResultRaw(op.operationId, false, "", msg);
    }

    void SendResultRaw(string operationId, bool ok, string resultJson, string errorMessage)
    {
        TakaroConfigData cfg = TakaroConfig.Get();
        if (!cfg) return;
        if (cfg.GameServerId == "")
        {
            TakaroLog.Warn("No GameServerId set; cannot deliver operation result " + operationId);
            return;
        }
        string body = BuildResultJson(operationId, ok, resultJson, errorMessage);
        string path = "/gameserver/" + cfg.GameServerId + "/operation/" + operationId + "/result";
        TakaroHttpCallback cb = new TakaroHttpCallback("opResult");
        m_Http.Post(path, body, cb);
    }

    // Wrap a string in JSON-style double quotes. Used in places where a
    // literal `\"` escape sequence in a string.Format pattern would confuse
    // Enforce Script's CParser (it chokes on multiple `\"` escapes,
    // especially the `\"\"` empty-string sequence).
    string Quote(string s)
    {
        return "\"" + s + "\"";
    }

    // Find `"<field>":"<value>"` and return the value, or "" if absent.
    // Used as a JsonSerializer escape hatch when Enforce's parser can't be
    // trusted (nested-ref classes, mixed shapes from Takaro). Caveats:
    //  - Stops at the first unescaped `"`. Messages containing escaped
    //    quotes will be truncated — not a problem for the cases we use it
    //    on (sendMessage's `message` is a 300-char ASCII line, gameId is a
    //    Steam64 digit string).
    //  - Searches the WHOLE buffer, so nesting depth is irrelevant. Only
    //    safe when the field name is unique across the payload, which it
    //    is for sendMessage's `gameId`/`message`.
    string ExtractJsonStringField(string json, string field)
    {
        string needle = "\"" + field + "\":\"";
        int p = json.IndexOf(needle);
        if (p < 0) return "";
        p += needle.Length();
        string rest = json.Substring(p, json.Length() - p);
        int e = rest.IndexOf("\"");
        if (e < 0) return "";
        return rest.Substring(0, e);
    }

    // Same flat-text approach for unquoted numeric fields like
    // giveItem's `amount`. Walks digits after `"field":` and converts.
    // Returns 0 if not found or not a parseable integer.
    int ExtractJsonIntField(string json, string field)
    {
        string needle = "\"" + field + "\":";
        int p = json.IndexOf(needle);
        if (p < 0) return 0;
        p += needle.Length();
        // Skip whitespace.
        while (p < json.Length() && json.Get(p) == " ") p++;
        int start = p;
        if (p < json.Length() && (json.Get(p) == "-" || json.Get(p) == "+")) p++;
        while (p < json.Length())
        {
            string ch = json.Get(p);
            if (ch < "0" || ch > "9") break;
            p++;
        }
        if (p == start) return 0;
        string num = json.Substring(start, p - start);
        return num.ToInt();
    }
}
