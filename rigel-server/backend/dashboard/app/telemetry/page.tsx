"use client";

import { useEffect, useState, useCallback } from "react";
import { api, type TelemetryEvent } from "../../lib/api";

function ago(ts: string) {
  const diff = Date.now() - new Date(ts).getTime();
  if (diff < 1000) return "just now";
  if (diff < 60000) return `${Math.floor(diff / 1000)}s ago`;
  if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
  return new Date(ts).toLocaleTimeString();
}

export default function TelemetryPage() {
  const [events, setEvents]   = useState<TelemetryEvent[] | null>(null);
  const [err, setErr]         = useState<string | null>(null);
  const [selected, setSelected] = useState<TelemetryEvent | null>(null);
  const [filter, setFilter]   = useState("");
  const [live, setLive]       = useState(true);

  const load = useCallback(() => {
    api.telemetry(500)
      .then(setEvents)
      .catch(e => setErr(e.message));
  }, []);

  useEffect(() => {
    load();
    if (!live) return;
    const t = setInterval(load, 2000);
    return () => clearInterval(t);
  }, [load, live]);

  const filtered = events?.filter(e =>
    !filter || e.eventName.toLowerCase().includes(filter.toLowerCase()) || (e.payload ?? "").toLowerCase().includes(filter.toLowerCase())
  ) ?? [];

  let prettyPayload = "";
  if (selected) {
    try { prettyPayload = JSON.stringify(JSON.parse(selected.payload ?? ""), null, 2); }
    catch { prettyPayload = selected.payload ?? ""; }
  }

  return (
    <div className="flex gap-4 h-full" style={{ minHeight: 0 }}>
      {/* Left — event list */}
      <div className="flex flex-col flex-1 min-w-0">
        <div className="flex items-center justify-between mb-4 gap-3">
          <h1 className="text-lg font-semibold shrink-0">Telemetry</h1>
          <input
            className="flex-1 px-3 py-1.5 rounded-md text-sm outline-none"
            style={{ background: "var(--surface2)", border: "1px solid var(--border)", color: "var(--text)" }}
            placeholder="Filter by event name or payload…"
            value={filter}
            onChange={e => setFilter(e.target.value)}
          />
          <button
            className="px-3 py-1.5 rounded-md text-xs font-medium shrink-0"
            style={{ background: live ? "var(--accent)" : "var(--surface2)", color: live ? "#fff" : "var(--muted)", border: "1px solid var(--border)" }}
            onClick={() => setLive(l => !l)}>
            {live ? "⏸ Live" : "▶ Paused"}
          </button>
          <button
            className="px-3 py-1.5 rounded-md text-xs shrink-0"
            style={{ background: "var(--surface2)", color: "var(--muted)", border: "1px solid var(--border)" }}
            onClick={load}>Refresh</button>
        </div>

        {err && (
          <div className="mb-3 px-4 py-2 rounded-md text-sm" style={{ background: "#2d1a1a", color: "var(--red)" }}>{err}</div>
        )}

        <div className="rounded-lg overflow-hidden flex-1 overflow-y-auto" style={{ border: "1px solid var(--border)" }}>
          <table className="w-full text-sm">
            <thead className="sticky top-0" style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              <tr>
                {["Time", "Event", "Preview"].map(h => (
                  <th key={h} className="px-4 py-2 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {!events ? (
                <tr><td colSpan={3} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
              ) : filtered.length === 0 ? (
                <tr><td colSpan={3} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>No events</td></tr>
              ) : filtered.map((ev, i) => (
                <tr key={ev.id}
                  onClick={() => setSelected(ev)}
                  className="cursor-pointer"
                  style={{
                    background: selected?.id === ev.id ? "var(--accent)22" : i % 2 === 0 ? "var(--surface)" : "var(--surface2)",
                    borderBottom: "1px solid var(--border)",
                    outline: selected?.id === ev.id ? "1px solid var(--accent)" : undefined,
                  }}>
                  <td className="px-4 py-2 text-xs whitespace-nowrap" style={{ color: "var(--muted)" }}>{ago(ev.timestamp)}</td>
                  <td className="px-4 py-2 font-mono text-xs" style={{ color: "var(--accent)" }}>{ev.eventName}</td>
                  <td className="px-4 py-2 text-xs truncate max-w-xs" style={{ color: "var(--muted)" }}>{(ev.payload ?? "").slice(0, 120)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <div className="mt-2 text-xs" style={{ color: "var(--muted)" }}>
          {filtered.length} event{filtered.length !== 1 ? "s" : ""}{filter ? " (filtered)" : ""}
        </div>
      </div>

      {/* Right — payload inspector */}
      {selected && (
        <div className="w-96 shrink-0 flex flex-col rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
          <div className="px-4 py-3 flex items-center justify-between" style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
            <span className="font-mono text-xs font-semibold" style={{ color: "var(--accent)" }}>{selected.eventName}</span>
            <button onClick={() => setSelected(null)} className="text-xs" style={{ color: "var(--muted)" }}>✕</button>
          </div>
          <div className="px-4 py-2 text-xs" style={{ color: "var(--muted)", borderBottom: "1px solid var(--border)" }}>
            {new Date(selected.timestamp).toLocaleString()}
          </div>
          <pre className="flex-1 overflow-auto p-4 text-xs" style={{ background: "var(--surface2)", color: "var(--text)", fontFamily: "monospace" }}>
            {prettyPayload}
          </pre>
        </div>
      )}
    </div>
  );
}
