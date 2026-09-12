using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace AUnrealFeatures.HalcyonSocket;

// One connected HalcyonAllocatorAgent (a game box). Newline-delimited JSON both directions.
public sealed class AgentConnection
{
    public TcpClient     Client { get; init; } = null!;
    public NetworkStream Stream { get; init; } = null!;
    public string   Box      { get; set; } = "?";
    public int      Capacity { get; set; }
    public int      Running  { get; set; }
    public DateTime LastSeen { get; set; } = DateTime.UtcNow;

    // When this connection came up. The watchdog waits this long again before judging a server on
    // SILENCE: whatever took the agent away (a box reboot, a network blip) may have been blocking the
    // game server's heartbeats too, and the backlog of quiet that follows a reconnect is not evidence
    // that the game is wedged.
    public DateTime ConnectedAt { get; } = DateTime.UtcNow;

    private readonly object _writeLock = new();

    public int Free => Math.Max(0, Capacity - Running);

    public bool SendLine(string json)
    {
        try
        {
            var bytes = Encoding.UTF8.GetBytes(json + "\n");
            lock (_writeLock) { Stream.Write(bytes, 0, bytes.Length); Stream.Flush(); }
            return true;
        }
        catch { return false; }
    }
}

// Raw-TCP hub for allocator agents. Mirrors the VirtexSocketServer accept/heartbeat/reap shape, but
// uses newline-delimited JSON to match HalcyonAllocatorAgent.cpp (both ends are ours).
public sealed class AgentHub
{
    private readonly TcpListener _listener;
    private readonly ConcurrentDictionary<Guid, AgentConnection> _agents = new();

    // Set by HalcyonSocketServer once the watchdog exists. Kept as callbacks so the hub stays a dumb
    // transport and every decision lives in one place (ServerWatchdog).
    public Action<string /*reqId*/, string /*pid*/, string /*status*/, string /*box*/>? OnSpunUp;
    public Action<string /*box*/,   string /*pid*/, string /*code*/>?                   OnExited;
    public Action<string /*box*/,   string /*pid*/, bool   /*alive*/>?                  OnProbeResult;

    public AgentHub(int port) => _listener = new TcpListener(IPAddress.Any, port);

    public void Start()
    {
        _listener.Start();
        _ = Task.Run(AcceptLoop);
        _ = Task.Run(ReapLoop);
        Console.WriteLine($"[HalcyonSocket] agent hub listening on TCP {((IPEndPoint)_listener.LocalEndpoint).Port}");
    }

    private async Task AcceptLoop()
    {
        while (true)
        {
            TcpClient client;
            try { client = await _listener.AcceptTcpClientAsync(); }
            catch { break; }
            var conn = new AgentConnection { Client = client, Stream = client.GetStream() };
            var id = Guid.NewGuid();
            _agents[id] = conn;
            _ = Task.Run(() => ClientLoop(id, conn));
        }
    }

    private async Task ClientLoop(Guid id, AgentConnection conn)
    {
        var buf = new byte[4096];
        var acc = new StringBuilder();
        try
        {
            while (true)
            {
                int n = await conn.Stream.ReadAsync(buf, 0, buf.Length);
                if (n <= 0) break;
                acc.Append(Encoding.UTF8.GetString(buf, 0, n));
                var s = acc.ToString();
                int nl;
                while ((nl = s.IndexOf('\n')) >= 0)
                {
                    var line = s.Substring(0, nl).Trim();
                    s = s.Substring(nl + 1);
                    if (line.Length > 0) HandleAgentMessage(conn, line);
                }
                acc.Clear();
                acc.Append(s);
            }
        }
        catch { /* fallthrough to disconnect */ }

        _agents.TryRemove(id, out _);
        try { conn.Client.Close(); } catch { }
        Console.WriteLine($"[HalcyonSocket] agent '{conn.Box}' disconnected");
    }

    private static string GetStr(JsonElement e, string k) => e.TryGetProperty(k, out var v) ? v.ToString() : "";
    private static int    GetInt(JsonElement e, string k) => e.TryGetProperty(k, out var v) && v.TryGetInt32(out var i) ? i : 0;

