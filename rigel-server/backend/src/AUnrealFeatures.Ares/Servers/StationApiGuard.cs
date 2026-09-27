using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Hosting.Http.Preprocessors;
using System;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Servers
{
    // [2026-09-27] Gates the station API on :78. It had NO authentication on any route, and :78 is reachable from the
    // internet both directly (firewall rule "Rigel Backend", 13.140.41.197:78) and through Cloudflare
    // (rigel.wwiggles.org) -- so anyone could delete stations and deployments (which now also kills their servers),
    // grant themselves roles, ban users and rewrite station config. Someone was doing exactly that.
    //
    // The rule: a request from OUTSIDE -- not from this machine, or carried by Cloudflare (a CF-Connecting-IP / CF-Ray
    // header) -- may read (GET), may log in, and may make the game's own reporting calls; every other write needs the
    // dashboard admin key (x-dashboard-key). Requests from this machine with no Cloudflare headers are the game servers
    // (DashboardApiUrl=http://127.0.0.1:78) and pass untouched, so nothing a running server does changes. The
    // dashboard's /game proxy verifies its session cookie and attaches the admin key (dashboard/app/game).
    // STATION_GUARD_ENFORCE=0 turns it into log-only.
    public sealed class StationApiGuard : IHttpRequestPreprocessor
    {
        public static bool Enforce = (Environment.GetEnvironmentVariable("STATION_GUARD_ENFORCE") ?? "1") != "0";

        // writes anyone may make: players logging in, servers reporting, event sign-ups (and routes with their own key)
        static readonly Regex OpenWrites = new(
            @"^/(users/log_in|users/log_in_with_key|users/log_in_server|v1/users/log_in_with_key|v2/users/log_in|" +
            @"server_heartbeat|update_player_count|register_server|stations/create|v1/board/upload|" +
            @"deployments/[^/]+/server_events|v1/spec/.*|" +
            @"v1/stations/[^/]+/event/[^/]+/users/[^/]+/signup|v1/stations/[^/]+/event/[^/]+/signup)/?$",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

        public Task<HttpPreprocessorContainer> TryPreprocessRequest(IHttpRequest request, IHttpResponse response)
        {
            var method = request.Method.Method.ToUpperInvariant();
            if (method == "GET" || method == "HEAD" || method == "OPTIONS") return Ok();
            var path = (request.Uri ?? "").Split('?')[0];
            if (!path.StartsWith("/")) path = "/" + path;
            if (OpenWrites.IsMatch(path)) return Ok();
            if (!IsExternal(request)) return Ok();
            if (HasAdminKey(request)) return Ok();

            var who = request.GetHeaderValue("CF-Connecting-IP") ?? request.Remote?.ToString() ?? "?";
            Console.WriteLine($"[STATION-GUARD] {(Enforce ? "401" : "would-401")} {method} {path} from {who}");
            if (!Enforce) return Ok();
            return Task.FromResult(new HttpPreprocessorContainer
            {
                result = HttpPreprocessorResult.FAIL | HttpPreprocessorResult.STOP_AFTER,
                actionResult = Results.Configurable(HttpStatusCode.Unauthorized, "application/json", "{\"error\":\"unauthorized\"}")
            });
        }

        static bool IsExternal(IHttpRequest req)
        {
            if (!string.IsNullOrEmpty(req.GetHeaderValue("CF-Connecting-IP")) || !string.IsNullOrEmpty(req.GetHeaderValue("CF-Ray")))
                return true;                                   // came in through the Cloudflare tunnel
            var ip = req.Remote;
            return ip == null || !IPAddress.IsLoopback(ip.IsIPv4MappedToIPv6 ? ip.MapToIPv4() : ip);
        }

        static bool HasAdminKey(IHttpRequest req)
        {
            var k = req.GetHeaderValue("x-dashboard-key") ?? "";
            var want = DashboardAuthPreprocessor.AdminKeyForDisplay;
            return !string.IsNullOrEmpty(k) && !string.IsNullOrEmpty(want) &&
                   CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(k), Encoding.UTF8.GetBytes(want));
        }

        static Task<HttpPreprocessorContainer> Ok()
            => Task.FromResult(new HttpPreprocessorContainer { result = HttpPreprocessorResult.OK });
    }
}
