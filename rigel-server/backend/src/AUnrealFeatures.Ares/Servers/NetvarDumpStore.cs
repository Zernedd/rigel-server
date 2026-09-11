using System.Collections.Concurrent;

namespace AUnrealFeatures.Ares.Servers;

// Latest full netvar/state dump per deployment, as reported by the game server itself.
//
// The game's native dashboard reporting posts a "netvars" server_event (~1.6 MB, the entire serialized
// netvar tree: world config, module slots, gamemode config) and a "state" event every few seconds.
// AddServerEvent deliberately never stores those in LiteDB (it would bloat production.db), so until now
// they were thrown away. They are, however, the only complete and authoritative list of every netvar the
// running server actually has, with live values -- exactly what the dashboard's netvar editor needs.
//
// Cost is deliberately minimal: the body has already been received and deserialised by the time we see
// it, so keeping the latest string per deployment is a reference swap. Disk persistence (so a backend
// restart still has a catalog) is throttled to once per minute per deployment+type.
public static class NetvarDumpStore
{
    public sealed record Dump(string DeploymentId, string EventType, string Data, DateTime ReceivedAtUtc);

    private static readonly ConcurrentDictionary<string, Dump> Latest = new();
    private static readonly ConcurrentDictionary<string, DateTime> LastDiskWrite = new();
    private static readonly TimeSpan DiskInterval = TimeSpan.FromSeconds(60);
    private static readonly string Dir = Path.Combine(Environment.CurrentDirectory, "netvar_dumps");

    public static bool IsDumpType(string? eventType) => eventType is "netvars" or "state";

    public static void Record(string deploymentId, string? eventType, string? data)
    {
        if (string.IsNullOrEmpty(data) || !IsDumpType(eventType)) return;
        string key = $"{deploymentId}|{eventType}";
        DateTime now = DateTime.UtcNow;
        Latest[key] = new Dump(deploymentId, eventType!, data, now);

        if (LastDiskWrite.TryGetValue(key, out var last) && now - last < DiskInterval) return;
        LastDiskWrite[key] = now;
        try
        {
            Directory.CreateDirectory(Dir);
            File.WriteAllText(FilePath(deploymentId, eventType!), data);
        }
        catch (Exception ex) { Console.WriteLine($"[NETVARS] could not persist dump: {ex.Message}"); }
    }

    // In-memory entries plus anything persisted on disk from before a restart.
    public static IReadOnlyList<object> List()
    {
        var rows = new Dictionary<string, object>();
        try
        {
            if (Directory.Exists(Dir))
                foreach (var f in Directory.GetFiles(Dir, "*.json"))
                {
                    var name = Path.GetFileNameWithoutExtension(f);          // <deployment>.<type>
                    int dot = name.LastIndexOf('.');
                    if (dot <= 0) continue;
                    var info = new FileInfo(f);
                    rows[$"{name[..dot]}|{name[(dot + 1)..]}"] = new
                    {
                        deployment_id = name[..dot], event_type = name[(dot + 1)..],
                        bytes = info.Length, received_at = info.LastWriteTimeUtc, source = "disk",
                    };
                }
        }
        catch { /* listing is best-effort */ }

        foreach (var d in Latest.Values)
            rows[$"{d.DeploymentId}|{d.EventType}"] = new
            {
                deployment_id = d.DeploymentId, event_type = d.EventType,
                bytes = d.Data.Length, received_at = d.ReceivedAtUtc, source = "live",
            };
        return rows.Values.ToList();
    }

    // deploymentId null/empty -> the most recently received dump of that type.
    public static Dump? Get(string? deploymentId, string eventType)
    {
        if (!IsDumpType(eventType)) return null;
        if (string.IsNullOrEmpty(deploymentId))
        {
            var newest = Latest.Values.Where(d => d.EventType == eventType)
                                      .OrderByDescending(d => d.ReceivedAtUtc).FirstOrDefault();
            if (newest != null) return newest;
            try
            {
                var f = Directory.Exists(Dir)
                    ? Directory.GetFiles(Dir, $"*.{eventType}.json").OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault()
                    : null;
                if (f == null) return null;
                var name = Path.GetFileNameWithoutExtension(f);
                return new Dump(name[..name.LastIndexOf('.')], eventType, File.ReadAllText(f), File.GetLastWriteTimeUtc(f));
            }
            catch { return null; }
        }

        if (Latest.TryGetValue($"{deploymentId}|{eventType}", out var hit)) return hit;
        try
        {
            var f = FilePath(deploymentId, eventType);
            return File.Exists(f) ? new Dump(deploymentId, eventType, File.ReadAllText(f), File.GetLastWriteTimeUtc(f)) : null;
        }
        catch { return null; }
    }

    private static string FilePath(string deploymentId, string eventType)
    {
        var safe = string.Concat(deploymentId.Select(c => Path.GetInvalidFileNameChars().Contains(c) || c == '.' ? '_' : c));
        return Path.Combine(Dir, $"{safe}.{eventType}.json");
    }
}
