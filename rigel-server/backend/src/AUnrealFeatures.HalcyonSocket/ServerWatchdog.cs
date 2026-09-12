using System.Collections.Concurrent;

namespace AUnrealFeatures.HalcyonSocket;

// ─── SERVER WATCHDOG ────────────────────────────────────────────────────────────────────────────
//
// [2026-09-12] The allocator could see a game server DIE -- the agent owns the process object, so an
// exit is reported -- but not FREEZE. A wedged server leaves its process alive, holding its port and
// its EOS session, so it stays in the browser and players keep joining a server that never ticks.
// Nobody notices until someone complains.
//
// This watches every deployment and puts a new one up when it is certain the old one is gone. The word
// that matters is CERTAIN. A watchdog that guesses is worse than none: it kills servers with players on
// them. So it acts on exactly three verdicts, each of which a healthy server cannot produce:
//
//   process-exited  the agent that owns the process says it exited. Not an inference.
//   engine-frozen   heartbeats are STILL ARRIVING (so the process is alive and its network is fine --
//                   it is talking to us right now) and they say the engine has not dispatched a single
//                   UFunction in FreezeDispatchMs. A live dedicated server dispatches constantly.
//   silent          the process is confirmed alive by the agent, it used to heartbeat every 5s, and it
//                   has sent nothing for SilentMs. The agent being connected is what makes this safe:
//                   the box and its network are demonstrably up, so the silence is the game's.
//
// Everything else is explicitly NOT enough, and is logged as a hold rather than acted on:
//   - agent not connected: cannot tell a dead server from a dead box. Never act.
//   - a server still inside its start-up grace: it has not had a chance to heartbeat yet.
//   - a server we have never heard a heartbeat from: we do not know it was ever up.
//
// Restarting reuses the SAME deployment id, so register_server upserts the existing station rather than
// minting a new one -- the station keeps its name, its roles and its config across the restart.
//
// A circuit breaker stops a crash-loop from spinning forever: MaxRestarts inside RestartWindow and the
// deployment is retired, left alone, and logged loudly for a human.
public sealed class ServerWatchdog
{
    // ─── knobs (env-overridable so a box can be tuned without a rebuild) ───
    static int Env(string name, int fallback)
        => int.TryParse(Environment.GetEnvironmentVariable(name), out var v) && v > 0 ? v : fallback;

    // A heartbeat lands every 5s (HalcyonA2 kHeartbeatMs).
    static readonly int HeartbeatGraceMs  = Env("RIGEL_WD_HB_GRACE_MS",   60_000);   // still "arriving" within this
    static readonly int FreezeDispatchMs  = Env("RIGEL_WD_FREEZE_MS",     90_000);   // no UFunction dispatch = wedged
    static readonly int SilentMs          = Env("RIGEL_WD_SILENT_MS",    180_000);   // alive but saying nothing
    static readonly int StartupGraceMs    = Env("RIGEL_WD_STARTUP_MS",   300_000);   // let a fresh server boot
    static readonly int CooldownMs        = Env("RIGEL_WD_COOLDOWN_MS",  180_000);   // after we act, leave it alone
    static readonly int MaxRestarts       = Env("RIGEL_WD_MAX_RESTARTS",      3);
    static readonly int RestartWindowMs   = Env("RIGEL_WD_WINDOW_MS",  1_800_000);   // 30 min
    static readonly int EvaluateEveryMs   = Env("RIGEL_WD_TICK_MS",       10_000);

    // Wired by the Ares host at start-up. Kept as a hook so this project keeps its single project
    // reference (Ares -> HalcyonSocket, never the other way): dropping the EOS session and marking the
    // DB row offline lives over there.
    public static Action<string, string>? TakeDeploymentOffline;   // (deploymentId, reason)

    public sealed class Watched
    {
        public string   DeploymentId = "";
        public string   Box          = "";
        public string   Name         = "";
        public string?  Map, Region;
        public string   Args         = "";
        public int      Pid;
        public DateTime CreatedAt    = DateTime.UtcNow;
        public DateTime? LastHeartbeat;
        public ulong    LastSeq;
        public ulong    DispatchAgeMs, TickAgeMs, UptimeMs;
        public int      Players = -1;

