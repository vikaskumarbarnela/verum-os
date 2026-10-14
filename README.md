# VERUM OS

A small operating system built on four rules that Windows, macOS/iOS, Android
and Linux all break. Each rule removes an entire class of attack *by
construction* instead of by patching, and each one is also what makes the
system good for real work: everything you make is reproducible, traceable and
undoable.

```
python -m verum demo      # guided tour (throwaway state)
python -m verum           # interactive shell, state persisted in ./state
python -m unittest discover -s tests -t .
```

Requires only Python 3.10+. No other dependencies.

## The four rules

| Conventional OS | VERUM | What it kills |
|---|---|---|
| Files in folders, named by path, mutable in place | **Objects named by the SHA-256 of their content**, immutable, found by tag. Every object records *who* made it, *how*, and *from what*. | Tampering, silent corruption, "which version is this?", ransomware (nothing can be overwritten) |
| `root` / Administrator, user accounts, permission bits | **No root. No users. Only capabilities.** A principal can do exactly what the unforgeable tokens it holds allow. Caps can be narrowed and revoked, never widened. | Privilege escalation (there is no privilege to reach), confused-deputy, "the app asked for all permissions" |
| Apps & processes running native code with ambient access to the machine | **Cells** running only *Verum Script* inside a kernel-owned interpreter, with a fuel budget, a size budget, no memory between messages, and exactly one way out: `(call <cap> "op" ...)`. A cell cannot see other cells, the store, the clock or the console unless handed a cap to them. | Memory corruption, syscall abuse, infinite loops / fork bombs, data exfiltration, spyware |
| Mutable system state, logs that can be edited | **The ledger *is* the system.** Every change is a hash-chained event; booting is replay; `rewind N` forks the universe at any point. | Log tampering, rootkits, irreversible mistakes |

## How a cell gets to do anything

```
verum> spawn wc wordcount          # born with NO authority at all
verum> grant wc store get,put      # a narrowed copy of *your* store cap
verum> grant wc console print
verum> link shout wc next          # shout may send messages to wc, nothing else
verum> cells                       # every cell's authority, listed in the open
  wc     store=store[get,put] held by wc
         console=console[print] held by wc
```

The hostile cell in [cells/malware.vs](cells/malware.vs) tries the five
classic attacks - forged capability, using a cap for the wrong operation,
CPU starvation, memory exhaustion, stack overflow. The kernel stops each one
and keeps running:

```
[kernel] cell mal stopped: call: first argument must be a capability
[kernel] cell mal stopped: denied: capability lacks right 'get' (has ['print'])
[kernel] cell mal stopped: fuel exhausted after 50000 steps
[kernel] cell mal stopped: value exceeds size budget (200000)
[kernel] cell mal stopped: call depth exceeded
```

## Why it's good for *work*, not just safe

Run a pipeline and ask the system why a result exists:

```
verum> why fc6dac8953bc
  fc6dac8953bc stats <- wc    via run  @tick 2: {words: 9, chars: 43, lines: 1}
      7889982f5dfd note <- shout via run  @tick 1: THE QUICK BROWN FOX ...
          98ba4e3d9315 note <- shell via note @tick 0: The quick brown fox ...
```

Every object knows its lineage. Cells are pure (no hidden state), so any
result can be recomputed from its inputs. `verify` proves nobody edited
history; `rewind 6` gives you the whole system as it was at ledger entry 6,
as a separate fork, with the original untouched.

## Layout

```
verum/store.py    content-addressed objects, provenance, hash-chained ledger
verum/caps.py     mint / attenuate / revoke; the check every syscall passes
verum/vm.py       Verum Script interpreter: reader, evaluator, fuel & size budgets
verum/kernel.py   cells, syscall gate, scheduler, event sourcing, fork()
verum/shell.py    the human's seat: intents, consent, audit, time travel
cells/*.vs        example cells: wordcount, shout, malware
tests/            17 tests, mostly attacks that must fail
```

## Bare metal

The same four rules, ported to C with nothing underneath - it boots on an
x86 PC (or QEMU) with no Windows, Linux or BIOS services:

```
cd metal
python build.py run     # build with nasm + clang/lld, boot in QEMU
python build.py test    # headless: types a script in, dumps the screen to build/*.png
```

```
metal/boot.asm          Multiboot header, stack, setjmp/longjmp for the VM
metal/linker.ld         kernel at 1 MiB
metal/kernel/lib.c      memory, strings, VGA text console + status bar, PS/2 keyboard, ATA PIO disk, SHA-256
metal/kernel/vs.c       Verum Script interpreter (reader, evaluator, fuel & size budgets, serializer)
metal/kernel/store.c    content-addressed objects + hash-chained ledger
metal/kernel/disk.c     the ledger on disk: header sector + append-only byte stream
metal/kernel/irq.c      IDT, PIC remap, PIT timer at 100 Hz, keyboard IRQ, exception panel, time-slice preemption
metal/kernel/caps.c     mint / attenuate / revoke / check
metal/kernel/kernel.c   cells, syscall gate, scheduler, event recording, replay, rewind
metal/kernel/cells.c    code objects embedded in the image (there is no filesystem to load from)
metal/kernel/shell.c    the human's seat: intents, `history`, confirmed `rewind`, `dash` panel view
```

