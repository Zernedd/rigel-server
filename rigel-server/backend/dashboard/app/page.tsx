"use client";

import { useEffect, useState } from "react";
import { useRouter } from "next/navigation";
import Image from "next/image";
import { api, type Station } from "../lib/api";
import { RefreshCw, Server, Plus, X, Trash2 } from "lucide-react";

const PAGE_SIZE = 16;

export default function HomePage() {
  const router = useRouter();
  const [stations, setStations]     = useState<Station[] | null>(null);
  const [err, setErr]               = useState<string | null>(null);
  const [onlineOnly, setOnlineOnly] = useState(false);
  const [loading, setLoading]       = useState(false);
  const [page, setPage]             = useState(0);
  const [creating, setCreating]     = useState(false);
  const [newName, setNewName]       = useState("");
  const [saving, setSaving]         = useState(false);
  const [delTarget, setDelTarget]   = useState<Station | null>(null);
  const [deleting, setDeleting]     = useState(false);
  const [delErr, setDelErr]         = useState<string | null>(null);

  function load() {
    setLoading(true);
    api.stations()
      .then(d  => { setStations(d); setErr(null); setPage(0); })
      .catch(e => setErr(e.message))
      .finally(() => setLoading(false));
  }

  useEffect(() => { load(); }, []);

  async function createStation() {
    if (!newName.trim()) return;
    setSaving(true);
    try {
      const res = await api.createStation(newName.trim());
      if (res.station_id) { setCreating(false); setNewName(""); load(); }
    } finally { setSaving(false); }
  }

  // The backend refuses a fleet that currently has players on it unless force=true, so a populated
  // fleet takes a second, deliberate click rather than being one misclick away from deletion.
  async function deleteStation(force: boolean) {
    if (!delTarget) return;
    setDeleting(true);
    setDelErr(null);
    try {
      const res = await api.deleteStation(delTarget.station_id, force);
      if (!res.success) { setDelErr(res.error ?? "Delete failed"); return; }
      setDelTarget(null);
      load();
    } catch (e: unknown) {
      setDelErr(e instanceof Error ? e.message : "Unknown error");
    } finally { setDeleting(false); }
  }

  const filtered   = stations ? (onlineOnly ? stations.filter(s => s.online) : stations) : null;
  const totalPages = filtered ? Math.ceil(filtered.length / PAGE_SIZE) : 0;
  const pageItems  = filtered ? filtered.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE) : null;

  return (
    <div className="container h-full mx-auto flex justify-center items-center px-4 py-8">
      <div className="flex flex-col lg:flex-row items-center lg:items-start gap-10 w-full max-w-6xl text-center lg:text-left">

        {/* Left: station picker */}
        <div className="flex-1 min-w-0 flex flex-col items-center lg:items-start gap-6 w-full">

          {/* Create station modal */}
          {creating && (
            <div className="fixed inset-0 z-50 flex items-center justify-center"
              style={{ background: "rgba(0,0,0,0.65)" }}
              onClick={e => { if (e.target === e.currentTarget) setCreating(false); }}>
              <div className="w-full max-w-sm rounded-lg p-6"
                style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
                <div className="flex items-center justify-between mb-5">
                  <h2 className="font-semibold text-sm">Create Station</h2>
                  <button onClick={() => setCreating(false)} style={{ color: "var(--muted)" }}><X size={16} /></button>
                </div>
                <label className="block text-xs mb-1.5" style={{ color: "var(--muted)" }}>Station Name</label>
                <input autoFocus
                  className="w-full px-3 py-2 rounded-md text-sm outline-none mb-5"
                  style={{ background: "var(--surface2)", border: "1px solid var(--border)", color: "var(--text)" }}
                  value={newName} onChange={e => setNewName(e.target.value)}
                  onKeyDown={e => e.key === "Enter" && createStation()}
                  placeholder="My Station" />
                <div className="flex gap-2 justify-end">
                  <button onClick={() => setCreating(false)}
                    className="px-4 py-2 rounded-md text-xs"
                    style={{ color: "var(--muted)", border: "1px solid var(--border)" }}>Cancel</button>
                  <button onClick={createStation} disabled={saving || !newName.trim()}
                    className="px-4 py-2 rounded-md text-xs font-medium"
                    style={{ background: "var(--accent)", color: "#fff", opacity: saving ? 0.5 : 1 }}>
                    {saving ? "Creating…" : "Create"}
                  </button>
                </div>
              </div>
            </div>
          )}

          {/* Delete fleet confirmation */}
          {delTarget && (
            <div className="fixed inset-0 z-50 flex items-center justify-center"
              style={{ background: "rgba(0,0,0,0.65)" }}
              onClick={e => { if (e.target === e.currentTarget) setDelTarget(null); }}>
              <div className="w-full max-w-sm rounded-lg p-6 text-left"
                style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
                <div className="flex items-center justify-between mb-4">
                  <h2 className="font-semibold text-sm">Delete “{delTarget.station_name}”?</h2>
                  <button onClick={() => setDelTarget(null)} style={{ color: "var(--muted)" }}><X size={16} /></button>
                </div>
                <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
                  Removes the fleet along with its {delTarget.deployments} deployment(s), roles, events
                  and EOS sessions. This cannot be undone.
                </p>
                {delTarget.player_count > 0 && (
                  <div className="mb-4 px-3 py-2 rounded text-xs"
                    style={{ background: "#2d1a1a", color: "var(--red)", border: "1px solid #5c2020" }}>
                    {delTarget.player_count} player(s) are on this fleet right now.
                  </div>
                )}
                {delErr && (
                  <div className="mb-4 px-3 py-2 rounded text-xs"
                    style={{ background: "#2d1a1a", color: "var(--red)" }}>{delErr}</div>
                )}
                <div className="flex gap-2 justify-end">
                  <button onClick={() => setDelTarget(null)}
                    className="px-4 py-2 rounded-md text-xs"
                    style={{ color: "var(--muted)", border: "1px solid var(--border)" }}>Cancel</button>
                  <button onClick={() => deleteStation(delTarget.player_count > 0 || delErr !== null)}
                    disabled={deleting}
                    className="px-4 py-2 rounded-md text-xs font-medium"
                    style={{ background: "var(--red)", color: "#fff", opacity: deleting ? 0.5 : 1 }}>
                    {deleting ? "Deleting…" : delTarget.player_count > 0 ? "Delete anyway" : "Delete"}
                  </button>
                </div>
              </div>
            </div>
          )}

          <div className="space-y-2">
            <h1 className="text-4xl font-bold">
              Welcome to{" "}
              <span style={{
                background: "linear-gradient(to bottom right, var(--accent), #a371f7)",
                WebkitBackgroundClip: "text",
                WebkitTextFillColor: "transparent",
              }}>Ares</span>!
            </h1>
            <h4 className="text-lg" style={{ color: "var(--muted)" }}>
              Please choose a station:
            </h4>
          </div>

          <div className="w-full">
            {/* Controls */}
            <div className="flex justify-between items-center mb-2">
              <label className="flex items-center gap-2 cursor-pointer select-none text-sm"
                style={{ color: "var(--muted)" }}>
                <div onClick={() => { setOnlineOnly(v => !v); setPage(0); }}
                  className="relative w-10 h-5 rounded-full cursor-pointer"
                  style={{ background: onlineOnly ? "var(--green)" : "var(--surface2)", border: "1px solid var(--border)" }}>
                  <div className="absolute top-0.5 w-4 h-4 rounded-full bg-white transition-all duration-200"
                    style={{ left: onlineOnly ? "calc(100% - 18px)" : "2px" }} />
                </div>
                Only show online stations
              </label>
              <div className="flex items-center gap-2">
                <button onClick={() => setCreating(true)}
                  className="flex items-center gap-1 px-3 py-1.5 rounded-md text-xs font-medium"
                  style={{ background: "var(--accent)", color: "#fff" }}>
                  <Plus size={12} /> New Station
                </button>
                <button onClick={load} disabled={loading}
                  className="p-1.5 rounded transition-opacity"
                  style={{ color: "var(--muted)", background: "var(--surface2)", border: "1px solid var(--border)", opacity: loading ? 0.5 : 1 }}>
                  <RefreshCw size={14} className={loading ? "animate-spin" : ""} />
                </button>
              </div>
            </div>

            {/* Table */}
            <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
              <table className="w-full">
                <thead>
                  <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
                    <th className="hidden md:table-cell w-10" />
                    <th className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>Station Name</th>
                    <th className="px-4 py-3 text-left text-xs font-medium hidden sm:table-cell" style={{ color: "var(--muted)" }}>Online</th>
                    <th className="px-4 py-3 text-center text-xs font-medium" style={{ color: "var(--muted)" }}>Player Count</th>
                    <th className="w-12" />
                  </tr>
                </thead>
                <tbody>
                  {!pageItems ? (
                    [...Array(6)].map((_, i) => (
                      <tr key={i} style={{ borderBottom: "1px solid var(--border)", background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)" }}>
                        <td className="hidden md:table-cell px-4 py-3"><div className="w-8 h-8 rounded animate-pulse" style={{ background: "var(--border)" }} /></td>
                        <td className="px-4 py-3"><div className="h-4 w-36 rounded animate-pulse" style={{ background: "var(--border)" }} /></td>
                        <td className="hidden sm:table-cell px-4 py-3"><div className="h-4 w-4 rounded-full animate-pulse" style={{ background: "var(--border)" }} /></td>
                        <td className="px-4 py-3"><div className="h-4 w-8 rounded animate-pulse mx-auto" style={{ background: "var(--border)" }} /></td>
                        <td className="px-4 py-3"><div className="h-4 w-4 rounded animate-pulse mx-auto" style={{ background: "var(--border)" }} /></td>
                      </tr>
                    ))
                  ) : pageItems.length === 0 ? (
                    <tr>
                      <td colSpan={5} className="px-4 py-12 text-center text-sm" style={{ color: "var(--muted)" }}>
                        {onlineOnly ? "No online stations" : "No stations found"}
                      </td>
                    </tr>
                  ) : pageItems.map((s, i) => (
                    <tr key={s.station_id}
                      onClick={() => router.push(`/stations/${s.station_id}`)}
                      className="cursor-pointer"
                      style={{ borderBottom: "1px solid var(--border)", background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)" }}
                      onMouseEnter={e => (e.currentTarget.style.background = "rgba(88,166,255,0.07)")}
                      onMouseLeave={e => (e.currentTarget.style.background = i % 2 === 0 ? "var(--surface)" : "var(--surface2)")}>
                      <td className="hidden md:table-cell px-4 py-3">
                        <div className="w-8 h-8 rounded flex items-center justify-center m-0"
                          style={{ background: "var(--surface2)" }}>
                          <Server size={14} style={{ color: "var(--muted)" }} />
                        </div>
                      </td>
                      <td className="px-4 py-3">
                        <div className="font-semibold text-sm">{s.station_name}</div>
                      </td>
                      <td className="hidden sm:table-cell px-4 py-3">
                        <span className="inline-flex items-center justify-center w-5 h-5 rounded-full text-xs"
                          style={{ background: s.online ? "var(--green)" : "var(--red)", opacity: 0.9 }}>
                          &nbsp;
                        </span>
                      </td>
                      <td className="px-4 py-3 text-center">
                        <div className="font-semibold text-sm">{s.player_count}</div>
                      </td>
                      <td className="px-4 py-3 text-right">
                        {/* stopPropagation: the row itself navigates to the station page. */}
                        <button
                          onClick={e => { e.stopPropagation(); setDelErr(null); setDelTarget(s); }}
                          title="Delete fleet"
                          className="p-1.5 rounded"
                          style={{ color: "var(--red)", background: "transparent", border: "1px solid var(--border)" }}>
                          <Trash2 size={13} />
                        </button>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            {/* Pagination */}
            {filtered && filtered.length > PAGE_SIZE && (
              <div className="flex items-center justify-between mt-3">
                <select className="px-3 py-1.5 rounded-md text-xs outline-none"
                  style={{ background: "var(--surface)", border: "1px solid var(--border)", color: "var(--text)" }}>
                  <option>{PAGE_SIZE} Items</option>
                </select>
                <div className="flex rounded-md overflow-hidden text-xs"
                  style={{ border: "1px solid var(--border)" }}>
                  <button onClick={() => setPage(p => Math.max(0, p - 1))} disabled={page === 0}
                    className="px-3 py-1.5"
                    style={{ background: "var(--surface)", color: page === 0 ? "var(--border)" : "var(--text)", borderRight: "1px solid var(--border)" }}>
                    ←
                  </button>
                  <span className="px-3 py-1.5 pointer-events-none text-xs"
                    style={{ background: "var(--surface2)", color: "var(--muted)" }}>
                    {page * PAGE_SIZE + 1}–{Math.min((page + 1) * PAGE_SIZE, filtered.length)}
                    <span style={{ opacity: 0.5 }}> of {filtered.length}</span>
                  </span>
                  <button onClick={() => setPage(p => Math.min(totalPages - 1, p + 1))} disabled={page >= totalPages - 1}
                    className="px-3 py-1.5"
                    style={{ background: "var(--surface)", color: page >= totalPages - 1 ? "var(--border)" : "var(--text)", borderLeft: "1px solid var(--border)" }}>
                    →
                  </button>
                </div>
              </div>
            )}

            {err && (
              <div className="mt-3 px-4 py-3 rounded-md text-sm"
                style={{ background: "#2d1a1a", color: "var(--red)", border: "1px solid #5c2020" }}>
                {err}
              </div>
            )}
          </div>
        </div>

        {/* Right: logo art (lg only) */}
        <div className="hidden lg:flex lg:w-80 xl:w-96 shrink-0 justify-center items-start pt-4">
          <div className="flex flex-col items-center gap-6">
            <Image src="/logo.webp" alt="Ares logo" width={160} height={160}
              className="rounded-2xl opacity-90" style={{ filter: "drop-shadow(0 0 40px rgba(88,166,255,0.3))" }} />
            <div className="text-center">
              <div className="text-2xl font-black tracking-widest uppercase"
                style={{
                  background: "linear-gradient(to bottom right, var(--accent), #a371f7)",
                  WebkitBackgroundClip: "text",
                  WebkitTextFillColor: "transparent",
                }}>
                Ares
              </div>
              <div className="text-xs tracking-widest uppercase mt-1" style={{ color: "var(--muted)" }}>
                Station Dashboard
              </div>
            </div>
          </div>
        </div>

      </div>
    </div>
  );
}
