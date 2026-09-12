using AUnrealFeatures.Ares.Models;
using AUnrealFeatures.EOSSDK;

namespace AUnrealFeatures.Ares.Servers;

// Station access control: which accounts may SEE a station in the EOS station browser.
//
// The list lives in the station's Config under "acl.whitelist" as a comma-separated list of usernames
// (deliberately NOT under the reserved "nv." prefix, so it never leaks into the netvar override file):
//
//   acl.whitelist = gooeyzern, ReZ-VR
//
// An empty/absent value means "visible to everyone" -- that is the default, so adding this feature
// changes nothing for a station that does not opt in.
//
// EosGatewayServer lives in a project that cannot reference this one (Ares -> EOSSDK, never the reverse),
// so the values are pushed across the boundary instead of read back. Push on every config change, and
// once for every station at startup -- otherwise a backend restart would leave the gateway with no
// whitelists at all, which for an allowlist fails OPEN and silently unhides a private station.
public static class StationAcl
{
    public const string WhitelistKey = "acl.whitelist";

    public static IEnumerable<string> Parse(string? value)
        => (value ?? "")
            .Split(new[] { ',', ';', '\n' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(s => s.Trim())
            .Where(s => s.Length > 0);

    public static void Push(string stationId)
    {
        try
        {
            var station = Program.Database.GetCollection<StationDbObject>(true)
                ?.FindAll().FirstOrDefault(s => s.StationId == stationId);
            string? raw = null;
            station?.Config?.TryGetValue(WhitelistKey, out raw);
            EosGatewayServer.SetStationWhitelist(stationId, Parse(raw));
        }
        catch (Exception ex) { Console.WriteLine($"[ACL] could not push whitelist for {stationId}: {ex.Message}"); }
    }

    public static void PushAll()
    {
        try
        {
            var stations = Program.Database.GetCollection<StationDbObject>(true)?.FindAll().ToList()
                           ?? new List<StationDbObject>();
            foreach (var s in stations)
            {
                string? raw = null;
                s.Config?.TryGetValue(WhitelistKey, out raw);
                EosGatewayServer.SetStationWhitelist(s.StationId, Parse(raw));
            }
        }
        catch (Exception ex) { Console.WriteLine($"[ACL] could not push whitelists: {ex.Message}"); }
    }
}
