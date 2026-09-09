"use client";

import { useEffect, useState } from "react";
import { api, type ServerEvent } from "../../lib/api";
import { fmt } from "../../lib/utils";

export default function EventsPage() {
  const [events, setEvents] = useState<ServerEvent[] | null>(null);
  const [err, setErr]       = useState<string | null>(null);

  useEffect(() => {
    let mounted = true;
    function load() {
      api.events()
        .then(d  => { if (mounted) setEvents(d); })
        .catch(e => { if (mounted) setErr(e.message); });
    }
    load();
    const id = setInterval(load, 5000);
    return () => { mounted = false; clearInterval(id); };
  }, []);

  return (
    <div>
      <div className="flex items-center justify-between mb-5">
        <h1 className="text-lg font-semibold" style={{ color: "var(--text)" }}>Events</h1>
        {events && (
          <span className="text-xs px-2 py-1 rounded-full"
            style={{ background: "var(--surface2)", color: "var(--muted)" }}>
            {events.length} recent
          </span>
        )}
      </div>

      {err && (
        <div className="mb-4 px-4 py-3 rounded-md text-sm"
          style={{ background: "#2d1a1a", color: "var(--red)", border: "1px solid #5c2020" }}>
          {err}
        </div>
      )}

      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["Event ID", "Deployment ID", "Event", "Time"].map(h => (
                <th key={h} className="px-4 py-3 text-left font-medium text-xs"
                  style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!events ? (
              <tr>
                <td colSpan={4} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>Loading…</td>
              </tr>
            ) : events.length === 0 ? (
              <tr>
                <td colSpan={4} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>No events</td>
              </tr>
            ) : events.map((e, i) => (
              <tr key={e.event_id}
                style={{
                  background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)",
                  borderBottom: "1px solid var(--border)",
                }}>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>
                  {e.event_id.slice(0, 8)}…
                </td>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>
                  {e.deployment_id.slice(0, 8)}…
                </td>
                <td className="px-4 py-3">
                  <span className="font-mono text-xs px-2 py-0.5 rounded"
                    style={{ background: "var(--surface2)", color: "var(--accent)" }}>
                    {e.event_name}
                  </span>
                </td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{fmt(e.created_at)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <p className="mt-4 text-xs" style={{ color: "var(--muted)" }}>
        Auto-refreshes every 5 seconds
      </p>
    </div>
  );
}
