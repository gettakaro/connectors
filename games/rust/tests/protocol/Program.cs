// Runs the connector's protocol region (lifted out of mod/TakaroConnector.cs by run.sh)
// against Generic Game Protocol v0 and v1.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using Newtonsoft.Json.Linq;

internal static class Program
{
    private static int _checks;
    private static int _failures;

    private static void Check(bool ok, string what)
    {
        _checks++;
        if (ok) return;
        _failures++;
        Console.WriteLine("  FAIL: " + what);
    }

    private static int Main()
    {
        Manifest();
        Identify();
        IdentifyReplies();
        Responses();
        Events();
        Profile();
        Console.WriteLine("checks: " + _checks + ", failures: " + _failures);
        return _failures == 0 ? 0 : 1;
    }

    private static List<string> Names(JArray manifest)
    {
        return manifest.Select(c => (string)c["name"]).ToList();
    }

    private static void Manifest()
    {
        var names = Names(Connector.BuildManifest());
        Check(names.Count == 22, "manifest: 16 actions and 6 events (" + names.Count + ")");
        foreach (var expected in new[]
        {
            "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
            "items.list", "items.give", "entities.list", "locations.list", "chat.send", "console.execute",
            "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown",
            "events.player-connected", "events.player-disconnected", "events.chat-message",
            "events.player-death", "events.entity-killed", "events.log",
        })
            Check(names.Contains(expected), "manifest: declares " + expected);
        Check(names.Distinct().Count() == names.Count, "manifest: no duplicate names");
        Check(!names.Contains("map"), "manifest: no map before the raster is ready");
        foreach (var name in names)
            Check(System.Text.RegularExpressions.Regex.IsMatch(name, @"^[a-z][a-zA-Z0-9]*(\.[a-z][a-zA-Z0-9-]*)*$"),
                "manifest: well-formed capability name " + name);

        var map = new JObject { ["name"] = "map", ["parameters"] = new JObject() };
        var withMap = Names(Connector.BuildManifest(map, null));
        Check(withMap.Count == 23 && withMap.Last() == "map", "manifest: optional capabilities are appended, nulls skipped");

        Check(Connector.IsDeclaredAction("getPlayers") && Connector.IsDeclaredAction("shutdown"), "declared: action handlers");
        Check(!Connector.IsDeclaredAction("getMapInfo") && !Connector.IsDeclaredAction("testReachability")
            && !Connector.IsDeclaredAction("bogus"), "declared: getMapInfo, testReachability and unknown actions are not capabilities");
    }

    private static void Identify()
    {
        var payload = Connector.BuildIdentifyPayload("ident", "reg", Connector.BuildManifest());
        Check((string)payload["identityToken"] == "ident" && (string)payload["registrationToken"] == "reg", "identify: tokens");
        var versions = payload["protocolVersions"].Select(v => (int)v).ToArray();
        Check(versions.SequenceEqual(new[] { 0, 1 }), "identify: offers protocol versions [0, 1]");
        Check(payload["capabilities"] is JArray caps && caps.Count == 22, "identify: carries the manifest");
        var empty = Connector.BuildIdentifyPayload(null, null, new JArray());
        Check((string)empty["identityToken"] == "" && (string)empty["registrationToken"] == "", "identify: missing tokens are empty strings");
        Check((string)payload["game"] == "rust", "identify: game is the slug rust");
        Check(empty["connector"] == null && empty["name"] == null, "identify: no connector or name when there is none");
    }

