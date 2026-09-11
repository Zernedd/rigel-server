"use client";

import { useCallback, useEffect, useMemo, useState } from "react";
import { api, type NetvarDumpInfo } from "../../../lib/api";
import { ago } from "../../../lib/utils";
import { Plus, Trash2, Save, RefreshCw, Search, X } from "lucide-react";

// Netvars for one station.
//
//  1. Catalog + overrides -- every configurable netvar in this build (world config, each gamemode's
//     module config), each with an override field. Overrides are stored in the station config under the
//     reserved "nv." prefix and pushed to the running server by the injected DLL, which polls for changes
//     every ~10s and applies only what changed.
//  2. Station config -- the flat key/value map the game pulls natively on its deployment fetch and
//     applies into config/stationConfig (boards, signs, announcement).
//  3. Live netvars -- the full tree the server reports about itself, when it reports it.

const NV = "nv.";

// Gamemode slots the server reported as loaded (SlotID of each UModuleState, 2026-09-10 boot).
const LIVE_SLOTS = [
  "*",
  "TKB_PlazaFront", "TKB_PlazaEast", "TKB_PlazaWest", "TKB_ComplexWest", "TKB_ComplexEast", "TKB_Basement", "TKB_Prime",
  "TKB_Quest_Full", "ScraprunPrime", "ClubGolf", "ClubGolfBack9", "TKBGolf", "PKR_Rockwall_1",
  "CLB_Quests_72582085", "Default_1157779114",
];
const inputCls = "w-full px-2.5 py-1.5 rounded-md text-sm outline-none font-mono";
const inputStyle: React.CSSProperties = { background: "var(--surface2)", border: "1px solid var(--border)", color: "var(--text)" };
const card: React.CSSProperties = { background: "var(--surface)", border: "1px solid var(--border)" };

function IconBtn({ title, onClick, children, danger, disabled }: {
  title: string; onClick: () => void; children: React.ReactNode; danger?: boolean; disabled?: boolean;
}) {
  return (
    <button title={title} onClick={onClick} disabled={disabled}
      className="p-1.5 rounded-md transition-opacity"
      style={{ color: danger ? "var(--red)" : "var(--muted)", border: "1px solid var(--border)", opacity: disabled ? 0.4 : 1 }}>
      {children}
    </button>
  );
}

function SearchBox({ value, onChange, placeholder }: { value: string; onChange: (v: string) => void; placeholder: string }) {
  return (
    <div className="relative">
      <Search size={13} className="absolute left-2.5 top-2.5" style={{ color: "var(--muted)" }} />
      <input className={inputCls} style={{ ...inputStyle, paddingLeft: 28 }} placeholder={placeholder}
        value={value} onChange={e => onChange(e.target.value)} />
    </div>
  );
}

function show(v: unknown) { return JSON.stringify(v); }

// ── shared station-config state (catalog overrides + raw editor use the same map) ──

function useStationConfig(stationId: string) {
  const [cfg, setCfg] = useState<Record<string, string>>({});
  const [err, setErr] = useState("");
  const [busy, setBusy] = useState(false);

  const load = useCallback(() => {
    api.stationConfig(stationId).then(c => { setCfg(c); setErr(""); }).catch(e => setErr(e.message));
  }, [stationId]);
  useEffect(() => { load(); }, [load]);

  const save = useCallback(async (kv: Record<string, string>) => {
    setBusy(true);
    try { setCfg(await api.patchStationConfig(stationId, kv)); setErr(""); }
    catch (e) { setErr((e as Error).message); }
    finally { setBusy(false); }
  }, [stationId]);

  const remove = useCallback(async (key: string) => {
    setBusy(true);
    try { await api.deleteStationConfigKey(stationId, key); setCfg(c => { const n = { ...c }; delete n[key]; return n; }); setErr(""); }
    catch (e) { setErr((e as Error).message); }
    finally { setBusy(false); }
  }, [stationId]);

  return { cfg, err, busy, load, save, remove };
}
type StationCfg = ReturnType<typeof useStationConfig>;

// ── 1. Catalog + overrides ───────────────────────────────────────────────────

type CatalogWorld  = { path: string; type: string; default: unknown; min?: number | null; max?: number | null };
type CatalogModule = { level: string; slot_key: string | null; vars: { name: string; type: string; default: unknown }[] };
type Catalog = {
  world: CatalogWorld[];
  presets: Record<string, { path: string; value: unknown }[]>;
  modules: CatalogModule[];
  station_keys: string[];
};

