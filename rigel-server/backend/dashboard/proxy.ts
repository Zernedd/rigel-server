import { NextRequest, NextResponse } from "next/server";

// Page + API gate for the dashboard UI.
//
// NOTE: this file is `proxy.ts`, not `middleware.ts` -- Next 16 deprecated the middleware
// convention and renamed it to `proxy` (node_modules/next/dist/docs version-16.md). The export
// must be named `proxy` (or be the default export).
//
// [2026-09-09] Was AUTH_ENFORCE=false with an SSO-only path: the Meta callback (/auth/meta on :8080)
// is the only thing that mints `a2dash_session`, and its redirect_uri is not registered against this
// hostname, so turning the old gate on would have redirected to /auth/login forever with no cookie
// ever set. Replaced with a key gate: /login checks the admin key server-side (see app/dash-login)
// and sets an httpOnly `rigel_dash` cookie.
//
// This gate covers the PAGES *and* the /api proxy route. That matters: app/api/[...path]/route.ts
// attaches the backend admin key server-side, so an ungated /api here would be an open relay to the
// whole control API for anyone who could reach this port.
//
// Only a PRESENCE check happens here — Edge middleware cannot use node:crypto to verify the HMAC.
// That is safe because the cookie is httpOnly, secure, and its value is an HMAC under a secret the
// browser never sees, so it cannot be forged without that secret; and every /api call is still
// authenticated upstream by the backend's own key check.
const SESSION_COOKIE = "rigel_dash";

export function proxy(req: NextRequest) {
  const { pathname } = req.nextUrl;

  // The login page, the login endpoint itself, and Next internals must stay reachable.
  if (
    pathname === "/login" ||
    pathname === "/dash-login" ||
    pathname.startsWith("/_next") ||
    pathname === "/favicon.ico"
  ) {
    return NextResponse.next();
  }

  if (!req.cookies.get(SESSION_COOKIE)) {
    // API calls get a 401 rather than an HTML redirect, so fetch() surfaces a real error.
    if (pathname.startsWith("/api") || pathname.startsWith("/game")) {
      return NextResponse.json({ error: "unauthorized" }, { status: 401 });
    }
    const url = req.nextUrl.clone();
    url.pathname = "/login";
    url.search = "";
    return NextResponse.redirect(url);
  }

  return NextResponse.next();
}

export const config = {
  matcher: ["/((?!_next/static|_next/image|favicon.ico).*)"],
};
