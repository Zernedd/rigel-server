"use client";

import { useEffect, useState, useCallback } from "react";
import { api, type WsClient, type WsMessage } from "../../lib/api";

function ago(ts: string) {
  const diff = Date.now() - new Date(ts).getTime();
  if (diff < 1000) return "just now";
  if (diff < 60000) return `${Math.floor(diff / 1000)}s ago`;
  if (diff < 3600000) return `${Math.floor(diff / 60000)}m ago`;
  return new Date(ts).toLocaleTimeString();
}

function shortPath(path: string) {
  return path.length > 48 ? "…" + path.slice(-47) : path;
}

export default function WsPage() {
  const [clients, setClients]   = useState<WsClient[] | null>(null);
  const [err, setErr]           = useState<string | null>(null);
  const [selected, setSelected] = useState<WsClient | null>(null);
  const [messages, setMessages] = useState<WsMessage[] | null>(null);
  const [msgErr, setMsgErr]     = useState<string | null>(null);
  const [live, setLive]         = useState(true);

  const loadClients = useCallback(() => {
    api.wsClients()
      .then(list => {
        setClients(list);
        setErr(null);
      })
      .catch(e => setErr(e.message));
  }, []);

  useEffect(() => {
    loadClients();
    if (!live) return;
    const t = setInterval(loadClients, 2000);
    return () => clearInterval(t);
  }, [loadClients, live]);

  const loadMessages = useCallback((sessionId: string) => {
    api.wsMessages(sessionId)
      .then(msgs => { setMessages(msgs); setMsgErr(null); })
      .catch(e => setMsgErr(e.message));
  }, []);

  useEffect(() => {
    if (!selected) { setMessages(null); return; }
    loadMessages(selected.session_id);
    if (!live) return;
    const t = setInterval(() => loadMessages(selected.session_id), 2000);
    return () => clearInterval(t);
  }, [selected, live, loadMessages]);

  const online = clients?.filter(c => !c.disconnected_at) ?? [];
  const offline = clients?.filter(c => c.disconnected_at) ?? [];
  const sorted = [...online, ...offline];

  return (
    <div className="flex gap-4 h-full" style={{ minHeight: 0 }}>
      {/* Left — client list */}
      <div className="flex flex-col w-80 shrink-0">
        <div className="flex items-center justify-between mb-4 gap-3">
          <h1 className="text-lg font-semibold shrink-0">WS Clients</h1>
          <button
            className="px-3 py-1.5 rounded-md text-xs font-medium shrink-0"
            style={{ background: live ? "var(--accent)" : "var(--surface2)", color: live ? "#fff" : "var(--muted)", border: "1px solid var(--border)" }}
            onClick={() => setLive(l => !l)}>
            {live ? "⏸ Live" : "▶ Paused"}
          </button>
          <button
            className="px-3 py-1.5 rounded-md text-xs shrink-0"
            style={{ background: "var(--surface2)", color: "var(--muted)", border: "1px solid var(--border)" }}
            onClick={loadClients}>Refresh</button>
        </div>

        {err && (
          <div className="mb-3 px-4 py-2 rounded-md text-sm" style={{ background: "#2d1a1a", color: "var(--red)" }}>{err}</div>
        )}

        <div className="flex-1 overflow-y-auto rounded-lg space-y-1.5" style={{ minHeight: 0 }}>
          {!clients ? (
            <p className="text-sm px-2 py-6 text-center" style={{ color: "var(--muted)" }}>Loading…</p>
          ) : sorted.length === 0 ? (
            <p className="text-sm px-2 py-6 text-center" style={{ color: "var(--muted)" }}>No clients</p>
          ) : sorted.map(c => {
            const isOnline  = !c.disconnected_at;
            const isSelected = selected?.session_id === c.session_id;
            return (
              <div key={c.session_id}
                onClick={() => setSelected(isSelected ? null : c)}
                className="rounded-lg px-3 py-2.5 cursor-pointer"
                style={{
                  background: isSelected ? "var(--accent)22" : "var(--surface2)",
                  border: `1px solid ${isSelected ? "var(--accent)" : "var(--border)"}`,
                }}>
                <div className="flex items-center gap-2 mb-1">
                  <span className="w-1.5 h-1.5 rounded-full shrink-0"
                    style={{ background: isOnline ? "var(--green)" : "var(--muted)" }} />
                  <span className="font-mono text-xs truncate" style={{ color: "var(--text)" }}>
                    {c.session_id}
                  </span>
                  <span className="ml-auto text-xs shrink-0 font-medium px-1.5 py-0.5 rounded"
                    style={{ background: "var(--surface)", color: "var(--muted)" }}>
                    {c.message_count}
                  </span>
                </div>
                <div className="text-xs truncate" style={{ color: "var(--muted)" }} title={c.path}>
                  {shortPath(c.path)}
                </div>
                <div className="text-xs mt-0.5" style={{ color: "var(--muted)" }}>
                  {isOnline ? `connected ${ago(c.connected_at)}` : `disconnected ${ago(c.disconnected_at!)}`}
                </div>
              </div>
            );
          })}
        </div>

        <div className="mt-2 text-xs" style={{ color: "var(--muted)" }}>
          {online.length} online · {offline.length} offline
        </div>
      </div>

      {/* Right — message log */}
      <div className="flex flex-col flex-1 min-w-0">
        {!selected ? (
          <div className="flex-1 flex items-center justify-center rounded-lg"
            style={{ border: "1px dashed var(--border)", color: "var(--muted)" }}>
            <span className="text-sm">Select a client to inspect messages</span>
          </div>
        ) : (
          <>
            <div className="flex items-center gap-3 mb-4">
              <div className="flex-1 min-w-0">
                <span className="font-mono text-xs font-semibold" style={{ color: "var(--accent)" }}>
                  {selected.session_id}
                </span>
                <span className="ml-2 text-xs" style={{ color: "var(--muted)" }} title={selected.path}>
                  {shortPath(selected.path)}
                </span>
              </div>
              <button
                className="px-3 py-1.5 rounded-md text-xs shrink-0"
                style={{ background: "var(--surface2)", color: "var(--muted)", border: "1px solid var(--border)" }}
                onClick={() => loadMessages(selected.session_id)}>Refresh</button>
            </div>

            {msgErr && (
              <div className="mb-3 px-4 py-2 rounded-md text-sm" style={{ background: "#2d1a1a", color: "var(--red)" }}>{msgErr}</div>
            )}

            <div className="flex-1 overflow-y-auto rounded-lg space-y-1.5" style={{ minHeight: 0 }}>
              {!messages ? (
                <p className="text-sm px-2 py-6 text-center" style={{ color: "var(--muted)" }}>Loading…</p>
              ) : messages.length === 0 ? (
                <p className="text-sm px-2 py-6 text-center" style={{ color: "var(--muted)" }}>No messages</p>
              ) : messages.map((m, i) => {
                const isIn = m.direction === "in";
                return (
                  <div key={i} className="rounded-lg px-3 py-2"
                    style={{ background: "var(--surface2)", border: "1px solid var(--border)" }}>
                    <div className="flex items-center gap-2 mb-1">
                      <span className="text-xs font-semibold px-1.5 py-0.5 rounded font-mono"
                        style={{
                          background: isIn ? "#1a2d3a" : "#1a2a1a",
                          color: isIn ? "#60aadd" : "var(--green)",
                        }}>
                        {isIn ? "↓ IN" : "↑ OUT"}
                      </span>
                      <span className="font-mono text-xs font-semibold" style={{ color: "var(--text)" }}>
                        {m.command}
                      </span>
                      <span className="ml-auto text-xs" style={{ color: "var(--muted)" }}>
                        {ago(m.timestamp)}
                      </span>
                    </div>
                    {m.headers && (
                      <div className="text-xs font-mono mb-1 truncate" style={{ color: "var(--muted)" }}>
                        {m.headers}
                      </div>
                    )}
                    {m.body && (
                      <pre className="text-xs font-mono whitespace-pre-wrap break-all"
                        style={{ color: "var(--text)", maxHeight: "6rem", overflow: "hidden" }}>
                        {m.body}
                      </pre>
                    )}
                  </div>
                );
              })}
            </div>

            <div className="mt-2 text-xs" style={{ color: "var(--muted)" }}>
              {messages?.length ?? 0} message{messages?.length !== 1 ? "s" : ""}
            </div>
          </>
        )}
      </div>
    </div>
  );
}
