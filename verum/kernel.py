"""The VERUM kernel: cells, the syscall gate, the scheduler, and time travel.

A *cell* is the unit of computation. It is not a process and not an app:

    * it has no memory of its own between messages - all state lives in the store
    * it runs only Verum Script, inside a fuel budget
    * it holds a handful of capabilities, bound to names in its environment
    * the only thing it can do to the world is (call <cap> "op" ...), and every
      such call passes through `Kernel.syscall`, which checks the capability

Every state change is an event appended to the ledger.  Booting the kernel
means replaying the ledger. Rewinding means replaying a prefix of it into a
fresh kernel (a fork). Nothing is ever edited in place.
"""

from __future__ import annotations

import os
from collections import deque
from dataclasses import dataclass, field
from typing import Any, Callable, Deque, Dict, List, Optional

from .caps import Cap, CapError, CapTable, RIGHTS
from .store import Ledger, Obj, Store
from . import vm


DEFAULT_FUEL = 50_000
SHELL = "shell"           # the human's principal name


class KernelError(Exception):
    pass


@dataclass
class Cell:
    id: str
    code: str                                  # hash of the code object
    env: vm.Env                                # module environment (defines + cap bindings)
    inbox: Deque[tuple] = field(default_factory=deque)
    fuel: int = DEFAULT_FUEL
    runs: int = 0
    errors: int = 0
    fuel_used: int = 0


