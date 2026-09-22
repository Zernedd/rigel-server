using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using AUnrealFeatures.Hosting.Http.Interfaces;

namespace AUnrealFeatures.Ares.Servers;

// Saved Spec Editor levels.
//
// The in-game Spec Editor (Levels panel) uploads a level as arbitrary UTF-8 text through the game server, which
// calls the port-78 v1/spec/* routes (A2StationDbServer). The dashboard (port 8080, /api/spec/*) lists them and
// flips two flags:
//   autoload  -- load this level whenever a game server boots (POST v1/spec/boot sets loaded = autoload)
//   loaded    -- the DESIRED state: game servers poll v1/spec/desired and load/unload to match
// Game servers report what they actually have loaded via v1/spec/status, recorded per level as serverLoaded
// (deployment ids) + serverSeen, so the dashboard can show "on server now".
//
// Storage: <backend working dir>\spec_levels\<name>.txt plus _index.json. All writes are tmp + move, under one lock.
public static class SpecLevelsStore
{
    public static readonly string Dir = Path.Combine(Environment.CurrentDirectory, "spec_levels");
    private static string IndexPath => Path.Combine(Dir, "_index.json");
    private static readonly object Gate = new();
    private static readonly Regex NameRx = new(@"^[A-Za-z0-9 _-]{1,64}$", RegexOptions.Compiled);

    public sealed class Entry
    {
        [JsonPropertyName("autoload")]     public bool Autoload { get; set; }
        [JsonPropertyName("loaded")]       public bool Loaded { get; set; }
        [JsonPropertyName("updated")]      public string Updated { get; set; } = "";
        [JsonPropertyName("size")]         public int Size { get; set; }
        [JsonPropertyName("serverLoaded")] public List<string> ServerLoaded { get; set; } = new();
        [JsonPropertyName("serverSeen")]   public string ServerSeen { get; set; } = "";
    }

    private static readonly JsonSerializerOptions JsonOpts = new() { WriteIndented = true };

    // ── Names ────────────────────────────────────────────────────────────────────────────────
    // Route params arrive percent-encoded (Uri.AbsolutePath), so "My%20Level" -> "My Level" first.
    // Returns null when the name is not [A-Za-z0-9 _-]{1,64} (callers answer 400).
    public static string? CleanName(string? raw)
    {
        if (raw == null) return null;
        string name;
        try { name = Uri.UnescapeDataString(raw); } catch { return null; }
        name = name.Trim();
        return NameRx.IsMatch(name) ? name : null;
    }

    private static string LevelPath(string name) => Path.Combine(Dir, name + ".txt");

    // Windows file names are case-insensitive; keep one index key per file by resolving an existing key
    // case-insensitively.
    private static string ResolveKey(Dictionary<string, Entry> idx, string name)
        => idx.Keys.FirstOrDefault(k => string.Equals(k, name, StringComparison.OrdinalIgnoreCase)) ?? name;

    // ── Index I/O (call under Gate) ──────────────────────────────────────────────────────────
    private static Dictionary<string, Entry> LoadIndex()
    {
        try
        {
            if (File.Exists(IndexPath))
            {
                var d = JsonSerializer.Deserialize<Dictionary<string, Entry>>(File.ReadAllText(IndexPath, Encoding.UTF8));
                if (d != null)
                {
                    foreach (var e in d.Values) e.ServerLoaded ??= new List<string>();
                    return new Dictionary<string, Entry>(d, StringComparer.Ordinal);
                }
            }
        }
        catch (Exception ex) { Console.WriteLine($"[SPEC-LEVELS] index unreadable, rebuilding: {ex.Message}"); }

        // Missing or corrupt index: rebuild from the .txt files so a level is never lost.
        var rebuilt = new Dictionary<string, Entry>(StringComparer.Ordinal);
        if (Directory.Exists(Dir))
            foreach (var f in Directory.GetFiles(Dir, "*.txt"))
            {
                var n = Path.GetFileNameWithoutExtension(f);
                if (!NameRx.IsMatch(n)) continue;
                var fi = new FileInfo(f);
                rebuilt[n] = new Entry { Loaded = false, Autoload = false, Size = (int)fi.Length,
                                         Updated = fi.LastWriteTimeUtc.ToString("o") };
            }
        return rebuilt;
    }

    private static void SaveIndex(Dictionary<string, Entry> idx)
        => WriteAtomic(IndexPath, Encoding.UTF8.GetBytes(JsonSerializer.Serialize(idx, JsonOpts)));

    private static void WriteAtomic(string path, byte[] bytes)
    {
        Directory.CreateDirectory(Dir);
        string tmp = path + ".tmp";
        File.WriteAllBytes(tmp, bytes);
        File.Move(tmp, path, true);
    }