    private void HandleAgentMessage(AgentConnection conn, string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement;
            var type = GetStr(root, "type");
            switch (type)
            {
                case "hello":
                    conn.Box      = GetStr(root, "box");
                    conn.Capacity = GetInt(root, "capacity");
                    conn.Running  = GetInt(root, "running");
                    conn.LastSeen = DateTime.UtcNow;
                    Console.WriteLine($"[HalcyonSocket] agent '{conn.Box}' online (cap={conn.Capacity}, running={conn.Running})");
                    break;
                case "heartbeat":
                    conn.Running  = GetInt(root, "running");
                    conn.LastSeen = DateTime.UtcNow;
                    break;
                case "spunup":
                    Console.WriteLine($"[HalcyonSocket] spunup id={GetStr(root, "id")} pid={GetStr(root, "pid")} status={GetStr(root, "status")} msg={GetStr(root, "msg")}");
                    OnSpunUp?.Invoke(GetStr(root, "id"), GetStr(root, "pid"), GetStr(root, "status"), conn.Box);
                    break;
                case "exited":
                    conn.Running = GetInt(root, "running");
                    Console.WriteLine($"[HalcyonSocket] instance exited pid={GetStr(root, "pid")} code={GetStr(root, "code")}");
                    // The agent holds the Process object, so this is not a guess about the process --
                    // it is the process telling us. The watchdog treats it as certain.
                    OnExited?.Invoke(conn.Box, GetStr(root, "pid"), GetStr(root, "code"));
                    break;
                case "probe_result":
                    OnProbeResult?.Invoke(conn.Box, GetStr(root, "pid"),
                                          root.TryGetProperty("alive", out var al) && al.ValueKind == JsonValueKind.True);
                    break;
            }
        }
        catch { /* ignore malformed */ }
    }

    // Reap agents that haven't heartbeated in 90s (the agent beats every 30s).
    private async Task ReapLoop()
    {
        while (true)
        {
            await Task.Delay(15000);
            var cutoff = DateTime.UtcNow.AddSeconds(-90);
            foreach (var kv in _agents)
            {
                if (kv.Value.LastSeen >= cutoff) continue;
                _agents.TryRemove(kv.Key, out _);
                try { kv.Value.Client.Close(); } catch { }
                Console.WriteLine($"[HalcyonSocket] reaped stale agent '{kv.Value.Box}'");
            }
        }
    }

    public IReadOnlyList<AgentConnection> Agents => _agents.Values.ToList();

    // The agent for a named box, or null when that box is not connected. The watchdog treats null as
    // "cannot verify" and refuses to act -- a missing agent means we cannot tell a dead server from a
    // dead box.
    public AgentConnection? ByBox(string box)
        => string.IsNullOrWhiteSpace(box) ? null : _agents.Values.FirstOrDefault(a => a.Box == box);

    // Ask a box whether a pid is still running. The answer comes back as "probe_result".
    public bool Probe(string box, string pid)
    {
        var a = ByBox(box);
        return a != null && a.SendLine(JsonSerializer.Serialize(new { type = "probe", pid }));
    }

    // Ask EVERY connected box about a pid. Used to find the owner of a server that heartbeats but was
    // not spun up through here (the scheduled-task one), so it can be watched properly instead of being
    // held forever for want of a box. Returns how many boxes were asked.
    public int ProbeAll(string pid)
    {
        var line = JsonSerializer.Serialize(new { type = "probe", pid });
        int n = 0;
        foreach (var a in _agents.Values) if (a.SendLine(line)) n++;
        return n;
    }

    // Force-stop a pid on a box. Used before every replacement launch: a FROZEN server is still alive
    // and still owns its game port, so launching next to it would leave two servers for one station.
    public bool Kill(string box, string pid, string reason)
    {
        var a = ByBox(box);
        return a != null && a.SendLine(JsonSerializer.Serialize(new { type = "kill", pid, reason }));
    }

    // Pick a target agent: exact box name if given (and it has free capacity), else the box with the
    // most free capacity.
    public AgentConnection? Pick(string? box)
    {
        var list = _agents.Values.ToList();
        if (!string.IsNullOrEmpty(box))
            return list.FirstOrDefault(a => a.Box == box && a.Free > 0);
        return list.Where(a => a.Free > 0).OrderByDescending(a => a.Free).FirstOrDefault();
    }
}