function OverrideCell({ sc, cfgKey, type, def }: { sc: StationCfg; cfgKey: string; type: string; def: unknown }) {
  const current = sc.cfg[cfgKey];
  const [draft, setDraft] = useState(current ?? "");
  useEffect(() => { setDraft(current ?? ""); }, [current]);
  const dirty = draft !== (current ?? "");

  if (type === "json") return <span className="text-xs" style={{ color: "var(--muted)" }}>object — edit via raw key</span>;
  return (
    <div className="flex items-center gap-1.5">
      {type === "bool" ? (
        <select className="px-2 py-1 rounded-md text-xs outline-none font-mono" style={inputStyle}
          value={draft} onChange={e => setDraft(e.target.value)}>
          <option value="">default ({show(def)})</option>
          <option value="true">true</option>
          <option value="false">false</option>
        </select>
      ) : (
        <input className="px-2 py-1 rounded-md text-xs outline-none font-mono" style={{ ...inputStyle, width: 160 }}
          placeholder={`default ${typeof def === "string" ? def || "(empty)" : show(def)}`} value={draft} onChange={e => setDraft(e.target.value)} />
      )}
      {dirty && draft !== "" && (
        <IconBtn title="Save override" disabled={sc.busy}
          onClick={() => sc.save({ [cfgKey]: draft.trim().replace(/^"(.*)"$/, "$1") })}><Save size={12} /></IconBtn>
      )}
      {current !== undefined && (
        <IconBtn title="Clear override (back to default)" danger disabled={sc.busy} onClick={() => sc.remove(cfgKey)}><X size={12} /></IconBtn>
      )}
    </div>
  );
}

