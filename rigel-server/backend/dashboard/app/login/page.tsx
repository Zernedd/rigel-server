"use client";

import { useState } from "react";

// Key gate for the dashboard UI.
//
// The SSO path (/auth/meta -> a2dash_session) needs its redirect_uri registered with Meta against
// this hostname, which is not done yet, so middleware's AUTH_ENFORCE would have redirected forever
// with no cookie ever minted. This is the key-based route instead: paste the admin key once, the
// server checks it and sets an httpOnly cookie. The key itself is never stored in the browser and
// never appears in a URL — only the opaque cookie is, and /api/* proxying happens server-side.

export default function Login() {
  const [key, setKey] = useState("");
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState(false);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    setBusy(true);
    setErr("");
    try {
      const res = await fetch("/dash-login", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ key }),
      });
      if (res.ok) {
        window.location.href = "/";
        return;
      }
      setErr(res.status === 401 ? "That key was rejected." : `Login failed (${res.status}).`);
    } catch (e2) {
      setErr(String(e2));
    }
    setBusy(false);
  }

  return (
    <div className="flex min-h-screen items-center justify-center bg-neutral-950 p-6">
      <form
        onSubmit={submit}
        className="w-full max-w-md rounded-xl border border-neutral-800 bg-neutral-900 p-7 shadow-xl"
      >
        <h1 className="mb-1 text-lg font-semibold text-neutral-100">Rigel dashboard</h1>
        <p className="mb-5 text-sm leading-relaxed text-neutral-400">
          This controls the live fleet. Enter the admin key to continue.
        </p>

        <input
          type="password"
          value={key}
          onChange={(e) => setKey(e.target.value)}
          placeholder="dashboard admin key"
          autoComplete="off"
          autoFocus
          className="w-full rounded-lg border border-neutral-700 bg-neutral-950 px-3 py-2.5 font-mono text-sm text-neutral-100 outline-none focus:border-blue-500"
        />

        <div className="mt-2 min-h-[18px] text-xs text-red-400">{err}</div>

        <button
          type="submit"
          disabled={busy || !key.trim()}
          className="mt-3 w-full rounded-lg bg-blue-600 py-2.5 text-sm font-medium text-white transition hover:bg-blue-500 disabled:opacity-40"
        >
          {busy ? "Checking…" : "Unlock"}
        </button>
      </form>
    </div>
  );
}
