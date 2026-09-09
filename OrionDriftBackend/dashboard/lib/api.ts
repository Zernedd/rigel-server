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
};
