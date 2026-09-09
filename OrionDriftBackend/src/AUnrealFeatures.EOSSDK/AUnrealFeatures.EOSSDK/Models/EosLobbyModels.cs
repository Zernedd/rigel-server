using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosLobbyFilterRequest
{
    [JsonPropertyName("criteria")]          public List<EosFilterCriterion>? Criteria         { get; set; }
    [JsonPropertyName("maxResults")]        public int                        MaxResults        { get; set; } = 100;
    [JsonPropertyName("minCurrentPlayers")] public int                        MinCurrentPlayers { get; set; } = 0;
}

public sealed class EosLobbyResponse
{
    [JsonPropertyName("count")]    public int Count { get; set; }
    [JsonPropertyName("sessions")] public List<EosLobbySession> Sessions { get; set; } = new();
}

public sealed class EosLobbySession
{
    [JsonPropertyName("deployment")]      public string                   Deployment      { get; set; } = "";
    [JsonPropertyName("id")]              public string                   Id              { get; set; } = "";
    [JsonPropertyName("bucket")]          public string                   Bucket          { get; set; } = "main";
    [JsonPropertyName("settings")]        public EosLobbySettings         Settings        { get; set; } = new();
    [JsonPropertyName("totalPlayers")]    public int                      TotalPlayers    { get; set; }
    [JsonPropertyName("openPublicPlayers")] public int                    OpenPublicPlayers { get; set; }
    [JsonPropertyName("publicPlayers")]   public List<string>             PublicPlayers   { get; set; } = new();
    [JsonPropertyName("started")]         public bool                     Started         { get; set; }
    [JsonPropertyName("lastUpdated")]     public string                   LastUpdated     { get; set; } = "";
    [JsonPropertyName("attributes")]      public Dictionary<string, string?> Attributes  { get; set; } = new();
    [JsonPropertyName("owner")]           public string                   Owner           { get; set; } = "";
    [JsonPropertyName("ownerPlatformId")] public int                      OwnerPlatformId { get; set; } = 4000;

    // Per-member data — tracked in-memory, included in WS lobbyinfo response
    [JsonIgnore]
    public Dictionary<string, EosLobbyMemberEntry> MemberData { get; set; } = new();

    // Lock token issued at create time, echoed back by client on every mutating frame
    [JsonIgnore]
    public string Lock { get; set; } = "";
}

public sealed class EosLobbySettings
{
    [JsonPropertyName("maxPublicPlayers")]          public int              MaxPublicPlayers     { get; set; } = 10;
    [JsonPropertyName("allowInvites")]              public bool             AllowInvites         { get; set; }
    [JsonPropertyName("shouldAdvertise")]           public bool             ShouldAdvertise      { get; set; } = true;
    [JsonPropertyName("allowReadById")]             public bool             AllowReadById        { get; set; } = true;
    [JsonPropertyName("allowJoinViaPresence")]      public bool             AllowJoinViaPresence { get; set; } = true;
    [JsonPropertyName("allowJoinInProgress")]       public bool             AllowJoinInProgress  { get; set; }
    [JsonPropertyName("allowConferenceRoom")]       public bool             AllowConferenceRoom  { get; set; }
    [JsonPropertyName("checkSanctions")]            public bool             CheckSanctions       { get; set; }
    [JsonPropertyName("allowMigration")]            public bool             AllowMigration       { get; set; } = true;
    [JsonPropertyName("rejoinAfterKick")]           public string           RejoinAfterKick      { get; set; } = "open";
    [JsonPropertyName("platforms")]                 public EosLobbyPlatforms? Platforms          { get; set; }
}

public sealed class EosLobbyPlatforms
{
    [JsonPropertyName("allowType")]        public string AllowType        { get; set; } = "any";
    [JsonPropertyName("allowedPlatformIds")] public int[]? AllowedPlatformIds { get; set; }
}

public sealed class EosLobbyMemberEntry
{
    [JsonPropertyName("data")]       public Dictionary<string, object?> Data       { get; set; } = new();
    [JsonPropertyName("platformId")] public int                          PlatformId { get; set; } = 4000;
    [JsonPropertyName("platform")]   public string                       Platform   { get; set; } = "other";
}

public sealed class EosLobbyCreateRequest
{
    [JsonPropertyName("bucket")]     public string?                   Bucket     { get; set; }
    [JsonPropertyName("settings")]   public EosLobbySettings?         Settings   { get; set; }
    [JsonPropertyName("attributes")] public Dictionary<string, string?>? Attributes { get; set; }
}

public sealed class EosLobbyUpdateRequest
{
    [JsonPropertyName("settings")]   public EosLobbySettings?         Settings   { get; set; }
    [JsonPropertyName("attributes")] public Dictionary<string, string?>? Attributes { get; set; }
    [JsonPropertyName("started")]    public bool?                     Started    { get; set; }
}
