using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.EOSSDK;

namespace AUnrealFeatures.Ares.Servers;

// Enforces station bans in the EOS station browser (2026-09-27: bans were saved but nothing read them).
//
// A ban lives on the user record (UserDataResponse.Bans: station id, expiry, revoked), written by the dashboard
// (/api/users/{id}/ban) and the station API (v1/stations/{id}/users/{id}/ban). The EOS gateway can't read this
// project's database (Ares -> EOSSDK, never back), so the active bans are pushed across as station id -> banned
// usernames. The gateway then leaves the banned station's servers out of that player's session list and refuses
// a join by session id.
//
// Which station a ban covers:
//   - "*" / "all" / "global" / empty: every station.
//   - a station that no longer exists: every station. Every dashboard Spin Up mints a NEW station id, so a ban
//     placed on last week's station would otherwise stop applying the moment the server is respun -- that is
//     how the live bans went dead. The ban meant "keep them off my server", so it follows to every station.
//   - otherwise just that station.
// The dashboard's station Bans tab and unban use the same rule (AppliesTo), so what is listed there is exactly
// what is enforced and can be lifted there.
//
// Pushed at startup, after every ban / unban, and every 30 s (expiries lapse and stations come and go on their
// own). The effective list is written to bans_active.txt beside the backend on every change.
public static class StationBans
{
    public const string All = "*";
    static Timer? _timer;
    static string _lastSummary = "";

    public static void Start()
    {
        PushAll();
        _timer ??= new Timer(_ => PushAll(), null, TimeSpan.FromSeconds(30), TimeSpan.FromSeconds(30));
    }

    public static bool IsGlobal(string? stationId)
        => string.IsNullOrWhiteSpace(stationId) || stationId == All
           || stationId.Equals("all", StringComparison.OrdinalIgnoreCase)
           || stationId.Equals("global", StringComparison.OrdinalIgnoreCase);

    // LiteDB hands dates back as local time, the dashboard writes UTC and the station API local: compare like with like
    public static bool IsActive(BanRequest? b)
    {
        if (b == null || b.Revoked) return false;
        var exp = b.Expiration;
        if (exp == default || exp.Year >= 9999) return true;   // permanent (MaxValue loses its ticks in LiteDB)
        return exp.Kind == DateTimeKind.Utc ? exp > DateTime.UtcNow : exp > DateTime.Now;
    }

    public static HashSet<string> LiveStationIds()
        => new(Program.Database.GetCollection<StationDbObject>(true)?.FindAll()
                   .Select(s => s.StationId).Where(id => !string.IsNullOrWhiteSpace(id)) ?? Enumerable.Empty<string>(),
               StringComparer.OrdinalIgnoreCase);

    // The station a ban is enforced on: its own, or "*" when it is global or its station is gone.
    public static string EffectiveStation(BanRequest b, HashSet<string> live)
        => IsGlobal(b.StationId) || !live.Contains(b.StationId) ? All : b.StationId;

    public static bool AppliesTo(BanRequest b, string stationId, HashSet<string> live)
    {
        var eff = EffectiveStation(b, live);
        return eff == All || IsGlobal(stationId) || string.Equals(eff, stationId, StringComparison.OrdinalIgnoreCase);
    }

    public static void PushAll()
    {
        try
        {
            var live = LiveStationIds();
            var byStation = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
            var lines = new List<string>();
            var users = Program.Database.GetCollection<UserDataResponse>(true)?.FindAll().ToList() ?? new List<UserDataResponse>();
            foreach (var u in users)
            {
                if (u.Bans == null || u.Bans.Count == 0) continue;
                foreach (var b in u.Bans.Where(IsActive))
                {
                    if (string.IsNullOrWhiteSpace(u.Username))
                    {
                        lines.Add($"UNENFORCEABLE (no username on record) user={u.UserId} station={b.StationId}");
                        continue;
                    }
                    var station = EffectiveStation(b, live);
                    if (!byStation.TryGetValue(station, out var set))
                        byStation[station] = set = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                    set.Add(u.Username.Trim());
                    var why = station == All && !IsGlobal(b.StationId) ? $" (station {b.StationId} no longer exists)" : "";
                    lines.Add($"{u.Username.Trim(),-24} user={u.UserId} station={station}{why} " +
                              $"until={(b.Expiration.Year >= 9999 ? "permanent" : b.Expiration.ToString("u"))} reason={b.Reason}");
                }
            }
            EosGatewayServer.SetBans(byStation);

            var summary = string.Join("\n", lines.OrderBy(l => l, StringComparer.OrdinalIgnoreCase));
            if (summary != _lastSummary)
            {
                _lastSummary = summary;
                File.WriteAllText(Path.Combine(Environment.CurrentDirectory, "bans_active.txt"),
                    $"# {DateTime.UtcNow:u}  {EosGatewayServer.BannedAccountCount} enforced ban(s); live stations: {string.Join(", ", live)}\n{summary}\n");
                Console.WriteLine($"[BANS] {EosGatewayServer.BannedAccountCount} ban(s) pushed to the EOS gateway");
            }
        }
        catch (Exception ex) { Console.WriteLine($"[BANS] could not push bans: {ex.Message}"); }
    }
}
