using System.Text.Json.Serialization;

namespace AUnrealFeatures.Ares.Models;

// Sent by a dedicated server (the injected DLL) when it spins up.
public sealed class RegisterServerRequest
{
    [JsonPropertyName("ip")]            public string? Ip           { get; set; }
    [JsonPropertyName("port")]          public string? Port         { get; set; }
    // Optional: id decided by the allocator/socket BEFORE launch and passed to the game as
    // -DashboardDeploymentId. When present, register under THIS id (upsert) so the game's config
    // fetch and the DB row match. Absent (manual launch) -> a fresh id is generated as before.
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
    [JsonPropertyName("server_name")] public string? ServerName { get; set; }
    [JsonPropertyName("region")]      public string? Region     { get; set; }
    [JsonPropertyName("imgui_port")]  public string? ImguiPort  { get; set; }
    [JsonPropertyName("max_players")] public int     MaxPlayers { get; set; }
}

public sealed class RegisterServerResponse
{
    [JsonPropertyName("success")]       public bool    Success      { get; set; }
    [JsonPropertyName("station_id")]    public string? StationId    { get; set; }
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
}

// Periodic heartbeat from a dedicated server (the injected DLL) reporting its REAL connected-player
// count (netdriver ClientConnections). The backend reconciles the EOS session's count to this so
// client-missed leaves (hard disconnects) don't leave the count stuck high.
public sealed class UpdatePlayerCountRequest
{
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
    [JsonPropertyName("player_count")]  public int     PlayerCount  { get; set; }
}
