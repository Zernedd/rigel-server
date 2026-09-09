using System.Collections.Concurrent;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using AUnrealFeatures.EOSSDK.Models;

namespace AUnrealFeatures.EOSSDK;

internal static class LobbyWsHelper
{
    // lobbyId → set of connected WebSockets (for broadcasting push events)
    static readonly ConcurrentDictionary<string, ConcurrentBag<WebSocket>> _connections = new();

    public static bool IsLobbyPath(string path) =>
        path.StartsWith("/lobby/v1/", StringComparison.OrdinalIgnoreCase) &&
        path.EndsWith("/lobbies/connect", StringComparison.OrdinalIgnoreCase);

    public static async Task RunAsync(WebSocket ws, string path, string ownerPuid, Action<string> log)
    {
        var buf = new byte[64 * 1024];
        string? joinedLobbyId = null;
        string? joinedPuid    = null;

        try
        {
            while (ws.State == WebSocketState.Open)
            {
                var result = await ws.ReceiveAsync(buf.AsMemory(), CancellationToken.None);
                if (result.MessageType == WebSocketMessageType.Close) break;

                var raw = Encoding.UTF8.GetString(buf, 0, result.Count).Trim();
                if (string.IsNullOrWhiteSpace(raw)) continue;

                log($"LOBBY-WS IN | {raw[..Math.Min(raw.Length, 200)]}");

                string name = "", requestId = "";
                JsonElement payload = default;
                try
                {
                    using var doc = JsonDocument.Parse(raw);
                    var root = doc.RootElement;
                    name      = root.TryGetProperty("name",      out var n) ? n.GetString() ?? "" : "";
                    requestId = root.TryGetProperty("requestId", out var r) ? r.GetString() ?? "" : "";
                    // Clone so payload survives after the JsonDocument is disposed
                    if (root.TryGetProperty("payload", out var rawPayload))
                        payload = rawPayload.Clone();
                }
                catch
                {
                    await SendError(ws, "bad_request", "could not parse json", requestId);
                    continue;
                }

                switch (name.ToLowerInvariant())
                {
                    case "create":
                    {
                        // Extract settings from payload.options
                        var opts = payload.ValueKind == JsonValueKind.Object &&
                                   payload.TryGetProperty("options", out var o) ? o : default;

                        var settings = new EosLobbySettings
                        {
                            MaxPublicPlayers     = opts.ValueKind == JsonValueKind.Object && opts.TryGetProperty("maxPlayers",          out var mp) ? mp.GetInt32() : 10,
                            ShouldAdvertise      = opts.ValueKind == JsonValueKind.Object && opts.TryGetProperty("shouldAdvertise",     out var sa) && sa.GetBoolean(),
                            AllowJoinViaPresence = opts.ValueKind == JsonValueKind.Object && opts.TryGetProperty("allowJoinViaPresence",out var ajvp) && ajvp.GetBoolean(),
                            AllowReadById        = opts.ValueKind == JsonValueKind.Object && opts.TryGetProperty("allowReadById",       out var arbi) && arbi.GetBoolean(),
                            AllowInvites         = opts.ValueKind == JsonValueKind.Object && opts.TryGetProperty("allowInvites",        out var ai) && ai.GetBoolean(),
                        };

                        var lobby = new EosLobbySession
                        {
                            Deployment        = EosGatewayServer.DeploymentId,
                            Id                = Guid.NewGuid().ToString("N"),
                            Bucket            = "",
                            Settings          = settings,
                            TotalPlayers      = 1,
                            OpenPublicPlayers = settings.MaxPublicPlayers - 1,
                            PublicPlayers     = new List<string> { ownerPuid },
                            Started           = false,
                            LastUpdated       = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
                            Attributes        = new Dictionary<string, string?>(),
                            Owner             = ownerPuid,
                            OwnerPlatformId   = 4000,
                            MemberData        = new Dictionary<string, EosLobbyMemberEntry>
                            {
                                [ownerPuid] = new EosLobbyMemberEntry { PlatformId = 4000, Platform = "steam" }
                            }
                        };

                        lock (EosGatewayServer.LobbyLock)
                            EosGatewayServer.Lobbies.Add(lobby);

                        joinedLobbyId = lobby.Id;
                        _connections.GetOrAdd(joinedLobbyId, _ => new ConcurrentBag<WebSocket>()).Add(ws);

                        lobby.Lock = Guid.NewGuid().ToString("N");

                        var publicData  = JsonSerializer.SerializeToElement(lobby);
                        var privateData = new
                        {
                            index             = Random.Shared.Next(),
                            @lock             = lobby.Lock,
                            invites           = Array.Empty<object>(),
                            historicalPlayers = new[] { ownerPuid },
                            pendingDelete     = (object?)null
                        };
                        var memberDataMap = BuildMemberDataMap(lobby);

                        var roomName0 = RtcHelper.RoomName(lobby.Id);
                        await SendJson(ws, new
                        {
                            name    = "lobbyinfo",
                            payload = new
                            {
                                publicData,
                                privateData = (object)privateData,
                                memberData  = new { memberData = memberDataMap },
                                room        = (object?)null, // Pavlov uses P2P voice (no SFU/conference room) — matches real Epic

                                requestId,
                                name = "lobbyinfo",
                            },
                            requestId
                        });
                        log($"LOBBY-WS created | lobbyId={lobby.Id} owner={ownerPuid} rtcRoom={roomName0}");
                        break;
                    }

                    case "join":
                    {
                        var lobbyId = payload.ValueKind == JsonValueKind.Object &&
                                      payload.TryGetProperty("lobbyId", out var li)
                                      ? li.GetString() : null;

                        if (lobbyId == null) { await SendError(ws, "bad_request", "missing lobbyId", requestId); break; }

                        JsonElement publicData  = default;
                        object? memberDataMap  = null;
                        string? roomName1      = null;

                        lock (EosGatewayServer.LobbyLock)
                        {
                            var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                            if (lobby != null)
                            {
                                // Add joining player to shared state
                                if (!lobby.PublicPlayers.Contains(ownerPuid))
                                {
                                    lobby.PublicPlayers.Add(ownerPuid);
                                    lobby.TotalPlayers      = lobby.PublicPlayers.Count;
                                    lobby.OpenPublicPlayers = Math.Max(0, lobby.Settings.MaxPublicPlayers - lobby.TotalPlayers);
                                    lobby.LastUpdated       = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
                                }

                                if (!lobby.MemberData.ContainsKey(ownerPuid))
                                {
                                    var entry = new EosLobbyMemberEntry { PlatformId = 4000, Platform = "other" };
                                    if (EosGatewayServer._userNames.TryGetValue(ownerPuid, out var knownName))
                                        entry.Data["DISPLAYNAME_s"] = (object?)knownName;
                                    lobby.MemberData[ownerPuid] = entry;
                                }

                                // First real player to join a community lobby becomes the owner
                                const string DUMMY_PUID = "0002000000000000000000000000000000";
                                if (lobby.Owner == DUMMY_PUID && ownerPuid != DUMMY_PUID)
                                {
                                    lobby.Owner = ownerPuid;
                                    lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
                                    log($"LOBBY-WS ownership transferred | lobbyId={lobbyId} newOwner={ownerPuid}");
                                }

                                log($"LOBBY-WS join | lobbyId={lobbyId} joiner={ownerPuid} players=[{string.Join(",", lobby.PublicPlayers)}] total={lobby.TotalPlayers}");

                                publicData    = JsonSerializer.SerializeToElement(lobby);
                                memberDataMap = BuildMemberDataMap(lobby);
                                roomName1     = RtcHelper.RoomName(lobby.Id);
                            }
                        }

                        if (memberDataMap == null)
                        {
                            await SendError(ws, "session_not_found", "", requestId, 404);
                            break;
                        }

                        joinedLobbyId = lobbyId;
                        _connections.GetOrAdd(joinedLobbyId, _ => new ConcurrentBag<WebSocket>()).Add(ws);

                        // Send lobbyinfo to the joining client
                        await SendJson(ws, new
                        {
                            name    = "lobbyinfo",
                            payload = new
                            {
                                publicData,
                                privateData = (object?)null,
                                memberData  = new { memberData = memberDataMap },
                                room        = (object)new { roomName = roomName1, clientBaseUrl = RtcHelper.SERVER_URL, token = RtcHelper.MakeToken(roomName1, ownerPuid) },
                                requestId,
                                name = "lobbyinfo",
                            },
                            requestId
                        });

                        // Broadcast memberjoined so the owner (and others) see the new player
                        _ = BroadcastAsync(lobbyId, new
                        {
                            name    = "memberjoined",
                            payload = new { lobbyId, puid = ownerPuid, platformId = 4000, platform = "steam", allowCrossplay = true, name = "memberjoined" }
                        });

                        // If ownership was just transferred, broadcast it
                        {
                            bool isNewOwner;
                            lock (EosGatewayServer.LobbyLock)
                            {
                                var l = EosGatewayServer.Lobbies.FirstOrDefault(x => x.Id == lobbyId);
                                isNewOwner = l?.Owner == ownerPuid && ownerPuid != "0002000000000000000000000000000000";
                            }
                            if (isNewOwner)
                                _ = BroadcastAsync(lobbyId, new
                                {
                                    name    = "lobbyownerchange",
                                    payload = new { lobbyId, newOwner = ownerPuid, name = "lobbyownerchange" }
                                });
                        }

                        log($"LOBBY-WS joined | lobbyId={lobbyId} joiner={ownerPuid} rtcRoom={roomName1}");
                        break;
                    }

                    case "lobbydata":
                    {
                        if (payload.ValueKind != JsonValueKind.Object) break;
                        var lobbyId   = payload.TryGetProperty("lobbyId",   out var li) ? li.GetString() : null;
                        var lobbyData = payload.TryGetProperty("lobbyData", out var ld) ? ld : default;

                        if (lobbyId == null || lobbyData.ValueKind != JsonValueKind.Object)
                        {
                            await SendError(ws, "bad_request", "missing lobbyId or lobbyData", requestId);
                            break;
                        }

                        Dictionary<string, string?>? updated = null;
                        lock (EosGatewayServer.LobbyLock)
                        {
                            var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                            if (lobby != null)
                            {
                                foreach (var prop in lobbyData.EnumerateObject())
                                    lobby.Attributes[prop.Name] = prop.Value.GetString();
                                lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
                                updated = new Dictionary<string, string?>(lobby.Attributes);
                            }
                        }

                        if (updated == null) { await SendError(ws, "session_not_found", "", requestId, 404); break; }

                        await SendSuccess(ws, requestId);

                        // Push lobbydatachange to all connected clients for this lobby
                        _ = BroadcastAsync(lobbyId, new
                        {
                            name    = "lobbydatachange",
                            payload = new { lobbyId, attributes = updated, name = "lobbydatachange" }
                        });
                        log($"LOBBY-WS lobbydata | lobbyId={lobbyId} attrs={updated.Count}");
                        break;
                    }

                    case "memberdata":
                    {
                        if (payload.ValueKind != JsonValueKind.Object) break;
                        var lobbyId  = payload.TryGetProperty("lobbyId", out var li) ? li.GetString() : null;
                        var dataEl   = payload.TryGetProperty("data",    out var de) ? de : default;

                        if (lobbyId == null) { await SendError(ws, "bad_request", "missing lobbyId", requestId); break; }

                        EosLobbyMemberEntry? entry = null;
                        lock (EosGatewayServer.LobbyLock)
                        {
                            var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                            if (lobby != null)
                            {
                                if (!lobby.MemberData.TryGetValue(ownerPuid, out entry))
                                    entry = lobby.MemberData[ownerPuid] = new EosLobbyMemberEntry();

                                if (dataEl.ValueKind == JsonValueKind.Object)
                                    foreach (var prop in dataEl.EnumerateObject())
                                        entry.Data[prop.Name] = prop.Value.ValueKind switch
                                        {
                                            JsonValueKind.True   => (object?)true,
                                            JsonValueKind.False  => false,
                                            JsonValueKind.Number => prop.Value.TryGetInt64(out var i) ? i : prop.Value.GetDouble(),
                                            _                    => prop.Value.GetString()
                                        };
                            }
                        }

                        if (entry == null) { await SendError(ws, "session_not_found", "", requestId, 404); break; }

                        await SendSuccess(ws, requestId);

                        _ = BroadcastAsync(lobbyId, new
                        {
                            name    = "memberdatachange",
                            payload = new
                            {
                                lobbyId,
                                puid       = ownerPuid,
                                memberData = new { data = entry.Data, platformId = entry.PlatformId, platform = entry.Platform },
                                name       = "memberdatachange"
                            }
                        });
                        log($"LOBBY-WS memberdata | lobbyId={lobbyId} puid={ownerPuid}");
                        break;
                    }

                    case "heartbeat":
                    {
                        var lobbyId = payload.ValueKind == JsonValueKind.Object &&
                                      payload.TryGetProperty("lobbyId", out var li) ? li.GetString() : null;
                        if (lobbyId != null)
                        {
                            lock (EosGatewayServer.LobbyLock)
                            {
                                var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                                if (lobby != null) lobby.LastUpdated = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
                            }
                        }
                        await SendSuccess(ws, requestId);
                        break;
                    }

                    case "kick":
                    {
                        if (payload.ValueKind != JsonValueKind.Object) break;
                        var lobbyId    = payload.TryGetProperty("lobbyId", out var li) ? li.GetString() : null;
                        var targetPuid = payload.TryGetProperty("puid",    out var tp) ? tp.GetString() : null;

                        if (lobbyId == null || targetPuid == null) { await SendError(ws, "bad_request", "missing lobbyId or puid", requestId); break; }

                        lock (EosGatewayServer.LobbyLock)
                        {
                            var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                            if (lobby != null && lobby.Owner == ownerPuid)
                            {
                                lobby.PublicPlayers.Remove(targetPuid);
                                lobby.MemberData.Remove(targetPuid);
                                lobby.TotalPlayers      = lobby.PublicPlayers.Count;
                                lobby.OpenPublicPlayers = Math.Max(0, lobby.Settings.MaxPublicPlayers - lobby.TotalPlayers);
                                lobby.LastUpdated       = DateTime.UtcNow.ToString("yyyy-MM-ddTHH:mm:ss.fffZ");
                            }
                        }

                        await SendSuccess(ws, requestId);

                        _ = BroadcastAsync(lobbyId, new
                        {
                            name    = "memberkicked",
                            payload = new { lobbyId, puid = targetPuid, name = "memberkicked" }
                        });

                        log($"LOBBY-WS kick | lobbyId={lobbyId} target={targetPuid} by={ownerPuid}");
                        break;
                    }

                    case "delete":
                    {
                        var lobbyId = payload.ValueKind == JsonValueKind.Object &&
                                      payload.TryGetProperty("lobbyId", out var li)
                                      ? li.GetString() : null;

                        if (lobbyId != null)
                        {
                            lock (EosGatewayServer.LobbyLock)
                            {
                                var lobby = EosGatewayServer.Lobbies.FirstOrDefault(l => l.Id == lobbyId);
                                if (lobby != null) EosGatewayServer.Lobbies.Remove(lobby);
                            }
                            _ = BroadcastAsync(lobbyId, new
                            {
                                name    = "lobbydeleted",
                                payload = new { lobbyId, name = "lobbydeleted" }
                            });
                        }

                        await SendSuccess(ws, requestId);
                        log($"LOBBY-WS delete | lobbyId={lobbyId}");
                        return;
                    }

                    case "leave":
                        await SendSuccess(ws, requestId);
                        return;

                    default:
                        await SendError(ws, "bad_request", $"unknown frameType '{name}", requestId);
                        break;
                }
            }
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            log($"LOBBY-WS error | {ex.Message}");
        }
        finally
        {
            if (joinedLobbyId != null && _connections.TryGetValue(joinedLobbyId, out var bag))
            {
                // ConcurrentBag doesn't support removal; mark by closing — stale entries cleaned on next broadcast
                _ = bag; // connection will be detected as closed on next broadcast attempt
            }
        }
    }

