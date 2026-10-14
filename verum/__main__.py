"""Entry points:

    python -m verum            interactive shell, state in ./state
    python -m verum demo       scripted tour of the system (fresh, throwaway state)
    python -m verum <dir>      interactive shell with state in <dir>
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile

from .kernel import Kernel
from .shell import Shell, repl


HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CELLS = os.path.join(HERE, "cells")

DEMO = [
    "# --- 1. there are no files: objects are named by their content and found by tag",
    'note The quick brown fox jumps over the lazy dog',
    'note VERUM has no root user, so there is nothing to escalate to',
    "find note",
    "# --- 2. programs are cells: born with NO authority at all",
    f'code wordcount "{os.path.join(CELLS, "wordcount.vs")}"',
    f'code shout "{os.path.join(CELLS, "shout.vs")}"',
    "spawn wc wordcount",
    "spawn shout shout",
    "cells",
    "# --- 3. authority is granted explicitly, narrowed, and listed in the open",
    "grant wc store get,put",
    "grant wc console print",
    "grant shout store get,put",
    "link shout wc next",
    "cells",
    "# --- 4. work flows as messages; every result carries its provenance",
    "send shout (head (call-less-find))",   # placeholder replaced below
    "run",
    "find stats",
    "# --- 5. ask the system WHY an object exists",
    "why stats",
    "# --- 6. a hostile cell tries five classic attacks",
    f'code malware "{os.path.join(CELLS, "malware.vs")}"',
    "spawn mal malware",
    "grant mal console print",
    "send mal 1", "send mal 2", "send mal 3", "send mal 4", "send mal 5",
    "run",
    "cells",
    "# --- 7. revoke and the authority is gone - including anything derived from it",
    "revoke wc console",
    "send wc <note>",
    "run",
    "# --- 8. the ledger is the system: tamper-evident and rewindable",
    "verify",
    "ledger 8",
    "rewind 6",
    "cells",
    "find note",
]


def demo() -> None:
    tmp = tempfile.mkdtemp(prefix="verum-demo-")
    try:
        k = Kernel(os.path.join(tmp, "state"))
        sh = Shell(k)
        for line in DEMO:
            if line.startswith("#"):
                print("\n" + line)
                continue
            # resolve placeholders that need a hash from the live system
            if line.startswith("send shout") or line.startswith("send wc"):
                line = f"{line.split()[0]} {line.split()[1]} \"{sh.k.store.find('note')[0]}\""
            if line == "why stats":
                line = f"why {sh.k.store.find('stats')[0][:12]}"
            print(f"verum> {line}")
            sh.do(line)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv) -> None:
    if argv and argv[0] == "demo":
        demo()
    else:
        repl(argv[0] if argv else os.path.join(HERE, "state"))


if __name__ == "__main__":
    main(sys.argv[1:])
