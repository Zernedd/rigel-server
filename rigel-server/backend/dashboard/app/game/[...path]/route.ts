import { NextRequest, NextResponse } from "next/server";
import { validSession } from "@/lib/session";

// Server-side proxy for /game/* -> A2StationDbServer on :78 (station / deployment writes).
//
// [2026-09-27] This was a plain next.config rewrite: it passed ANY request through, cookie or not, to an API with no
// authentication of its own. Now the dashboard session is verified here and the admin key attached server-side,
// which :78 requires for writes that don't come from the game servers on this machine (StationApiGuard).

const BACKEND_HOST = process.env.BACKEND_HOST ?? "127.0.0.1";
const STATION_PORT = process.env.STATION_API_PORT ?? "78";
const ADMIN_KEY = process.env.DASHBOARD_ADMIN_KEY ?? "";

async function proxy(req: NextRequest, path: string[]) {
  if (!validSession(req)) return NextResponse.json({ error: "unauthorized" }, { status: 401 });
  const target = `http://${BACKEND_HOST}:${STATION_PORT}/${path.join("/")}${req.nextUrl.search ?? ""}`;

  const headers = new Headers();
  const ct = req.headers.get("content-type");
  if (ct) headers.set("content-type", ct);
  if (ADMIN_KEY) headers.set("x-dashboard-key", ADMIN_KEY);

  const method = req.method.toUpperCase();
  const body = method === "GET" || method === "HEAD" ? undefined : Buffer.from(await req.arrayBuffer());
  try {
    const res = await fetch(target, { method, headers, body, cache: "no-store" });
    const buf = Buffer.from(await res.arrayBuffer());
    return new NextResponse(buf, {
      status: res.status,
      headers: { "content-type": res.headers.get("content-type") ?? "application/json", "cache-control": "no-store" },
    });
  } catch (e) {
    return NextResponse.json({ error: "backend unreachable", detail: String(e) }, { status: 502 });
  }
}

type Ctx = { params: Promise<{ path: string[] }> };

export async function GET(req: NextRequest, ctx: Ctx) {
  return proxy(req, (await ctx.params).path);
}
export async function POST(req: NextRequest, ctx: Ctx) {
  return proxy(req, (await ctx.params).path);
}
export async function PUT(req: NextRequest, ctx: Ctx) {
  return proxy(req, (await ctx.params).path);
}
export async function PATCH(req: NextRequest, ctx: Ctx) {
  return proxy(req, (await ctx.params).path);
}
export async function DELETE(req: NextRequest, ctx: Ctx) {
  return proxy(req, (await ctx.params).path);
}

export const dynamic = "force-dynamic";
