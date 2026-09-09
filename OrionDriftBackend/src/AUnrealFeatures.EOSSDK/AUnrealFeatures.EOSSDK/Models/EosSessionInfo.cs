using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosSessionInfo
{
    [JsonPropertyName("id")]            public string  Id           { get; set; } = Guid.NewGuid().ToString("N");
    [JsonPropertyName("server_name")]   public string  ServerName   { get; set; } = "";
    [JsonPropertyName("ip_address")]    public string  IpAddress    { get; set; } = "";
    [JsonPropertyName("port")]          public string  Port         { get; set; } = "";
    [JsonPropertyName("max_players")]   public int     MaxPlayers   { get; set; } = 10;
    [JsonPropertyName("bucket")]        public string  Bucket       { get; set; } = "A2_4729";
    [JsonPropertyName("build_id")]      public int     BuildId      { get; set; } = 4729;
    [JsonPropertyName("imgui_port")]    public string  ImguiPort    { get; set; } = "";
    [JsonPropertyName("station_id")]    public string? StationId    { get; set; }
    [JsonPropertyName("deployment_id")] public string? DeploymentId { get; set; }
    [JsonPropertyName("public_players")] public List<string> PublicPlayers { get; set; } = new();
}
