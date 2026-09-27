import type { NextConfig } from "next";

// Where the backend is running. Set BACKEND_HOST to point the UI somewhere else; on the VPS
// everything is loopback.
//   /api   -> handled by app/api/[...path]/route.ts, NOT a rewrite. The backend enforces auth on
//             /api/*, so the request needs the admin key attached SERVER-SIDE; a plain rewrite
//             cannot add a header, and putting the key in the browser would defeat the point.
//   /auth  -> AresDashboardServer :8080 (Meta SSO round-trip; no key needed)
//   /game  -> handled by app/game/[...path]/route.ts (A2StationDbServer :78): verifies the session and
//             attaches the admin key, which :78 now requires for outside writes (StationApiGuard).
const BACKEND_HOST = process.env.BACKEND_HOST ?? "127.0.0.1";
const DASHBOARD_PORT = process.env.DASHBOARD_API_PORT ?? "8080";

const nextConfig: NextConfig = {
  async rewrites() {
    return [
      { source: "/auth/:path*", destination: `http://${BACKEND_HOST}:${DASHBOARD_PORT}/auth/:path*` },
    ];
  },
};

export default nextConfig;
