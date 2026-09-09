import type { NextConfig } from "next";

// Where the backend is running. Set BACKEND_HOST to point the UI somewhere else,
// e.g. BACKEND_HOST=157.173.194.216 for a VPS; run-dashboard.cmd sets 127.0.0.1.
//   /api,/auth -> AresDashboardServer :8080   (reads, dashboard login)
//   /game      -> A2StationDbServer   :78     (writes, kept off :8080 to avoid
//                                              LiteDB concurrent-access errors)
const BACKEND_HOST = process.env.BACKEND_HOST ?? "127.0.0.1";
const DASHBOARD_PORT = process.env.DASHBOARD_API_PORT ?? "8080";
const STATION_PORT = process.env.STATION_API_PORT ?? "78";

const nextConfig: NextConfig = {
  async rewrites() {
    return [
      { source: "/api/:path*",  destination: `http://${BACKEND_HOST}:${DASHBOARD_PORT}/api/:path*`  },
      { source: "/auth/:path*", destination: `http://${BACKEND_HOST}:${DASHBOARD_PORT}/auth/:path*` },
      { source: "/game/:path*", destination: `http://${BACKEND_HOST}:${STATION_PORT}/:path*`        },
    ];
  },
};

export default nextConfig;
