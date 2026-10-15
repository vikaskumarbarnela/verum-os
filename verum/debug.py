"""D1 - the debugger for cells on the hosted kernel.

A cell runs inside the kernel's scheduler on the shell's thread. To pause it
mid-message we run the scheduler on a worker thread; the VM's per-instruction
trace hook (armed through the same `preempt` flag the timer uses on bare
metal) blocks that thread on a breakpoint or a step, and the shell thread
inspects the snapshot and decides when to resume.

Everything here is deterministic: the same ledger prefix and message pause
at the same instruction every time, so a breakpoint is reproducible.
"""

from __future__ import annotations

import threading
from typing import Any, Dict, List, Optional, Set

from . import bytecode, vm


class Debugger:
    def __init__(self) -> None:
        self.breakpoints: Dict[str, Set[int]] = {}      # cell -> source lines
        self.stepping = False                           # pause at the next new line
        self.profile_on = False
        self.profile: Dict[str, int] = {}               # function name -> instructions executed
        self.paused: Optional[dict] = None              # snapshot while a cell is paused
        self._pause = threading.Event()
        self._resume = threading.Event()
        self._last_line: Dict[str, int] = {}

    # ---------------------------------------------------------------- configuration

    def add_break(self, cell: str, line: int) -> None:
        self.breakpoints.setdefault(cell, set()).add(line)

    def remove_break(self, cell: str, line: Optional[int] = None) -> None:
        if line is None:
            self.breakpoints.pop(cell, None)
        else:
            self.breakpoints.get(cell, set()).discard(line)

    def wants(self, cell: str) -> bool:
        return bool(self.breakpoints.get(cell)) or self.stepping or self.profile_on

    # ---------------------------------------------------------------- VM side (worker thread)

    def attach(self, machine: bytecode.VM, cell: str) -> None:
        machine.trace = lambda code, pc, stack, frame: self._hook(machine, cell, code, pc, stack, frame)
        machine.preempt = True                          # the VM calls the hook before its next instruction

    def _hook(self, machine: bytecode.VM, cell: str, code: bytecode.Code, pc: int, stack: list, frame: bytecode.Frame) -> None:
        machine.preempt = True                          # stay armed for the next instruction
        if self.profile_on:
            self.profile[code.name] = self.profile.get(code.name, 0) + 1
        line = code.lines[pc] if pc < len(code.lines) else 0
        if not line or line == self._last_line.get(cell):
            return
        self._last_line[cell] = line
        entering = pc == 0 or code.lines[pc - 1] != line        # the first instruction of this line in this function
        if self.stepping or (entering and line in self.breakpoints.get(cell, ())):
            self.paused = {"cell": cell, "code": code, "pc": pc, "line": line,
                           "stack": list(stack), "frame": frame, "vm": machine}
            self.stepping = False
            self._pause.set()
            self._resume.wait()                         # the shell thread decides when we go on
            self._resume.clear()
            self.paused = None

    # ---------------------------------------------------------------- shell side

    def wait_pause(self, worker: threading.Thread, timeout: float = 30.0) -> bool:
        """Block until the cell pauses (True) or the scheduler finishes (False)."""
        import time
        end = time.time() + timeout
        while time.time() < end:
            if self._pause.wait(0.02):
                self._pause.clear()
                return True
            if not worker.is_alive():
                return False
        return False

    def resume(self, step: bool = False) -> None:
        self.stepping = step
        self._resume.set()

    def finish(self, cell: str) -> None:
        self._last_line.pop(cell, None)

    # ---------------------------------------------------------------- views

    def frame_view(self) -> List[tuple]:
        """[(name, value)] for the paused frame, innermost first, then each enclosing frame."""
        p = self.paused
        if not p:
            return []
        out: List[tuple] = []
        code, frame = p["code"], p["frame"]
        names = code.slotnames
        for i, v in enumerate(frame.slots):
            out.append((names[i] if i < len(names) else f"slot{i}", v))
        return out

    def profile_view(self, top: int = 12) -> List[tuple]:
        total = sum(self.profile.values()) or 1
        rows = sorted(self.profile.items(), key=lambda kv: -kv[1])[:top]
        return [(name, n, 100.0 * n / total) for name, n in rows]


def source_line(src: str, line: int) -> str:
    lines = src.split("\n")
    return lines[line - 1].strip() if 0 < line <= len(lines) else ""
