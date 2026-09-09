using AUnrealFeatures.Hosting.Http;
using AUnrealFeatures.Hosting.Http.Actions;
using AUnrealFeatures.Hosting.Http.Attributes;
using AUnrealFeatures.Hosting.Http.Interfaces;
using System.Net;

namespace AUnrealFeatures.EOSSDK;

public interface IEosWsServer { }

/// <summary>
/// Listens on port 80 and accepts the EOS STOMP WebSocket connections
/// (notifications + lobby) that the EOS SDK sends to the bare hostname
/// before Fiddler/proxy redirection is in place for WS traffic.
/// </summary>
public sealed class EosWsServer : AstraHttpServer, IEosWsServer
{
    const string HOSTNAME = "localhost";
    const ushort PORT     = 80;

    public EosWsServer() : base(HOSTNAME, PORT) { }

    // Return 404 for any plain HTTP that lands here
    [HttpGet("/")]
    public Task<IHttpActionResult> Root(IHttpRequest req, IHttpResponse res)
        => Task.FromResult<IHttpActionResult>(Results.NotFound());

    // ── WebSocket upgrade handler ─────────────────────────────────────────────

    protected override async Task<bool> HandleWebSocketAsync(HttpListenerContext ctx)
    {
        var path = ctx.Request.Url?.AbsolutePath ?? "";

        if (LobbyWsHelper.IsLobbyPath(path))
        {
            Logger.Information($"WS[80] lobby | {path}");
            // Epic sends Sec-WebSocket-Protocol: ws but expects no subprotocol echoed back
            var ownerPuid = EosGatewayServer.ExtractPuidFromBearer(ctx.Request.Headers["Authorization"]);
            var wsCtx = await ctx.AcceptWebSocketAsync(null);
            var ws    = wsCtx.WebSocket;
            try   { await LobbyWsHelper.RunAsync(ws, path, ownerPuid, msg => Logger.Information(msg)); }
            catch (Exception ex) { Logger.Error($"LobbyWS error | {ex.Message}"); }
            finally
            {
                if (ws.State == System.Net.WebSockets.WebSocketState.Open)
                    await ws.CloseAsync(System.Net.WebSockets.WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
                ws.Dispose();
            }
            return true;
        }

        if (!StompHelper.IsStompPath(path)) return false;

        Logger.Information($"WS[80] stomp | {path}");
        var offered     = ctx.Request.Headers["Sec-WebSocket-Protocol"];
        var subprotocol = offered?.Split(',').Select(p => p.Trim()).FirstOrDefault() ?? "v12.stomp";
        var wsCtx2 = await ctx.AcceptWebSocketAsync(subprotocol);
        var ws2    = wsCtx2.WebSocket;
        try
        {
            await StompHelper.RunAsync(ws2, path, msg => Logger.Information(msg));
        }
        catch (Exception ex) { Logger.Error($"STOMP error | {ex.Message}"); }
        finally
        {
            if (ws2.State == System.Net.WebSockets.WebSocketState.Open)
                await ws2.CloseAsync(System.Net.WebSockets.WebSocketCloseStatus.NormalClosure, "bye", CancellationToken.None);
            ws2.Dispose();
        }
        return true;
    }
}