Toolchain on Windows: `winget install NASM.NASM LLVM.LLVM SoftwareFreedomConservancy.QEMU`.
The 70 KiB kernel needs no libc, no cross-GCC: clang targets `i386-elf`
directly and `ld.lld` links it.

### Persistence and rewind on bare metal

`build.py` attaches a raw 8 MiB image (`build/verum.img`) as the primary
ATA disk. Every ledger event is appended to it as one line; on boot the
kernel reads the stream back, verifies the hash chain and replays it, so
objects, cells, capabilities and counters survive a power cycle.

```
verum> history            numbered timeline of every event
verum> rewind 8           shows what would be discarded, asks you to type yes,
                          rebuilds the system from the first 8 entries and truncates the disk
verum> dash               panel view: cells, ledger tail, store totals, disk status
```

The status bar on row 0 always shows objects, cells, ledger length, tick and
how many entries the disk holds. `build.py test` exercises all of this:
it boots, works, kills QEMU, boots again from the disk, rewinds, reboots
once more and screenshots each stage into `build/*.png`.

### Nothing is lost: the attic

`rewind n` no longer destroys history. The discarded entries are copied to
a second region of the disk (the attic) as a *branch* before the ledger is
shortened. `attic` lists branches; `restore k` re-applies one, which is
allowed only when the ledger is back at that branch's starting point, so the
hash chain stays consistent.

### Seats: more than one human

A seat is a named human principal. `seat alice` creates one with no
capabilities; `grant alice store get,find` hands her a narrowed copy of
yours; `switch alice` puts her at the keyboard. From then on every intent
is checked against *her* capabilities: she cannot `note` without
`store[put]`, cannot `spawn` without `kernel[spawn]`, and can only grant
onward what she holds. There is still no root: the shell seat is simply the
one seated at genesis.

### Cells can delegate

A capability granted with the `delegate` right may be passed on by its
holder, narrowed, to a cell it is linked to:

```
(call store "delegate" next "get" "store")   ; next = a cell->x[send] capability
```

The kernel checks the delegate right, the link, and that the rights are a
subset of the holder's; the new capability's parent chain leads back through
the delegating cell, so revoking the original revokes the copy. The Python
version has the same syscall and three tests for it.

### Seats with passphrases, alarms, and background work

`seat bob hunter2` creates a seat that `switch bob hunter2` must unlock;
only a salted SHA-256 is stored, and every login attempt, failed or not,
is a ledger event. The hosted kernel has the same (`seat`, `switch`,
`whoami`, `seats`, 4 tests).

`alarm e 1 "tick"` makes the clock send cell `e` the message `"tick"`
every second (recorded in the ledger, so it survives a reboot). With
`bg on`, cells and alarms run while the shell waits for a key: the
keyboard idle loop hands the CPU to the scheduler one round at a time.
Background output does not scribble over what you are typing; it goes to
`log`, and the status bar shows how many lines are waiting.

### Fuzzing and CI

`python -m tests.fuzz --iterations 20000 --seed 7` builds random worlds
(random cells, grants, links, hostile programs, broken source) and checks
three invariants on every one: nothing but a VM/capability/kernel error
ever escapes a run, no cell ever holds a capability it was not granted or
rights wider than its parent, and the ledger verifies and replays to the
same state. Its first run found a real containment bug (a value holding a
capability handle could reach the ledger encoder and crash the kernel);
both kernels now flatten data crossing into the store or an inbox.
`.github/workflows/ci.yml` runs the unit tests, a short fuzz, the demo,
and the bare-metal build on every push.

### Interrupts, timer and preemptive multitasking on bare metal

`irq.c` installs a GDT/IDT, remaps the PIC, runs the PIT at 100 Hz and takes
the keyboard by interrupt (the CPU halts between keys instead of spinning).
CPU exceptions print a register panel and halt rather than triple-faulting;
`trap` raises one deliberately so you can see it, and `uptime` reads the timer.

Every cell with work runs on its own kernel stack. The scheduler gives each
runnable cell a **50 ms time slice**; when the timer sees the slice expire
it sets a flag, and the VM yields at its next step (`ctx_switch` in
`boot.asm`), so a cell is paused mid-message and resumed later. Switches
happen only between VM steps, never inside a syscall, so the ledger records
effects in their true order and replay stays deterministic. `cells` shows
`PAUSED mid-run` and how often a cell was preempted; `run` resumes paused
cells.

Each cell also has an **arena**: everything a message allocates lives there
and is dropped when the next message starts. Only values that must outlive
the run (objects put in the store, message bodies, ledger text, capability
bindings) are deep-copied into the persistent heap. A run that fills its
arena ends with `memory budget exceeded`, which is now the real memory bound
(fuel still bounds steps at 2M).

## Honest limits

The Python version is a *hosted* kernel that models the machine; the C
version boots on bare metal but is single-core and VGA-text, with no paging
or network. Preemption ends a cell's run; it does not yet switch between
cells mid-run (the scheduler is still one message per cell per tick). The
disk driver is PIO, one sector at a time, on the primary ATA bus only. `rewind` on bare metal is
destructive (the discarded tail is truncated from the one disk), unlike the
Python version, which forks into a new directory. The security claims are
about the model - a cell cannot escape Verum Script - and hold as far as
the interpreter has no bugs. The scheduler is cooperative and deterministic
on purpose.
