import type { NextConfig } from "next";

// Where the backend is running. Set BACKEND_HOST to point the UI somewhere else; on the VPS
// everything is loopback.
//   /api   -> handled by app/api/[...path]/route.ts, NOT a rewrite. The backend enforces auth on
//             /api/*, so the request needs the admin key attached SERVER-SIDE; a plain rewrite
//             cannot add a header, and putting the key in the browser would defeat the point.
//   /auth  -> AresDashboardServer :8080 (Meta SSO round-trip; no key needed)
//   /game  -> A2StationDbServer   :78   (writes, kept off :8080 to avoid LiteDB concurrent-access
//                                        errors)
const BACKEND_HOST = process.env.BACKEND_HOST ?? "127.0.0.1";
const DASHBOARD_PORT = process.env.DASHBOARD_API_PORT ?? "8080";
const STATION_PORT = process.env.STATION_API_PORT ?? "78";

const nextConfig: NextConfig = {
  async rewrites() {
    return [
      { source: "/auth/:path*", destination: `http://${BACKEND_HOST}:${DASHBOARD_PORT}/auth/:path*` },
      { source: "/game/:path*", destination: `http://${BACKEND_HOST}:${STATION_PORT}/:path*` },
    ];
  },
};

export default nextConfig;