class Kernel:
    def __init__(self, state_dir: Optional[str] = None, console: Optional[Callable[[str], None]] = None) -> None:
        self.state_dir = state_dir
        self.store = Store()
        self.caps = CapTable()
        self.cells: Dict[str, Cell] = {}
        self.tick = 0
        self.console_lines: List[str] = []
        self._console = console
        self._replaying = False
        self.shell_caps: Dict[str, str] = {}   # label -> token, the human's seat

        ledger_path = None
        if state_dir:
            os.makedirs(state_dir, exist_ok=True)
            ledger_path = os.path.join(state_dir, "ledger.jsonl")
        self.ledger = Ledger.load(ledger_path) if ledger_path and os.path.exists(ledger_path) else Ledger(ledger_path)

        if self.ledger.entries:
            self._replay(self.ledger.entries)
        else:
            self._genesis()

    # ------------------------------------------------------------------ events

    def _record(self, event: Dict[str, Any]) -> None:
        if not self._replaying:
            self.ledger.append(event)

    def _replay(self, entries) -> None:
        self._replaying = True
        try:
            for e in entries:
                self._apply(e.event)
        finally:
            self._replaying = False

    def _apply(self, ev: Dict[str, Any]) -> None:
        t = ev["type"]
        if t == "genesis":
            self.tick = ev["tick"]
            for label, c in ev["caps"].items():
                cap = self.caps.mint(c["kind"], c["rights"], SHELL, c.get("target"), token=c["token"])
                self.shell_caps[label] = cap.token
        elif t == "put":
            self.store.put(Obj.from_dict(ev["obj"]))
        elif t == "mint":
            self.caps.mint(ev["kind"], ev["rights"], ev["holder"], ev.get("target"), token=ev["token"])
            if ev["holder"] == SHELL:
                self.shell_caps[ev["label"]] = ev["token"]
        elif t == "grant":
            self.caps.attenuate(ev["parent"], ev["by"], ev["rights"], ev["holder"], new_token=ev["token"])
            self._bind(ev["holder"], ev["label"], ev["token"])
        elif t == "revoke":
            self.caps.revoke(ev["token"], ev["by"])
        elif t == "spawn":
            self._spawn(ev["cell"], ev["code"], ev.get("fuel", DEFAULT_FUEL))
        elif t == "send":
            self.cells[ev["to"]].inbox.append((ev["from"], ev["body"]))
        elif t == "ran":
            # Effects of the run (puts, sends...) are recorded as their own events;
            # the handler is deliberately NOT re-executed on replay.
            self.tick = ev["tick"]
            c = self.cells.get(ev["cell"])
            if c:
                if c.inbox:
                    c.inbox.popleft()          # the message this run consumed
                c.runs += 1
                c.fuel_used += ev["fuel"]
                if not ev["ok"]:
                    c.errors += 1
        elif t == "kill":
            self.cells.pop(ev["cell"], None)
        elif t == "tick":
            self.tick = ev["tick"]
        else:
            raise KernelError(f"unknown ledger event {t!r}")

    def _genesis(self) -> None:
        """The human's seat: the shell holds the initial capabilities. Nobody is root."""
        caps = {
            "store":   {"kind": "store",   "rights": sorted(RIGHTS["store"])},
            "console": {"kind": "console", "rights": sorted(RIGHTS["console"])},
            "clock":   {"kind": "clock",   "rights": sorted(RIGHTS["clock"])},
            "kernel":  {"kind": "kernel",  "rights": sorted(RIGHTS["kernel"])},
        }
        for label, c in caps.items():
            cap = self.caps.mint(c["kind"], c["rights"], SHELL)
            c["token"] = cap.token
            self.shell_caps[label] = cap.token
        self._record({"type": "genesis", "tick": 0, "caps": caps})

    # ------------------------------------------------------------------ store ops

    def put(self, kind: str, data: Any, tags, by: str, op: str, inputs=()) -> str:
        obj = Obj(kind=kind, data=data, tags=tuple(tags), by=by, op=op, inputs=tuple(inputs), tick=self.tick)
        h = self.store.put(obj)
        self._record({"type": "put", "obj": obj.to_dict()})
        return h

    # ------------------------------------------------------------------ cells

    def _bind(self, holder: str, label: str, token: str) -> None:
        if holder == SHELL:
            self.shell_caps[label] = token
        else:
            self.cells[holder].env.vars[label] = vm.CapHandle(token, label)

    def _spawn(self, name: str, code_hash: str, fuel: int) -> Cell:
        code = self.store.get(code_hash)
        if code is None or code.kind != "code":
            raise KernelError("spawn: code hash does not name a code object")
        if name in self.cells:
            raise KernelError(f"a cell named {name!r} already exists")
        env = vm.new_env()
        cell = Cell(id=name, code=code_hash, env=env, fuel=fuel)
        self.cells[name] = cell
        # Module load has no capabilities bound yet: it can only define functions.
        try:
            vm.load_module(code.data, env, self._make_syscall(cell), fuel)
        except vm.VMError as e:
            del self.cells[name]
            raise KernelError(f"spawn: cell code failed to load: {e}")
        return cell

    def spawn(self, name: str, code_hash: str, by: str, fuel: int = DEFAULT_FUEL) -> Cell:
        self.caps.check(self._cap_of(by, "kernel"), by, "spawn")
        cell = self._spawn(name, code_hash, fuel)
        self._record({"type": "spawn", "cell": name, "code": code_hash, "fuel": fuel, "by": by})
        return cell

    def kill(self, name: str, by: str) -> None:
        self.caps.check(self._cap_of(by, "kernel"), by, "spawn")
        if name not in self.cells:
            raise KernelError(f"no cell {name!r}")
        del self.cells[name]
        self._record({"type": "kill", "cell": name, "by": by})

    def grant(self, parent_token: str, by: str, rights, holder: str, label: str) -> Cap:
        """Attenuate one of `by`'s capabilities and hand it to `holder` under `label`."""
        if holder != SHELL and holder not in self.cells:
            raise KernelError(f"no cell {holder!r}")
        cap = self.caps.attenuate(parent_token, by, rights, holder)
        self._bind(holder, label, cap.token)
        self._record({"type": "grant", "parent": parent_token, "by": by, "rights": sorted(cap.rights),
                      "holder": holder, "label": label, "token": cap.token})
        return cap

    def mint_cell_cap(self, target: str, by: str, label: Optional[str] = None) -> Cap:
        """A 'cell' capability lets its holder send messages to `target`. Needs kernel.mint."""
        self.caps.check(self._cap_of(by, "kernel"), by, "mint")
        if target not in self.cells:
            raise KernelError(f"no cell {target!r}")
        label = label or f"cell:{target}"
        cap = self.caps.mint("cell", {"send"}, by, target=target)
        self._bind(by, label, cap.token)
        self._record({"type": "mint", "kind": "cell", "rights": ["send"], "holder": by,
                      "target": target, "label": label, "token": cap.token})
        return cap

    def revoke(self, token: str, by: str) -> List[str]:
        revoked = self.caps.revoke(token, by)
        self._record({"type": "revoke", "token": token, "by": by})
        return revoked

    def send(self, to: str, body: Any, by: str, token: Optional[str] = None) -> None:
        if to not in self.cells:
            raise KernelError(f"no cell {to!r}")
        if by != SHELL:
            cap = self.caps.check(token, by, "send")
            if cap.target != to:
                raise CapError("capability targets a different cell")
        self.cells[to].inbox.append((by, body))
        self._record({"type": "send", "from": by, "to": to, "body": body})

    def _cap_of(self, who: str, kind: str) -> str:
        if who == SHELL:
            tok = self.shell_caps.get(kind)
            if tok is None:
                raise CapError(f"shell holds no {kind} capability")
            return tok
        raise CapError(f"{who} must present a capability explicitly")

    # ------------------------------------------------------------------ syscall gate

    def _make_syscall(self, cell: Cell):
        def syscall(handle: vm.CapHandle, op: str, args: list) -> Any:
            try:
                return self.syscall(cell.id, handle.token, op, args)
            except (CapError, KernelError) as e:
                raise vm.VMError(f"denied: {e}")
        return syscall

    def syscall(self, caller: str, token: str, op: str, args: list) -> Any:
        cap = self.caps.check(token, caller, op)       # kind-agnostic right check
        kind = cap.kind
        if op == "delegate":
            # (call <cap> "delegate" <link-cap> "rights" "label"): a narrower copy for the linked cell
            if len(args) != 3 or not isinstance(args[0], vm.CapHandle):
                raise KernelError('delegate: (call cap "delegate" link "rights" "label")')
            link = self.caps.check(args[0].token, caller, "send")
            if link.kind != "cell":
                raise KernelError("delegate: second argument must be a cell capability")
            rights = [r for r in str(args[1]).split(",") if r]
            self.grant(token, caller, rights, link.target, str(args[2]))
            return None
        if kind == "store":
            return self._sys_store(caller, op, args)
        if kind == "console":
            line = " ".join(vm.show(a) for a in args)
            self.emit(f"[{caller}] {line}")
            return None
        if kind == "clock":
            return self.tick
        if kind == "cell":
            if len(args) != 1:
                raise KernelError('send: (call cap "send" body)')
            self.send(cap.target, args[0], caller, token)
            return None
        if kind == "kernel":
            if op == "cells":
                return sorted(self.cells)
            raise KernelError(f"kernel.{op} is only available from the shell")
        raise KernelError(f"unknown capability kind {kind}")

    def _sys_store(self, caller: str, op: str, args: list) -> Any:
        if op == "get":
            h = self.store.resolve(str(args[0])) if args else None
            obj = self.store.get(h) if h else None
            if obj is None:
                raise KernelError("no such object")
            return {"hash": h, "kind": obj.kind, "data": obj.data, "tags": list(obj.tags), "by": obj.by}
        if op == "put":
            if len(args) < 2:
                raise KernelError('put: (call store "put" kind data [tags] [inputs])')
            kind, data = str(args[0]), args[1]
            tags = [str(t) for t in (args[2] if len(args) > 2 and isinstance(args[2], list) else [])]
            inputs = [str(i) for i in (args[3] if len(args) > 3 and isinstance(args[3], list) else [])]
            for i in inputs:
                if self.store.get(i) is None:
                    raise KernelError(f"put: unknown input {i[:12]}")
            return self.put(kind, data, tags, by=caller, op="run", inputs=inputs)
        if op == "find":
            return self.store.find(str(args[0])) if args else []
        if op == "lineage":
            h = self.store.resolve(str(args[0])) if args else None
            return [{"depth": d, "hash": hh, "kind": o.kind, "by": o.by} for d, hh, o in self.store.lineage(h or "")]
        raise KernelError(f"store.{op} does not exist")

    # ------------------------------------------------------------------ scheduler

    def emit(self, line: str) -> None:
        self.console_lines.append(line)
        if self._console and not self._replaying:
            self._console(line)

    def run(self, max_ticks: int = 100) -> int:
        """Cooperative round-robin: one message per cell per tick, each within its fuel budget."""
        ticks = 0
        while ticks < max_ticks and any(c.inbox for c in self.cells.values()):
            self.tick += 1
            ticks += 1
            for cell in list(self.cells.values()):
                if not cell.inbox:
                    continue
                sender, body = cell.inbox.popleft()
                self._run_cell(cell, sender, body)
        return ticks

    def _run_cell(self, cell: Cell, sender: str, body: Any) -> None:
        handler = cell.env.vars.get("on-message")
        interp = vm.Interp(self._make_syscall(cell), cell.fuel)
        ok, err = True, None
        try:
            if not isinstance(handler, vm.Closure):
                raise vm.VMError("cell defines no (on-message from body)")
            interp.apply(handler, [sender, body])
        except vm.VMError as e:
            ok, err = False, str(e)
            self.emit(f"[kernel] cell {cell.id} stopped: {err}")
        except RecursionError:
            ok, err = False, "recursion limit"
            self.emit(f"[kernel] cell {cell.id} stopped: {err}")
        cell.runs += 1
        cell.fuel_used += interp.used
        if not ok:
            cell.errors += 1
        self._record({"type": "ran", "cell": cell.id, "from": sender, "tick": self.tick,
                      "fuel": interp.used, "ok": ok, "error": err})

    # ------------------------------------------------------------------ time travel

    def fork(self, upto_seq: int, state_dir: Optional[str] = None) -> "Kernel":
        """A new kernel whose history is this ledger's first `upto_seq` entries."""
        if not 0 < upto_seq <= len(self.ledger.entries):
            raise KernelError(f"seq must be in 1..{len(self.ledger.entries)}")
        k = Kernel.__new__(Kernel)
        k.state_dir = state_dir
        k.store, k.caps, k.cells, k.tick = Store(), CapTable(), {}, 0
        k.console_lines, k._console, k._replaying, k.shell_caps = [], self._console, False, {}
        path = None
        if state_dir:
            os.makedirs(state_dir, exist_ok=True)
            path = os.path.join(state_dir, "ledger.jsonl")
            if os.path.exists(path):
                os.remove(path)
        k.ledger = Ledger(path)
        for e in self.ledger.entries[:upto_seq]:
            k.ledger.append(e.event)             # re-append so the new file is written
        k._replay(k.ledger.entries)
        return k
