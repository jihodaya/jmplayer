# -*- coding: utf-8 -*-
"""Render docs/MANUAL.*.md to the PDFs that ship in the release.

    python docs/make_manual_pdf.py            # both languages
    python docs/make_manual_pdf.py ko         # just one

Output goes to etc/JMPlayer_Manual_KO.pdf and etc/JMPlayer_Manual_EN.pdf, which
is where build_release.bat picks them up. Re-run this whenever the manuals
change - nothing else regenerates them, and a stale PDF is what ships.

Why a browser and not a PDF library: the manuals are table-heavy and mix Korean
with emoji (the tool buttons are drawn as characters). Chrome already has the
font fallback for both - Malgun Gothic and Segoe UI Emoji are on every Windows
install - and gets the table layout right for free. reportlab, the only PDF
library present here, would need a hand-built font stack and cannot draw colour
emoji at all.

The Markdown converter below is deliberately small and handles exactly what
these two files use: headings, tables, bullet and numbered lists, blockquotes,
horizontal rules, inline code, bold, italic and links. It is not a general
Markdown implementation and does not try to be - it exists so this script has
nothing to install before it runs.
"""

import html
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BROWSERS = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

CSS = """
@page { size: A4; margin: 16mm 14mm 16mm 14mm; }
body {
  font-family: "Malgun Gothic", "Segoe UI", "Apple SD Gothic Neo", sans-serif;
  font-size: 10pt; line-height: 1.65; color: #1b1b1b; margin: 0;
}
h1 { font-size: 20pt; margin: 0 0 4pt; color: #10305c; letter-spacing: -0.3pt; }
h2 {
  font-size: 13.5pt; margin: 20pt 0 7pt; padding-bottom: 3pt; color: #10305c;
  border-bottom: 1.2pt solid #c9d6e6; page-break-after: avoid;
}
h3 { font-size: 11pt; margin: 13pt 0 5pt; color: #2c4a72; page-break-after: avoid; }
p { margin: 5pt 0; }
ul, ol { margin: 5pt 0 5pt 0; padding-left: 17pt; }
li { margin: 2.5pt 0; }
code {
  font-family: Consolas, "Courier New", monospace; font-size: 9pt;
  background: #eef2f7; padding: 0.5pt 3pt; border-radius: 2.5pt; color: #1f3d63;
}
a { color: #1f5fa8; text-decoration: none; }
hr { border: 0; border-top: 0.8pt solid #d8dee6; margin: 14pt 0; }
blockquote {
  margin: 8pt 0; padding: 6pt 11pt; background: #f4f7fb;
  border-left: 2.6pt solid #5a8ac6; page-break-inside: avoid;
}
blockquote p { margin: 3pt 0; }
table {
  border-collapse: collapse; width: 100%; margin: 7pt 0; font-size: 9pt;
}
th, td {
  border: 0.6pt solid #cbd4de; padding: 3.5pt 6pt; text-align: left;
  vertical-align: top;
}
th { background: #e8eef6; color: #10305c; font-weight: 600; }
pre {
  background: #23272e; color: #e6e9ef; padding: 7pt 10pt; border-radius: 3pt;
  font-size: 8.5pt; line-height: 1.45; overflow-wrap: anywhere;
  white-space: pre-wrap; page-break-inside: avoid; margin: 7pt 0;
}
pre code { background: none; color: inherit; padding: 0; font-size: inherit; }
tr { page-break-inside: avoid; }
tr:nth-child(even) td { background: #fafbfd; }
"""


def inline(text):
    """Inline Markdown -> HTML. Code spans are lifted out first so the emphasis
    rules cannot chew on their contents."""
    spans = []

    def stash(m):
        spans.append(m.group(1))
        return "\x00%d\x00" % (len(spans) - 1)

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![*\w])\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", text)
    text = text.replace("\\~", "~").replace("\\*", "*")
    for i, s in enumerate(spans):
        text = text.replace("\x00%d\x00" % i,
                            "<code>%s</code>" % html.escape(s, quote=False))
    return text


def cells(row):
    row = row.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|"):
        row = row[:-1]
    return [c.strip() for c in row.split("|")]


