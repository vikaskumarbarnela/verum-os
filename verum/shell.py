"""The shell: the human's seat in VERUM.

It is not a terminal over a filesystem - there is no filesystem. It is a
principal that holds capabilities and issues intents. Before running anything
on your behalf it tells you exactly which capabilities will be used.
"""

from __future__ import annotations

import os
import shlex
import sys
from typing import List, Optional

from . import vm
from .caps import CapError
from .kernel import SHELL, Kernel, KernelError


HELP = """
VERUM shell - intents you can state
  note <text...>                 store a note object (tag: note)
  code <name> <path.vs>          store a Verum Script file as a code object (tag: code,<name>)
  find <tag>                     list objects carrying a tag
  tags                           list all tags
  show <hash|prefix>             show an object
  why <hash|prefix>              provenance: what it was made from, by whom
  spawn <cell> <code-hash|name>  create a cell from a code object (no capabilities yet)
  grant <cell> <cap> [rights]    hand the cell a narrowed copy of one of your caps
                                 e.g. grant wc store get,put     grant wc console print
  link <cell> <target> [label]   let <cell> send messages to <target> (bound as label)
  send <cell> <expr>             queue a message for a cell (expr is Verum Script data)
  run [ticks]                    let the scheduler run queued messages
  cells                          list cells and their capabilities
  caps                           your own capabilities
  revoke <cell> <label>          revoke a capability you granted (and everything derived)
  kill <cell>                    remove a cell
  eval <expr>                    run Verum Script here with your own capabilities
  ledger [n]                     last n ledger events
  verify                         check the ledger hash chain
  rewind <seq>                   fork the whole system back to ledger entry <seq>
  seat <name> [passphrase]       another human, with no capabilities (grant to them like a cell)
  switch <name> [passphrase]     take that seat; every intent is then checked against its caps
  seats / whoami                 list seats and their capabilities; who is at the keyboard
  help / quit
"""


