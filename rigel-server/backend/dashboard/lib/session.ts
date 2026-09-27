import { NextRequest } from "next/server";
import { createHmac, timingSafeEqual } from "crypto";

// The dashboard UI session: the `rigel_dash` cookie is an HMAC of a fixed marker under DASHBOARD_SESSION_SECRET
// (falling back to the admin key), minted by app/dash-login. Server-side routes MUST verify it with
// validSession() before acting -- the edge gate (proxy.ts) can only check that a cookie is present, and the API
// proxies attach the admin key themselves, so an unverified cookie would be admin for anyone who sets one.
// Rotating DASHBOARD_SESSION_SECRET (or the admin key when no secret is set) signs every session out.

export const COOKIE = "rigel_dash";
const ADMIN_KEY = process.env.DASHBOARD_ADMIN_KEY ?? "";
const SESSION_SECRET = process.env.DASHBOARD_SESSION_SECRET ?? "";

export function sessionValue() {
  return createHmac("sha256", SESSION_SECRET || ADMIN_KEY || "unset")
    .update("rigel-dashboard-v1")
    .digest("hex");
}

export function constantTimeEquals(a: string, b: string) {
  const ab = Buffer.from(a);
  const bb = Buffer.from(b);
  if (ab.length !== bb.length) return false;
  return timingSafeEqual(ab, bb);
}

export function validSession(req: NextRequest) {
  if (!ADMIN_KEY) return false;
  const v = req.cookies.get(COOKIE)?.value ?? "";
  return v.length > 0 && constantTimeEquals(v, sessionValue());
}
