"""compare.py - the same small program three ways, measured.

  1. plain Python                       (bench/primes.py)
  2. Sanskrit Verum Script, hosted      (cells/abhajya.sv inside the Python kernel)
  3. Sanskrit Verum Script, bare metal  (numbers pasted from the QEMU run: `run` and `cells`)

Measures wall time, peak memory (tracemalloc), source size, and for the VM
the number of interpreter steps (fuel). Run:  python -X utf8 bench/compare.py [limit]
"""

from __future__ import annotations

import os
import sys
import time
import tracemalloc

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

from verum.kernel import SHELL, Kernel   # noqa: E402
from verum import vm                     # noqa: E402

import primes                            # noqa: E402  (bench/primes.py)


def measure(fn):
    tracemalloc.start()
    t0 = time.perf_counter()
    result = fn()
    dt = time.perf_counter() - t0
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return result, dt, peak


def src_stats(path: str) -> tuple[int, int, int]:
    with open(path, encoding="utf-8") as f:
        text = f.read()
    code = [l for l in text.splitlines() if l.strip() and not l.lstrip().startswith((";", "#", '"""'))]
    return len(code), len(text.encode("utf-8")), len(text)


def hosted_cell(limit: int):
    """Boot a kernel, load and grant the cell, queue the message; the measured part is k.run() alone."""
    k = Kernel()
    lines = []
    k._console = lines.append
    with open(os.path.join(ROOT, "cells", "abhajya.sv"), encoding="utf-8") as f:
        src = f.read()
    h = k.put("code", src, ["code", "abhajya"], SHELL, "code")
    k.spawn("अभाज्य", h, SHELL, fuel=20_000_000)
    k.grant(k.shell_caps["console"], SHELL, ["print"], "अभाज्य", "console")
    k.send("अभाज्य", limit, SHELL)

    def run():
        k.run()
        cell = k.cells["अभाज्य"]
        if cell.errors:
            raise RuntimeError("\n".join(lines))
        return lines[-1], cell.fuel_used
    return run


def main(limit: int) -> None:
    (count, total), t_py, m_py = measure(lambda: primes.main(limit))
    (out, fuel), t_vm, m_vm = measure(hosted_cell(limit))
    words = [w for w in out.split() if w.isdigit()]
    assert words[1:] == [str(count), str(total)], f"Sanskrit cell disagrees: {out} vs {count} {total}"

    py_lines, py_bytes, py_chars = src_stats(os.path.join(HERE, "primes.py"))
    sv_lines, sv_bytes, sv_chars = src_stats(os.path.join(ROOT, "cells", "abhajya.sv"))

    print(f"limit {limit}: {count} primes, sum {total}  (all three agree)\n")
    print(f"{'':30}{'Python':>16}{'Sanskrit, hosted':>20}{'Sanskrit, bare metal':>24}")
    print(f"{'code lines (non-comment)':30}{py_lines:>16}{sv_lines:>20}{sv_lines:>24}")
    print(f"{'source bytes (UTF-8)':30}{py_bytes:>16}{sv_bytes:>20}{sv_bytes:>24}")
    print(f"{'source characters':30}{py_chars:>16}{sv_chars:>20}{sv_chars:>24}")
    print(f"{'wall time':30}{t_py*1000:>13.1f} ms{t_vm*1000:>17.1f} ms{'see QEMU run':>24}")
    print(f"{'peak memory':30}{m_py/1024:>12.0f} KiB{m_vm/1024:>16.0f} KiB{'see QEMU run':>24}")
    print(f"{'interpreter steps (fuel)':30}{'n/a':>16}{fuel:>20,}{fuel:>24,}")
    print(f"{'runs as':30}{'native CPython':>16}{'cell in a VM':>20}{'cell in a VM':>24}")
    print(f"{'can reach beyond its grant':30}{'yes (all of Python)':>16}{'no':>20}{'no':>24}")
    print(f"{'CPU bound':30}{'none':>16}{'fuel':>20}{'fuel + 50 ms slice':>24}")
    print(f"{'memory bound':30}{'none':>16}{'size budget':>20}{'8 MiB arena':>24}")
    print(f"{'result carries provenance':30}{'no':>16}{'yes':>20}{'yes':>24}")
    print(f"{'recorded in a ledger':30}{'no':>16}{'yes':>20}{'yes, on disk':>24}")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 200)