    private static void Profile()
    {
        // Identity
        var payload = Connector.BuildIdentifyPayload("i", "r", new JArray(), "  My Rust Server ", Versions.Info);
        Check((string)payload["connector"]["name"] == "takaro-rust", "identify: connector name is takaro-rust");
        Check((string)payload["connector"]["version"] == Versions.Info, "identify: connector version is the [Info] version");
        Check(Versions.Info == Versions.VersionTxt, "version: [Info] " + Versions.Info + " equals version.txt " + Versions.VersionTxt);
        Check((string)payload["name"] == "My Rust Server", "identify: the hostname is sent trimmed");
        Check(Connector.BuildIdentifyPayload("i", "r", new JArray(), "ab", null)["name"] == null, "identify: a name under 3 characters is not sent");
        foreach (var slug in new[] { Connector.GameIdentifier, Connector.ConnectorName })
            Check(System.Text.RegularExpressions.Regex.IsMatch(slug, "^[a-z0-9][a-z0-9-]*$") && slug.Length <= 64, "identify: slug " + slug);
        foreach (var good in new[] { "0.3.0", "1.4.0", "2.0.0-beta.1", "0.8.1+abc1234" })
            Check(Connector.IsStrictSemver(good), "semver: " + good + " is valid");
        foreach (var bad in new[] { "", null, "0.3", "v1.0.0", "01.0.0", "1.0.0-", "1.0.0.0" })
            Check(!Connector.IsStrictSemver(bad), "semver: '" + bad + "' is refused");
        Check(Connector.BuildIdentifyPayload("i", "r", new JArray(), null, "0.3")["connector"] == null,
            "identify: a version that is not strict semver leaves connector out instead of failing identify");

        // Game build label
        Check(Connector.GameVersionLabel(" 2611.264.1 ") == "2611.264.1", "build: trimmed");
        Check(Connector.GameVersionLabel(null) == null && Connector.GameVersionLabel("  ") == null, "build: empty is omitted");
        Check(Connector.GameVersionLabel("a\u0001b\u00e9c") == "abc", "build: only printable ASCII");
        Check(Connector.GameVersionLabel(new string('9', 40)).Length == 32, "build: at most 32 characters");

        // Icon
        var shipped = File.ReadAllBytes("icon-256.png");
        var icon = Connector.GameIcon.FromBase64(Connector.GameIconBase64);
        Check(icon.Bytes.SequenceEqual(shipped), "icon: the embedded constant is exactly assets/icon-256.png (run scripts/embed-icon.sh)");
        Check(icon.Size == shipped.Length && icon.Size > 0 && icon.Size <= 131072, "icon: size is the byte length, at most 128 KiB (" + icon.Size + ")");
        string sha;
        using (var hash = SHA256.Create())
            sha = string.Concat(hash.ComputeHash(shipped).Select(b => b.ToString("x2")));
        Check(icon.Revision == sha.Substring(0, 12), "icon: revision is the first 12 hex of the sha256 (" + icon.Revision + ")");
        Check(shipped.Take(8).SequenceEqual(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }), "icon: PNG signature");
        Check(System.Text.Encoding.ASCII.GetString(shipped, 12, 4) == "IHDR"
            && ReadBigEndian(shipped, 16) == 256 && ReadBigEndian(shipped, 20) == 256, "icon: IHDR says 256 x 256");

        // game.profile
        var profile = Connector.GameProfileCapability(" 2611.264.1", icon);
        Check((string)profile["name"] == "game.profile", "profile: capability name");
        var parameters = (JObject)profile["parameters"];
        Check((string)parameters["name"] == "Rust", "profile: display name Rust");
        Check((string)parameters["version"] == "2611.264.1", "profile: game build");
        Check((string)parameters["icon"]["revision"] == icon.Revision && (int)parameters["icon"]["size"] == shipped.Length,
            "profile: icon revision and size");
        Check(parameters.Properties().Select(p => p.Name).All(n => n == "name" || n == "version" || n == "icon"), "profile: no other keys");
        var bare = (JObject)Connector.GameProfileCapability(null, null)["parameters"];
        Check(bare["version"] == null && bare["icon"] == null && (string)bare["name"] == "Rust", "profile: build and icon are optional");
        var names = Connector.BuildManifest(profile).Select(c => (string)c["name"]).ToList();
        Check(names.Count(n => n == "game.profile") == 1, "manifest: game.profile declared once");

