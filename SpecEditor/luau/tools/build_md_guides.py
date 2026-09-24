#!/usr/bin/env python3
"""
build_md_guides.py -- builds Rigel-GameModes-Guide.pdf (from GAMEMODES.md) and Rigel-MCP-Guide.pdf (from mcp/MCP.md)
with the same look and Edge print step as build_guide.py.

    python tools/build_md_guides.py
"""
from __future__ import annotations

import html
import os
import re
import subprocess
import sys

import build_guide as bg

GUIDES = [
    ("GAMEMODES.md", "Rigel-GameModes-Guide.pdf", "Game Modes Guide",
     "Build team games: teams, rounds, scores, traps, balls, scoreboards and your own Luau."),
    (os.path.join("mcp", "MCP.md"), "Rigel-MCP-Guide.pdf", "MCP Guide",
     "Let an AI agent build in the Spec Editor: place objects, write, check, attach and fix Luau, make game modes."),
]


def inline(t: str) -> str:
    t = html.escape(t)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", t)
    t = re.sub(r"(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])", r"<i>\1</i>", t)
    return t


def md_to_chapters(md: str):
    chapters = []                      # (title, html)
    cur, buf = None, []
    lines = md.splitlines()
    i = 0
    out: list[str] = []

    def flush():
        if cur is not None:
            chapters.append((cur, "".join(out)))

    while i < len(lines):
        ln = lines[i]
        if ln.startswith("# "):
            i += 1
            continue
        if ln.startswith("## "):
            flush()
            cur, out = ln[3:].strip(), []
            i += 1
            continue
        mimg = re.match(r"^!\[(.*)\]\((.*)\)\s*$", ln)
        if mimg:                                           # a picture with its caption
            src = "../" + mimg.group(2)
            out.append(f"<figure class='shot'><img src='{html.escape(src)}'><figcaption>{inline(mimg.group(1))}</figcaption></figure>")
            i += 1
            continue
        if ln.startswith("### "):
            out.append(f"<h3>{inline(ln[4:].strip())}</h3>")
            i += 1
            continue
        if ln.startswith("```"):
            j = i + 1
            code = []
            while j < len(lines) and not lines[j].startswith("```"):
                code.append(lines[j])
                j += 1
            out.append(bg.code("\n".join(code)))
            i = j + 1
            continue
        if ln.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                if not all(re.match(r"^-+$", c) for c in cells if c):
                    rows.append(cells)
                i += 1
            if rows:
                head = "".join(f"<th>{inline(c)}</th>" for c in rows[0])
                body = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in rows[1:])
                out.append(f"<table class='t'><tr>{head}</tr>{body}</table>")
            continue
        m = re.match(r"^(\s*)(-|\d+\.)\s+(.*)$", ln)
        if m:
            ordered = m.group(2)[0].isdigit()
            items = []
            while i < len(lines):
                m2 = re.match(r"^(\s*)(-|\d+\.)\s+(.*)$", lines[i])
                if m2 and len(m2.group(1)) == 0:
                    items.append(m2.group(3))
                elif m2 and items:
                    items[-1] += "\x01" + m2.group(3)          # a nested bullet: its own line
                elif lines[i].startswith("  ") and items:
                    items[-1] += " " + lines[i].strip()
                else:
                    break
                i += 1
            tag = "ol" if ordered else "ul"
            out.append(f"<{tag}>" + "".join("<li>" + "<br>&bull; ".join(inline(part) for part in x.split("\x01")) + "</li>" for x in items) + f"</{tag}>")
            continue
        if not ln.strip():
            i += 1
            continue
        para = [ln.strip()]
        i += 1
        while i < len(lines) and lines[i].strip() and not re.match(r"^(#|```|\||\s*(-|\d+\.)\s)", lines[i]):
            para.append(lines[i].strip())
            i += 1
        out.append(f"<p>{inline(' '.join(para))}</p>")
    flush()
    return chapters


def build(src, pdf, title, sub):
    with open(os.path.join(bg.ROOT, src), encoding="utf-8") as fh:
        chapters = md_to_chapters(fh.read())
    toc = "".join(f'<div><span class="n">{n}</span>{html.escape(t)}</div>' for n, (t, _) in enumerate(chapters, 1))
    cover = f"""<section class="cover">
  <div class="band">
    <div style="font-size:11pt;letter-spacing:.14em;text-transform:uppercase;opacity:.85">Rigel &middot; Spec Editor</div>
    <h1>{html.escape(title)}</h1>
    <div class="sub">{html.escape(sub)}</div>
  </div>
  <div><h3>Contents</h3><div class="toc">{toc}</div></div>
  <div class="meta">Orion Drift build 22284 &middot; F12 shows the editor</div>
</section>"""
    body = "".join(f'<section class="chapter"><h2><span class="num">{n}</span>{html.escape(t)}</h2>{b}</section>'
                   for n, (t, b) in enumerate(chapters, 1))
    extra = ("figure.shot{margin:10px 0 14px;break-inside:avoid;text-align:center}"
             "figure.shot img{max-width:100%;max-height:520px;border:1px solid #ccc;border-radius:6px}"
             "figure.shot figcaption{font-size:9pt;color:#444;margin-top:5px;text-align:left}")
    page = f"<!doctype html><html><head><meta charset='utf-8'><title>Rigel {html.escape(title)}</title><style>{bg.CSS}{extra}</style></head><body>{cover}{body}</body></html>"
    out_html = os.path.join(bg.HERE, os.path.splitext(pdf)[0] + ".html")
    with open(out_html, "w", encoding="utf-8") as fh:
        fh.write(page)
    edge = bg.find_edge()
    if not edge:
        sys.exit("Edge not found")
    out_pdf = os.path.join(bg.ROOT, pdf)
    subprocess.run([edge, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--no-first-run",
                    f"--user-data-dir={os.path.join(os.environ.get('TEMP', bg.HERE), 'rigel-guide-edge')}",
                    f"--print-to-pdf={out_pdf}", "file:///" + out_html.replace(os.sep, "/")], check=True, timeout=180,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(out_pdf, os.path.getsize(out_pdf), "bytes", len(chapters), "chapters")


if __name__ == "__main__":
    for g in GUIDES:
        build(*g)
