export type Stats = {
  total_users: number;
  total_stations: number;
  total_deployments: number;
  online_deployments: number;
  total_players: number;
  total_events: number;
};

export type User = {
  user_id: string;
  username: string;
  platform: string | null;
  last_login: string;
  created_at: string;
  role_count: number;
  banned: boolean;
  is_admin: boolean;
};

export type WhitelistEntry = {
  username: string;
  /** false when no account with this username has ever logged in - likely a typo. */
  known: boolean;
};

export type WhitelistAccount = {
  user_id: string;
  username: string;
  platform: string | null;
  last_login: string;
};

export type Whitelist = {
  station_id: string;
  /** false means the list is empty, which is the default: the station is visible to EVERYONE. */
  enabled: boolean;
  entries: WhitelistEntry[];
  /** every known account, for the picker - not just the 50 most recent. */
  accounts: WhitelistAccount[];
};

/** A level saved from the in-game Spec Editor (Levels panel). */
export type SpecLevel = {
  name: string;
  /** load this level whenever a game server boots */
  autoload: boolean;
  /** desired state: game servers poll this and load/unload to match (~15 s) */
  loaded: boolean;
  updated: string;
  size: number;
  /** deployment ids that last reported this level as loaded */
  serverLoaded: string[];
  /** when a game server last reported its loaded levels */
  serverSeen: string;
};

export type Station = {
  station_id: string;
  station_name: string;
  created_at: string;
  online: boolean;
  last_online: string | null;
  deployments: number;
  online_deps: number;
  player_count: number;
};

export type Deployment = {
  deployment_id: string;
  station_id: string;
  deployment_name: string | null;
  ip_address: string | null;
  region: string | null;
  version: string | null;
  online: boolean;
  player_count: number;
  last_event: string | null;
  created_at: string;
};

export type ServerEvent = {
  event_id: string;
  deployment_id: string;
  event_name: string;
  created_at: string;
};

export type Ban = {
  user_id: string;
  username: string;
  station_id: string;
  reason: string;
  expiration: string | null;
  revoked: boolean;
};

export type Role = {
  role_id: string;
  station_id: string;
  role_name: string;
  role_description: string;
  permissions: string[];
};

export type Member = {
  user_id: string;
  username: string;
  platform: string | null;
  roles: string[];
};

export type ActionResult = {
  success: boolean;
  error?: string;
  id?: string;
};

export type LaunchResult = {
  deployment_id?: string;
  api_key?: string;
};

export type EosSession = {
  id: string;
  server_name: string;
  ip_address: string;
  port: string;
  max_players: number;
  bucket: string;
  build_id: number;
  imgui_port: string;
  station_id: string | null;
  deployment_id: string | null;
};

export type TelemetryEvent = {
  id: string;
  eventName: string;
  payload: string;
  timestamp: string;
};

export type WsClient = {
  session_id: string;
  path: string;
  connected_at: string;
  disconnected_at: string | null;
  message_count: number;
};

export type WsMessage = {
  direction: "in" | "out";
  command: string;
  headers: string;
  body: string;
  timestamp: string;
};

export type NetvarDumpInfo = {
  deployment_id: string;
  event_type: string;
  bytes: number;
  received_at: string;
  source: "live" | "disk";
};

export type StationEvent = {
  event_id: string;
  station_id: string;
  deployment_id: string | null;
  title: string;
  description: string;
  start_time: string;
  duration: number;
  public: boolean;
  signups_open: boolean;
};

// The dashboard API is gated server-side (DashboardAuthPreprocessor). A 401 means no valid session
// cookie -> bounce the browser to Meta sign-in. Only fires once the backend gate is enforcing; while
// it's capture-first there are no 401s, so this is inert.
function check(res: Response) {
  if (res.status === 401 && typeof window !== "undefined") {
    window.location.href = "/auth/login";
    throw new Error("unauthorized");
  }
  if (!res.ok) throw new Error(`API error ${res.status}`);
}

async function get<T>(path: string): Promise<T> {
  const res = await fetch(`/api/${path}`, { cache: "no-store" });
  check(res);
  return res.json();
}

