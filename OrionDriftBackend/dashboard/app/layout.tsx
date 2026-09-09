import type { Metadata } from "next";
import "./globals.css";
import { TopBar } from "../components/TopBar";

export const metadata: Metadata = {
  title: "Ares Dashboard",
  description: "OrionDrift server management",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en" className="h-full">
      <body className="h-full flex flex-col" style={{ background: "var(--bg)", color: "var(--text)" }}>
        <TopBar />
        <main className="flex-1 overflow-y-auto p-6">
          {children}
        </main>
      </body>
    </html>
  );
}
