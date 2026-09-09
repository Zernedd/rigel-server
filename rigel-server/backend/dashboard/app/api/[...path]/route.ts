import { NextRequest, NextResponse } from "next/server";

// Server-side proxy for /api/* -> AresDashboardServer on :8080.
//
// This replaces the plain next.config rewrite because the backend now ENFORCES auth on /api/*
// (DashboardAuthPreprocessor), so every call needs the admin key. Doing the proxy here rather
// than in a rewrite means the key is read from the server's environment and attached on the
// server — the browser never receives it, so it cannot leak through devtools, an extension, a
// copied HAR, or a screenshot. The browser's only credential is the httpOnly session cookie that
// middleware.ts checks.
//
// DASHBOARD_ADMIN_KEY must match the backend's (set machine-wide on the VPS).

const BACKEND_HOST = process.env.BACKEND_HOST ?? "127.0.0.1";
const DASHBOARD_PORT = process.env.DASHBOARD_API_PORT ?? "8080";
const ADMIN_KEY = process.env.DASHBOARD_ADMIN_KEY ?? "";

async function proxy(req: NextRequest, path: string[]) {
  const search = req.nextUrl.search ?? "";
  const target = `http://${BACKEND_HOST}:${DASHBOARD_PORT}/api/${path.join("/")}${search}`;

  const headers = new Headers();
  // Forward only what the backend needs; never forward the browser's cookies upstream.
  const ct = req.headers.get("content-type");
  if (ct) headers.set("content-type", ct);
  if (ADMIN_KEY) headers.set("x-dashboard-key", ADMIN_KEY);

  const method = req.method.toUpperCase();
  const body =
    method === "GET" || method === "HEAD" ? undefined : Buffer.from(await req.arrayBuffer());

  try {
    const res = await fetch(target, { method, headers, body, cache: "no-store" });
    const buf = Buffer.from(await res.arrayBuffer());
    return new NextResponse(buf, {
      status: res.status,
      headers: {
        "content-type": res.headers.get("content-type") ?? "application/json",
        "cache-control": "no-store",
      },
    });
  } catch (e) {
    return NextResponse.json(
      { error: "backend unreachable", detail: String(e), target },
      { status: 502 },
    );
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
