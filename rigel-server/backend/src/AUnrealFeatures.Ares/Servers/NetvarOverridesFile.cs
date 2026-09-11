using System.Security.Cryptography;
using System.Text;
using AUnrealFeatures.Ares.Models;

namespace AUnrealFeatures.Ares.Servers;

// Dashboard netvar overrides, pushed to co-located game servers as files instead of being polled.
//
// Overrides live in a station's Config under the reserved "nv." prefix (see A2StationDbServer.NetvarOverridePrefix):
//   nv.module.<SlotID|*>.<Variable>   gamemode config variable
//   nv.world.<path>                   world netvar, e.g. nv.world.config/player/brakeStrength
//
// Whenever a station's config changes, the text below is written atomically to
// <backend working dir>\netvar_overrides\<deploymentId>.txt for every deployment of that station, and to
// _latest.txt for the station of the most recent online deployment (servers launched with a fixed id resolve
// to that). The injected DLL blocks on a folder-change notification and re-reads only when a file changes, so
// nothing polls. GET /v1/deployments/{id}/netvar_overrides returns the same text for the server's startup load.
//
// Format (plain text so the C++ side needs no parser):
//   v=<sha1 of the lines>
//   module<TAB><SlotID|*><TAB><Variable><TAB><value>
//   world<TAB><TAB><path><TAB><value>
public static class NetvarOverridesFile
{
    public static readonly string Dir = Path.Combine(Environment.CurrentDirectory, "netvar_overrides");
    private static readonly object Gate = new();
    private static bool _initialised;

    // Values typed into the dashboard sometimes arrive JSON-quoted ("DRIFTBALLIS"); store/serve them bare.
    public static string Clean(string? v)
    {
        v = (v ?? "").Trim();
        if (v.Length >= 2 && v[0] == '"' && v[^1] == '"') v = v[1..^1];
        return v.Replace('\t', ' ').Replace('\r', ' ').Replace('\n', ' ');
    }

    public static string BuildText(StationDbObject? station)
    {
        var lines = new List<string>();
        if (station?.Config != null)
        {
            foreach (var kv in station.Config.Where(k => k.Key.StartsWith(A2StationDbServer.NetvarOverridePrefix))
                                             .OrderBy(k => k.Key, StringComparer.Ordinal))
            {
                string rest = kv.Key.Substring(A2StationDbServer.NetvarOverridePrefix.Length);
                string value = Clean(kv.Value);
                if (rest.StartsWith("module."))
                {
                    string body = rest.Substring("module.".Length);
                    int dot = body.IndexOf('.');
                    if (dot <= 0 || dot == body.Length - 1) continue;
                    lines.Add($"module\t{body[..dot]}\t{body[(dot + 1)..]}\t{value}");
                }
                else if (rest.StartsWith("world."))
                {
                    lines.Add($"world\t\t{rest.Substring("world.".Length)}\t{value}");
                }
            }
        }
        string joined = string.Join("\n", lines);
        string version;
        using (var sha = SHA1.Create())
            version = Convert.ToHexString(sha.ComputeHash(Encoding.UTF8.GetBytes(joined)));
        return $"v={version}\n{joined}";
    }

    // Same resolution as GetDeploymentV1: an unknown/fixed launch id maps to the most recent online deployment.
    public static StationDbObject? ResolveStation(string deploymentId)
    {
        var deps = Program.Database.GetCollection<DeploymentDbObject>(true);
        string stationId = deps?.FindAll().Where(d => d.DeploymentId == deploymentId).Select(d => d.StationId).FirstOrDefault() ?? "";
        if (string.IsNullOrEmpty(stationId))
            stationId = LatestOnlineStationId() ?? "";
        if (string.IsNullOrEmpty(stationId)) return null;
        return Program.Database.GetCollection<StationDbObject>(true)?.FindAll().FirstOrDefault(s => s.StationId == stationId);
    }

    private static string? LatestOnlineStationId()
        => Program.Database.GetCollection<DeploymentDbObject>(true)?.FindAll()
               .Where(d => d.Online && !string.IsNullOrEmpty(d.StationId))
               .OrderByDescending(d => d.CreatedAt)
               .Select(d => d.StationId)
               .FirstOrDefault();

    public static void OnStationConfigChanged(string stationId)
    {
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(Dir);
                var station = Program.Database.GetCollection<StationDbObject>(true)?.FindAll().FirstOrDefault(s => s.StationId == stationId);
                string text = BuildText(station);
                var deps = Program.Database.GetCollection<DeploymentDbObject>(true)?.FindAll()
                               .Where(d => d.StationId == stationId).Select(d => d.DeploymentId).ToList() ?? new List<string>();
                foreach (var dep in deps) WriteAtomic(Path.Combine(Dir, Safe(dep) + ".txt"), text);
                if (LatestOnlineStationId() == stationId) WriteAtomic(Path.Combine(Dir, "_latest.txt"), text);
            }
        }
        catch (Exception ex) { Console.WriteLine($"[NETVARS] could not write override files for {stationId}: {ex.Message}"); }
    }

    // First call (the game server's startup fetch) makes sure every station's files exist before it starts watching.
    public static void EnsureAllWritten()
    {
        if (_initialised) return;
        _initialised = true;
        var ids = Program.Database.GetCollection<StationDbObject>(true)?.FindAll().Select(s => s.StationId).ToList() ?? new List<string>();
        foreach (var id in ids) OnStationConfigChanged(id);
    }

    private static void WriteAtomic(string path, string text)
    {
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, text);
        File.Move(tmp, path, true);
    }

    private static string Safe(string s)
        => string.Concat(s.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c));
}
