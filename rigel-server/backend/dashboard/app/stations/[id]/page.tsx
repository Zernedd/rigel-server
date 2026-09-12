"use client";

import { useEffect, useState, useCallback } from "react";
import { useParams } from "next/navigation";
import Link from "next/link";
import { api, type Ban, type Role, type Member, type User, type Station, type Deployment, type StationEvent, type EosSession } from "../../../lib/api";
import { fmt, ago } from "../../../lib/utils";
import {
  ChevronLeft, Plus, Trash2, X, Shield,
  Users, Cpu, Wifi, Clock, Rocket, Calendar, Radio,
} from "lucide-react";
import NetvarsTab from "./NetvarsTab";

type Tab = "bans" | "roles" | "members" | "deployments" | "events" | "sessions" | "netvars" | "whitelist";

// ── Shared UI ────────────────────────────────────────────────────────────────

function Modal({ title, onClose, children }: { title: string; onClose: () => void; children: React.ReactNode }) {
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center"
      style={{ background: "rgba(0,0,0,0.65)" }}
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
const inputStyle: React.CSSProperties = { background: "var(--surface2)", border: "1px solid var(--border)", color: "var(--text)" };

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="mb-4">
      <label className="block text-xs mb-1.5" style={{ color: "var(--muted)" }}>{label}</label>
      {children}
    </div>
  );
}

function Btn({ children, onClick, variant = "primary", disabled, small }: {
  children: React.ReactNode; onClick?: () => void;
  variant?: "primary" | "danger" | "ghost"; disabled?: boolean; small?: boolean;
}) {
  const styles: Record<string, React.CSSProperties> = {
    primary: { background: "var(--accent)", color: "#fff" },
    danger:  { background: "var(--red)",    color: "#fff" },
    ghost:   { background: "transparent", color: "var(--muted)", border: "1px solid var(--border)" },
  };
  return (
    <button onClick={onClick} disabled={disabled}
      className={`rounded-md font-medium transition-opacity ${small ? "px-2.5 py-1 text-xs" : "px-4 py-2 text-xs"}`}
      style={{ ...styles[variant], opacity: disabled ? 0.5 : 1 }}>
      {children}
    </button>
  );
}

function ErrBox({ msg }: { msg: string }) {
  return (
    <div className="mb-4 px-4 py-3 rounded-md text-sm"
      style={{ background: "#2d1a1a", color: "var(--red)", border: "1px solid #5c2020" }}>
      {msg}
    </div>
  );
}

// ── Bans tab ─────────────────────────────────────────────────────────────────