async function post<T>(path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api/${path}`, {
    method: "POST",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  check(res);
  return res.json();
}

async function patch<T>(path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api/${path}`, {
    method: "PATCH",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  check(res);
  return res.json();
}

async function put<T>(path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api/${path}`, {
    method: "PUT",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  check(res);
  return res.json();
}

async function del<T>(path: string): Promise<T> {
  const res = await fetch(`/api/${path}`, { method: "DELETE" });
  check(res);
  return res.json();
}

async function gamePost<T>(path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/game/${path}`, {
    method: "POST",
    headers: body ? { "Content-Type": "application/json" } : {},
    body: body ? JSON.stringify(body) : undefined,
  });
  check(res);
  return res.json();
}

async function gameDel<T>(path: string): Promise<T> {
  const res = await fetch(`/game/${path}`, { method: "DELETE" });
  check(res);
  return res.json();
}

export const api = {
  // Read
  stats:       ()                  => get<Stats>("stats"),
  users:       ()                  => get<User[]>("users"),
  stations:    ()                  => get<Station[]>("stations"),
  deployments: ()                  => get<Deployment[]>("deployments"),
  events:      ()                  => get<ServerEvent[]>("events"),

  // Station moderation
  stationBans:    (stationId: string) => get<Ban[]>(`stations/${stationId}/bans`),
  stationRoles:   (stationId: string) => get<Role[]>(`stations/${stationId}/roles`),
  stationMembers: (stationId: string) => get<Member[]>(`stations/${stationId}/members`),

  // Ban / unban
  banUser:   (userId: string, stationId: string, reason: string, duration: number) =>
    post<ActionResult>(`users/${userId}/ban`, { station_id: stationId, reason, duration }),
  unbanUser: (userId: string, stationId: string) =>
    patch<ActionResult>(`users/${userId}/unban`, { station_id: stationId }),

  // Role management
  createRole: (stationId: string, roleName: string, roleDescription: string) =>
    post<ActionResult>(`stations/${stationId}/roles`, { role_name: roleName, role_description: roleDescription }),
  deleteRole: (stationId: string, roleId: string) =>
    del<ActionResult>(`stations/${stationId}/roles/${roleId}`),
  setRolePermissions: (stationId: string, roleId: string, permissions: string[]) =>
    patch<ActionResult>(`stations/${stationId}/roles/${roleId}/permissions`, { permissions }),

  // Create station / deployment — via game server (port 7811) to avoid LiteDB issue
  createStation: (stationName: string) =>
    gamePost<{ station_id: string }>(`stations/create`, { station_name: stationName }),
  // Delete goes through the dashboard API (:8080), not the game server -- it cascades to the
  // station's deployments, roles, events and EOS sessions. `force` re-sends after the backend
  // refuses a fleet that currently has players on it.
  deleteStation: (stationId: string, force = false) =>
    del<ActionResult>(`stations/${stationId}${force ? "?force=true" : ""}`),
  launchDeployment: (stationId: string, body: { deployment_name?: string; ip?: string; region?: string; version?: string }) =>
    gamePost<LaunchResult>(`stations/${stationId}/deployments`, body),

  // Spin up a server via HalcyonSocket (replaces manual ip/port entry). An allocator agent launches
  // the game server on a box; it self-registers and appears in the deployment list. Goes through
  // AresDashboardServer on :8080 (the /api/* proxy).
  spinUpServer: (stationId: string, body: { box?: string; map?: string; region?: string; args?: string; name?: string }) =>
    post<{ success: boolean; request_id?: string; agent?: string; error?: string }>(`stations/${stationId}/spinup`, body),
  agents: () =>
    get<{ agents: { box: string; capacity: number; running: number; free: number }[] }>("agents"),

  // Create event via game server (uses query params)
  createEvent: (stationId: string, p: {
    title: string; description?: string; start_time?: string;
    duration?: number; deployment_id?: string; public?: boolean; signups_open?: boolean;
  }) => {
    const q = new URLSearchParams({
      title:        p.title,
      description:  p.description  ?? "",
      duration:     String(p.duration ?? 3600),
      start_time:   p.start_time    ?? new Date().toISOString(),
      public:       String(p.public ?? true),
      signups_open: String(p.signups_open ?? true),
      ...(p.deployment_id ? { deployment_id: p.deployment_id } : {}),
    });
    return fetch(`/game/v1/stations/${stationId}/event?${q}`, { method: "POST", body: "{}" })
      .then(r => r.json() as Promise<Record<string, unknown>>);
  },
  deleteEvent: (stationId: string, eventId: string) =>
    gameDel<ActionResult>(`v1/stations/${stationId}/event/${eventId}`),

  // Station detail lists
  stationDeployments: (stationId: string) => get<Deployment[]>(`stations/${stationId}/deployments`),
  stationEvents:      (stationId: string) => get<StationEvent[]>(`stations/${stationId}/events`),

  // EOS session management (in-memory on the server, managed via dashboard)
  sessions:       ()                  => get<EosSession[]>("sessions"),
  addSession:     (s: Omit<EosSession, "id">) => post<ActionResult>("sessions", s),
  removeSession:  (id: string)        => del<ActionResult>(`sessions/${id}`),

  // Bulk-create N EOS sessions with auto-incrementing ports
  bulkAddSessions: (
    stationId: string,
    count: number,
    opts: { server_name: string; ip_address: string; base_port: number; max_players: number; bucket: string; build_id: number; imgui_port_base: number }
  ) => {
    const tasks: Promise<ActionResult>[] = [];
    for (let i = 0; i < count; i++) {
      tasks.push(post<ActionResult>("sessions", {
        server_name:   `${opts.server_name}_${i + 1}`,
        ip_address:    opts.ip_address,
        port:          String(opts.base_port + i),
        max_players:   opts.max_players,
        bucket:        opts.bucket,
        build_id:      opts.build_id,
        imgui_port:    opts.imgui_port_base > 0 ? String(opts.imgui_port_base + i) : "",
        station_id:    stationId,
        deployment_id: null,
      }));
    }
    return Promise.all(tasks);
  },

  // Role assignment
  assignRole: (stationId: string, userId: string, roleId: string) =>
    post<ActionResult>(`stations/${stationId}/users/${userId}/roles/${roleId}`),
  removeRole: (stationId: string, userId: string, roleId: string) =>
    del<ActionResult>(`stations/${stationId}/users/${userId}/roles/${roleId}`),

  // Observability
  telemetry:      (limit?: number)   => get<TelemetryEvent[]>(`telemetry${limit ? `?limit=${limit}` : ""}`),
  wsClients:      ()                 => get<WsClient[]>("ws-clients"),
  wsMessages:     (sessionId: string) => get<WsMessage[]>(`ws-clients/${sessionId}/messages`),

  // Global admin
  grantAdmin:  (userId: string) => post<ActionResult>(`users/${userId}/grant_admin`),
  revokeAdmin: (userId: string) => del<ActionResult>(`users/${userId}/grant_admin`),

  // Netvars: the live dump the game server reports, and the station config it pulls as netvars
  netvarDumps: () => get<NetvarDumpInfo[]>("netvars/dumps"),
  netvarDump: (deploymentId?: string, type: string = "netvars") =>
    get<unknown>(`netvars/dump?type=${encodeURIComponent(type)}${deploymentId ? `&deployment_id=${encodeURIComponent(deploymentId)}` : ""}`),
  stationConfig: (stationId: string) => get<Record<string, string>>(`stations/${stationId}/config`),
  patchStationConfig: (stationId: string, kv: Record<string, string>) =>
    patch<Record<string, string>>(`stations/${stationId}/config`, kv),
  deleteStationConfigKey: (stationId: string, key: string) =>
    del<{ success: boolean; key: string }>(`stations/${stationId}/config?key=${encodeURIComponent(key)}`),

  // Station whitelist: which accounts may SEE the station in the browser.
  stationWhitelist: (stationId: string) => get<Whitelist>(`stations/${stationId}/whitelist`),
  setStationWhitelist: (stationId: string, usernames: string[]) =>
    put<{ success: boolean; station_id: string; enabled: boolean; usernames: string[] }>(
      `stations/${stationId}/whitelist`, { usernames }),

  // Spec Editor saved levels (uploaded by game servers; the dashboard flips autoload/loaded)
  specLevels: () => get<SpecLevel[]>("spec/levels"),
  specLevel: (name: string) => get<{ name: string; text: string }>(`spec/levels/${encodeURIComponent(name)}`),
  patchSpecLevel: (name: string, body: { autoload?: boolean; loaded?: boolean }) =>
    patch<{ ok: boolean }>(`spec/levels/${encodeURIComponent(name)}`, body),
  deleteSpecLevel: (name: string) => del<{ ok: boolean }>(`spec/levels/${encodeURIComponent(name)}`),
};
