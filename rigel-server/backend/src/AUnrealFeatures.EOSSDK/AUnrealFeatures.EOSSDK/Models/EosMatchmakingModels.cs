using System.Text.Json;
using System.Text.Json.Serialization;

namespace AUnrealFeatures.EOSSDK.Models;

public sealed class EosFilterCriterion
{
    [JsonPropertyName("key")]   public string      Key   { get; set; } = "";
    [JsonPropertyName("op")]    public string      Op    { get; set; } = "";
    [JsonPropertyName("value")] public JsonElement Value { get; set; }
}

public sealed class EosFilterRequest
{
    [JsonPropertyName("criteria")]   public List<EosFilterCriterion>? Criteria   { get; set; }
    [JsonPropertyName("maxResults")] public int                        MaxResults { get; set; } = 100;
}

public sealed class EosMatchmakingResponse
{
    [JsonPropertyName("count")]
    public int Count { get; set; }

    [JsonPropertyName("sessions")]
    public List<EosMatchmakingSession> Sessions { get; set; } = new();
}

public sealed class EosMatchmakingSession
{
    [JsonPropertyName("deployment")]
    public string Deployment { get; set; } = string.Empty;

    [JsonPropertyName("id")]
    public string Id { get; set; } = string.Empty;

    [JsonPropertyName("bucket")]
    public string Bucket { get; set; } = string.Empty;

    [JsonPropertyName("settings")]
    public EosSessionSettings Settings { get; set; } = new();

    [JsonPropertyName("maxPublicPlayers")]
    public int MaxPublicPlayers { get; set; }

    [JsonPropertyName("openPublicPlayers")]
    public int OpenPublicPlayers { get; set; }

    [JsonPropertyName("maxPrivatePlayers")]
    public int MaxPrivatePlayers { get; set; }

    [JsonPropertyName("openPrivatePlayers")]
    public int OpenPrivatePlayers { get; set; }

    [JsonPropertyName("publicPlayers")]
    public List<string> PublicPlayers { get; set; } = new();

    [JsonPropertyName("privatePlayers")]
    public List<string> PrivatePlayers { get; set; } = new();

    [JsonPropertyName("totalPlayers")]
    public int TotalPlayers { get; set; }

    [JsonPropertyName("allowJoinInProgress")]
    public bool AllowJoinInProgress { get; set; }

    [JsonPropertyName("shouldAdvertise")]
    public bool ShouldAdvertise { get; set; }

    [JsonPropertyName("isDedicated")]
    public bool IsDedicated { get; set; }

    [JsonPropertyName("usesStats")]
    public bool UsesStats { get; set; }

    [JsonPropertyName("allowInvites")]
    public bool AllowInvites { get; set; }

    [JsonPropertyName("usesPresence")]
    public bool UsesPresence { get; set; }

    [JsonPropertyName("allowJoinViaPresence")]
    public bool AllowJoinViaPresence { get; set; }

    [JsonPropertyName("allowJoinViaPresenceFriendsOnly")]
    public bool AllowJoinViaPresenceFriendsOnly { get; set; }

    [JsonPropertyName("buildUniqueId")]
    public int BuildUniqueId { get; set; }

    [JsonPropertyName("started")]
    public bool Started { get; set; }

    [JsonPropertyName("lastUpdated")]
    public string LastUpdated { get; set; } = string.Empty;

    [JsonPropertyName("attributes")]
    public EosSessionAttributes Attributes { get; set; } = new();

    [JsonPropertyName("owner")]
    public string Owner { get; set; } = string.Empty;

    [JsonPropertyName("ownerPlatformId")]
    public string? OwnerPlatformId { get; set; } = null;
}

public sealed class EosSessionAttributes
{
    [JsonPropertyName("SERVERNAME_s")]            public string ServerName          { get; set; } = "";
    [JsonPropertyName("BUSESSTATS_b")]            public bool   BUsesStats          { get; set; } = false;
    [JsonPropertyName("NUMPRIVATECONNECTIONS_l")] public int    NumPrivateConns     { get; set; } = 0;
    [JsonPropertyName("USESPRESENCE_b")]          public bool   UsesPresence        { get; set; } = false;
    [JsonPropertyName("ADDRESS_s")]               public string Address             { get; set; } = "";
    [JsonPropertyName("PRESENCESEARCH_b")]        public bool   PresenceSearch      { get; set; } = true;
    [JsonPropertyName("NUMPUBLICCONNECTIONS_l")]  public int    NumPublicConns      { get; set; } = 0;
    [JsonPropertyName("PUBLICPORT_s")]            public string PublicPort          { get; set; } = "";
    [JsonPropertyName("BUILDUNIQUEID_l")]         public int    BuildUniqueId       { get; set; } = 0;
    [JsonPropertyName("BANTICHEATPROTECTED_b")]   public bool   AntiCheatProtected  { get; set; } = true;
    [JsonPropertyName("BISDEDICATED_b")]          public bool   IsDedicated         { get; set; } = true;
    [JsonPropertyName("IMGUIPORT_s")]             public string ImguiPort           { get; set; } = "";
}

public sealed class EosSessionSettings
{
    [JsonPropertyName("maxPublicPlayers")]
    public int MaxPublicPlayers { get; set; }

    [JsonPropertyName("allowInvites")]
    public bool AllowInvites { get; set; }

    [JsonPropertyName("shouldAdvertise")]
    public bool ShouldAdvertise { get; set; }

    [JsonPropertyName("allowReadById")]
    public bool AllowReadById { get; set; }

    [JsonPropertyName("allowJoinViaPresence")]
    public bool AllowJoinViaPresence { get; set; }

    [JsonPropertyName("allowJoinInProgress")]
    public bool AllowJoinInProgress { get; set; }

    [JsonPropertyName("allowConferenceRoom")]
    public bool AllowConferenceRoom { get; set; }

    [JsonPropertyName("checkSanctions")]
    public bool CheckSanctions { get; set; }

    [JsonPropertyName("allowMigration")]
    public bool AllowMigration { get; set; }

    [JsonPropertyName("rejoinAfterKick")]
    public string RejoinAfterKick { get; set; } = string.Empty;

    [JsonPropertyName("platforms")]
    public string? Platforms { get; set; } = null;
}