function BansTab({ stationId }: { stationId: string }) {
  const [bans, setBans]       = useState<Ban[] | null>(null);
  const [err, setErr]         = useState<string | null>(null);
  const [revoking, setRevoking] = useState<string | null>(null);

  const load = useCallback(() => {
    api.stationBans(stationId).then(setBans).catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  async function revoke(userId: string) {
    setRevoking(userId);
    try { await api.unbanUser(userId, stationId); load(); }
    finally { setRevoking(null); }
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["User", "Reason", "Expires", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!bans ? (
              <tr><td colSpan={4} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : bans.length === 0 ? (
              <tr><td colSpan={4} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>No active bans</td></tr>
            ) : bans.map((b, i) => (
              <tr key={b.user_id + b.reason}
                style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                <td className="px-4 py-3 font-medium">{b.username}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{b.reason || "—"}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--yellow)" }}>
                  {b.expiration ? fmt(b.expiration) : "Permanent"}
                </td>
                <td className="px-4 py-3">
                  <Btn variant="ghost" small disabled={revoking === b.user_id} onClick={() => revoke(b.user_id)}>
                    {revoking === b.user_id ? "…" : "Revoke"}
                  </Btn>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Roles tab ────────────────────────────────────────────────────────────────

function RolesTab({ stationId }: { stationId: string }) {
  const [roles, setRoles]         = useState<Role[] | null>(null);
  const [err, setErr]             = useState<string | null>(null);
  const [creating, setCreating]   = useState(false);
  const [editPerms, setEditPerms] = useState<Role | null>(null);
  const [newName, setNewName]     = useState("");
  const [newDesc, setNewDesc]     = useState("");
  const [newPerms, setNewPerms]   = useState("");
  const [saving, setSaving]       = useState(false);

  const load = useCallback(() => {
    api.stationRoles(stationId).then(setRoles).catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  async function create() {
    if (!newName.trim()) return;
    setSaving(true);
    try {
      await api.createRole(stationId, newName.trim(), newDesc.trim());
      setCreating(false); setNewName(""); setNewDesc("");
      load();
    } finally { setSaving(false); }
  }

  async function remove(roleId: string) {
    await api.deleteRole(stationId, roleId);
    load();
  }

  async function savePerms() {
    if (!editPerms) return;
    setSaving(true);
    try {
      const perms = newPerms.split(",").map(s => s.trim()).filter(Boolean);
      await api.setRolePermissions(stationId, editPerms.role_id, perms);
      setEditPerms(null);
      load();
    } finally { setSaving(false); }
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <div className="flex justify-end mb-3">
        <Btn onClick={() => setCreating(true)}><Plus size={12} className="inline mr-1" />New Role</Btn>
      </div>

      {creating && (
        <Modal title="Create Role" onClose={() => setCreating(false)}>
          <Field label="Name"><input className={inputCls} style={inputStyle} value={newName} onChange={e => setNewName(e.target.value)} placeholder="Moderator" autoFocus /></Field>
          <Field label="Description (optional)"><input className={inputCls} style={inputStyle} value={newDesc} onChange={e => setNewDesc(e.target.value)} placeholder="Can manage bans" /></Field>
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setCreating(false)}>Cancel</Btn>
            <Btn onClick={create} disabled={saving || !newName.trim()}>Create</Btn>
          </div>
        </Modal>
      )}

      {editPerms && (
        <Modal title={`Permissions — ${editPerms.role_name}`} onClose={() => setEditPerms(null)}>
          <Field label="Permissions (comma-separated)">
            <textarea className={inputCls} style={{ ...inputStyle, resize: "vertical", minHeight: 80 }}
              value={newPerms} onChange={e => setNewPerms(e.target.value)}
              placeholder="ban_user, kick_user, manage_events" />
          </Field>
          <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
            Current: {editPerms.permissions.join(", ") || "none"}
          </p>
          <div className="flex gap-2 justify-end">
            <Btn variant="ghost" onClick={() => setEditPerms(null)}>Cancel</Btn>
            <Btn onClick={savePerms} disabled={saving}>Save</Btn>
          </div>
        </Modal>
      )}

      <div className="space-y-2">
        {!roles ? (
          <p className="text-sm py-10 text-center" style={{ color: "var(--muted)" }}>Loading…</p>
        ) : roles.length === 0 ? (
          <p className="text-sm py-10 text-center" style={{ color: "var(--muted)" }}>No roles yet — create one above</p>
        ) : roles.map(role => (
          <div key={role.role_id} className="rounded-lg px-4 py-3 flex items-start justify-between gap-4"
            style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
            <div className="flex-1 min-w-0">
              <div className="flex items-center gap-2">
                <Shield size={13} style={{ color: "var(--accent)" }} />
                <span className="font-medium text-sm">{role.role_name}</span>
                {role.role_description && (
                  <span className="text-xs" style={{ color: "var(--muted)" }}>— {role.role_description}</span>
                )}
              </div>
              <div className="flex flex-wrap gap-1 mt-2">
                {role.permissions.length === 0 ? (
                  <span className="text-xs" style={{ color: "var(--muted)" }}>No permissions</span>
                ) : role.permissions.map(p => (
                  <span key={p} className="text-xs px-2 py-0.5 rounded"
                    style={{ background: "var(--surface2)", color: "var(--muted)" }}>{p}</span>
                ))}
              </div>
            </div>
            <div className="flex gap-2 shrink-0 items-center">
              <Btn variant="ghost" small onClick={() => { setEditPerms(role); setNewPerms(role.permissions.join(", ")); }}>
                Permissions
              </Btn>
              <button onClick={() => remove(role.role_id)}
                className="p-1.5 rounded transition-colors"
                style={{ color: "var(--muted)" }}
                onMouseEnter={e => (e.currentTarget.style.color = "var(--red)")}
                onMouseLeave={e => (e.currentTarget.style.color = "var(--muted)")}>
                <Trash2 size={13} />
              </button>
            </div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ── Members tab ───────────────────────────────────────────────────────────────

function MembersTab({ stationId }: { stationId: string }) {
  const [members, setMembers]     = useState<Member[] | null>(null);
  const [roles, setRoles]         = useState<Role[]>([]);
  const [users, setUsers]         = useState<User[]>([]);
  const [err, setErr]             = useState<string | null>(null);
  const [assigning, setAssigning] = useState(false);
  const [selUser, setSelUser]     = useState("");
  const [selRole, setSelRole]     = useState("");
  const [saving, setSaving]       = useState(false);

  const load = useCallback(() => {
    Promise.all([
      api.stationMembers(stationId),
      api.stationRoles(stationId),
      api.users(),
    ]).then(([m, r, u]) => { setMembers(m); setRoles(r); setUsers(u); })
      .catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  async function assign() {
    if (!selUser || !selRole) return;
    setSaving(true);
    try {
      await api.assignRole(stationId, selUser, selRole);
      setAssigning(false); setSelUser(""); setSelRole("");
      load();
    } finally { setSaving(false); }
  }

  async function removeRole(userId: string, roleId: string) {
    await api.removeRole(stationId, userId, roleId);
    load();
  }

  const roleMap = Object.fromEntries(roles.map(r => [r.role_id, r.role_name]));

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <div className="flex justify-end mb-3">
        <Btn onClick={() => setAssigning(true)}><Plus size={12} className="inline mr-1" />Assign Role</Btn>
      </div>

      {assigning && (
        <Modal title="Assign Role" onClose={() => setAssigning(false)}>
          <Field label="User">
            <select className={inputCls} style={inputStyle} value={selUser} onChange={e => setSelUser(e.target.value)}>
              <option value="">Select user…</option>
              {users.map(u => <option key={u.user_id} value={u.user_id}>{u.username}</option>)}
            </select>
          </Field>
          <Field label="Role">
            <select className={inputCls} style={inputStyle} value={selRole} onChange={e => setSelRole(e.target.value)}>
              <option value="">Select role…</option>
              {roles.map(r => <option key={r.role_id} value={r.role_id}>{r.role_name}</option>)}
            </select>
          </Field>
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setAssigning(false)}>Cancel</Btn>
            <Btn onClick={assign} disabled={saving || !selUser || !selRole}>Assign</Btn>
          </div>
        </Modal>
      )}

      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["User", "Platform", "Roles", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!members ? (
              <tr><td colSpan={4} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : members.length === 0 ? (
              <tr><td colSpan={4} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>No staff assigned</td></tr>
            ) : members.map((m, i) => (
              <tr key={m.user_id}
                style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                <td className="px-4 py-3 font-medium">{m.username}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{m.platform ?? "—"}</td>
                <td className="px-4 py-3">
                  <div className="flex flex-wrap gap-1">
                    {m.roles.map(rId => (
                      <span key={rId} className="inline-flex items-center gap-1 text-xs px-2 py-0.5 rounded"
                        style={{ background: "#1a2a3a", color: "var(--accent)", border: "1px solid #1e3a5a" }}>
                        {roleMap[rId] ?? rId}
                        <button onClick={() => removeRole(m.user_id, rId)}
                          style={{ color: "var(--muted)" }}
                          onMouseEnter={e => (e.currentTarget.style.color = "var(--red)")}
                          onMouseLeave={e => (e.currentTarget.style.color = "var(--muted)")}>
                          <X size={10} />
                        </button>
                      </span>
                    ))}
                  </div>
                </td>
                <td className="px-4 py-3" />
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Deployments tab ───────────────────────────────────────────────────────────

const DEFAULT_EOS_FORM = {
  server_name: "", ip_address: "", port: "", max_players: "10",
  bucket: "A2_4729", build_id: "4729", imgui_port: "",
};

const DEFAULT_BULK_FORM = {
  server_name: "OCE", ip_address: "127.0.0.1", base_port: "7777",
  count: "1", max_players: "10", bucket: "20996", build_id: "20996", imgui_port_base: "25108",
};

function DeploymentsTab({ stationId }: { stationId: string }) {
  const [deps, setDeps]           = useState<Deployment[] | null>(null);
  const [err, setErr]             = useState<string | null>(null);
  // Spin-up (replaces the manual ip/port "Launch Deployment"): ask HalcyonSocket to have an allocator
  // agent launch a server on a box; it self-registers and appears in the list below.
  const [spinning, setSpinning]     = useState(false);
  const [spinForm, setSpinForm]     = useState({ box: "", map: "", region: "", args: "", name: "" });
  const [spinSaving, setSpinSaving] = useState(false);
  const [spinResult, setSpinResult] = useState<string | null>(null);
  const [agents, setAgents]         = useState<{ box: string; capacity: number; running: number; free: number }[]>([]);

  // EOS allocation state
  const [allocTarget, setAllocTarget] = useState<Deployment | null>(null);
  const [eosForm, setEosForm]         = useState(DEFAULT_EOS_FORM);
  const [allocSaving, setAllocSaving] = useState(false);
  const [allocErr, setAllocErr]       = useState<string | null>(null);

  // Bulk EOS session launch state
  const [bulkOpen, setBulkOpen]     = useState(false);
  const [bulkForm, setBulkForm]     = useState(DEFAULT_BULK_FORM);
  const [bulkSaving, setBulkSaving] = useState(false);
  const [bulkErr, setBulkErr]       = useState<string | null>(null);
  const [bulkDone, setBulkDone]     = useState<number | null>(null);

  const load = useCallback(() => {
    api.stationDeployments(stationId).then(setDeps).catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);
  useEffect(() => { api.agents().then(r => setAgents(r.agents)).catch(() => {}); }, []);

  async function spinUp() {
    setSpinSaving(true);
    setSpinResult(null);
    try {
      const res = await api.spinUpServer(stationId, {
        box:    spinForm.box    || undefined,
        map:    spinForm.map    || undefined,
        region: spinForm.region || undefined,
        args:   spinForm.args   || undefined,
        name:   spinForm.name   || undefined,
      });
      if (res.success) {
        setSpinning(false);
        setSpinResult(`Spin-up requested on agent "${res.agent}". The server will appear below once it boots and self-registers.`);
        setTimeout(load, 8000);   // give it time to launch, inject, and register
      } else {
        setSpinResult(`Failed: ${res.error ?? "unknown error"}`);
      }
    } catch (e: unknown) {
      setSpinResult(e instanceof Error ? e.message : "error");
    } finally { setSpinSaving(false); }
  }

  function openAlloc(dep: Deployment) {
    setEosForm({
      server_name: dep.deployment_name ?? "",
      ip_address:  dep.ip_address ?? "",
      port:        "",
      max_players: "10",
      bucket:      "A2_4729",
      build_id:    "4729",
      imgui_port:  "",
    });
    setAllocErr(null);
    setAllocTarget(dep);
  }

  async function bulkLaunch() {
    setBulkSaving(true);
    setBulkErr(null);
    try {
      const results = await api.bulkAddSessions(stationId, parseInt(bulkForm.count, 10) || 1, {
        server_name:    bulkForm.server_name,
        ip_address:     bulkForm.ip_address,
        base_port:      parseInt(bulkForm.base_port, 10) || 7777,
        max_players:    parseInt(bulkForm.max_players, 10) || 10,
        bucket:         bulkForm.bucket,
        build_id:       parseInt(bulkForm.build_id, 10) || 20996,
        imgui_port_base: parseInt(bulkForm.imgui_port_base, 10) || 0,
      });
      const failed = results.filter(r => !r.success);
      if (failed.length > 0) {
        setBulkErr(`${failed.length} session(s) failed to create`);
      } else {
        setBulkDone(results.length);
        setBulkOpen(false);
        setBulkForm(DEFAULT_BULK_FORM);
      }
    } catch (e: unknown) {
      setBulkErr(e instanceof Error ? e.message : "error");
    } finally { setBulkSaving(false); }
  }

  async function allocate() {
    if (!allocTarget) return;
    setAllocSaving(true);
    setAllocErr(null);
    try {
      const res = await api.addSession({
        server_name:   eosForm.server_name,
        ip_address:    eosForm.ip_address,
        port:          eosForm.port,
        max_players:   parseInt(eosForm.max_players, 10) || 10,
        bucket:        eosForm.bucket,
        build_id:      parseInt(eosForm.build_id, 10) || 4729,
        imgui_port:    eosForm.imgui_port,
        station_id:    stationId,
        deployment_id: allocTarget.deployment_id,
      });
      if (res.success) { setAllocTarget(null); }
      else setAllocErr(res.error ?? "Failed to allocate session");
    } catch (e: unknown) {
      setAllocErr(e instanceof Error ? e.message : "error");
    } finally { setAllocSaving(false); }
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <div className="flex justify-end gap-2 mb-3">
        <Btn variant="ghost" onClick={() => { setBulkErr(null); setBulkDone(null); setBulkOpen(true); }}>
          <Radio size={12} className="inline mr-1" />Bulk EOS Sessions
        </Btn>
        <Btn onClick={() => { setSpinResult(null); api.agents().then(r => setAgents(r.agents)).catch(() => {}); setSpinning(true); }}>
          <Rocket size={12} className="inline mr-1" />Spin Up Server
        </Btn>
      </div>

      {bulkDone !== null && (
        <div className="mb-4 px-4 py-3 rounded-md text-sm"
          style={{ background: "#1a2d1a", color: "var(--green)", border: "1px solid #1e5a20" }}>
          {bulkDone} EOS session{bulkDone !== 1 ? "s" : ""} launched successfully.
          <button className="ml-3 text-xs underline" style={{ color: "var(--muted)" }} onClick={() => setBulkDone(null)}>dismiss</button>
        </div>
      )}

      {bulkOpen && (
        <Modal title="Bulk Launch EOS Sessions" onClose={() => setBulkOpen(false)}>
          <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
            Creates N EOS matchmaking sessions with ports starting from the base port.
            Server names will be suffixed _1, _2, …
          </p>
          {bulkErr && <ErrBox msg={bulkErr} />}
          <div className="grid grid-cols-2 gap-3">
            <Field label="Session Count">
              <input type="number" min="1" max="50" className={inputCls} style={inputStyle}
                value={bulkForm.count} onChange={e => setBulkForm(f => ({ ...f, count: e.target.value }))} />
            </Field>
            <Field label="Base Port">
              <input type="number" className={inputCls} style={inputStyle}
                value={bulkForm.base_port} onChange={e => setBulkForm(f => ({ ...f, base_port: e.target.value }))} />
            </Field>
          </div>
          <Field label="Server Name Prefix">
            <input className={inputCls} style={inputStyle}
              value={bulkForm.server_name} onChange={e => setBulkForm(f => ({ ...f, server_name: e.target.value }))}
              placeholder="OCE" />
          </Field>
          <Field label="IP Address">
            <input className={inputCls} style={inputStyle}
              value={bulkForm.ip_address} onChange={e => setBulkForm(f => ({ ...f, ip_address: e.target.value }))}
              placeholder="127.0.0.1" />
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Bucket">
              <input className={inputCls} style={inputStyle}
                value={bulkForm.bucket} onChange={e => setBulkForm(f => ({ ...f, bucket: e.target.value }))} />
            </Field>
            <Field label="Build ID">
              <input type="number" className={inputCls} style={inputStyle}
                value={bulkForm.build_id} onChange={e => setBulkForm(f => ({ ...f, build_id: e.target.value }))} />
            </Field>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Max Players">
              <input type="number" className={inputCls} style={inputStyle}
                value={bulkForm.max_players} onChange={e => setBulkForm(f => ({ ...f, max_players: e.target.value }))} />
            </Field>
            <Field label="IMGUI Base Port">
              <input type="number" className={inputCls} style={inputStyle}
                value={bulkForm.imgui_port_base} onChange={e => setBulkForm(f => ({ ...f, imgui_port_base: e.target.value }))}
                placeholder="25108 (0 to skip)" />
            </Field>
          </div>
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setBulkOpen(false)}>Cancel</Btn>
            <Btn onClick={bulkLaunch} disabled={bulkSaving || !bulkForm.server_name || !bulkForm.ip_address}>
              {bulkSaving ? "Launching…" : "Launch Sessions"}
            </Btn>
          </div>
        </Modal>
      )}

      {spinning && (
        <Modal title="Spin Up Server" onClose={() => setSpinning(false)}>
          <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
            Requests an allocator agent to launch a game server on a box. It self-registers and appears
            below — no manual IP/port needed.
          </p>
          <Field label="Deployment Name">
            <input className={inputCls} style={inputStyle}
              value={spinForm.name} onChange={e => setSpinForm(f => ({ ...f, name: e.target.value }))}
              placeholder="e.g. NA-East 1" />
          </Field>
          <Field label="Box">
            <select className={inputCls} style={inputStyle}
              value={spinForm.box} onChange={e => setSpinForm(f => ({ ...f, box: e.target.value }))}>
              <option value="">Auto (least loaded)</option>
              {agents.map(a => (
                <option key={a.box} value={a.box} disabled={a.free <= 0}>
                  {a.box} — {a.free}/{a.capacity} free
                </option>
              ))}
            </select>
          </Field>
          <Field label="Map / Playlist (optional)">
            <input className={inputCls} style={inputStyle}
              value={spinForm.map} onChange={e => setSpinForm(f => ({ ...f, map: e.target.value }))}
              placeholder="e.g. Station_Prime_P" />
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Region (optional)">
              <input className={inputCls} style={inputStyle}
                value={spinForm.region} onChange={e => setSpinForm(f => ({ ...f, region: e.target.value }))} />
            </Field>
            <Field label="Extra Args (optional)">
              <input className={inputCls} style={inputStyle}
                value={spinForm.args} onChange={e => setSpinForm(f => ({ ...f, args: e.target.value }))} />
            </Field>
          </div>
          {agents.length === 0 && (
            <p className="text-xs mt-2" style={{ color: "var(--red)" }}>No allocator agents connected to HalcyonSocket.</p>
          )}
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setSpinning(false)}>Cancel</Btn>
            <Btn onClick={spinUp} disabled={spinSaving}>{spinSaving ? "Requesting…" : "Spin Up"}</Btn>
          </div>
        </Modal>
      )}

      {spinResult && (
        <div className="mb-4 px-4 py-3 rounded-md text-sm"
          style={{ background: "#1a2d1a", color: "var(--green)", border: "1px solid #1e5a20" }}>
          {spinResult}
          <button className="ml-3 text-xs underline" style={{ color: "var(--muted)" }} onClick={() => setSpinResult(null)}>dismiss</button>
        </div>
      )}

      {allocTarget && (
        <Modal title="Allocate to EOS Matchmaking" onClose={() => setAllocTarget(null)}>
          <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
            This creates an EOS session that players can discover via matchmaking.
          </p>
          {allocErr && <ErrBox msg={allocErr} />}
          <Field label="Server Name">
            <input className={inputCls} style={inputStyle}
              value={eosForm.server_name} onChange={e => setEosForm(f => ({ ...f, server_name: e.target.value }))}
              placeholder="OCE_1" />
          </Field>
          <Field label="IP Address">
            <input className={inputCls} style={inputStyle}
              value={eosForm.ip_address} onChange={e => setEosForm(f => ({ ...f, ip_address: e.target.value }))}
              placeholder="192.168.1.1" />
          </Field>
          <Field label="Port">
            <input className={inputCls} style={inputStyle}
              value={eosForm.port} onChange={e => setEosForm(f => ({ ...f, port: e.target.value }))}
              placeholder="7777" />
          </Field>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Max Players">
              <input type="number" className={inputCls} style={inputStyle}
                value={eosForm.max_players} onChange={e => setEosForm(f => ({ ...f, max_players: e.target.value }))} />
            </Field>
            <Field label="Build ID">
              <input type="number" className={inputCls} style={inputStyle}
                value={eosForm.build_id} onChange={e => setEosForm(f => ({ ...f, build_id: e.target.value }))} />
            </Field>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <Field label="Bucket">
              <input className={inputCls} style={inputStyle}
                value={eosForm.bucket} onChange={e => setEosForm(f => ({ ...f, bucket: e.target.value }))} />
            </Field>
            <Field label="IMGUI Port">
              <input className={inputCls} style={inputStyle}
                value={eosForm.imgui_port} onChange={e => setEosForm(f => ({ ...f, imgui_port: e.target.value }))}
                placeholder="25108" />
            </Field>
          </div>
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setAllocTarget(null)}>Cancel</Btn>
            <Btn onClick={allocate} disabled={allocSaving || !eosForm.ip_address || !eosForm.port}>
              {allocSaving ? "Allocating…" : "Allocate"}
            </Btn>
          </div>
        </Modal>
      )}

      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["Name", "IP", "Region", "Status", "Players", "Created", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!deps ? (
              <tr><td colSpan={7} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : deps.length === 0 ? (
              <tr><td colSpan={7} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>No deployments — launch one above</td></tr>
            ) : deps.map((d, i) => (
              <tr key={d.deployment_id}
                style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                <td className="px-4 py-3 font-medium">{d.deployment_name ?? "—"}</td>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>{d.ip_address ?? "—"}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{d.region ?? "—"}</td>
                <td className="px-4 py-3">
                  {d.online
                    ? <span className="text-xs px-2 py-0.5 rounded-full" style={{ background: "#1a2d1a", color: "var(--green)" }}>Online</span>
                    : <span className="text-xs px-2 py-0.5 rounded-full" style={{ background: "var(--surface2)", color: "var(--muted)" }}>Offline</span>}
                </td>
                <td className="px-4 py-3 text-center" style={{ color: "var(--accent)" }}>{d.player_count}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{fmt(d.created_at)}</td>
                <td className="px-4 py-3">
                  <Btn variant="ghost" small onClick={() => openAlloc(d)}>
                    <Radio size={11} className="inline mr-1" />EOS
                  </Btn>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Events tab ────────────────────────────────────────────────────────────────

function EventsTab({ stationId }: { stationId: string }) {
  const [events, setEvents]   = useState<StationEvent[] | null>(null);
  const [deps, setDeps]       = useState<Deployment[]>([]);
  const [err, setErr]         = useState<string | null>(null);
  const [creating, setCreating] = useState(false);
  const [saving, setSaving]   = useState(false);
  const [form, setForm]       = useState({
    title: "", description: "", start_time: "", duration: "3600",
    deployment_id: "", public: true, signups_open: true,
  });

  const load = useCallback(() => {
    Promise.all([api.stationEvents(stationId), api.stationDeployments(stationId)])
      .then(([ev, d]) => { setEvents(ev); setDeps(d); })
      .catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  async function create() {
    if (!form.title.trim()) return;
    setSaving(true);
    try {
      await api.createEvent(stationId, {
        title:         form.title,
        description:   form.description,
        start_time:    form.start_time || new Date().toISOString(),
        duration:      parseInt(form.duration, 10) || 3600,
        deployment_id: form.deployment_id || undefined,
        public:        form.public,
        signups_open:  form.signups_open,
      });
      setCreating(false);
      setForm({ title: "", description: "", start_time: "", duration: "3600", deployment_id: "", public: true, signups_open: true });
      load();
    } finally { setSaving(false); }
  }

  async function remove(eventId: string) {
    await api.deleteEvent(stationId, eventId);
    load();
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <div className="flex justify-end mb-3">
        <Btn onClick={() => setCreating(true)}><Calendar size={12} className="inline mr-1" />Create Event</Btn>
      </div>

      {creating && (
        <Modal title="Create Event" onClose={() => setCreating(false)}>
          <Field label="Title">
            <input className={inputCls} style={inputStyle} autoFocus
              value={form.title} onChange={e => setForm(f => ({ ...f, title: e.target.value }))} placeholder="Weekly Tournament" />
          </Field>
          <Field label="Description">
            <textarea className={inputCls} style={{ ...inputStyle, resize: "vertical", minHeight: 60 }}
              value={form.description} onChange={e => setForm(f => ({ ...f, description: e.target.value }))} />
          </Field>
          <Field label="Start Time">
            <input type="datetime-local" className={inputCls} style={inputStyle}
              value={form.start_time} onChange={e => setForm(f => ({ ...f, start_time: e.target.value }))} />
          </Field>
          <Field label="Duration (seconds)">
            <input type="number" className={inputCls} style={inputStyle}
              value={form.duration} onChange={e => setForm(f => ({ ...f, duration: e.target.value }))} />
          </Field>
          {deps.length > 0 && (
            <Field label="Deployment (optional)">
              <select className={inputCls} style={inputStyle}
                value={form.deployment_id} onChange={e => setForm(f => ({ ...f, deployment_id: e.target.value }))}>
                <option value="">None</option>
                {deps.map(d => <option key={d.deployment_id} value={d.deployment_id}>{d.deployment_name ?? d.deployment_id.slice(0,8)}</option>)}
              </select>
            </Field>
          )}
          <div className="flex gap-4 mb-4 text-sm">
            <label className="flex items-center gap-2 cursor-pointer" style={{ color: "var(--muted)" }}>
              <input type="checkbox" checked={form.public} onChange={e => setForm(f => ({ ...f, public: e.target.checked }))} />
              Public
            </label>
            <label className="flex items-center gap-2 cursor-pointer" style={{ color: "var(--muted)" }}>
              <input type="checkbox" checked={form.signups_open} onChange={e => setForm(f => ({ ...f, signups_open: e.target.checked }))} />
              Signups open
            </label>
          </div>
          <div className="flex gap-2 justify-end">
            <Btn variant="ghost" onClick={() => setCreating(false)}>Cancel</Btn>
            <Btn onClick={create} disabled={saving || !form.title.trim()}>Create</Btn>
          </div>
        </Modal>
      )}

      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["Title", "Start", "Duration", "Public", "Signups", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!events ? (
              <tr><td colSpan={6} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : events.length === 0 ? (
              <tr><td colSpan={6} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>No events yet</td></tr>
            ) : events.map((e, i) => (
              <tr key={e.event_id}
                style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                <td className="px-4 py-3 font-medium">{e.title}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{fmt(e.start_time)}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>
                  {e.duration >= 3600 ? `${e.duration / 3600}h` : `${e.duration / 60}m`}
                </td>
                <td className="px-4 py-3">
                  {e.public
                    ? <span style={{ color: "var(--green)" }} className="text-xs">Yes</span>
                    : <span style={{ color: "var(--muted)" }} className="text-xs">No</span>}
                </td>
                <td className="px-4 py-3">
                  {e.signups_open
                    ? <span style={{ color: "var(--green)" }} className="text-xs">Open</span>
                    : <span style={{ color: "var(--muted)" }} className="text-xs">Closed</span>}
                </td>
                <td className="px-4 py-3">
                  <button onClick={() => remove(e.event_id)}
                    className="p-1.5 rounded"
                    style={{ color: "var(--muted)" }}
                    onMouseEnter={ev => (ev.currentTarget.style.color = "var(--red)")}
                    onMouseLeave={ev => (ev.currentTarget.style.color = "var(--muted)")}>
                    <Trash2 size={13} />
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Sessions tab ──────────────────────────────────────────────────────────────

function SessionsTab({ stationId }: { stationId: string }) {
  const [sessions, setSessions] = useState<EosSession[] | null>(null);
  const [err, setErr]           = useState<string | null>(null);
  const [removing, setRemoving] = useState<string | null>(null);

  const load = useCallback(() => {
    api.sessions()
      .then(all => setSessions(all.filter(s => s.station_id === stationId)))
      .catch(e => setErr(e.message));
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  async function remove(id: string) {
    setRemoving(id);
    try { await api.removeSession(id); load(); }
    finally { setRemoving(null); }
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}
      <p className="text-xs mb-3" style={{ color: "var(--muted)" }}>
        Active EOS matchmaking sessions for this station. Allocate new sessions from the Deployments tab.
      </p>
      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["Session ID", "Server Name", "IP", "Port", "Max Players", "Bucket", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {!sessions ? (
              <tr><td colSpan={7} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : sessions.length === 0 ? (
              <tr><td colSpan={7} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>
                No active EOS sessions — allocate one from the Deployments tab
              </td></tr>
            ) : sessions.map((s, i) => (
              <tr key={s.id}
                style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>{s.id}</td>
                <td className="px-4 py-3 font-medium">{s.server_name || "—"}</td>
                <td className="px-4 py-3 font-mono text-xs" style={{ color: "var(--muted)" }}>{s.ip_address || "—"}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{s.port || "—"}</td>
                <td className="px-4 py-3 text-center" style={{ color: "var(--accent)" }}>{s.max_players}</td>
                <td className="px-4 py-3 text-xs" style={{ color: "var(--muted)" }}>{s.bucket}</td>
                <td className="px-4 py-3">
                  <button onClick={() => remove(s.id)} disabled={removing === s.id}
                    className="p-1.5 rounded"
                    style={{ color: "var(--muted)", opacity: removing === s.id ? 0.4 : 1 }}
                    onMouseEnter={e => (e.currentTarget.style.color = "var(--red)")}
                    onMouseLeave={e => (e.currentTarget.style.color = "var(--muted)")}>
                    <Trash2 size={13} />
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Whitelist tab ─────────────────────────────────────────────────────────────
// Stored as station config "acl.whitelist" (comma-separated usernames). Empty = visible to everyone,
// so a station only becomes private once a name is actually added. The EOS gateway hides a whitelisted
// station's sessions from anyone not on the list.

const WHITELIST_KEY = "acl.whitelist";

function WhitelistTab({ stationId }: { stationId: string }) {
  const [names, setNames] = useState<string[] | null>(null);
  const [users, setUsers] = useState<User[]>([]);
  const [err, setErr]     = useState<string | null>(null);
  const [adding, setAdding] = useState(false);
  const [selUser, setSelUser] = useState("");
  const [manual, setManual]   = useState("");
  const [saving, setSaving]   = useState(false);

  const load = useCallback(() => {
    api.stationConfig(stationId)
      .then(c => { setNames(parseList(c[WHITELIST_KEY] ?? "")); setErr(null); })
      .catch(e => setErr(e.message));
    api.users().then(setUsers).catch(() => {});
  }, [stationId]);

  useEffect(() => { load(); }, [load]);

  function parseList(v: string): string[] {
    return v.split(/[,;\n]/).map(s => s.trim()).filter(Boolean);
  }

  async function persist(next: string[]) {
    setSaving(true);
    setErr(null);
    try {
      if (next.length === 0) await api.deleteStationConfigKey(stationId, WHITELIST_KEY);
      else await api.patchStationConfig(stationId, { [WHITELIST_KEY]: next.join(", ") });
      setNames(next);
      setAdding(false); setSelUser(""); setManual("");
    } catch (e: unknown) {
      setErr(e instanceof Error ? e.message : "Save failed");
    } finally { setSaving(false); }
  }

  function add() {
    const who = (selUser || manual).trim();
    if (!who || !names) return;
    if (names.some(n => n.toLowerCase() === who.toLowerCase())) { setAdding(false); return; }
    persist([...names, who]);
  }

  function remove(name: string) {
    if (!names) return;
    persist(names.filter(n => n !== name));
  }

  return (
    <div>
      {err && <ErrBox msg={err} />}

      <div className="flex items-center justify-between mb-3">
        <div className="text-xs" style={{ color: "var(--muted)" }}>
          {names === null ? "Loading…"
            : names.length === 0
              ? "No whitelist — this station is visible to everyone."
              : `Only these ${names.length} account(s) can see this station in the browser.`}
        </div>
        <Btn onClick={() => setAdding(true)}><Plus size={12} className="inline mr-1" />Add Account</Btn>
      </div>

      {adding && (
        <Modal title="Add to whitelist" onClose={() => setAdding(false)}>
          <Field label="Account">
            <select className={inputCls} style={inputStyle} value={selUser}
              onChange={e => { setSelUser(e.target.value); if (e.target.value) setManual(""); }}>
              <option value="">Select account…</option>
              {users.map(u => <option key={u.user_id} value={u.username}>{u.username}</option>)}
            </select>
          </Field>
          <Field label="…or type a username">
            <input className={inputCls} style={inputStyle} value={manual}
              onChange={e => { setManual(e.target.value); if (e.target.value) setSelUser(""); }}
              placeholder="Exact in-game username" />
          </Field>
          <div className="flex gap-2 justify-end mt-2">
            <Btn variant="ghost" onClick={() => setAdding(false)}>Cancel</Btn>
            <Btn onClick={add} disabled={saving || !(selUser || manual.trim())}>Add</Btn>
          </div>
        </Modal>
      )}

      <div className="rounded-lg overflow-hidden" style={{ border: "1px solid var(--border)" }}>
        <table className="w-full text-sm">
          <thead>
            <tr style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>
              {["Username", "Known Account", ""].map(h => (
                <th key={h} className="px-4 py-3 text-left text-xs font-medium" style={{ color: "var(--muted)" }}>{h}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {names === null ? (
              <tr><td colSpan={3} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>Loading…</td></tr>
            ) : names.length === 0 ? (
              <tr><td colSpan={3} className="px-4 py-10 text-center text-sm" style={{ color: "var(--muted)" }}>
                Whitelist empty — station is public
              </td></tr>
            ) : names.map((n, i) => {
              const known = users.some(u => u.username?.toLowerCase() === n.toLowerCase());
              return (
                <tr key={n}
                  style={{ background: i % 2 === 0 ? "var(--surface)" : "var(--surface2)", borderBottom: "1px solid var(--border)" }}>
                  <td className="px-4 py-3 font-medium">{n}</td>
                  <td className="px-4 py-3 text-xs" style={{ color: known ? "var(--green)" : "var(--muted)" }}>
                    {known ? "yes" : "not seen yet"}
                  </td>
                  <td className="px-4 py-3 text-right">
                    <button onClick={() => remove(n)} disabled={saving}
                      className="text-xs px-2 py-1 rounded"
                      style={{ color: "var(--red)", border: "1px solid var(--border)" }}>
                      Remove
                    </button>
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// ── Page ─────────────────────────────────────────────────────────────────────

const TABS: { id: Tab; label: string }[] = [
  { id: "bans",        label: "Bans"        },
  { id: "roles",       label: "Roles"       },
  { id: "members",     label: "Members"     },
  { id: "deployments", label: "Deployments" },
  { id: "events",      label: "Events"      },
  { id: "sessions",    label: "EOS Sessions" },
  { id: "netvars",     label: "Netvars"     },
  { id: "whitelist",   label: "Whitelist"   },
];

export default function StationDetailPage() {
  const params    = useParams<{ id: string }>();
  const stationId = params.id;
  const [tab, setTab]           = useState<Tab>("bans");
  const [station, setStation]   = useState<Station | null>(null);

  useEffect(() => {
    api.stations().then(list => {
      const found = list.find(s => s.station_id === stationId);
      if (found) setStation(found);
    });
  }, [stationId]);

  return (
    <div>
      {/* Breadcrumb */}
      <div className="flex items-center gap-2 mb-5 text-xs" style={{ color: "var(--muted)" }}>
        <Link href="/stations" className="hover:underline" style={{ color: "var(--muted)" }}>
          <ChevronLeft size={13} className="inline" /> Stations
        </Link>
        <span>/</span>
        <span style={{ color: "var(--text)" }}>{station?.station_name ?? stationId.slice(0, 12) + "…"}</span>
      </div>

      {/* Station header */}
      <div className="rounded-lg p-5 mb-5"
        style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
        <div className="flex items-start justify-between gap-4">
          <div>
            <div className="flex items-center gap-2 mb-1">
              <h1 className="text-lg font-semibold">
                {station?.station_name ?? <span style={{ color: "var(--muted)" }}>Loading…</span>}
              </h1>
              {station && (
                station.online ? (
                  <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                    style={{ background: "#1a2d1a", color: "var(--green)" }}>Online</span>
                ) : (
                  <span className="text-xs px-2 py-0.5 rounded-full font-medium"
                    style={{ background: "var(--surface2)", color: "var(--muted)" }}>Offline</span>
                )
              )}
            </div>
            <div className="font-mono text-xs" style={{ color: "var(--muted)" }}>{stationId}</div>
          </div>
        </div>

        {station && (
          <div className="flex flex-wrap gap-5 mt-4 pt-4" style={{ borderTop: "1px solid var(--border)" }}>
            <Stat icon={<Cpu size={13} />} label="Deployments" value={station.deployments} />
            <Stat icon={<Wifi size={13} />} label="Online" value={station.online_deps} accent="var(--green)" />
            <Stat icon={<Users size={13} />} label="Players" value={station.player_count} accent="var(--accent)" />
            <Stat icon={<Clock size={13} />} label="Last Seen" value={ago(station.last_online)} />
          </div>
        )}
      </div>

      {/* Tabs */}
      <div className="flex gap-1 mb-5 p-1 rounded-lg w-fit"
        style={{ background: "var(--surface)", border: "1px solid var(--border)" }}>
        {TABS.map(t => (
          <button key={t.id} onClick={() => setTab(t.id)}
            className="px-4 py-1.5 rounded-md text-sm transition-colors"
            style={{
              background: tab === t.id ? "var(--surface2)" : "transparent",
              color: tab === t.id ? "var(--text)" : "var(--muted)",
              fontWeight: tab === t.id ? 500 : 400,
            }}>
            {t.label}
          </button>
        ))}
      </div>

      {tab === "bans"        && <BansTab        stationId={stationId} />}
      {tab === "roles"       && <RolesTab       stationId={stationId} />}
      {tab === "members"     && <MembersTab     stationId={stationId} />}
      {tab === "deployments" && <DeploymentsTab stationId={stationId} />}
      {tab === "events"      && <EventsTab      stationId={stationId} />}
      {tab === "sessions"    && <SessionsTab    stationId={stationId} />}
      {tab === "netvars"     && <NetvarsTab     stationId={stationId} />}
      {tab === "whitelist"   && <WhitelistTab   stationId={stationId} />}
    </div>
  );
}

function Stat({ icon, label, value, accent }: { icon: React.ReactNode; label: string; value: string | number; accent?: string }) {
  return (
    <div className="flex items-center gap-2">
      <span style={{ color: accent ?? "var(--muted)" }}>{icon}</span>
      <div>
        <div className="text-xs" style={{ color: "var(--muted)" }}>{label}</div>
        <div className="text-sm font-medium" style={{ color: accent ?? "var(--text)" }}>{value}</div>
      </div>
    </div>
  );
}
