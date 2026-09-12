using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace AUnrealFeatures.HalcyonSocket;

public interface IHalcyonSocketServer { }

// The allocator socket. HTTP control surface on 9095 (for testing / direct use) that OWNS the raw
// agent TCP hub on 9100. The dashboard's "Spin Up" button reaches this in-process via
// AresDashboardServer -> HalcyonSocketServer.Instance.RequestSpinUp(...) (so no new dashboard proxy
// is needed). The launched game server then SELF-REGISTERS via /register_server exactly as today.
public sealed class HalcyonSocketServer : AstraHttpServer, IHalcyonSocketServer
{
    const string HOSTNAME   = "localhost";
    const ushort PORT       = 9095;   // HTTP control
    const int    AGENT_PORT = 9100;   // raw TCP for HalcyonAllocatorAgent connections

    public static HalcyonSocketServer? Instance { get; private set; }

    // Dashboard-supplied deployment names, keyed by the deployment id we mint at spin-up. register_server
    // (A2StationDbServer, same process) reads this so a name typed in the "Spin Up" modal becomes the
    // deployment's DeploymentName instead of the DLL's default. Survives until the server registers.
    public static readonly ConcurrentDictionary<string, string> PendingNames = new();

    private readonly AgentHub _hub = new(AGENT_PORT);

    // Watches every deployment and replaces one when it is CERTAIN it is down -- a crashed process, or
    // a frozen engine, which the agent alone cannot see. See ServerWatchdog for the three verdicts and
    // for why nothing weaker is allowed to act.
    public ServerWatchdog Watchdog { get; }

    public HalcyonSocketServer() : base(HOSTNAME, PORT)
    {
        _hub.Start();
        Watchdog = new ServerWatchdog(_hub, (b, m, r, a, n) => RequestSpinUp(b, m, r, a, n));
        _hub.OnSpunUp      = Watchdog.OnSpunUp;
        _hub.OnExited      = Watchdog.OnExited;
        _hub.OnProbeResult = Watchdog.OnProbeResult;
        Instance = this;
        Console.WriteLine($"[HalcyonSocket] control HTTP on {PORT}, agent TCP on {AGENT_PORT}, watchdog armed");
    }

    // Relay a spin-up request to a connected agent. box=null -> least-loaded box. Fire-and-forget:
    // the agent launches + injects, then the game server self-registers and appears in the list.
    public SpinUpResult RequestSpinUp(string? box, string? map, string? region, string? args, string? name = null)
    {
        var agent = _hub.Pick(box);
        if (agent == null)
        {
            return new SpinUpResult
            {
                success = false,
                error   = string.IsNullOrEmpty(box) ? "no agent with free capacity" : $"agent '{box}' unavailable or full"
            };
        }

        var reqId = Guid.NewGuid().ToString("N");

        // Decide a UNIQUE deployment id here, BEFORE the game launches, and pass it as the launch
        // arg the game reads for its dashboard/config fetch (-DashboardDeploymentId). The injected
        // DLL reads the same arg and registers the DB row under it, so the game's config GET and the
        // real registration match — no more the fixed "halcyon" id reused across every deployment.
        // Reuse an id the caller already supplied; otherwise mint one. Either way we KNOW the id, so
        // we can stash the dashboard-entered name against it for register_server to pick up.
        var finalArgs = args ?? "";
        var existing  = Regex.Match(finalArgs, @"-DashboardDeploymentId=(\S+)", RegexOptions.IgnoreCase);
        string deploymentId;
        if (existing.Success)
        {
            deploymentId = existing.Groups[1].Value;
        }
        else
        {
            deploymentId = Guid.NewGuid().ToString("N");
            finalArgs = (finalArgs + $" -DashboardDeploymentId={deploymentId}").Trim();
        }
        if (!string.IsNullOrWhiteSpace(name))
            PendingNames[deploymentId] = name!.Trim();

        var payload = new
        {
            type   = "spinup",
            id     = reqId,
            map    = map ?? "",
            region = region ?? "",
            args   = finalArgs
        };
        var json = JsonSerializer.Serialize(payload);
        if (!agent.SendLine(json))
            return new SpinUpResult { success = false, request_id = reqId, agent = agent.Box, error = "send to agent failed" };

        // Tell the watchdog how to put this exact server back up later: same box, same map, same args,
        // same deployment id. Recorded for EVERY launch, the dashboard's included, so nothing that gets
        // spun up here is left unwatched.
        Watchdog.OnSpinUpRequested(deploymentId, reqId, agent.Box, map, region, finalArgs, name);

        Console.WriteLine($"[HalcyonSocket] spinup {reqId} -> agent '{agent.Box}' (map='{map}', region='{region}')");
        return new SpinUpResult { success = true, request_id = reqId, agent = agent.Box };
    }

