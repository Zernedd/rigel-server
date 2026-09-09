using System.Net.WebSockets;
using System.Text;

namespace AUnrealFeatures.EOSSDK;

internal static class StompHelper
{
    public static bool IsStompPath(string path) =>
        path.StartsWith("/notifications/v1/", StringComparison.OrdinalIgnoreCase) &&
        path.EndsWith("/connect", StringComparison.OrdinalIgnoreCase);

    public static async Task RunAsync(WebSocket ws, string path, Action<string> log)
    {
        var buf       = new byte[64 * 1024];
        var sessionId = Guid.NewGuid().ToString("N")[..16];
        var client    = EosObservabilityStore.OpenClient(sessionId, path);

        using var heartbeatCts = new CancellationTokenSource();

        _ = Task.Run(async () =>
        {
            var nl = Encoding.UTF8.GetBytes("\n");
            while (!heartbeatCts.Token.IsCancellationRequested)
            {
                await Task.Delay(30_000, heartbeatCts.Token).ContinueWith(_ => { });
                if (ws.State == WebSocketState.Open)
                    await ws.SendAsync(nl, WebSocketMessageType.Text, true, CancellationToken.None);
            }
        });

        try
        {
            while (ws.State == WebSocketState.Open)
            {
                var result = await ws.ReceiveAsync(buf.AsMemory(), CancellationToken.None);

                if (result.MessageType == WebSocketMessageType.Close)
                    break;

                var raw = Encoding.UTF8.GetString(buf, 0, result.Count).TrimEnd('\0');
                if (string.IsNullOrWhiteSpace(raw)) continue;

                var (command, headers, body) = ParseFrame(raw);
                log($"STOMP {command} | {sessionId} | {path}");

                // Log inbound frame
                var headerStr = string.Join(", ", headers.Select(kv => $"{kv.Key}:{kv.Value}"));
                EosObservabilityStore.PushWsMessage(sessionId, "in", command, headerStr, body);

                switch (command.ToUpperInvariant())
                {
                    case "CONNECT":
                    case "STOMP":
                        var connected = new Dictionary<string, string>
                        {
                            ["version"]    = "1.2",
                            ["session"]    = sessionId,
                            ["heart-beat"] = "0,0",
                            ["server"]     = "EOS/1.0",
                        };
                        await SendFrameTracked(ws, sessionId, "CONNECTED", connected);
                        break;

                    case "SUBSCRIBE":
                    case "UNSUBSCRIBE":
                    case "SEND":
                        if (headers.TryGetValue("receipt", out var rid))
                            await SendFrameTracked(ws, sessionId, "RECEIPT", new() { ["receipt-id"] = rid });
                        break;

                    case "DISCONNECT":
                        heartbeatCts.Cancel();
                        if (headers.TryGetValue("receipt", out var drid))
                            await SendFrameTracked(ws, sessionId, "RECEIPT", new() { ["receipt-id"] = drid });
                        return;
                }
            }
        }
        finally
        {
            heartbeatCts.Cancel();
            EosObservabilityStore.CloseClient(sessionId);
        }
    }

    static async Task SendFrameTracked(WebSocket ws, string sessionId, string command, Dictionary<string, string> headers, string body = "")
    {
        var headerStr = string.Join(", ", headers.Select(kv => $"{kv.Key}:{kv.Value}"));
        EosObservabilityStore.PushWsMessage(sessionId, "out", command, headerStr, body);
        await SendFrame(ws, command, headers, body);
    }

    public static async Task SendFrame(WebSocket ws, string command, Dictionary<string, string> headers, string body = "")
    {
        var sb = new StringBuilder();
        sb.Append(command).Append('\n');
        foreach (var kv in headers) sb.Append(kv.Key).Append(':').Append(kv.Value).Append('\n');
        sb.Append('\n').Append(body).Append('\0');

        var bytes = Encoding.UTF8.GetBytes(sb.ToString());
        if (ws.State == WebSocketState.Open)
            await ws.SendAsync(bytes, WebSocketMessageType.Text, true, CancellationToken.None);
    }

    public static (string Command, Dictionary<string, string> Headers, string Body) ParseFrame(string raw)
    {
        var lines   = raw.Replace("\r\n", "\n").Split('\n');
        var command = lines[0].Trim();
        var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int i = 1;
        for (; i < lines.Length; i++)
        {
            var line = lines[i];
            if (line.Length == 0) { i++; break; }
            var colon = line.IndexOf(':');
            if (colon > 0) headers[line[..colon].Trim()] = line[(colon + 1)..].Trim();
        }
        var body = i < lines.Length ? string.Join("\n", lines[i..]).TrimEnd('\0') : "";
        return (command, headers, body);
    }
}
