"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import {
  LayoutDashboard,
  Users,
  Server,
  Cpu,
  Activity,
  Radio,
  Zap,
  Cable,
} from "lucide-react";

const nav = [
  { href: "/",             label: "Overview",    icon: LayoutDashboard },
  { href: "/users",        label: "Users",       icon: Users },
  { href: "/stations",     label: "Stations",    icon: Server },
  { href: "/deployments",  label: "Deployments", icon: Cpu },
  { href: "/events",       label: "Events",      icon: Activity },
  { href: "/telemetry",    label: "Telemetry",   icon: Zap },
  { href: "/ws",           label: "WS Monitor",  icon: Cable },
];

export function Sidebar() {
  const path = usePathname();

  return (
    <aside className="fixed inset-y-0 left-0 w-56 flex flex-col"
      style={{ background: "var(--surface)", borderRight: "1px solid var(--border)" }}>

      {/* Logo */}
      <div className="flex items-center gap-2 px-5 py-4"
        style={{ borderBottom: "1px solid var(--border)" }}>
        <Radio size={18} style={{ color: "var(--accent)" }} />
        <span className="font-semibold text-sm tracking-wide">Ares</span>
        <span className="ml-auto text-xs px-2 py-0.5 rounded-full font-medium"
          style={{ background: "#1a3a2a", color: "var(--green)" }}>
          LIVE
        </span>
      </div>

      {/* Nav */}
      <nav className="flex-1 py-3 px-2 space-y-0.5">
        {nav.map(({ href, label, icon: Icon }) => {
          const active = href === "/" ? path === "/" : path.startsWith(href);
          return (
            <Link key={href} href={href}
              className="flex items-center gap-3 px-3 py-2 rounded-md text-sm transition-colors"
              style={{
                color: active ? "var(--text)" : "var(--muted)",
                background: active ? "var(--surface2)" : "transparent",
                fontWeight: active ? 500 : 400,
              }}>
              <Icon size={15} style={{ color: active ? "var(--accent)" : "var(--muted)" }} />
              {label}
            </Link>
          );
        })}
      </nav>

      {/* Footer */}
      <div className="px-4 py-3 text-xs" style={{ color: "var(--muted)", borderTop: "1px solid var(--border)" }}>
        localhost:8080
      </div>
    </aside>
  );
}