        public DateTime? LastRestartAt;
        public DateTime  WindowStart = DateTime.UtcNow;
        public int       RestartsInWindow;
        public bool      Retired;            // circuit breaker tripped, or stopped on purpose
        public string    RetiredReason = "";
        public string    LastVerdict = "starting";

        // probe handshake with the agent: we ask "is this pid alive?" and read the answer next pass.
        public DateTime? ProbeSentAt;
        public bool?     ProbeAlive;
        public DateTime? ProbeAnsweredAt;

        // Adopting a server we did not launch: boxes that have claimed this pid as theirs.
        public DateTime? AdoptAskedAt;
        public readonly HashSet<string> BoxClaims = new();
    }

    private readonly ConcurrentDictionary<string, Watched> _servers = new();          // deploymentId -> state
    private readonly ConcurrentDictionary<string, string>  _reqToDeployment = new();  // spin-up request id -> deployment
    private readonly ConcurrentDictionary<string, string>  _pidToDeployment = new();  // "box/pid" -> deployment
    private readonly AgentHub _hub;
    private readonly Func<string?, string?, string?, string?, string?, SpinUpResult> _spinUp;

    public ServerWatchdog(AgentHub hub, Func<string?, string?, string?, string?, string?, SpinUpResult> spinUp)
    {
        _hub    = hub;
        _spinUp = spinUp;
        _ = Task.Run(EvaluateLoop);
    }

    static void Log(string m) => Console.WriteLine($"[Watchdog] {m}");

    public IReadOnlyCollection<Watched> Servers => _servers.Values.ToList();

    // ─── inputs ────────────────────────────────────────────────────────────────────────────────

    // Called by RequestSpinUp for every launch, ours or the dashboard's, so we know how to put this
    // exact server back up: same box, same map, same args, same deployment id.
    public void OnSpinUpRequested(string deploymentId, string reqId, string box, string? map, string? region, string args, string? name)
    {
        var w = _servers.GetOrAdd(deploymentId, _ => new Watched { DeploymentId = deploymentId });
        w.Box    = box;
        w.Map    = map;
        w.Region = region;
        w.Args   = args;
        if (!string.IsNullOrWhiteSpace(name)) w.Name = name!;
        // A fresh launch restarts the start-up grace, and clears a stale pid so a late "exited" for the
        // PREVIOUS process cannot be read as this one dying.
        w.CreatedAt   = DateTime.UtcNow;
        w.Pid         = 0;
        w.ProbeAlive  = null;
        w.ProbeSentAt = null;
        w.LastVerdict = "starting";
        _reqToDeployment[reqId] = deploymentId;
    }

    public void OnSpunUp(string reqId, string pid, string status, string box)
    {
        if (!_reqToDeployment.TryRemove(reqId, out var dep)) return;
        if (!_servers.TryGetValue(dep, out var w)) return;
        if (status == "ok" && int.TryParse(pid, out var p) && p > 0)
        {
            w.Pid = p;
            _pidToDeployment[$"{box}/{p}"] = dep;
            Log($"{Short(dep)} launched on '{box}' pid={p}");
        }
        else
        {
            // The agent refused, or the process died during init. It told us so -- that part is certain
            // -- but a full box or a failed inject will not fix itself, so leave it to a human rather
            // than hammering the box in a loop.
            w.Retired       = true;
            w.RetiredReason = $"spin-up failed: {status}";
            w.LastVerdict   = "spinup-failed";
            Log($"{Short(dep)} spin-up failed on '{box}' ({status}) -- not watching it");
        }
    }

    public void OnExited(string box, string pid, string code)
    {
        if (!_pidToDeployment.TryRemove($"{box}/{pid}", out var dep)) return;
        if (!_servers.TryGetValue(dep, out var w)) return;
        if (w.Pid.ToString() != pid) return;              // a stale exit for a process we already replaced
        w.ProbeAlive      = false;
        w.ProbeAnsweredAt = DateTime.UtcNow;
        Log($"{Short(dep)} pid {pid} exited (code {code}) -- the agent owns the process, so this is certain");
        TryRestart(w, "process-exited", $"the agent reported pid {pid} exited with code {code}");
    }