function CatalogView({ sc }: { sc: StationCfg }) {
  const [cat, setCat] = useState<Catalog | null>(null);
  const [q, setQ]     = useState("");
  const [err, setErr] = useState("");
  const [slots, setSlots] = useState<Record<string, string>>({});   // level -> slot the overrides target

  useEffect(() => {
    (async () => {
      try {
        const r = await fetch("/netvar-catalog.json", { cache: "no-store" });
        if (!r.ok) throw new Error(`catalog ${r.status}`);
        const base: Catalog = await r.json();
        setCat(base);
        // The static catalog can lag the game: add every world value the running server actually reports
        // (config/...) that it does not list yet, so anything visible in the live report can be overridden.
        const dumps = await api.netvarDumps();
        const d = dumps.find(x => x.event_type === "netvars");
        if (!d) return;
        const live = await api.netvarDump(d.deployment_id, "netvars") as { config?: unknown };
        const known = new Set(base.world.map(w => w.path));
        const extra: CatalogWorld[] = [];
        const walk = (node: unknown, path: string) => {
          if (node !== null && typeof node === "object" && !Array.isArray(node)) {
            for (const [k, v] of Object.entries(node as object)) walk(v, `${path}/${k}`);
          } else if (!known.has(path) && (typeof node === "boolean" || typeof node === "number" || typeof node === "string")) {
            extra.push({ path, type: typeof node === "boolean" ? "bool" : typeof node, default: node });
          }
        };
        if (live?.config) walk(live.config, "config");
        if (extra.length) setCat({ ...base, world: [...base.world, ...extra] });
      } catch (e) { setErr((e as Error).message); }
    })();
  }, []);

  const needle = q.trim().toLowerCase();
  const world = useMemo(() => (cat?.world ?? []).filter(w => !needle || w.path.toLowerCase().includes(needle)), [cat, needle]);
  const modules = useMemo(() => (cat?.modules ?? []).map(m => ({
    ...m, vars: m.vars.filter(v => !needle || v.name.toLowerCase().includes(needle) || m.level.toLowerCase().includes(needle)),
  })).filter(m => m.vars.length), [cat, needle]);
  const groups = useMemo(() => {
    const g: Record<string, CatalogWorld[]> = {};
    for (const w of world) { const sec = w.path.split("/")[1] ?? ""; (g[sec] ??= []).push(w); }
    return g;
  }, [world]);

  const overrideCount = Object.keys(sc.cfg).filter(k => k.startsWith(NV)).length;

  return (
    <div className="rounded-lg p-5 mb-5" style={card}>
      <div className="flex items-center justify-between mb-1">
        <h2 className="font-semibold text-sm">Netvars — every configurable variable in this build</h2>
        <IconBtn title="Reload overrides" onClick={sc.load}><RefreshCw size={13} /></IconBtn>
      </div>
      <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
        {cat ? `${cat.world.length} world netvars · ${cat.modules.reduce((n, m) => n + m.vars.length, 0)} gamemode variables across ${cat.modules.length} gamemodes · ` : "Loading… · "}
        {overrideCount} override(s) set. Saving pushes the change to the running server immediately (no polling); it is written into the gamemode&apos;s real config, so the whitelist, team names and scripts all see it. Clearing restores the original.
      </p>
      {(err || sc.err) && <div className="mb-3 text-xs" style={{ color: "var(--red)" }}>{err || sc.err}</div>}
      <div className="mb-4"><SearchBox value={q} onChange={setQ} placeholder="Search (e.g. brake, MatchLength, jakeball)" /></div>
      <datalist id="nv-live-slots">
        {LIVE_SLOTS.map(s => <option key={s} value={s} />)}
      </datalist>

      <div style={{ maxHeight: 720, overflowY: "auto" }}>
        {modules.map(m => {
          const slot = slots[m.level] ?? m.slot_key ?? "*";
          return (
            <details key={m.level} open={!!needle} className="mb-2">
              <summary className="cursor-pointer text-xs font-medium py-1">
                gamemode · {m.level} <span style={{ color: "var(--muted)" }}>({m.vars.length})</span>
              </summary>
              <div className="flex items-center gap-2 my-2 text-xs" style={{ color: "var(--muted)" }}>
                target slot
                <input className="px-2 py-1 rounded-md text-xs outline-none font-mono" style={{ ...inputStyle, width: 190 }}
                  list="nv-live-slots"
                  value={slot} onChange={e => setSlots(s => ({ ...s, [m.level]: e.target.value.trim() || "*" }))} />
                <span>
                  (pick a live slot — jakeball runs in TKB_PlazaFront/East/West, TKB_ComplexWest/East, TKB_Basement;{" "}
                  <span className="font-mono">*</span> = any gamemode that reads a variable with this name)
                </span>
              </div>
              <table className="w-full text-xs font-mono">
                <tbody>
                  {m.vars.map(v => (
                    <tr key={v.name} style={{ borderTop: "1px solid var(--border)" }}>
                      <td className="py-1.5 pr-3" style={{ width: "34%" }}>{v.name}</td>
                      <td className="py-1.5 pr-3" style={{ color: "var(--muted)", width: 60 }}>{v.type}</td>
                      <td className="py-1.5 pr-3" style={{ color: "var(--muted)", wordBreak: "break-all", width: "22%" }}>{show(v.default)}</td>
                      <td className="py-1.5"><OverrideCell sc={sc} cfgKey={`${NV}module.${slot}.${v.name}`} type={v.type} def={v.default} /></td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </details>
          );
        })}

        {Object.entries(groups).map(([sec, rows]) => (
          <details key={sec} open={!!needle} className="mb-2">
            <summary className="cursor-pointer text-xs font-medium py-1">
              world · config/{sec} <span style={{ color: "var(--muted)" }}>({rows.length})</span>
            </summary>
            {sec === "stationConfig" ? (
              <p className="text-xs my-2" style={{ color: "var(--muted)" }}>These are set through <b>Station config</b> below (the game pulls them natively).</p>
            ) : (
              <p className="text-xs my-2" style={{ color: "var(--muted)" }}>Saving applies the value to the running server immediately and it replicates to players; clearing it restores the default.</p>
            )}
            <table className="w-full text-xs font-mono">
              <tbody>
                {rows.map(w => (
                  <tr key={w.path} style={{ borderTop: "1px solid var(--border)" }}>
                    <td className="py-1.5 pr-3" style={{ width: "34%", wordBreak: "break-all" }}>{w.path}</td>
                    <td className="py-1.5 pr-3" style={{ color: "var(--muted)", width: 60 }}>{w.type}</td>
                    <td className="py-1.5 pr-3" style={{ color: "var(--muted)", width: "22%" }}>
                      {show(w.default)}{w.min != null || w.max != null ? <span> [{w.min ?? ""}…{w.max ?? ""}]</span> : null}
                    </td>
                    <td className="py-1.5">
                      {sec === "stationConfig" ? null : <OverrideCell sc={sc} cfgKey={`${NV}world.${w.path}`} type={w.type} def={w.default} />}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </details>
        ))}

        {cat && !needle && (
          <details className="mb-2">
            <summary className="cursor-pointer text-xs font-medium py-1">presets (reference) <span style={{ color: "var(--muted)" }}>({Object.keys(cat.presets).length})</span></summary>
            {Object.entries(cat.presets).map(([name, vals]) => (
              <div key={name} className="text-xs font-mono mt-2">
                <div className="font-medium">{name}</div>
                {vals.map(v => <div key={v.path} style={{ color: "var(--muted)" }}>{v.path} = <span style={{ color: "var(--accent)" }}>{show(v.value)}</span></div>)}
              </div>
            ))}
          </details>
        )}
      </div>
    </div>
  );
}

// ── 2. Station config (raw keys the game pulls natively) ─────────────────────

function StationConfigEditor({ sc }: { sc: StationCfg }) {
  const [draft, setDraft]   = useState<Record<string, string>>({});
  const [newKey, setNewKey] = useState("");
  const [newVal, setNewVal] = useState("");
  const [filter, setFilter] = useState("");

  const raw = useMemo(() => Object.fromEntries(Object.entries(sc.cfg).filter(([k]) => !k.startsWith(NV))), [sc.cfg]);
  useEffect(() => { setDraft(raw); }, [raw]);

  const keys = Object.keys(draft).sort().filter(k => !filter || k.toLowerCase().includes(filter.toLowerCase()));
  const dirty = Object.keys(draft).filter(k => draft[k] !== raw[k]);

  return (
    <div className="rounded-lg p-5 mb-5" style={card}>
      <div className="flex items-center justify-between mb-1">
        <h2 className="font-semibold text-sm">Station config</h2>
        <div className="flex gap-2">
          <IconBtn title="Reload" onClick={sc.load}><RefreshCw size={13} /></IconBtn>
          <button disabled={!dirty.length || sc.busy}
            onClick={() => sc.save(Object.fromEntries(dirty.map(k => [k, draft[k]])))}
            className="flex items-center gap-1.5 px-3 py-1.5 rounded-md text-xs font-medium"
            style={{ background: "var(--accent)", color: "#fff", opacity: !dirty.length || sc.busy ? 0.5 : 1 }}>
            <Save size={12} /> Save {dirty.length ? `(${dirty.length})` : ""}
          </button>
        </div>
      </div>
      <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
        Keys the game pulls on its config fetch into <span className="font-mono">config/stationConfig</span> (boards, signs, announcement). Removing a key falls back to the default.
      </p>
      <div className="mb-3"><SearchBox value={filter} onChange={setFilter} placeholder="Filter keys" /></div>

      <table className="w-full text-sm">
        <tbody>
          {keys.map(k => (
            <tr key={k} style={{ borderTop: "1px solid var(--border)" }}>
              <td className="py-1.5 pr-3 font-mono text-xs align-middle" style={{ width: "34%", wordBreak: "break-all",
                color: draft[k] !== raw[k] ? "var(--accent)" : "var(--text)" }}>{k}</td>
              <td className="py-1.5 pr-2">
                <input className={inputCls} style={inputStyle} value={draft[k] ?? ""}
                  onChange={e => setDraft(d => ({ ...d, [k]: e.target.value }))} />
              </td>
              <td className="py-1.5 w-8">
                <IconBtn title="Remove key" danger onClick={() => sc.remove(k)} disabled={sc.busy}><Trash2 size={13} /></IconBtn>
              </td>
            </tr>
          ))}
          {!keys.length && (
            <tr><td colSpan={3} className="py-4 text-center text-xs" style={{ color: "var(--muted)" }}>No keys</td></tr>
          )}
          <tr style={{ borderTop: "1px solid var(--border)" }}>
            <td className="py-2 pr-3"><input className={inputCls} style={inputStyle} placeholder="new key"
              value={newKey} onChange={e => setNewKey(e.target.value)} /></td>
            <td className="py-2 pr-2"><input className={inputCls} style={inputStyle} placeholder="value"
              value={newVal} onChange={e => setNewVal(e.target.value)} /></td>
            <td className="py-2 w-8">
              <IconBtn title="Add key" disabled={!newKey.trim() || newKey.trim().startsWith(NV) || sc.busy}
                onClick={() => { sc.save({ [newKey.trim()]: newVal }); setNewKey(""); setNewVal(""); }}>
                <Plus size={13} />
              </IconBtn>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  );
}

// ── 3. Live netvars (read-only view of what the server reports) ──────────────

type Leaf = { path: string; value: string };

function flatten(node: unknown, path: string, out: Leaf[]) {
  if (node === null || typeof node !== "object") { out.push({ path, value: node === null ? "null" : String(node) }); return; }
  const entries = Array.isArray(node) ? node.map((v, i) => [String(i), v] as const) : Object.entries(node as object);
  if (!entries.length) { out.push({ path, value: Array.isArray(node) ? "[]" : "{}" }); return; }
  for (const [k, v] of entries) flatten(v, path ? `${path}/${k}` : k, out);
}

function LiveNetvars() {
  const [dumps, setDumps]   = useState<NetvarDumpInfo[]>([]);
  const [sel, setSel]       = useState("");
  const [leaves, setLeaves] = useState<Leaf[]>([]);
  const [meta, setMeta]     = useState("");
  const [q, setQ]           = useState("");
  const [err, setErr]       = useState("");
  const [loading, setLoading] = useState(false);

  const refreshList = useCallback(() => {
    api.netvarDumps().then(d => {
      const list = d.filter(x => x.event_type === "netvars").concat(d.filter(x => x.event_type !== "netvars"));
      setDumps(list);
      if (!sel && list.length) setSel(`${list[0].deployment_id}|${list[0].event_type}`);
    }).catch(e => setErr(e.message));
  }, [sel]);
  useEffect(() => { refreshList(); }, [refreshList]);

  const load = useCallback(async () => {
    if (!sel) return;
    const [dep, type] = sel.split("|");
    setLoading(true);
    try {
      const out: Leaf[] = [];
      flatten(await api.netvarDump(dep, type), "", out);
      setLeaves(out);
      const info = dumps.find(d => d.deployment_id === dep && d.event_type === type);
      setMeta(`${out.length.toLocaleString()} values · ${info ? ago(info.received_at) : ""}`);
      setErr("");
    } catch (e) { setErr((e as Error).message); setLeaves([]); }
    finally { setLoading(false); }
  }, [sel, dumps]);
  useEffect(() => { load(); }, [load]);

  const shown = useMemo(() => {
    const needle = q.trim().toLowerCase();
    const hits = needle ? leaves.filter(l => l.path.toLowerCase().includes(needle) || l.value.toLowerCase().includes(needle)) : leaves;
    return { total: hits.length, rows: hits.slice(0, 500) };
  }, [leaves, q]);

  return (
    <div className="rounded-lg p-5" style={card}>
      <div className="flex items-center justify-between mb-1">
        <h2 className="font-semibold text-sm">Live server report</h2>
        <IconBtn title="Refresh" onClick={() => { refreshList(); load(); }}><RefreshCw size={13} /></IconBtn>
      </div>
      <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
        What the running server last reported about itself, flattened to path = value. {meta}
      </p>
      {err && <div className="mb-3 text-xs" style={{ color: "var(--red)" }}>{err}</div>}
      <div className="flex gap-2 mb-3">
        <select className="px-2.5 py-1.5 rounded-md text-xs outline-none" style={inputStyle} value={sel} onChange={e => setSel(e.target.value)}>
          {!dumps.length && <option value="">Nothing reported yet</option>}
          {dumps.map(d => (
            <option key={`${d.deployment_id}|${d.event_type}`} value={`${d.deployment_id}|${d.event_type}`}>
              {d.event_type} · {d.deployment_id.slice(0, 12)} · {d.bytes >= 1024 ? `${(d.bytes / 1024).toFixed(0)} KB` : `${d.bytes} B`}
            </option>
          ))}
        </select>
        <div className="flex-1"><SearchBox value={q} onChange={setQ} placeholder="Search path or value" /></div>
      </div>
      {loading ? (
        <div className="py-6 text-center text-xs" style={{ color: "var(--muted)" }}>Loading…</div>
      ) : (
        <>
          <div className="text-xs mb-2" style={{ color: "var(--muted)" }}>
            {shown.total > shown.rows.length ? `Showing 500 of ${shown.total.toLocaleString()} — refine the search` : `${shown.total.toLocaleString()} match(es)`}
          </div>
          <div style={{ maxHeight: 420, overflowY: "auto" }}>
            <table className="w-full text-xs font-mono">
              <tbody>
                {shown.rows.map(l => (
                  <tr key={l.path} style={{ borderTop: "1px solid var(--border)" }}>
                    <td className="py-1 pr-3 align-top" style={{ width: "55%", wordBreak: "break-all" }}>{l.path}</td>
                    <td className="py-1 align-top" style={{ color: "var(--accent)", wordBreak: "break-all" }}>{l.value}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  );
}

export default function NetvarsTab({ stationId }: { stationId: string }) {
  const sc = useStationConfig(stationId);
  return (
    <div>
      <CatalogView sc={sc} />
      <StationConfigEditor sc={sc} />
      <LiveNetvars />
    </div>
  );
}
