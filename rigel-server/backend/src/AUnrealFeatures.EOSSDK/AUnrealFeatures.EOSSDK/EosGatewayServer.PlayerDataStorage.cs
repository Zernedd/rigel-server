using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using System.Net;
using System.Security.Cryptography;
using System.Text.Json;

namespace AUnrealFeatures.EOSSDK;

/* ─── EOS Player Data Storage ─────────────────────────────────────────────
 * Real traffic observed:
 *   GET /datastorage/v2/access/deployment/{deploymentId}/private/{productUserId}/{filename}
 *       ?getDuration=300&putDuration=300&full=true
 * which returns short-lived "presigned" read/write URLs the SDK then hits
 * directly to actually transfer file bytes. Since this is all local, the
 * "presigned" URLs just point back at this same gateway's /data/ endpoints
 * with no real signing — good enough for a private server.
 *
 * Files are stored flat on disk under playerdata/{deploymentId}/{productUserId}/{filename}. */
public sealed partial class EosGatewayServer
{
    static string PlayerDataRoot => Path.Combine(AppContext.BaseDirectory, "playerdata");

    static string PlayerDataFilePath(string deploymentId, string productUserId, string filename)
    {
        var dir = Path.Combine(PlayerDataRoot, deploymentId, productUserId);
        Directory.CreateDirectory(dir);
        // filenames are opaque EOS-assigned strings, but guard against path traversal regardless.
        var safeName = Path.GetFileName(filename);
        return Path.Combine(dir, safeName);
    }

    [HttpGet("/datastorage/v2/access/deployment/{deployment_id}/private/{product_user_id}/{filename}")]
    public async Task<IHttpActionResult> PlayerDataStorageAccess(IHttpRequest request, IHttpResponse response, string deployment_id, string product_user_id, string filename)
    {
        var path = PlayerDataFilePath(deployment_id, product_user_id, filename);
        var exists = File.Exists(path);

        var baseUrl = $"http://{Hostname}:{Port}/datastorage/v2/data/deployment/{deployment_id}/private/{product_user_id}/{filename}";

        object? fileMeta = null;
        if (exists)
        {
            var info = new FileInfo(path);
            var md5 = Convert.ToHexString(MD5.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
            fileMeta = new
            {
                filename,
                md5Hash = md5,
                lastModified = info.LastWriteTimeUtc.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
            };
        }

        var result = new Dictionary<string, object?>
        {
            ["accountId"] = product_user_id,
            ["deploymentId"] = deployment_id,
            ["writeRequests"] = new[] { new { id = Guid.NewGuid().ToString("N"), uri = baseUrl } },
        };
        if (exists)
        {
            result["readRequests"] = new[] { new { id = Guid.NewGuid().ToString("N"), deviceId = (string?)null, uri = baseUrl } };
            result["file"] = fileMeta;
        }
        else
        {
            result["readRequests"] = Array.Empty<object>();
        }

        return Results.Configurable(HttpStatusCode.OK, "application/json", JsonSerializer.SerializeToUtf8Bytes(result));
    }

    [HttpGet("/datastorage/v2/data/deployment/{deployment_id}/private/{product_user_id}/{filename}")]
    public async Task<IHttpActionResult> PlayerDataStorageGet(IHttpRequest request, IHttpResponse response, string deployment_id, string product_user_id, string filename)
    {
        var path = PlayerDataFilePath(deployment_id, product_user_id, filename);
        if (!File.Exists(path)) return Results.NotFound();
        return Results.Configurable(HttpStatusCode.OK, "application/octet-stream", await File.ReadAllBytesAsync(path));
    }

    [HttpPut("/datastorage/v2/data/deployment/{deployment_id}/private/{product_user_id}/{filename}")]
    public async Task<IHttpActionResult> PlayerDataStoragePut(IHttpRequest request, IHttpResponse response, string deployment_id, string product_user_id, string filename)
    {
        var path = PlayerDataFilePath(deployment_id, product_user_id, filename);
        await File.WriteAllBytesAsync(path, request.Body ?? Array.Empty<byte>());
        return Results.Ok();
    }
}