    // ── Broadcast push events to all connected clients for a lobby ────────────

    public static async Task BroadcastAsync(string lobbyId, object frame)
    {
        if (!_connections.TryGetValue(lobbyId, out var bag)) return;
        var bytes = JsonSerializer.SerializeToUtf8Bytes(frame);
        foreach (var ws in bag)
        {
            try
            {
                if (ws.State == WebSocketState.Open)
                    await ws.SendAsync(bytes, WebSocketMessageType.Text, true, CancellationToken.None);
            }
            catch { }
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    static Dictionary<string, object> BuildMemberDataMap(EosLobbySession lobby)
    {
        var map = new Dictionary<string, object>();
        foreach (var puid in lobby.PublicPlayers)
        {
            lobby.MemberData.TryGetValue(puid, out var entry);
            map[puid] = new
            {
                data       = (object)(entry?.Data ?? new Dictionary<string, object?>()),
                platformId = entry?.PlatformId ?? 4000,
                platform   = entry?.Platform   ?? "other",
            };
        }
        return map;
    }

    static Task SendSuccess(WebSocket ws, string requestId) =>
        SendJson(ws, new { name = "success", payload = new { requestId, name = "success" }, requestId });

    static async Task SendError(WebSocket ws, string code, string message, string requestId, int statusCode = 400)
    {
        await SendJson(ws, new
        {
            name    = "error",
            payload = new { statusCode, errorCode = $"errors.com.epicgames.lobbies.{code}", errorMessage = message, requestId, name = "error" },
            requestId
        });
    }

    static async Task SendJson(WebSocket ws, object obj)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(obj);
        if (ws.State == WebSocketState.Open)
            await ws.SendAsync(bytes, WebSocketMessageType.Text, true, CancellationToken.None);
    }
}
