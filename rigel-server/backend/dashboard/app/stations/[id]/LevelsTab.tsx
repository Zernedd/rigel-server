"use client";

import { useCallback, useEffect, useState } from "react";
import { api, type SpecLevel } from "../../../lib/api";
import { fmt, ago } from "../../../lib/utils";
import { RefreshCw, Trash2, Eye, X } from "lucide-react";

// Levels saved from the in-game Spec Editor.
//
// Game servers upload levels to the backend (port 78, v1/spec/*). Each level has two flags:
//   autoload -- load it whenever a game server boots
//   loaded   -- the desired state right now; servers poll it and load/unload to match (~15 s)
// Servers also report what they actually have loaded, shown here as "On server now".

const card: React.CSSProperties = { background: "var(--surface)", border: "1px solid var(--border)" };
const REFRESH_MS = 10_000;

function fmtSize(n: number) {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function useSpecLevels() {
  const [levels, setLevels] = useState<SpecLevel[] | null>(null);
  const [err, setErr]       = useState("");
  const [busy, setBusy]     = useState<string | null>(null);   // name of the level being changed

  const load = useCallback(() => {
    api.specLevels().then(l => { setLevels(l); setErr(""); }).catch(e => setErr((e as Error).message));
  }, []);
  useEffect(() => {
    load();
    const t = setInterval(load, REFRESH_MS);
    return () => clearInterval(t);
  }, [load]);

  const patch = useCallback(async (name: string, body: { autoload?: boolean; loaded?: boolean }) => {
    setBusy(name);
    try {
      await api.patchSpecLevel(name, body);
      setLevels(ls => ls?.map(l => l.name === name ? { ...l, ...body } : l) ?? ls);
      setErr("");
    }
    catch (e) { setErr((e as Error).message); }
    finally { setBusy(null); }
  }, []);

  const remove = useCallback(async (name: string) => {
    setBusy(name);
    try {
      await api.deleteSpecLevel(name);
      setLevels(ls => ls?.filter(l => l.name !== name) ?? ls);
      setErr("");
    }
    catch (e) { setErr((e as Error).message); }
    finally { setBusy(null); }
  }, []);

  return { levels, err, busy, load, patch, remove };
}

function Toggle({ on, disabled, onChange, title }: { on: boolean; disabled?: boolean; onChange: (v: boolean) => void; title: string }) {
  return (
    <button title={title} disabled={disabled} onClick={() => onChange(!on)}
      className="relative inline-flex items-center rounded-full transition-colors"
      style={{
        width: 34, height: 18, opacity: disabled ? 0.5 : 1,
        background: on ? "var(--accent)" : "var(--surface2)", border: "1px solid var(--border)",
      }}>
      <span className="absolute rounded-full transition-all"
        style={{ width: 12, height: 12, top: 2, left: on ? 18 : 2, background: on ? "#fff" : "var(--muted)" }} />
    </button>
  );
}

function ViewModal({ name, onClose }: { name: string; onClose: () => void }) {
  const [text, setText] = useState<string | null>(null);
  const [err, setErr]   = useState("");
  useEffect(() => {
    api.specLevel(name).then(r => setText(r.text)).catch(e => setErr((e as Error).message));
  }, [name]);
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4"
      style={{ background: "rgba(0,0,0,0.65)" }}
      onClick={e => { if (e.target === e.currentTarget) onClose(); }}>
      <div className="w-full max-w-4xl rounded-lg p-6 flex flex-col" style={{ ...card, maxHeight: "85vh" }}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold text-sm">Level · <span className="font-mono">{name}</span> <span className="text-xs font-normal" style={{ color: "var(--muted)" }}>(read-only)</span></h2>
          <button onClick={onClose} style={{ color: "var(--muted)" }}><X size={16} /></button>
        </div>
        {err && <div className="mb-3 text-xs" style={{ color: "var(--red)" }}>{err}</div>}
        <pre className="text-xs font-mono p-3 rounded-md overflow-auto flex-1"
          style={{ background: "var(--surface2)", border: "1px solid var(--border)", whiteSpace: "pre", minHeight: 120 }}>
          {text ?? (err ? "" : "Loading…")}
        </pre>
      </div>
    </div>
  );
}

