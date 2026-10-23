// Research probe for issue #298 (C7, Terraria transport gate). Runs on the TShock image's own
// .NET runtime and checks what a plugin-only connector would need from System.Net.WebSockets:
// valid-certificate WSS upgrade, a payload round trip (an identify with a deliberately invalid
// registration token, so Takaro answers with an error and creates nothing), clean close,
// reconnect, and refusal of DNS failures and bad certificates. No credentials are used.
using System.Net.WebSockets;
using System.Text;

var url = args.Length > 0 ? args[0] : "wss://connect.k8s.takaro.dev/";
var failures = 0;

async Task<string> RoundTrip(string target, int attempt)
{
    using var ws = new ClientWebSocket();
    using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
    var t0 = DateTime.UtcNow;
    await ws.ConnectAsync(new Uri(target), cts.Token);
    var upgradeMs = (DateTime.UtcNow - t0).TotalMilliseconds;
    var identify = "{\"type\":\"identify\",\"payload\":{\"identityToken\":\"research-298-probe\"," +
                   "\"registrationToken\":\"research-298-invalid-token\",\"name\":\"research-298-probe\"," +
                   "\"protocolVersions\":[0,1]}}";
    await ws.SendAsync(Encoding.UTF8.GetBytes(identify), WebSocketMessageType.Text, true, cts.Token);
    var buf = new byte[64 * 1024];
    WebSocketReceiveResult r;
    var reply = "";
    for (var n = 0; n < 3; n++) {  // Takaro greets with "connected" before it answers identify
        var sb = new StringBuilder();
        do {  // assemble until EndOfMessage, the check Valheim's runner skips
            r = await ws.ReceiveAsync(buf, cts.Token);
            sb.Append(Encoding.UTF8.GetString(buf, 0, r.Count));
        } while (!r.EndOfMessage && r.MessageType != WebSocketMessageType.Close);
        reply = sb.ToString();
        if (r.MessageType == WebSocketMessageType.Close || !reply.Contains("\"connected\"")) break;
    }
    if (ws.State == WebSocketState.Open)
        await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "probe done", cts.Token);
    Console.WriteLine($"attempt {attempt}: upgrade {upgradeMs:F0} ms, " +
                      $"len={reply.Length}: {(reply.Length > 200 ? reply[..200] : reply)}; state after close={ws.State}");
    return reply;
}

async Task ExpectFailure(string label, string target)
{
    try {
        using var ws = new ClientWebSocket();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        await ws.ConnectAsync(new Uri(target), cts.Token);
        Console.WriteLine($"FAIL {label}: connected to {target} (verification not enforced)");
        failures++;
    } catch (Exception e) {
        var inner = e.InnerException?.GetType().Name ?? "-";
        Console.WriteLine($"ok   {label}: refused ({e.GetType().Name} / {inner}: {(e.InnerException ?? e).Message})");
    }
}

Console.WriteLine($"runtime {System.Runtime.InteropServices.RuntimeInformation.FrameworkDescription} " +
                  $"on {System.Runtime.InteropServices.RuntimeInformation.OSDescription}");
for (var i = 1; i <= 2; i++) {  // second attempt = reconnect on a fresh socket
    try {
        var reply = await RoundTrip(url, i);
        if (!reply.Contains("error", StringComparison.OrdinalIgnoreCase)) { Console.WriteLine("FAIL unexpected reply"); failures++; }
    } catch (Exception e) { Console.WriteLine($"FAIL attempt {i}: {e.GetType().Name}: {e.Message}"); failures++; }
}
await ExpectFailure("dns failure", "wss://takaro-research-298.invalid/");
await ExpectFailure("self-signed certificate", "wss://self-signed.badssl.com/");
await ExpectFailure("wrong host name", "wss://wrong.host.badssl.com/");
await ExpectFailure("expired certificate", "wss://expired.badssl.com/");
Console.WriteLine(failures == 0 ? "RESULT: transport gate passed" : $"RESULT: {failures} check(s) failed");
return failures == 0 ? 0 : 1;
