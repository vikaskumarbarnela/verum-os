"""VERUM OS - a capability-secured, content-addressed, ledger-driven operating system.

Package layout:
    store.py   content-addressed object store + hash-chained ledger
    caps.py    unforgeable capabilities (mint / attenuate / revoke)
    vm.py      Verum Script: the only language cells may run in
    kernel.py  cells, scheduler, syscall gate, event sourcing, time travel
    shell.py   the user's seat: intent-style command shell
"""

__version__ = "0.1.0"
