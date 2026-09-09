import { NextRequest, NextResponse } from "next/server";
import { createHmac, timingSafeEqual } from "crypto";

// Validates the pasted admin key and mints the UI session cookie.
//
// The cookie is NOT the key — it is an HMAC of a fixed marker under the session secret, so a stolen
// cookie cannot be turned back into the admin key, and the value is stable enough to verify without
// server-side session storage. httpOnly + sameSite=lax keeps it out of JavaScript's reach.

const ADMIN_KEY = process.env.DASHBOARD_ADMIN_KEY ?? "";
const SESSION_SECRET = process.env.DASHBOARD_SESSION_SECRET ?? "";
export const COOKIE = "rigel_dash";

export function sessionValue() {
  return createHmac("sha256", SESSION_SECRET || ADMIN_KEY || "unset")
    .update("rigel-dashboard-v1")
    .digest("hex");
}

function constantTimeEquals(a: string, b: string) {
  const ab = Buffer.from(a);
  const bb = Buffer.from(b);
  if (ab.length !== bb.length) return false;
  return timingSafeEqual(ab, bb);
}

export async function POST(req: NextRequest) {
  if (!ADMIN_KEY) {
    return NextResponse.json(
      { error: "DASHBOARD_ADMIN_KEY is not set on the dashboard process" },
      { status: 500 },
    );
  }

  let key = "";
  try {
    key = ((await req.json()) as { key?: string }).key ?? "";
  } catch {
    return NextResponse.json({ error: "bad body" }, { status: 400 });
  }

  if (!key || !constantTimeEquals(key, ADMIN_KEY)) {
    return NextResponse.json({ error: "unauthorized" }, { status: 401 });
  }

  // Cloudflare terminates TLS and forwards plain HTTP to 127.0.0.1:3000, so the request's own
  // scheme is always http here -- trust x-forwarded-proto to decide. Marking the cookie `secure`
  // unconditionally would be correct in production but makes the cookie undeliverable over a
  // loopback/SSH-tunnel session, which is exactly how this gets reached before DNS is live.
  const proto = req.headers.get("x-forwarded-proto") ?? "http";
  const isHttps = proto.split(",")[0].trim() === "https";

  const res = NextResponse.json({ ok: true });
  res.cookies.set(COOKIE, sessionValue(), {
    httpOnly: true,
    sameSite: "lax",
    secure: isHttps,
    path: "/",
    maxAge: 60 * 60 * 12,
  });
  return res;
}

export const dynamic = "force-dynamic";