    // Connected boxes + capacity — used by the dashboard's box picker (via AresDashboardServer's
    // /api/agents on :8080, and directly here on :9095).
    public AgentInfo[] GetAgents() => _hub.Agents.Select(a => new AgentInfo
    {
        box       = a.Box,
        capacity  = a.Capacity,
        running   = a.Running,
        free      = a.Free,
        last_seen = a.LastSeen
    }).ToArray();

    [HttpGet("/agents")]
    public async Task<IHttpActionResult> ListAgents(IHttpRequest request, IHttpResponse response)
        => Results.Ok(new AgentListResult { agents = GetAgents() });

    // POST /spinup — direct/test entry (the dashboard normally goes through AresDashboardServer).
    [HttpPost("/spinup")]
    public async Task<IHttpActionResult> SpinUp(IHttpRequest request, IHttpResponse response)
    {
        SpinUpBody? body;
        try { body = JsonSerializer.Deserialize<SpinUpBody>(request.Body); }
        catch { body = null; }
        return Results.Ok(RequestSpinUp(body?.box, body?.map, body?.region, body?.args, body?.name));
    }

    // GET /watchdog — what the watchdog believes about every server, and why. The "verdict" field is the
    // one to read when a server did or did not get replaced.
    [HttpGet("/watchdog")]
    public async Task<IHttpActionResult> WatchdogStatus(IHttpRequest request, IHttpResponse response)
        => Results.Ok(new WatchdogStatusResult { servers = WatchdogRows() });

    public WatchdogRow[] WatchdogRows() => Watchdog.Servers.Select(w => new WatchdogRow
    {
        deployment_id     = w.DeploymentId,
        name              = w.Name,
        box               = w.Box,
        pid               = w.Pid,
        players           = w.Players,
        uptime_minutes    = (int)(w.UptimeMs / 60000),
        last_heartbeat    = w.LastHeartbeat,
        dispatch_age_ms   = w.DispatchAgeMs,
        tick_age_ms       = w.TickAgeMs,
        verdict           = w.LastVerdict,
        restarts          = w.RestartsInWindow,
        last_restart      = w.LastRestartAt,
        retired           = w.Retired,
        retired_reason    = w.RetiredReason
    }).OrderBy(r => r.name).ToArray();

    // POST /watchdog/retire — stop watching one deployment, so a DELIBERATE shutdown is not undone.
    // POST /watchdog/resume — put it back under watch (also clears the give-up state).
    [HttpPost("/watchdog/retire")]
    public async Task<IHttpActionResult> WatchdogRetire(IHttpRequest request, IHttpResponse response)
    {
        var b = ReadWatchdogBody(request);
        return Results.Ok(new SuccessResult
        {
            success = b != null && Watchdog.Retire(b.deployment_id ?? "", b.reason ?? "stopped on purpose")
        });
    }

    [HttpPost("/watchdog/resume")]
    public async Task<IHttpActionResult> WatchdogResume(IHttpRequest request, IHttpResponse response)
    {
        var b = ReadWatchdogBody(request);
        return Results.Ok(new SuccessResult { success = b != null && Watchdog.Resume(b.deployment_id ?? "") });
    }

    private static WatchdogBody? ReadWatchdogBody(IHttpRequest request)
    {
        try { return JsonSerializer.Deserialize<WatchdogBody>(request.Body); }
        catch { return null; }
    }
}

public sealed class WatchdogBody
{
    public string? deployment_id { get; set; }
    public string? reason        { get; set; }
}

public sealed class SuccessResult
{
    public bool success { get; set; }
}

public sealed class WatchdogRow
{
    public string    deployment_id   { get; set; } = "";
    public string    name            { get; set; } = "";
    public string    box             { get; set; } = "";
    public int       pid             { get; set; }
    public int       players         { get; set; }
    public int       uptime_minutes  { get; set; }
    public DateTime? last_heartbeat  { get; set; }
    public ulong     dispatch_age_ms { get; set; }
    public ulong     tick_age_ms     { get; set; }
    public string    verdict         { get; set; } = "";
    public int       restarts        { get; set; }
    public DateTime? last_restart    { get; set; }
    public bool      retired         { get; set; }
    public string    retired_reason  { get; set; } = "";
}

public sealed class WatchdogStatusResult
{
    public WatchdogRow[] servers { get; set; } = Array.Empty<WatchdogRow>();
}

public sealed class SpinUpBody
{
    public string? box    { get; set; }
    public string? map    { get; set; }
    public string? region { get; set; }
    public string? args   { get; set; }
    public string? name   { get; set; }   // dashboard-entered deployment name
}

public sealed class SpinUpResult
{
    public bool   success    { get; set; }
    public string request_id { get; set; } = "";
    public string agent      { get; set; } = "";
    public string error      { get; set; } = "";
}

public sealed class AgentInfo
{
    public string   box       { get; set; } = "";
    public int      capacity  { get; set; }
    public int      running   { get; set; }
    public int      free      { get; set; }
    public DateTime last_seen { get; set; }
}

public sealed class AgentListResult
{
    public AgentInfo[] agents { get; set; } = Array.Empty<AgentInfo>();
}
