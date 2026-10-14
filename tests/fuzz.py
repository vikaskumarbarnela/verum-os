"""Fuzzer for the hosted kernel: random programs and random ledger lines.

The security story of VERUM rests on three invariants. This script tries to
break them with random input and reports the first counter-example:

  1. CONTAINMENT  no exception other than VMError / CapError / KernelError
                  ever escapes a cell run or a syscall; the kernel never crashes
  2. AUTHORITY    a cell never holds a capability it was not granted, and never
                  holds rights wider than the capability it was derived from
  3. INTEGRITY    the ledger always verifies, and a kernel rebuilt by replaying
                  it has the same objects, cells and capabilities

Run:  python -m tests.fuzz --iterations 20000 --seed 7
      python -m tests.fuzz --forever
"""

from __future__ import annotations

import argparse
import random
import sys
import time
from typing import List

from verum import vm
from verum.caps import CapError
from verum.kernel import SHELL, Kernel, KernelError

ATOMS = ['1', '0', '-7', '42', '"a"', '""', '"x y z"', 'true', 'false', 'nil', 'x', 'y', 'f', 'g', 'body', 'from',
         'store', 'console', 'next', 'bogus', '(list)', '(dict)']
BUILTINS = ['+', '-', '*', '/', 'mod', '=', '<', '>', 'not', 'list', 'head', 'tail', 'nth', 'len', 'append', 'cons',
            'range', 'reverse', 'sort', 'dict', 'get', 'keys', 'assoc', 'str', 'split', 'join', 'upper', 'lower',
            'contains', 'slice', 'error', 'map', 'filter', 'reduce', 'nil?', 'number?']
OPS = ['get', 'put', 'find', 'lineage', 'print', 'now', 'send', 'delegate', 'spawn', 'mint', 'revoke', 'cells', 'zzz']
CAPS = ['store', 'console', 'next', 'bogus', '"deadbeef"', '42', 'f']


def gen(rng: random.Random, depth: int = 0) -> str:
    """A random Verum Script expression, biased toward things that touch the gate."""
    if depth > 4 or rng.random() < 0.3:
        return rng.choice(ATOMS)
    k = rng.random()
    if k < 0.25:
        return f'(call {rng.choice(CAPS)} "{rng.choice(OPS)}" ' + ' '.join(gen(rng, depth + 1) for _ in range(rng.randint(0, 4))) + ')'
    if k < 0.35:
        return f'(if {gen(rng, depth + 1)} {gen(rng, depth + 1)} {gen(rng, depth + 1)})'
    if k < 0.45:
        return f'(let ((x {gen(rng, depth + 1)}) (y {gen(rng, depth + 1)})) {gen(rng, depth + 1)})'
    if k < 0.52:
        return f'(lambda (x) {gen(rng, depth + 1)})'
    if k < 0.58:
        return f'(f {gen(rng, depth + 1)})'           # recursion into the cell's own helper
    if k < 0.62:
        return f'(do {gen(rng, depth + 1)} {gen(rng, depth + 1)})'
    return f'({rng.choice(BUILTINS)} ' + ' '.join(gen(rng, depth + 1) for _ in range(rng.randint(0, 3))) + ')'


def gen_cell(rng: random.Random) -> str:
    return (f'(define (f x) {gen(rng)})\n'
            f'(define (on-message from body) {gen(rng)})\n')


def gen_garbage(rng: random.Random) -> str:
    """Syntactically broken or hostile source text."""
    base = gen_cell(rng)
    r = rng.random()
    if r < 0.3:
        i = rng.randrange(len(base) + 1)
        return base[:i] + rng.choice(['(', ')', '"', '\\', ';', ' ', '(call store "put" "x"']) + base[i:]
    if r < 0.5:
        return base.replace('(', '((', rng.randint(1, 3))
    if r < 0.6:
        return '(' * 500 + ')' * 500
    if r < 0.7:
        return '"' + 'A' * 300000 + '"'
    if r < 0.8:
        return '(define (on-message from body) (' + 'list ' * 300 + '))'
    return base


ALLOWED = (vm.VMError, CapError, KernelError)