    // The game server's own heartbeat (POST /server_heartbeat, every 5s, from a worker thread that is
    // separate from the engine -- see HalcyonA2 HeartbeatWorker). Its value is that it keeps arriving
    // while the ENGINE is wedged, which is what makes the freeze verdict safe.
    public void OnHeartbeat(string deploymentId, int pid, ulong seq, ulong dispatchAgeMs, ulong tickAgeMs,
                            ulong uptimeMs, int players, string? box = null)
    {
        if (string.IsNullOrWhiteSpace(deploymentId)) return;
        var w = _servers.GetOrAdd(deploymentId, _ =>
        {
            Log($"{Short(deploymentId)} is heartbeating but we did not launch it -- watching it too");
            return new Watched { DeploymentId = deploymentId, CreatedAt = DateTime.UtcNow };
        });
        if (pid > 0 && w.Pid != pid)
        {
            w.Pid = pid;
            if (!string.IsNullOrWhiteSpace(w.Box)) _pidToDeployment[$"{w.Box}/{pid}"] = deploymentId;
            // A different process now: any box that claimed the OLD pid was answering about something
            // else, so start the ownership question over rather than inheriting a stale answer.
            lock (w.BoxClaims) w.BoxClaims.Clear();
            w.AdoptAskedAt = null;
        }
        if (!string.IsNullOrWhiteSpace(box) && string.IsNullOrWhiteSpace(w.Box)) w.Box = box!;
        w.LastHeartbeat = DateTime.UtcNow;
        w.LastSeq       = seq;
        w.DispatchAgeMs = dispatchAgeMs;
        w.TickAgeMs     = tickAgeMs;
        w.UptimeMs      = uptimeMs;
        w.Players       = players;
        w.ProbeAlive    = null;          // it is obviously alive; drop any stale probe answer
    }

    public void OnProbeResult(string box, string pid, bool alive)
    {
        if (_pidToDeployment.TryGetValue($"{box}/{pid}", out var dep) && _servers.TryGetValue(dep, out var known))
        {
            known.ProbeAlive      = alive;
            known.ProbeAnsweredAt = DateTime.UtcNow;
            return;
        }

        // An answer we did not have a mapping for. This is how a server we never launched -- the one the
        // scheduled task starts -- gets adopted: it heartbeats, we do not know which box runs it, so we
        // ask every box about its pid and the one that owns the process says yes.
        if (!alive || !int.TryParse(pid, out var p)) return;
        foreach (var w in _servers.Values)
            if (w.Pid == p && string.IsNullOrWhiteSpace(w.Box))
                lock (w.BoxClaims) w.BoxClaims.Add(box);
    }

    // Deliberate stop: stop watching, so a planned shutdown is never "restored".
    public bool Retire(string deploymentId, string reason)
    {
        if (!_servers.TryGetValue(deploymentId, out var w)) return false;
        w.Retired       = true;
        w.RetiredReason = reason;
        w.LastVerdict   = "retired";
        Log($"{Short(deploymentId)} retired: {reason}");
        return true;
    }

    public bool Resume(string deploymentId)
    {
        if (!_servers.TryGetValue(deploymentId, out var w)) return false;
        w.Retired          = false;
        w.RetiredReason    = "";
        w.RestartsInWindow = 0;
        w.WindowStart      = DateTime.UtcNow;
        w.LastVerdict      = "watching";
        Log($"{Short(deploymentId)} back under watch");
        return true;
    }

    public bool Forget(string deploymentId) => _servers.TryRemove(deploymentId, out _);

    // ─── the decision ──────────────────────────────────────────────────────────────────────────

    private async Task EvaluateLoop()
    {
        while (true)
        {
            try { Evaluate(); }
            catch (Exception ex) { Log($"evaluate failed: {ex.Message}"); }
            await Task.Delay(EvaluateEveryMs);
        }
    }

