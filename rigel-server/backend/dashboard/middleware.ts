import { NextRequest, NextResponse } from "next/server";

// Page gate for the dashboard UI. CAPTURE-FIRST: keep AUTH_ENFORCE=false until the Meta SSO callback
// (/auth/meta on :8080) actually mints the `a2dash_session` cookie — otherwise every page would
// redirect to /auth/login and, with no cookie ever set, loop forever. Flip this true together with
// DashboardAuthPreprocessor.Enforce on the backend once a real session is landing in the logs.
const AUTH_ENFORCE = false;
const SESSION_COOKIE = "a2dash_session";

export function middleware(req: NextRequest) {
  if (!AUTH_ENFORCE) return NextResponse.next();

  const { pathname } = req.nextUrl;
  // Never gate the auth round-trip, the API/game proxies (those gate server-side), or Next internals.
  if (
    pathname.startsWith("/auth") ||
    pathname.startsWith("/api") ||
    pathname.startsWith("/game") ||
    pathname.startsWith("/_next") ||
    pathname === "/favicon.ico"
  ) {
    return NextResponse.next();
  }

  // Presence check only — the backend validates the signature/expiry on every /api call.
  if (!req.cookies.get(SESSION_COOKIE)) {
    const url = req.nextUrl.clone();
    url.pathname = "/auth/login";
    return NextResponse.redirect(url);
  }
  return NextResponse.next();
}

export const config = {
  matcher: ["/((?!_next/static|_next/image|favicon.ico).*)"],
};