        // getGameIcon
        var response = Connector.GameIconResponse(icon);
        Check((string)response["revision"] == icon.Revision, "getGameIcon: answers the declared revision");
        Check(Convert.FromBase64String((string)response["data"]).SequenceEqual(shipped), "getGameIcon: data is the whole PNG, exactly size bytes");
        var frame = Connector.ResponseFrame(1, "icon-req", response, null);
        Check((string)frame["requestId"] == "icon-req" && frame["payload"]["data"] != null && frame["error"] == null, "getGameIcon: response frame");
    }

    private static int ReadBigEndian(byte[] bytes, int offset)
    {
        return (bytes[offset] << 24) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) | bytes[offset + 3];
    }

    private static void IdentifyReplies()
    {
        var v1 = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-1\",\"protocolVersion\":1},\"requestId\":\"r\"}"));
        Check(v1.Accepted && v1.ProtocolVersion == 1 && v1.GameServerId == "gs-1", "reply: v1 accepted");

        var v0 = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs-0\",\"protocolVersion\":0}}"));
        Check(v0.Accepted && v0.ProtocolVersion == 0, "reply: v0 chosen");

        var old = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"server\":{\"id\":\"gs-old\"}}}"));
        Check(old.Accepted && old.ProtocolVersion == 0 && old.GameServerId == "gs-old", "reply: an older Takaro without protocolVersion is v0");

        var v0Error = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"error\":{\"name\":\"BadRequestError\",\"message\":\"Invalid registration token\"}}}"));
        Check(!v0Error.Accepted && v0Error.ErrorCode == null && v0Error.Reason.Contains("Invalid registration token"), "reply: v0 rejection");

        var v1Error = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":null,\"error\":{\"code\":\"invalid_args\",\"message\":\"invalid capability name\"}}"));
        Check(!v1Error.Accepted && v1Error.ErrorCode == "invalid_args" && v1Error.Reason.Contains("invalid capability name"),
            "reply: v1 rejection carries its code");

        var jwt = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":null,\"error\":{\"code\":\"internal\",\"message\":\"failed eyJhbGciOi.eyJzdWIi.c2lnbmF0dXJl\"}}"));
        Check(!jwt.Reason.Contains("eyJ"), "reply: a JWT in a rejection is redacted");
        Check(jwt.Retryable, "reply: an internal rejection is retried, not treated as a bad token");
        Check(!v1Error.Retryable && !v0Error.Retryable && !v1.Retryable, "reply: other rejections and acceptances are not retried");

        var future = Connector.ParseIdentifyResponse(JObject.Parse(
            "{\"type\":\"identifyResponse\",\"payload\":{\"gameServerId\":\"gs\",\"protocolVersion\":2}}"));
        Check(!future.Accepted && future.ErrorCode == "no_shared_protocol_version", "reply: a version never offered is refused");
    }

    private static void Responses()
    {
        var v1Ok = Connector.ResponseFrame(1, "r1", null, null);
        Check((string)v1Ok["type"] == "response" && (string)v1Ok["requestId"] == "r1" && v1Ok["payload"].Type == JTokenType.Null
            && v1Ok["error"] == null, "response: v1 void answer is payload null");
        var v0Ok = Connector.ResponseFrame(0, "r0", null, null);
        Check(v0Ok["payload"] is JObject o && o.Count == 0, "response: v0 void answer stays an empty object");
        var data = Connector.ResponseFrame(1, "r2", new JArray(1, 2), null);
        Check(data["payload"] is JArray a && a.Count == 2, "response: data payload kept");

        var error = new Connector.ProtocolError(Connector.ErrorNotFound, "Player not found");
        var v1Err = Connector.ResponseFrame(1, "r3", new JObject(), error);
        Check((string)v1Err["error"]["code"] == "not_found" && (string)v1Err["error"]["message"] == "Player not found"
            && v1Err["payload"] == null, "response: v1 error is {code, message} without a payload");
        var v0Err = Connector.ResponseFrame(0, "r4", null, error);
        Check(v0Err["error"].Type == JTokenType.String && (string)v0Err["error"] == "Player not found", "response: v0 error is a string");

        var legacy = new Connector.ProtocolError(Connector.ErrorUnsupported, "Action not supported: x", "Action not implemented: x");
        Check((string)Connector.ResponseFrame(0, "r5", null, legacy)["error"] == "Action not implemented: x"
            && (string)Connector.ResponseFrame(1, "r5", null, legacy)["error"]["code"] == "unsupported",
            "response: v0 keeps its old text, v1 gets the code");
    }

    private static void Events()
    {
        var stream = new Connector.EventStream("stream-a");
        var first = stream.Add("chat-message", new JObject { ["msg"] = "one" }, keep: true);
        var second = stream.Add("log", new JObject { ["msg"] = "two" }, keep: true);
        Check((string)first["type"] == "gameEvent" && (string)first["payload"]["type"] == "chat-message"
            && (string)first["payload"]["data"]["msg"] == "one", "events: v1 frame carries type and data");
        Check((string)first["payload"]["streamId"] == "stream-a" && (long)first["payload"]["seq"] == 1
            && (long)second["payload"]["seq"] == 2, "events: streamId and seq from 1");
        Check(stream.PendingCount == 2, "events: kept until acked");

        string ackStream;
        long upTo;
        Check(Connector.TryParseEventAck(JObject.Parse("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"stream-a\",\"upTo\":1}}"),
            out ackStream, out upTo) && ackStream == "stream-a" && upTo == 1, "ack: parsed");
        Check(!Connector.TryParseEventAck(JObject.Parse("{\"type\":\"eventAck\",\"payload\":{\"streamId\":\"stream-a\"}}"),
            out ackStream, out upTo), "ack: without upTo is ignored");

        stream.Ack("other-stream", 10);
        Check(stream.PendingCount == 2, "ack: another stream's ack changes nothing");
        stream.Ack("stream-a", 1);
        var pending = stream.Pending();
        Check(pending.Count == 1 && (long)pending[0]["payload"]["seq"] == 2, "ack: cumulative, up to and including upTo");

        var third = stream.Add("log", new JObject { ["msg"] = "three" }, keep: false);
        Check((long)third["payload"]["seq"] == 3 && stream.PendingCount == 1, "events: a v0 event is numbered but not kept");
        stream.Ack("stream-a", 3);
        Check(stream.PendingCount == 0, "ack: everything acked");

        var resend = new Connector.EventStream("stream-b");
        for (var i = 0; i < 5; i++) resend.Add("log", new JObject { ["msg"] = i.ToString() }, keep: true);
        resend.Ack("stream-b", 2);
        var again = resend.Pending().Select(f => (long)f["payload"]["seq"]).ToArray();
        Check(again.SequenceEqual(new long[] { 3, 4, 5 }), "resend: unacked events in seq order");
        var drained = resend.TakeAll();
        Check(drained.Count == 3 && resend.PendingCount == 0, "v0 backlog: taken once and forgotten");

        var full = new Connector.EventStream("stream-c");
        for (var i = 0; i < Connector.EventStream.Capacity + 5; i++) full.Add("log", new JObject(), keep: true);
        var kept = full.Pending();
        Check(kept.Count == Connector.EventStream.Capacity && full.Dropped == 5 && (long)kept[0]["payload"]["seq"] == 6,
            "overflow: the oldest events are dropped");
        Check(Connector.EventStream.Capacity >= 1000, "overflow: room for at least 1000 events");

        var v0 = Connector.V0EventFrame(first);
        Check((string)v0["type"] == "gameEvent" && (string)v0["payload"]["type"] == "chat-message"
            && v0["payload"]["streamId"] == null && v0["payload"]["seq"] == null && (string)v0["payload"]["data"]["msg"] == "one",
            "v0 frame: no streamId or seq");
    }
}