    private void Evaluate()
    {
        var now = DateTime.UtcNow;
        foreach (var w in _servers.Values)
        {
            if (w.Retired) continue;

            // Just acted: a replacement needs time to launch, inject and register before it can be
            // judged. Without this a slow boot looks exactly like a dead server and we would loop.
            if (w.LastRestartAt is { } last && (now - last).TotalMilliseconds < CooldownMs)
            { w.LastVerdict = "cooling down"; continue; }

            // A server that heartbeats but has no box is one we did not launch -- the scheduled task
            // starts one on every game box. Without a box we could never verify or replace it, so find
            // its owner: ask every connected box about its pid, and the one holding the process answers.
            // Adopt only on a SINGLE claim; pids are not unique across boxes, and guessing which machine
            // to kill a process on is exactly the kind of certainty this class refuses to fake.
            if (string.IsNullOrWhiteSpace(w.Box) && w.Pid > 0)
            {
                string[] claims;
                lock (w.BoxClaims) claims = w.BoxClaims.ToArray();
                if (claims.Length == 1)
                {
                    w.Box = claims[0];
                    _pidToDeployment[$"{w.Box}/{w.Pid}"] = w.DeploymentId;
                    Log($"{Short(w.DeploymentId)} adopted: pid {w.Pid} belongs to box '{w.Box}'");
                }
                else
                {
                    if (claims.Length > 1)
                    {
                        w.LastVerdict = $"holding: pid {w.Pid} was claimed by {claims.Length} boxes -- will not guess";
                        continue;
                    }
                    if (w.AdoptAskedAt == null || (now - w.AdoptAskedAt.Value).TotalMilliseconds > EvaluateEveryMs * 3)
                    {
                        w.AdoptAskedAt = now;
                        var asked = _hub.ProbeAll(w.Pid.ToString());
                        w.LastVerdict = $"asking {asked} box(es) which one runs pid {w.Pid}";
                    }
                    continue;
                }
            }

            // We can only act on a box we are still talking to. If the agent is gone we cannot tell a
            // dead SERVER from a dead BOX -- and spinning a replacement elsewhere while the original is
            // quietly fine would double-run the station.
            var agent = string.IsNullOrWhiteSpace(w.Box) ? null : _hub.ByBox(w.Box);
            if (agent == null) { w.LastVerdict = "holding: agent for this box is not connected"; continue; }

            if (w.LastHeartbeat == null)
            {
                // Never heard from it. Either it is still booting, or it came up without the DLL. Both
                // are "we do not know", and a real launch failure is reported by the agent separately.
                w.LastVerdict = (now - w.CreatedAt).TotalMilliseconds < StartupGraceMs
                    ? "starting (no heartbeat yet)"
                    : "holding: never heartbeated, so we never knew it was up";
                continue;
            }

            var sinceHb = (now - w.LastHeartbeat.Value).TotalMilliseconds;

            // ── verdict: engine-frozen ──
            // The heartbeat is still arriving, so the process is alive and its network works -- it is
            // talking to us as we decide. And it says nothing has dispatched a UFunction in
            // FreezeDispatchMs. A healthy dedicated server cannot produce that pair.
            if (sinceHb < HeartbeatGraceMs)
            {
                if (w.DispatchAgeMs >= (ulong)FreezeDispatchMs)
                {
                    TryRestart(w, "engine-frozen",
                        $"still heartbeating (so the process is alive) but no UFunction dispatch for " +
                        $"{w.DispatchAgeMs / 1000}s, and our own tick has not run for {w.TickAgeMs / 1000}s");
                }
                else
                {
                    w.LastVerdict = $"healthy ({w.Players} player(s), up {w.UptimeMs / 60000}m)";
                }
                continue;
            }

            // ── verdict: silent ──
            // Nothing for a while. The process may be gone (we would usually get "exited", but only for
            // servers this agent launched) or wedged so hard the heartbeat thread stopped too. Ask the
            // agent, which owns the box, and decide on its answer -- never on the silence alone.
            if (sinceHb < SilentMs) { w.LastVerdict = $"quiet for {sinceHb / 1000:F0}s -- waiting"; continue; }

            // The agent has only just (re)connected. Whatever took it away -- a box reboot, a network
            // blip -- could have been dropping this server's heartbeats at the same time, so the quiet
            // we are looking at may be the network's and not the game's. Give it the same window again
            // to prove itself before any of that silence counts. Found by the watchdog test harness:
            // without this, an agent reconnect immediately condemned a server on stale silence.
            var agentUp = (now - agent.ConnectedAt).TotalMilliseconds;
            if (agentUp < SilentMs)
            {
                w.LastVerdict = $"silent, but its box only came back {agentUp / 1000:F0}s ago -- waiting for a fresh heartbeat";
                continue;
            }

            if (w.Pid <= 0) { w.LastVerdict = "holding: silent, but we do not know its pid to check"; continue; }

            if (w.ProbeAlive is null || w.ProbeAnsweredAt is null || w.ProbeAnsweredAt < w.LastHeartbeat)
            {
                if (w.ProbeSentAt == null || (now - w.ProbeSentAt.Value).TotalMilliseconds > EvaluateEveryMs * 2)
                {
                    w.ProbeSentAt = now;
                    _hub.Probe(w.Box, w.Pid.ToString());
                    w.LastVerdict = $"silent for {sinceHb / 1000:F0}s -- asking the agent whether pid {w.Pid} is alive";
                }
                continue;   // decide next pass, on the answer
            }

            if (w.ProbeAlive == false)
            {
                TryRestart(w, "process-exited",
                    $"silent for {sinceHb / 1000:F0}s and the agent confirms pid {w.Pid} is not running");
            }
            else
            {
                TryRestart(w, "silent",
                    $"pid {w.Pid} is alive and its box is online, but it has sent nothing for " +
                    $"{sinceHb / 1000:F0}s after heartbeating every 5s");
            }
        }
    }