// Levels are backend-wide (not per station): every game server reads the same store.
export default function LevelsTab() {
  const s = useSpecLevels();
  const [viewing, setViewing] = useState<string | null>(null);

  const onDelete = (name: string) => {
    if (!confirm(`Delete the level "${name}"? This removes the saved file for good. A server that has it loaded keeps it until it is unloaded or restarts.`)) return;
    s.remove(name);
  };

  return (
    <div className="rounded-lg p-5" style={card}>
      <div className="flex items-center justify-between mb-1">
        <h2 className="font-semibold text-sm">Editor levels</h2>
        <button title="Refresh" onClick={s.load} className="p-1.5 rounded-md"
          style={{ color: "var(--muted)", border: "1px solid var(--border)" }}><RefreshCw size={13} /></button>
      </div>
      <p className="text-xs mb-4" style={{ color: "var(--muted)" }}>
        Levels are saved from the in-game Spec Editor (the <b>Levels</b> panel). Turn on <b>Load on server boot</b> to
        have a level load automatically whenever a game server starts. <b>Load now</b> / <b>Unload</b> applies to running
        servers live: they pick up the change within about 15 seconds, and <b>On server now</b> shows what the servers
        last reported. This list refreshes every 10 seconds.
      </p>
      {s.err && <div className="mb-3 text-xs" style={{ color: "var(--red)" }}>{s.err}</div>}

      {s.levels === null ? (
        <div className="text-xs" style={{ color: "var(--muted)" }}>Loading…</div>
      ) : s.levels.length === 0 ? (
        <div className="text-xs py-6 text-center" style={{ color: "var(--muted)" }}>
          No saved levels yet. Save one from the Spec Editor&apos;s Levels panel in game.
        </div>
      ) : (
        <div style={{ overflowX: "auto" }}>
          <table className="w-full text-sm">
            <thead>
              <tr className="text-xs text-left" style={{ color: "var(--muted)" }}>
                <th className="py-2 pr-3 font-medium">Name</th>
                <th className="py-2 pr-3 font-medium">Updated</th>
                <th className="py-2 pr-3 font-medium">Size</th>
                <th className="py-2 pr-3 font-medium">On server now</th>
                <th className="py-2 pr-3 font-medium">Load on server boot</th>
                <th className="py-2 pr-3 font-medium">Live</th>
                <th className="py-2 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              {s.levels.map(l => {
                const busy = s.busy === l.name;
                const onServer = l.serverLoaded.length > 0;
                const pending = l.loaded !== onServer;
                return (
                  <tr key={l.name} style={{ borderTop: "1px solid var(--border)" }}>
                    <td className="py-2 pr-3 font-mono text-xs">{l.name}</td>
                    <td className="py-2 pr-3 text-xs" title={fmt(l.updated)} style={{ color: "var(--muted)" }}>{ago(l.updated)}</td>
                    <td className="py-2 pr-3 text-xs" style={{ color: "var(--muted)" }}>{fmtSize(l.size)}</td>
                    <td className="py-2 pr-3 text-xs">
                      <span className="px-2 py-0.5 rounded-full font-medium"
                        style={onServer
                          ? { background: "#1a2d1a", color: "var(--green)" }
                          : { background: "var(--surface2)", color: "var(--muted)" }}
                        title={onServer ? `Loaded on: ${l.serverLoaded.join(", ")}` : undefined}>
                        {onServer ? "Loaded" : "Not loaded"}
                      </span>
                      <span className="ml-2" style={{ color: "var(--muted)" }} title={fmt(l.serverSeen)}>
                        {l.serverSeen ? `seen ${ago(l.serverSeen)}` : "no report yet"}
                      </span>
                      {pending && <span className="ml-2" style={{ color: "var(--muted)" }}>({l.loaded ? "loading…" : "unloading…"})</span>}
                    </td>
                    <td className="py-2 pr-3">
                      <Toggle on={l.autoload} disabled={busy} title="Load on server boot"
                        onChange={v => s.patch(l.name, { autoload: v })} />
                    </td>
                    <td className="py-2 pr-3">
                      <button disabled={busy} onClick={() => s.patch(l.name, { loaded: !l.loaded })}
                        title={l.loaded ? "Unload from running servers (applies within ~15 s)" : "Load on running servers now (applies within ~15 s)"}
                        className="px-2.5 py-1 rounded-md text-xs"
                        style={{
                          border: "1px solid var(--border)", opacity: busy ? 0.5 : 1,
                          color: l.loaded ? "var(--red)" : "var(--accent)",
                        }}>
                        {l.loaded ? "Unload" : "Load now"}
                      </button>
                    </td>
                    <td className="py-2">
                      <div className="flex items-center gap-1.5 justify-end">
                        <button title="View level text" onClick={() => setViewing(l.name)} className="p-1.5 rounded-md"
                          style={{ color: "var(--muted)", border: "1px solid var(--border)" }}><Eye size={12} /></button>
                        <button title="Delete level" disabled={busy} onClick={() => onDelete(l.name)} className="p-1.5 rounded-md"
                          style={{ color: "var(--red)", border: "1px solid var(--border)", opacity: busy ? 0.4 : 1 }}><Trash2 size={12} /></button>
                      </div>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}

      {viewing && <ViewModal name={viewing} onClose={() => setViewing(null)} />}
    </div>
  );
}
