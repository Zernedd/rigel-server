"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import Image from "next/image";

const nav = [
  { href: "/",            label: "Stations"    },
  { href: "/users",       label: "Users"       },
  { href: "/deployments", label: "Deployments" },
  { href: "/events",      label: "Events"      },
];

export function TopBar() {
  const path = usePathname();

  return (
    <header className="flex items-center gap-4 px-4 py-3"
      style={{ background: "var(--surface)", borderBottom: "1px solid var(--border)" }}>

      {/* Lead: logo + title */}
      <Link href="/" className="flex items-center gap-2 shrink-0 hover:opacity-80 active:opacity-50">
        <Image src="/logo.webp" alt="logo" width={28} height={28} className="rounded" />
        <strong className="text-sm uppercase tracking-wide hidden sm:block">
          Ares Dashboard
        </strong>
      </Link>

      {/* Nav */}
      <nav className="flex items-center gap-1 flex-1 overflow-x-auto">
        {nav.map(({ href, label }) => {
          const active = href === "/"
            ? (path === "/" || path.startsWith("/stations"))
            : path.startsWith(href);
          return (
            <Link key={href} href={href}
              className="px-3 py-1.5 rounded-md text-sm whitespace-nowrap transition-colors"
              style={{
                color:      active ? "var(--text)"    : "var(--muted)",
                background: active ? "var(--surface2)" : "transparent",
                fontWeight: active ? 500 : 400,
              }}>
              {label}
            </Link>
          );
        })}
      </nav>

      {/* Trail: live badge + server */}
      <div className="flex items-center gap-3 shrink-0">
        <span className="text-xs px-2 py-0.5 rounded-full font-medium hidden sm:inline-block"
          style={{ background: "#1a3a2a", color: "var(--green)" }}>
          LIVE
        </span>
        <span className="text-xs hidden md:block" style={{ color: "var(--muted)" }}>
          localhost:8080
        </span>
        {/* Full navigation (not next/Link): /auth/login is a backend 302 to Meta sign-in. */}
        <a href="/auth/login"
          className="px-3 py-1.5 rounded-md text-sm whitespace-nowrap transition-colors hover:opacity-80 active:opacity-50"
          style={{ background: "var(--surface2)", color: "var(--text)", fontWeight: 500 }}>
          Log in with Meta
        </a>
      </div>
    </header>
  );
}
