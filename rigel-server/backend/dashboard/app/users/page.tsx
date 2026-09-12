"use client";

import { useEffect, useState } from "react";
import { api, type User, type Station } from "../../lib/api";
import { fmt, ago } from "../../lib/utils";
import { X } from "lucide-react";

function Modal({ title, onClose, children }: { title: string; onClose: () => void; children: React.ReactNode }) {
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center"
      style={{ background: "rgba(0,0,0,0.6)" }}
      onClick={e => { if (e.target === e.currentTarget) onClose(); }}>
      <div className="w-full max-w-md rounded-lg p-6"
        style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
        <div className="flex items-center justify-between mb-5">
          <h2 className="font-semibold text-sm">{title}</h2>
          <button onClick={onClose} style={{ color: "var(--muted)" }}><X size={16} /></button>
        </div>
        {children}
      </div>
    </div>
  );
}

const inputCls = "w-full px-3 py-2 rounded-md text-sm outline-none";
const inputStyle = { background: "var(--surface2)", border: "1px solid var(--border)", color: "var(--text)" };

export default function UsersPage() {
  const [users, setUsers]       = useState<User[] | null>(null);
  const [stations, setStations] = useState<Station[]>([]);
  const [err, setErr]           = useState<string | null>(null);

  // Ban modal state
  const [banTarget, setBanTarget]   = useState<User | null>(null);
  const [banStation, setBanStation] = useState("");
  const [banReason, setBanReason]   = useState("");
  const [banDuration, setBanDuration] = useState("0");
  const [saving, setSaving]         = useState(false);
  const [actionErr, setActionErr]   = useState<string | null>(null);

  useEffect(() => {
    Promise.all([api.users(), api.stations()])
      .then(([u, s]) => { setUsers(u); setStations(s); })
      .catch(e => setErr(e.message));
  }, []);

  function openBan(user: User) {
    setBanTarget(user);
    setBanStation(stations[0]?.station_id ?? "");
    setBanReason("");
    setBanDuration("0");
    setActionErr(null);
  }

  async function submitBan() {
    if (!banTarget || !banStation) return;
    setSaving(true);
    setActionErr(null);
    try {
      const res = await api.banUser(banTarget.user_id, banStation, banReason, parseInt(banDuration, 10));
      if (!res.success) { setActionErr(res.error ?? "Ban failed"); return; }
      setBanTarget(null);
      // Refresh users
      api.users().then(setUsers).catch(() => {});
    } catch (e: unknown) {
      setActionErr(e instanceof Error ? e.message : "Unknown error");
    } finally {
      setSaving(false);
    }
  }

  async function unban(user: User) {
    const stationId = prompt("Station ID to unban from:");
    if (!stationId) return;
    await api.unbanUser(user.user_id, stationId);
    api.users().then(setUsers).catch(() => {});
  }

  async function toggleAdmin(user: User) {
    if (user.is_admin) {
      await api.revokeAdmin(user.user_id);
    } else {
      await api.grantAdmin(user.user_id);
    }
    api.users().then(setUsers).catch(() => {});
  }

  return (
    <div>
      <div className="flex items-center justify-between mb-5">
        <h1 className="text-lg font-semibold" style={{ color: "var(--text)" }}>Users</h1>
        {users && (
          <span className="text-xs px-2 py-1 rounded-full"
            style={{ background: "var(--surface2)", color: "var(--muted)" }}>
            {users.length} total
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
              {["User ID", "Username", "Platform", "Roles", "Last Login", "Created", "Status", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left font-medium text-xs"
                  style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!users ? (
              <tr>
                <td colSpan={8} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>Loading…</td>
              </tr>
            ) : users.length === 0 ? (
              <tr>
                <td colSpan={8} className="px-4 py-8 text-center text-sm"
                  style={{ color: "var(--muted)" }}>No users</td>
              </tr>
            ) : users.map((u, i) => (
              <tr key={u.user_id || `row-${i}`}
                style={{
                  background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)",
                  borderBottom: "1px solid var(--border)",
                }}>
                {/* Guarded even though /api/users now filters id-less rows: this table renders whatever
                    the backend sends, and one null here previously threw inside render and blanked the
                    entire tab rather than the single bad row. */}
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>
                  {u.user_id ? `${u.user_id.slice(0, 8)}…` : "—"}
                </td>
                <td className="px-4 py-3 font-medium">
                {u.username || "(unknown)"}
                {u.is_admin && (
                  <span className="ml-2 text-xs px-1.5 py-0.5 rounded-full font-medium"
                    style={{ background: "#2d1f0e", color: "#f0883e" }}>Admin</span>
                )}
              </td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{u.platform ?? "—"}</td>
                <td className="px-4 py-3" style={{ color: "var(--muted)" }}>{u.role_count}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{ago(u.last_login)}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{fmt(u.created_at)}</td>
                <td className="px-4 py-3">
                  {u.banned ? (
                    <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                      style={{ background: "#2d1a1a", color: "var(--red)" }}>Banned</span>
                  ) : (
                    <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                      style={{ background: "#1a2d1a", color: "var(--green)" }}>Active</span>
                  )}
                </td>
                <td className="px-4 py-3">
                  <div className="flex gap-2">
                    {u.banned ? (
                      <button onClick={() => unban(u)}
                        className="text-xs px-2 py-1 rounded"
                        style={{ color: "var(--green)", border: "1px solid var(--border)" }}>
                        Unban
                      </button>
                    ) : (
                      <button onClick={() => openBan(u)}
                        className="text-xs px-2 py-1 rounded"
                        style={{ color: "var(--red)", border: "1px solid var(--border)" }}>
                        Ban
                      </button>
                    )}
                    <button onClick={() => toggleAdmin(u)}
                      className="text-xs px-2 py-1 rounded"
                      style={{ color: u.is_admin ? "#f0883e" : "var(--muted)", border: "1px solid var(--border)" }}>
                      {u.is_admin ? "Revoke Admin" : "Grant Admin"}
                    </button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      {/* Ban modal */}
      {banTarget && (
        <Modal title={`Ban ${banTarget.username}`} onClose={() => setBanTarget(null)}>
          {actionErr && (
            <div className="mb-4 px-3 py-2 rounded text-xs"
              style={{ background: "#2d1a1a", color: "var(--red)" }}>{actionErr}</div>
          )}
          <div className="mb-4">
            <label className="block text-xs mb-1.5" style={{ color: "var(--muted)" }}>Station</label>
            <select className={inputCls} style={inputStyle} value={banStation} onChange={e => setBanStation(e.target.value)}>
              {stations.map(s => <option key={s.station_id} value={s.station_id}>{s.station_name}</option>)}
            </select>
          </div>
          <div className="mb-4">
            <label className="block text-xs mb-1.5" style={{ color: "var(--muted)" }}>Reason</label>
            <input className={inputCls} style={inputStyle} value={banReason}
              onChange={e => setBanReason(e.target.value)} placeholder="Optional reason" />
          </div>
          <div className="mb-5">
            <label className="block text-xs mb-1.5" style={{ color: "var(--muted)" }}>
              Duration (seconds, 0 = permanent)
            </label>
            <input className={inputCls} style={inputStyle} type="number" min="0"
              value={banDuration} onChange={e => setBanDuration(e.target.value)} />
          </div>
          <div className="flex gap-2 justify-end">
            <button onClick={() => setBanTarget(null)}
              className="px-4 py-2 rounded-md text-xs"
              style={{ background: "transparent", color: "var(--muted)", border: "1px solid var(--border)" }}>
              Cancel
            </button>
            <button onClick={submitBan} disabled={saving || !banStation}
              className="px-4 py-2 rounded-md text-xs font-medium"
              style={{ background: "var(--red)", color: "#fff", opacity: saving ? 0.5 : 1 }}>
              {saving ? "Banning…" : "Ban User"}
            </button>
          </div>
        </Modal>
      )}
    </div>
  );
}
