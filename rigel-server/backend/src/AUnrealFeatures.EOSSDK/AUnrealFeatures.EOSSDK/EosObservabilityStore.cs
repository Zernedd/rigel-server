using System.Collections.Concurrent;

namespace AUnrealFeatures.EOSSDK;

// ── Telemetry ─────────────────────────────────────────────────────────────────

public sealed record TelemetryEvent(
    string Id,
    string EventName,
    string Payload,
    DateTime Timestamp);

// ── WebSocket messages ────────────────────────────────────────────────────────

public sealed record WsMessage(
    string Direction,   // "in" | "out"
    string Command,
    string Headers,
    string Body,
    DateTime Timestamp);

public sealed class WsClientSession
{
    public string   SessionId      { get; init; } = "";
    public string   Path           { get; init; } = "";
    public DateTime ConnectedAt    { get; init; } = DateTime.UtcNow;
    public DateTime? DisconnectedAt { get; set; }

    private readonly List<WsMessage> _messages = new();
    private readonly object _lock = new();

    public int MessageCount { get { lock (_lock) return _messages.Count; } }

    public void AddMessage(WsMessage msg)
    {
        lock (_lock)
        {
            if (_messages.Count >= 500) _messages.RemoveAt(0);
            _messages.Add(msg);
        }
    }

    public List<WsMessage> GetMessages()
    {
        lock (_lock) return _messages.ToList();
    }
}

// ── Store ─────────────────────────────────────────────────────────────────────

public static class EosObservabilityStore
{
    // Telemetry — ring buffer, newest last
    const int MAX_TELEMETRY = 1000;
    static readonly List<TelemetryEvent> _telemetry = new();
    static readonly object _telLock = new();

    // WS clients — keyed by session_id, capped at 200
    const int MAX_CLIENTS = 200;
    static readonly ConcurrentDictionary<string, WsClientSession> _clients = new();
    static readonly ConcurrentQueue<string> _clientOrder = new();

    // ── Telemetry ─────────────────────────────────────────────────────────────

    public static void PushTelemetry(string eventName, string payload)
    {
        var ev = new TelemetryEvent(Guid.NewGuid().ToString("N")[..12], eventName, payload, DateTime.UtcNow);
        lock (_telLock)
        {
            if (_telemetry.Count >= MAX_TELEMETRY) _telemetry.RemoveAt(0);
            _telemetry.Add(ev);
        }
    }

    public static List<TelemetryEvent> GetTelemetry(int limit = 200)
    {
        lock (_telLock)
        {
            var start = Math.Max(0, _telemetry.Count - limit);
            return _telemetry.GetRange(start, _telemetry.Count - start)
                             .OrderByDescending(e => e.Timestamp)
                             .ToList();
        }
    }

    // ── WS clients ────────────────────────────────────────────────────────────

    public static WsClientSession OpenClient(string sessionId, string path)
    {
        var session = new WsClientSession { SessionId = sessionId, Path = path, ConnectedAt = DateTime.UtcNow };
        _clients[sessionId] = session;
        _clientOrder.Enqueue(sessionId);

        // Evict oldest if over cap
        while (_clients.Count > MAX_CLIENTS && _clientOrder.TryDequeue(out var old))
            _clients.TryRemove(old, out _);

        return session;
    }

    public static void CloseClient(string sessionId)
    {
        if (_clients.TryGetValue(sessionId, out var s))
            s.DisconnectedAt = DateTime.UtcNow;
    }

    public static void PushWsMessage(string sessionId, string direction, string command, string headers, string body)
    {
        if (_clients.TryGetValue(sessionId, out var s))
            s.AddMessage(new WsMessage(direction, command, headers, body, DateTime.UtcNow));
    }

    public static List<WsClientSession> GetClients() =>
        _clients.Values.OrderByDescending(c => c.ConnectedAt).ToList();

    public static WsClientSession? GetClient(string sessionId) =>
        _clients.TryGetValue(sessionId, out var s) ? s : null;
}
