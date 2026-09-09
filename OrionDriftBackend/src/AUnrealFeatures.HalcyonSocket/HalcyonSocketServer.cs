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

    public HalcyonSocketServer() : base(HOSTNAME, PORT)
    {
        _hub.Start();
        Instance = this;
        Console.WriteLine($"[HalcyonSocket] control HTTP on {PORT}, agent TCP on {AGENT_PORT}");
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