def to_html(md):
    out = []
    lines = md.split("\n")
    i = 0
    n = len(lines)

    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        # Fenced code block.
        if stripped.startswith("```"):
            i += 1
            body = []
            while i < n and not lines[i].strip().startswith("```"):
                body.append(lines[i])
                i += 1
            i += 1
            out.append("<pre><code>%s</code></pre>"
                       % html.escape("\n".join(body), quote=False))
            continue

        if re.fullmatch(r"-{3,}|\*{3,}", stripped):
            out.append("<hr>")
            i += 1
            continue

        m = re.match(r"(#{1,4})\s+(.*)", stripped)
        if m:
            lvl = len(m.group(1))
            out.append("<h%d>%s</h%d>" % (lvl, inline(m.group(2)), lvl))
            i += 1
            continue

        # A header row followed by a |---|---| separator is a table.
        if stripped.startswith("|") and i + 1 < n and re.fullmatch(
                r"\|[\s:|-]+\|", lines[i + 1].strip()):
            head = cells(stripped)
            i += 2
            body = []
            while i < n and lines[i].strip().startswith("|"):
                body.append(cells(lines[i].strip()))
                i += 1
            out.append("<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>" % (
                "".join("<th>%s</th>" % inline(c) for c in head),
                "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r)
                        for r in body)))
            continue

        # Consecutive '>' lines. The prefix is stripped and the remainder run
        # through this same function, because the manuals put bullet lists
        # inside their callouts - flattening them into one paragraph turned the
        # "what is new" box into a run-on sentence with stray asterisks in it.
        if stripped.startswith(">"):
            body = []
            while i < n and lines[i].strip().startswith(">"):
                body.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            out.append("<blockquote>%s</blockquote>" % to_html("\n".join(body)))
            continue

        # Lists, with one level of nesting (an indent of two spaces or more).
        m = re.match(r"(\s*)([*+-]|\d+\.)\s+(.*)", line)
        if m:
            tag = "ol" if re.match(r"\d+\.", m.group(2)) else "ul"
            out.append("<%s>" % tag)
            depth = 0
            while i < n:
                mm = re.match(r"(\s*)([*+-]|\d+\.)\s+(.*)", lines[i])
                if not mm:
                    if lines[i].strip():
                        break
                    i += 1
                    if i < n and re.match(r"(\s*)([*+-]|\d+\.)\s+", lines[i]):
                        continue
                    break
                want = 1 if len(mm.group(1)) >= 2 else 0
                while depth < want:
                    out.append("<ul>")
                    depth += 1
                while depth > want:
                    out.append("</ul>")
                    depth -= 1
                out.append("<li>%s</li>" % inline(mm.group(3)))
                i += 1
            while depth > 0:
                out.append("</ul>")
                depth -= 1
            out.append("</%s>" % tag)
            continue

        para = []
        while i < n and lines[i].strip() and not re.match(
                r"\s*(#{1,4}\s|[*+-]\s|\d+\.\s|\||>|```|-{3,}$)", lines[i]):
            para.append(lines[i].strip())
            i += 1
        if para:
            out.append("<p>%s</p>" % inline(" ".join(para)))
        else:
            i += 1

    return "\n".join(out)


def render(md_path, pdf_path):
    md = open(md_path, encoding="utf-8").read()
    # The cross-language link at the top points at the other .md, which means
    # nothing once this is a PDF.
    md = re.sub(r"^\*\[[^\]]*\]\([^)]*\.md\)\*\s*$", "", md, flags=re.M)

    page = ('<!doctype html><html><head><meta charset="utf-8">'
            "<style>%s</style></head><body>%s</body></html>" % (CSS, to_html(md)))

    browser = next((b for b in BROWSERS if os.path.exists(b)), None)
    if not browser:
        sys.exit("No Chrome or Edge found - looked in:\n  " + "\n  ".join(BROWSERS))

    fd, tmp_html = tempfile.mkstemp(suffix=".html")
    os.close(fd)
    open(tmp_html, "w", encoding="utf-8").write(page)
    profile = tempfile.mkdtemp(prefix="jmp-pdf-")
    try:
        url = "file:///" + tmp_html.replace("\\", "/")
        cmd = [browser, "--headless=new", "--disable-gpu", "--no-sandbox",
               "--no-pdf-header-footer", "--user-data-dir=" + profile,
               "--print-to-pdf=" + pdf_path, url]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        if not os.path.exists(pdf_path):
            sys.exit("Chrome did not write %s\n%s\n%s" % (pdf_path, r.stdout, r.stderr))
        print("  %-30s %8d bytes" % (os.path.basename(pdf_path),
                                     os.path.getsize(pdf_path)))
    finally:
        os.remove(tmp_html)


JOBS = {
    "ko": ("MANUAL.ko.md", "JMPlayer_Manual_KO.pdf"),
    "en": ("MANUAL.en.md", "JMPlayer_Manual_EN.pdf"),
}

if __name__ == "__main__":
    which = sys.argv[1:] or ["ko", "en"]
    etc = os.path.join(ROOT, "etc")
    os.makedirs(etc, exist_ok=True)
    for key in which:
        if key not in JOBS:
            sys.exit("unknown language %r - use ko and/or en" % key)
        src, dst = JOBS[key]
        render(os.path.join(HERE, src), os.path.join(etc, dst))