class Fuzzer:
    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.k = Kernel()
        self.lines: List[str] = []
        self.k._console = self.lines.append
        self.granted = {}           # cell -> {label: (kind, rights)} as granted by the shell
        self.cells = []
        self.runs = 0

    # ------------------------------------------------------------ building a random world

    def random_world(self) -> None:
        self.k = Kernel()
        self.k._console = self.lines.append
        self.granted = {}
        self.cells = []
        for i in range(self.rng.randint(1, 4)):
            src = gen_garbage(self.rng) if self.rng.random() < 0.2 else gen_cell(self.rng)
            try:
                vm.parse(src)
            except vm.VMError:
                continue
            h = self.k.put("code", src, ["code"], SHELL, "code")
            name = f"c{i}"
            try:
                self.k.spawn(name, h, SHELL)
            except ALLOWED:
                continue
            self.cells.append(name)
            self.granted[name] = {}
            if self.rng.random() < 0.8:
                rights = self.rng.sample(["get", "put", "find", "lineage", "delegate"], self.rng.randint(1, 4))
                self.k.grant(self.k.shell_caps["store"], SHELL, rights, name, "store")
                self.granted[name]["store"] = ("store", frozenset(rights))
            if self.rng.random() < 0.7:
                self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], name, "console")
                self.granted[name]["console"] = ("console", frozenset({"print"}))
        for name in self.cells:                      # random links
            if self.rng.random() < 0.6 and len(self.cells) > 1:
                target = self.rng.choice(self.cells)
                cap = self.k.mint_cell_cap(target, SHELL)
                self.k.grant(cap.token, SHELL, ["send"], name, "next")
                self.granted[name]["next"] = ("cell", frozenset({"send"}))
        for _ in range(self.rng.randint(1, 3)):
            self.k.put("note", "".join(self.rng.choice("ab c") for _ in range(self.rng.randint(0, 40))), ["note"], SHELL, "note")

    # ------------------------------------------------------------ invariants

    def check_authority(self) -> None:
        for name in self.cells:
            cell = self.k.cells.get(name)
            if cell is None:
                continue
            for label, v in cell.env.vars.items():
                if not isinstance(v, vm.CapHandle):
                    continue
                cap = self.k.caps.get(v.token)
                assert cap is not None, f"{name} holds a handle to a non-existent capability"
                assert cap.holder == name, f"{name} holds a capability whose holder is {cap.holder}"
                # walk up to the shell's cap: rights must only ever narrow
                cur = cap
                while cur.parent:
                    parent = self.k.caps.get(cur.parent)
                    assert cur.rights <= parent.rights, f"{name}: {label} has rights {sorted(cur.rights)} wider than its parent {sorted(parent.rights)}"
                    cur = parent
                assert cur.holder == SHELL, f"{name}: {label} does not descend from a genesis capability"
                if label in self.granted.get(name, {}):
                    kind, rights = self.granted[name][label]
                    assert cap.kind == kind and cap.rights <= rights, f"{name}: {label} escalated from {sorted(rights)} to {sorted(cap.rights)}"

    def check_integrity(self) -> None:
        ok, why = self.k.ledger.verify()
        assert ok, why
        # replay into a fresh kernel and compare
        k2 = self.k.fork(len(self.k.ledger.entries))     # a fresh kernel built only from the ledger
        assert set(k2.store) == set(self.k.store), "replayed store differs"
        assert set(k2.cells) == set(self.k.cells), "replayed cells differ"
        assert {c.token for c in k2.caps.all()} == {c.token for c in self.k.caps.all()}, "replayed capabilities differ"
        assert {c.token for c in k2.caps.all() if c.revoked} == {c.token for c in self.k.caps.all() if c.revoked}, "replayed revocations differ"

    # ------------------------------------------------------------ one iteration

    def step(self) -> None:
        self.random_world()
        for _ in range(self.rng.randint(1, 6)):
            if not self.cells:
                break
            target = self.rng.choice(self.cells)
            body = self.rng.choice([1, 2, "x", self.rng.choice(list(self.k.store)) if len(self.k.store) else "x", ["a", 1], {"k": 1}, None])
            try:
                self.k.send(target, body, SHELL)
            except ALLOWED:
                pass
        try:
            self.k.run(max_ticks=20)
        except ALLOWED:
            pass
        except RecursionError:
            raise AssertionError("RecursionError escaped the VM")
        self.runs += 1
        self.check_authority()
        if self.rng.random() < 0.1:                      # replay is slow-ish; sample it
            self.check_integrity()
        if self.rng.random() < 0.3 and self.cells:       # revoke something and make sure it is really gone
            name = self.rng.choice(self.cells)
            cell = self.k.cells.get(name)
            handles = [v for v in cell.env.vars.values() if isinstance(v, vm.CapHandle)] if cell else []
            if handles:
                h = self.rng.choice(handles)
                self.k.revoke(h.token, SHELL)
                try:
                    self.k.syscall(name, h.token, "print", ["x"])
                    raise AssertionError("revoked capability still works")
                except CapError:
                    pass


def main(argv) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--forever", action="store_true")
    a = ap.parse_args(argv)
    sys.setrecursionlimit(10000)
    fz = Fuzzer(a.seed)
    print(f"fuzz: seed {a.seed}")
    start = time.time()
    i = 0
    while a.forever or i < a.iterations:
        try:
            fz.step()
        except AssertionError as e:
            print(f"\nINVARIANT BROKEN at iteration {i} (seed {a.seed}): {e}")
            return 1
        except Exception as e:                            # anything else is a containment failure
            print(f"\nCONTAINMENT BROKEN at iteration {i} (seed {a.seed}): {type(e).__name__}: {e}")
            return 1
        i += 1
        if i % 500 == 0:
            print(f"  {i} worlds, {fz.runs} runs, {time.time() - start:.1f}s")
    print(f"ok: {i} random worlds, all invariants held ({time.time() - start:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
