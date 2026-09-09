"use client";

import { useEffect, useState } from "react";
import { api, type Deployment } from "../../lib/api";
import { fmt, ago } from "../../lib/utils";

export default function DeploymentsPage() {
  const [deps, setDeps] = useState<Deployment[] | null>(null);
  const [err, setErr]   = useState<string | null>(null);

  useEffect(() => {
    let mounted = true;
    api.deployments()
      .then(d  => { if (mounted) setDeps(d); })
      .catch(e => { if (mounted) setErr(e.message); });
  }, []);

  return (
    <div>
      <div className="flex items-center justify-between mb-5">
        <h1 className="text-lg font-semibold" style={{ color: "var(--text)" }}>Deployments</h1>
        {deps && (
          <span className="text-xs px-2 py-1 rounded-full"
            style={{ background: "var(--surface2)", color: "var(--muted)" }}>
            {deps.length} total
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
              {["Name", "Deployment ID", "IP", "Region", "Version", "Status", "Players", "Last Event", "Created"].map(h => (
                <th key={h} className="px-4 py-3 text-left font-medium text-xs"
                  style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!deps ? (
              <tr>
                <td colSpan={9} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>Loading…</td>
              </tr>
            ) : deps.length === 0 ? (
              <tr>
                <td colSpan={9} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>No deployments</td>
              </tr>
            ) : deps.map((d, i) => (
              <tr key={d.deployment_id}
                style={{
                  background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)",
                  borderBottom: "1px solid var(--border)",
                }}>
                <td className="px-4 py-3 font-medium">{d.deployment_name ?? "—"}</td>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>
                  {d.deployment_id.slice(0, 8)}…
                </td>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>
                  {d.ip_address ?? "—"}
                </td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{d.region ?? "—"}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{d.version ?? "—"}</td>
                <td className="px-4 py-3">
                  {d.online ? (
                    <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                      style={{ background: "#1a2d1a", color: "var(--green)" }}>Online</span>
                  ) : (
                    <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                      style={{ background: "var(--surface2)", color: "var(--muted)" }}>Offline</span>
                  )}
                </td>
                <td className="px-4 py-3 text-center" style={{ color: "var(--accent)" }}>{d.player_count}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{ago(d.last_event)}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{fmt(d.created_at)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