    private static string Now() => DateTime.UtcNow.ToString("o");

    // ── Operations ───────────────────────────────────────────────────────────────────────────
    public sealed record LevelInfo(string Name, Entry Entry);

    public static List<LevelInfo> List()
    {
        lock (Gate)
            return LoadIndex().OrderBy(k => k.Key, StringComparer.OrdinalIgnoreCase)
                              .Select(k => new LevelInfo(k.Key, k.Value)).ToList();
    }

    public static string? ReadText(string name)
    {
        lock (Gate)
        {
            var p = LevelPath(name);
            return File.Exists(p) ? File.ReadAllText(p, Encoding.UTF8) : null;
        }
    }

    public static void Put(string name, byte[] body)
    {
        lock (Gate)
        {
            var idx = LoadIndex();
            var key = ResolveKey(idx, name);
            if (!idx.TryGetValue(key, out var e))
            {
                e = new Entry { Autoload = false, Loaded = true };
                idx[key] = e;
            }
            WriteAtomic(LevelPath(key), body);
            e.Updated = Now();
            e.Size = body.Length;
            SaveIndex(idx);
        }
    }

    // Returns false when the level does not exist.
    public static bool Update(string name, bool? autoload, bool? loaded)
    {
        lock (Gate)
        {
            var idx = LoadIndex();
            var key = ResolveKey(idx, name);
            if (!idx.TryGetValue(key, out var e)) return false;
            if (autoload.HasValue) e.Autoload = autoload.Value;
            if (loaded.HasValue) e.Loaded = loaded.Value;
            SaveIndex(idx);
            return true;
        }
    }

    public static bool Delete(string name)
    {
        lock (Gate)
        {
            var idx = LoadIndex();
            var key = ResolveKey(idx, name);
            bool had = idx.Remove(key);
            var p = LevelPath(key);
            if (File.Exists(p)) { File.Delete(p); had = true; }
            if (had) SaveIndex(idx);
            return had;
        }
    }

    // A server booted: every level's desired state resets to its autoload flag. Returns the names to load.
    public static List<string> Boot(string deploymentId)
    {
        lock (Gate)
        {
            var idx = LoadIndex();
            foreach (var e in idx.Values)
            {
                e.Loaded = e.Autoload;
                // This deployment starts with nothing loaded until it reports status.
                if (!string.IsNullOrEmpty(deploymentId)) e.ServerLoaded.Remove(deploymentId);
            }
            SaveIndex(idx);
            return idx.Where(k => k.Value.Loaded).Select(k => k.Key)
                      .OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
        }
    }

    public static List<string> Desired()
    {
        lock (Gate)
            return LoadIndex().Where(k => k.Value.Loaded).Select(k => k.Key)
                              .OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
    }

    // A server reports the full set of level names it currently has loaded.
    public static void Status(string deploymentId, IEnumerable<string> loadedNames)
    {
        var set = new HashSet<string>(loadedNames, StringComparer.OrdinalIgnoreCase);
        lock (Gate)
        {
            var idx = LoadIndex();
            string now = Now();
            foreach (var kv in idx)
            {
                var e = kv.Value;
                bool has = e.ServerLoaded.Contains(deploymentId);
                if (set.Contains(kv.Key)) { if (!has) e.ServerLoaded.Add(deploymentId); }
                else if (has) e.ServerLoaded.Remove(deploymentId);
                e.ServerSeen = now;
            }
            SaveIndex(idx);
        }
    }

    // ── Game-server auth (x-server-api-key) ──────────────────────────────────────────────────
    // Same source order as MothershipServer.LoadServerApiKey: env RIGEL_SERVER_API_KEY, then server_api_key.txt
    // next to the backend, then the legacy baked fallback. Keep the two in sync.
    private static readonly string ServerApiKey = LoadServerApiKey();
    private static string LoadServerApiKey()
    {
        var env = Environment.GetEnvironmentVariable("RIGEL_SERVER_API_KEY");
        if (!string.IsNullOrWhiteSpace(env)) return env.Trim();
        try
        {
            var path = Path.Combine(AppContext.BaseDirectory, "server_api_key.txt");
            if (File.Exists(path))
            {
                var k = File.ReadAllText(path).Trim();
                if (k.Length > 0) return k;
            }
        }
        catch { /* fall through to the legacy literal */ }
        return "7b93a32d659d7bb73eb74c6f3f46a6a1934d47b383295a878f91ea2852d917d2";
    }

    public static bool IsTrustedServer(IHttpRequest req)
    {
        var k = req.GetHeaderValue("x-server-api-key");
        if (string.IsNullOrEmpty(k)) return false;
        return CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(k), Encoding.UTF8.GetBytes(ServerApiKey));
    }
}
