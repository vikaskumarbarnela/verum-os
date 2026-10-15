"""पत्र (patra, "leaf, page") - the document format of the VERUM browser.

A page is an ordinary content-addressed object of kind "patra". Its address is
its hash, so a page can never change under you; its provenance says who made
it from what. The format is line-oriented, deliberately small:

    # Title                 heading (## and ### for lower levels)
    plain text              paragraph (consecutive lines join)
    * item                  list item
    > quoted                quotation
    ```                     preformatted block until the next ```
    => target label         link: target is an object hash (or prefix), tag:<name>, or page:<title-tag>
    @cell <code> cap[r,r]…  an interactive cell the page would like to run, with the
                            capabilities it asks for; the browser shows them and asks you

Nothing in a page can act. A link resolves to exact content or not at all; a
script runs only as a cell with exactly the capabilities you grant.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Link:
    n: int
    target: str
    label: str


@dataclass
class Script:
    code: str                     # hash or name of a code object
    caps: List[tuple]             # [(kind, [rights])]


@dataclass
class Page:
    title: str = ""
    blocks: List[tuple] = field(default_factory=list)   # (kind, text) kind in h1 h2 h3 p li quote pre link
    links: List[Link] = field(default_factory=list)
    script: Optional[Script] = None


def parse(src: str) -> Page:
    page = Page()
    para: List[str] = []
    pre: Optional[List[str]] = None

    def flush():
        if para:
            page.blocks.append(("p", " ".join(para)))
            para.clear()

    for raw in src.split("\n"):
        line = raw.rstrip()
        if pre is not None:
            if line.strip() == "```":
                page.blocks.append(("pre", "\n".join(pre)))
                pre = None
            else:
                pre.append(raw)
            continue
        if line.strip() == "```":
            flush(); pre = []
        elif line.startswith("###"):
            flush(); page.blocks.append(("h3", line[3:].strip()))
        elif line.startswith("##"):
            flush(); page.blocks.append(("h2", line[2:].strip()))
        elif line.startswith("#"):
            flush(); t = line[1:].strip(); page.blocks.append(("h1", t))
            if not page.title:
                page.title = t
        elif line.startswith("* "):
            flush(); page.blocks.append(("li", line[2:].strip()))
        elif line.startswith("> "):
            flush(); page.blocks.append(("quote", line[2:].strip()))
        elif line.startswith("=>"):
            flush()
            parts = line[2:].strip().split(None, 1)
            if parts:
                link = Link(len(page.links) + 1, parts[0], parts[1] if len(parts) > 1 else parts[0])
                page.links.append(link)
                page.blocks.append(("link", link))
        elif line.startswith("@cell"):
            flush()
            parts = line[5:].strip().split()
            if parts:
                caps = []
                for spec in parts[1:]:
                    kind, _, rights = spec.partition("[")
                    caps.append((kind, [r for r in rights.rstrip("]").split(",") if r]))
                page.script = Script(parts[0], caps)
        elif not line.strip():
            flush()
        else:
            para.append(line.strip())
    flush()
    if pre is not None:
        page.blocks.append(("pre", "\n".join(pre)))
    return page


def render(page: Page, width: int = 72) -> List[str]:
    """Plain-text rendering for the hosted shell: one list of lines."""
    out: List[str] = []
    for kind, text in page.blocks:
        if kind == "h1":
            out.append(""); out.append(text.upper()); out.append("=" * min(len(text), width))
        elif kind == "h2":
            out.append(""); out.append(text); out.append("-" * min(len(text), width))
        elif kind == "h3":
            out.append(""); out.append("· " + text)
        elif kind == "p":
            out.extend(_wrap(text, width))
        elif kind == "li":
            out.extend(_wrap("• " + text, width, indent="  "))
        elif kind == "quote":
            out.extend(_wrap("│ " + text, width, indent="│ "))
        elif kind == "pre":
            out.extend("    " + l for l in text.split("\n"))
        elif kind == "link":
            out.append(f"  [{text.n}] {text.label}")
    if page.script:
        caps = " ".join(f"{k}[{','.join(r)}]" for k, r in page.script.caps)
        out.append(""); out.append(f"  ⚙ this page would like to run a cell ({page.script.code}) with: {caps}  — `allow` to run it")
    return out


def _wrap(text: str, width: int, indent: str = "") -> List[str]:
    words = text.split()
    lines, cur = [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width:
            lines.append(cur); cur = indent + w
        else:
            cur = (cur + " " + w) if cur else w
    if cur:
        lines.append(cur)
    return lines or [""]
