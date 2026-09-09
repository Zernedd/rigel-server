using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Interfaces;
using AUnrealFeatures.Hosting.Http.Preprocessors;
using System;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;

namespace AUnrealFeatures.Ares.Servers
{
    // Gates the DASHBOARD API on :8080 ONLY (/api/*). Nothing else is touched — A2StationDb (:78) and
    // Mothership are out of scope (per the deployment, :78 is never publicly exposed). A request under
    // /api/* must carry EITHER a valid dashboard session cookie (minted by the Meta SSO callback) OR the
    // dashboard admin key header (for non-browser callers). Everything else — "/", "/auth/*", the HTML,
    // static assets — passes untouched, so the login page and SSO round-trip stay reachable.
    //
    // CAPTURE-FIRST: while Enforce=false it only LOGS the would-block decisions ([DASH-AUTH]) so we can
    // watch which /api/* calls the real dashboard makes and confirm they carry the session BEFORE we
    // start returning 401. Flip Enforce=true once SSO sessions are minted and the log shows them landing.
    public sealed class DashboardAuthPreprocessor : IHttpRequestPreprocessor
    {
        // [2026-09-09] ENFORCED. Was capture-first (log-only) with placeholder secrets, which meant
        // every /api/* call on :8080 was allowed -- anyone who could reach the port could delete
        // stations, ban users, grant roles and tear down servers.
        //
        // Enable/disable with DASHBOARD_AUTH_ENFORCE=0 only for local debugging; it defaults to ON.
        public static bool Enforce =
            (Environment.GetEnvironmentVariable("DASHBOARD_AUTH_ENFORCE") ?? "1") != "0";

        // These must match what the /auth/meta callback uses to MINT the session cookie.
        public const string SessionCookieName = "a2dash_session";

        // Secrets come from the ENVIRONMENT, never from source -- a key committed to the repo is not
        // a key. If either is unset we generate a strong random one AT STARTUP and print it once, so
        // the service can never silently fall back to a guessable placeholder. A generated key only
        // lives for that process lifetime, so set DASHBOARD_ADMIN_KEY in .env for a stable one.
        static readonly string SessionSecret     = ResolveSecret("DASHBOARD_SESSION_SECRET", "session secret");
        static readonly string DashboardAdminKey = ResolveSecret("DASHBOARD_ADMIN_KEY",     "admin key");

        static string ResolveSecret(string envVar, string label)
        {
            var v = Environment.GetEnvironmentVariable(envVar);
            if (!string.IsNullOrWhiteSpace(v) && !v.StartsWith("CHANGE-ME", StringComparison.OrdinalIgnoreCase))
                return v;

            var bytes = RandomNumberGenerator.GetBytes(32);
            var gen = Convert.ToHexString(bytes).ToLowerInvariant();
            Console.WriteLine($"[DASH-AUTH] {envVar} not set -- GENERATED a random {label} for this run:");
            Console.WriteLine($"[DASH-AUTH]     {gen}");
            Console.WriteLine($"[DASH-AUTH] Set {envVar} in .env to keep it stable across restarts.");
            return gen;
        }

        // The key the dashboard page itself needs in order to call /api/*. Exposed so the server can
        // print it on boot; never returned over HTTP.
        public static string AdminKeyForDisplay => DashboardAdminKey;

        public Task<HttpPreprocessorContainer> TryPreprocessRequest(IHttpRequest request, IHttpResponse response)
        {
            var path = request.Uri ?? "";
            bool guarded = path.StartsWith("/api/", StringComparison.OrdinalIgnoreCase);
            if (!guarded || request.Method.Method == "OPTIONS")
                return Ok();   // login page, SSO callback, HTML, CORS preflight -> always allow

            if (HasValidAdminKey(request) || HasValidSession(request))
                return Ok();

            if (!Enforce)
            {
                Console.WriteLine($"[DASH-AUTH] would-401 (log-only) {request.Method.Method} {path} from {request.Remote} — no session/key");
                return Ok();
            }
            Console.WriteLine($"[DASH-AUTH] 401 {request.Method.Method} {path} from {request.Remote} — no session/key");
            return Task.FromResult(new HttpPreprocessorContainer
            {
                result = HttpPreprocessorResult.FAIL | HttpPreprocessorResult.STOP_AFTER,
                actionResult = Results.Configurable(HttpStatusCode.Unauthorized, "application/json", "{\"error\":\"unauthorized\"}")
            });
        }

        static Task<HttpPreprocessorContainer> Ok()
            => Task.FromResult(new HttpPreprocessorContainer { result = HttpPreprocessorResult.OK });

        static bool HasValidAdminKey(IHttpRequest req)
        {
            var k = req.GetHeaderValue("x-dashboard-key") ?? req.GetHeaderValue("x-api-key") ?? "";
            return !string.IsNullOrEmpty(k) && CtEquals(k, DashboardAdminKey);
        }

        static bool HasValidSession(IHttpRequest req)
        {
            var cookieHdr = req.GetHeaderValue("Cookie") ?? "";
            foreach (var part in cookieHdr.Split(';'))
            {
                var p = part.Trim();
                if (p.StartsWith(SessionCookieName + "=", StringComparison.Ordinal))
                    return ValidateJwt(p.Substring(SessionCookieName.Length + 1));
            }
            return false;
        }

        // session cookie = base64url(header).base64url(payload{sub,exp,...}).base64url(HMACSHA256(header.payload))
        static bool ValidateJwt(string token)
        {
            try
            {
                var parts = token.Split('.');
                if (parts.Length != 3) return false;
                var signing = parts[0] + "." + parts[1];
                using var hmac = new HMACSHA256(Encoding.UTF8.GetBytes(SessionSecret));
                var sig = B64Url(hmac.ComputeHash(Encoding.UTF8.GetBytes(signing)));
                if (!CtEquals(sig, parts[2])) return false;   // signature must verify
                var payloadJson = Encoding.UTF8.GetString(B64UrlDecode(parts[1]));
                using var doc = System.Text.Json.JsonDocument.Parse(payloadJson);
                if (doc.RootElement.TryGetProperty("exp", out var e) && e.TryGetInt64(out var exp))
                    return exp > DateTimeOffset.UtcNow.ToUnixTimeSeconds();   // not expired
                return true;
            }
            catch { return false; }
        }

        static string B64Url(byte[] b) => Convert.ToBase64String(b).Replace("+", "-").Replace("/", "_").TrimEnd('=');
        static byte[] B64UrlDecode(string s)
        {
            s = s.Replace("-", "+").Replace("_", "/");
            switch (s.Length % 4) { case 2: s += "=="; break; case 3: s += "="; break; }
            return Convert.FromBase64String(s);
        }
        static bool CtEquals(string a, string b)
            => CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(a), Encoding.UTF8.GetBytes(b));
    }
}