class Shell:
    def __init__(self, kernel: Kernel, out=None) -> None:
        self.k = kernel
        self.out = out or (lambda s: print(s))
        self.k._console = self.out

    # ------------------------------------------------------------------ helpers

    def _h(self, ref: str) -> str:
        h = self.k.store.resolve(ref)
        if h is None:
            raise KernelError(f"no unique object matching {ref!r}")
        return h

    def _code_hash(self, ref: str) -> str:
        h = self.k.store.resolve(ref)
        if h:
            return h
        for cand in self.k.store.find(ref):
            if self.k.store.get(cand).kind == "code":
                return cand
        raise KernelError(f"no code object {ref!r}")

    @property
    def me(self) -> str:
        return self.k.me

    def _cap_token(self, holder: str, label: str) -> str:
        if holder in self.k.seats:
            tok = self.k.seats[holder].caps.get(label)
        else:
            cell = self.k.cells.get(holder)
            handle = cell.env.vars.get(label) if cell else None
            tok = handle.token if isinstance(handle, vm.CapHandle) else None
        if tok is None:
            raise KernelError(f"{holder} holds no capability labelled {label!r}")
        return tok

    def _describe_cell_caps(self, name: str) -> List[str]:
        cell = self.k.cells[name]
        out = []
        for label, v in cell.env.vars.items():
            if isinstance(v, vm.CapHandle):
                cap = self.k.caps.get(v.token)
                out.append(f"{label}={cap.describe() if cap else '?'}")
        return out

    # ------------------------------------------------------------------ commands

    def do(self, line: str) -> bool:
        """Execute one line. Returns False when the shell should exit."""
        line = line.strip()
        if not line:
            return True
        try:
            parts = shlex.split(line)
        except ValueError as e:
            self.out(f"parse error: {e}")
            return True
        cmd, args = parts[0].lower(), parts[1:]
        fn = getattr(self, "cmd_" + cmd, None)
        if fn is None:
            self.out(f"unknown intent {cmd!r} - type help")
            return True
        try:
            r = fn(args, line)
            return r if r is not None else True
        except (KernelError, CapError, vm.VMError) as e:
            self.out(f"refused: {e}")
        except (IndexError, ValueError) as e:
            self.out(f"usage error: {e} - type help")
        return True

    def cmd_help(self, a, line):
        self.out(HELP.strip())

    def cmd_quit(self, a, line):
        return False

    cmd_exit = cmd_quit

    def cmd_note(self, a, line):
        text = line.split(None, 1)[1] if " " in line else ""
        if not text:
            raise ValueError("note needs text")
        self.k.caps.check(self.k._cap_of(self.me, "store"), self.me, "put")
        h = self.k.put("note", text, ["note"], by=self.me, op="note")
        self.out(f"stored note {h[:12]}")

    def cmd_code(self, a, line):
        name, path = a[0], a[1]
        with open(path, encoding="utf-8") as f:
            src = f.read()
        vm.parse(src)                          # syntax check before it enters the store
        self.k.caps.check(self.k._cap_of(self.me, "store"), self.me, "put")
        h = self.k.put("code", src, ["code", name], by=self.me, op="code")
        self.out(f"stored code {name} as {h[:12]}")

    def cmd_find(self, a, line):
        hs = self.k.store.find(a[0])
        if not hs:
            self.out(f"nothing tagged {a[0]!r}")
        for h in hs:
            o = self.k.store.get(h)
            self.out(f"  {h[:12]}  {o.kind:<8} by {o.by:<10} {self._preview(o.data)}")

    def cmd_tags(self, a, line):
        for t, n in self.k.store.tags().items():
            self.out(f"  {t:<16} {n}")

    def cmd_show(self, a, line):
        h = self._h(a[0])
        o = self.k.store.get(h)
        self.out(f"object {h}")
        self.out(f"  kind   {o.kind}    tags {list(o.tags)}    by {o.by} via {o.op} at tick {o.tick}")
        self.out(f"  inputs {[i[:12] for i in o.inputs] or '-'}")
        self.out(f"  data   {vm.show(o.data)}")

    def cmd_why(self, a, line):
        h = self._h(a[0])
        for depth, hh, o in self.k.store.lineage(h):
            self.out(f"  {'    ' * depth}{hh[:12]} {o.kind} <- {o.by} via {o.op} @tick {o.tick}: {self._preview(o.data)}")

    def cmd_spawn(self, a, line):
        name, ref = a[0], a[1]
        self.k.spawn(name, self._code_hash(ref), by=self.me)
        self.out(f"cell {name} spawned with NO capabilities. Use: grant {name} <cap> [rights]")

    def cmd_grant(self, a, line):
        cell, label = a[0], a[1]
        parent = self._cap_token(self.me, label)
        src = self.k.caps.get(parent)
        rights = a[2].split(",") if len(a) > 2 else sorted(src.rights)
        cap = self.k.grant(parent, self.me, rights, cell, label)
        self.out(f"granted {cap.describe()}")

    def cmd_link(self, a, line):
        src, target = a[0], a[1]
        label = a[2] if len(a) > 2 else target
        cap = self.k.mint_cell_cap(target, self.me, f"cell:{target}")
        self.k.grant(cap.token, self.me, ["send"], src, label)
        self.out(f"{src} may now send to {target} (bound as {label})")

    def cmd_send(self, a, line):
        cell = a[0]
        expr = line.split(None, 2)[2] if len(line.split(None, 2)) > 2 else "nil"
        body = self._eval_data(expr)
        self.k.send(cell, body, by=self.me)
        self.out(f"queued for {cell}: {vm.show(body)}")

    def cmd_run(self, a, line):
        ticks = self.k.run(int(a[0]) if a else 100)
        self.out(f"ran {ticks} tick(s); now at tick {self.k.tick}")

    def cmd_cells(self, a, line):
        if not self.k.cells:
            self.out("no cells")
        for name, c in self.k.cells.items():
            self.out(f"  {name:<12} code {c.code[:12]}  runs {c.runs} errors {c.errors} fuel {c.fuel_used}  inbox {len(c.inbox)}")
            for d in self._describe_cell_caps(name):
                self.out(f"      {d}")

    def cmd_caps(self, a, line):
        for label, tok in self.k.my_caps.items():
            self.out(f"  {label:<14} {self.k.caps.get(tok).describe()}")

    def cmd_seat(self, a, line):
        name = a[0]
        passphrase = a[1] if len(a) > 1 else None
        self.k.seat_create(name, self.me, passphrase)
        self.out(f"seat {name} created with NO capabilities{' (passphrase set)' if passphrase else ''}. "
                 f"grant {name} <cap> [rights], then: switch {name}")

    def cmd_switch(self, a, line):
        self.k.switch(a[0], a[1] if len(a) > 1 else None)
        self.out(f"you are now {self.me}")

    def cmd_whoami(self, a, line):
        self.out(self.me)

    def cmd_seats(self, a, line):
        for name, s in self.k.seats.items():
            self.out(f"  {name}{'  (you)' if name == self.me else ''}{'  [passphrase]' if s.secret else ''}")
            for label, tok in s.caps.items():
                self.out(f"      {label}={self.k.caps.get(tok).describe()}")

    def cmd_revoke(self, a, line):
        holder, label = a[0], a[1]
        tok = self._cap_token(holder, label)
        n = self.k.revoke(tok, self.me)
        self.out(f"revoked {len(n)} capability(ies)")

    def cmd_kill(self, a, line):
        self.k.kill(a[0], self.me)
        self.out(f"cell {a[0]} removed")

    def cmd_eval(self, a, line):
        expr = line.split(None, 1)[1]
        env = vm.new_env({label: vm.CapHandle(tok, label) for label, tok in self.k.my_caps.items()})
        used: List[str] = []

        def syscall(handle, op, args):
            used.append(f"{handle.label}.{op}")
            try:
                return self.k.syscall(self.me, handle.token, op, args)
            except (CapError, KernelError) as e:
                raise vm.VMError(f"denied: {e}")

        it = vm.Interp(syscall, 200_000)
        result = None
        for form in vm.parse(expr):
            result = it.eval(form, env)
        self.out(f"=> {vm.show(result)}")
        if used:
            self.out(f"   (capabilities used: {', '.join(used)}; fuel {it.used})")

    def cmd_ledger(self, a, line):
        n = int(a[0]) if a else 15
        for e in self.k.ledger.entries[-n:]:
            ev = dict(e.event)
            t = ev.pop("type")
            if t == "put":
                o = ev["obj"]
                desc = f"{o['kind']} by {o['by']} {self._preview(o['data'])}"
            elif t == "genesis":
                desc = f"shell seated with {list(ev['caps'])}"
            else:
                desc = " ".join(f"{k}={self._preview(v)}" for k, v in ev.items() if k != "token")
            self.out(f"  #{e.seq:<4} {e.hash[:10]}  {t:<8} {desc}")

    def cmd_verify(self, a, line):
        ok, msg = self.k.ledger.verify()
        self.out(("OK   " if ok else "FAIL ") + msg)

    def cmd_rewind(self, a, line):
        seq = int(a[0])
        base = self.k.state_dir
        new_dir = os.path.join(os.path.dirname(base) or ".", f"{os.path.basename(base)}-fork{seq}") if base else None
        self.k = self.k.fork(seq, new_dir)
        self.k._console = self.out
        self.out(f"forked at ledger entry {seq}: {len(self.k.store)} objects, {len(self.k.cells)} cells, tick {self.k.tick}"
                 + (f"; new state in {new_dir}" if new_dir else ""))

    # ------------------------------------------------------------------ util

    def _eval_data(self, expr: str):
        """Evaluate a pure expression (no capabilities) into message data."""
        it = vm.Interp(lambda *_: (_ for _ in ()).throw(vm.VMError("no capabilities here")), 20_000)
        env = vm.new_env()
        forms = vm.parse(expr)
        result = None
        for f in forms:
            result = it.eval(f, env)
        return result

    @staticmethod
    def _preview(v, n: int = 48) -> str:
        s = vm.show(v).replace("\n", " ")
        return s if len(s) <= n else s[: n - 1] + "..."


def repl(state_dir: Optional[str]) -> None:
    k = Kernel(state_dir)
    sh = Shell(k)
    sh.out(f"VERUM OS  - {len(k.store)} objects, {len(k.cells)} cells, ledger head {k.ledger.head[:12]}")
    sh.out("no files. no root. no apps. state what you want; type help.")
    while True:
        try:
            line = input(f"{sh.me}> ")
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not sh.do(line):
            break


if __name__ == "__main__":
    repl(sys.argv[1] if len(sys.argv) > 1 else None)