    // ─── the action ────────────────────────────────────────────────────────────────────────────

    private void TryRestart(Watched w, string verdict, string why)
    {
        var now = DateTime.UtcNow;
        if (w.Retired) return;
        if (w.LastRestartAt is { } last && (now - last).TotalMilliseconds < CooldownMs) return;

        if ((now - w.WindowStart).TotalMilliseconds > RestartWindowMs)
        {
            w.WindowStart      = now;
            w.RestartsInWindow = 0;
        }
        if (w.RestartsInWindow >= MaxRestarts)
        {
            w.Retired       = true;
            w.RetiredReason = $"{MaxRestarts} restarts inside {RestartWindowMs / 60000}m -- it is not coming back on its own";
            w.LastVerdict   = "GIVING UP";
            Log($"*** {Short(w.DeploymentId)} '{w.Name}' GIVING UP: {w.RetiredReason}. Last verdict was {verdict}: {why}");
            return;
        }

        w.LastRestartAt = now;
        w.RestartsInWindow++;
        w.LastVerdict   = $"restarting ({verdict})";
        Log($"*** {Short(w.DeploymentId)} '{w.Name}' is DOWN [{verdict}]: {why}");

        // Stop players being sent to a corpse. A frozen server still holds its EOS session, so without
        // this the station stays in the browser through the whole restart.
        try { TakeDeploymentOffline?.Invoke(w.DeploymentId, verdict); }
        catch (Exception ex) { Log($"  could not take {Short(w.DeploymentId)} offline: {ex.Message}"); }

        // Kill first, ALWAYS. On a freeze the process is still alive and still owns the game port; a
        // replacement launched next to it would bind a different port and we would have two servers for
        // one station. The agent no-ops on a pid that is already gone.
        if (w.Pid > 0 && !string.IsNullOrWhiteSpace(w.Box))
        {
            if (_hub.Kill(w.Box, w.Pid.ToString(), verdict)) Log($"  killed pid {w.Pid} on '{w.Box}'");
            else                                            Log($"  could not reach '{w.Box}' to kill pid {w.Pid}");
            _pidToDeployment.TryRemove($"{w.Box}/{w.Pid}", out _);
        }

        // Put it back up AS THE SAME DEPLOYMENT. register_server upserts on the id, so the station keeps
        // its name, roles and config instead of a new one appearing next to the old.
        var args   = EnsureDeploymentArg(w.Args, w.DeploymentId);
        var result = _spinUp(w.Box, w.Map, w.Region, args, string.IsNullOrWhiteSpace(w.Name) ? null : w.Name);
        if (result.success)
        {
            w.CreatedAt     = now;      // restart the start-up grace
            w.Pid           = 0;
            w.LastHeartbeat = null;
            w.ProbeAlive    = null;
            w.ProbeSentAt   = null;
            Log($"  replacement requested on '{result.agent}' (request {result.request_id}), restart {w.RestartsInWindow}/{MaxRestarts}");
        }
        else
        {
            Log($"  replacement REFUSED: {result.error}");
        }
    }

    internal static string EnsureDeploymentArg(string args, string deploymentId)
    {
        args ??= "";
        return args.Contains("-DashboardDeploymentId=", StringComparison.OrdinalIgnoreCase)
            ? args
            : (args + $" -DashboardDeploymentId={deploymentId}").Trim();
    }

    static string Short(string id) => id.Length > 8 ? id[..8] : id;
}
