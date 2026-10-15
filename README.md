# VERUM OS

A small operating system for running code you do not trust, seeing exactly
what it touched, and proving what it produced. It is built on four rules that
Windows, macOS/iOS, Android and Linux all break. Each rule removes an entire
class of attack *by construction* instead of by patching, and each one is also
what makes the system good for real work: everything you make is
reproducible, traceable and undoable.

**What is new here, and what is not.** None of the four rules is original:
capability-only authority is KeyKOS/EROS/seL4/Fuchsia; content addressing is
Unison and Nix; the ledger-as-system is Urbit and event sourcing; fuel is
Ethereum gas and Wasmtime fuel. VERUM is a *synthesis* of those four in one
small system, hosted and on bare metal, with per-object provenance, a fuzzer
that attacks its invariants, and a Sanskrit surface language. See
[Prior art](#prior-art) below for the citations.

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

### संस्कृतलिपि: programming in Sanskrit

Verum Script is a Lisp, so its syntax is only parentheses; every word can be
Sanskrit. `verum/sanskrit.py` names every special form, builtin, capability
and operation in Sanskrit, and Devanagari digits read as numbers. The reader
maps each Sanskrit word to its canonical form while parsing, so a Sanskrit
program and the same program in English are literally the same program to
the kernel: same hash, same provenance, same capabilities. Both kernels
support it; [cells/ganana.sv](cells/ganana.sv) is the word counter in Sanskrit:

```lisp
(परिभाषा (सन्देशे प्रेषकः विषयः)
  (अस्तु ((वस्तु (आह्वानम् भाण्डारः "प्राप्" विषयः))
          (लेखः  (गृहाण वस्तु "data")))
    (आह्वानम् पटलः "मुद्रय" "शब्दाः" (परिमाणम् (विभज लेखः " ")))))
```

| Sanskrit | meaning | canonical |
|---|---|---|
| परिभाषा | definition | `define` |
| कार्यम् | work, function | `lambda` |
| अस्तु | let it be | `let` |
| यदि / अन्यथा | if / otherwise | `if` / `else` |
| आह्वानम् | invocation | `call` (the only effect) |
| सन्देशे | upon a message | `on-message` |
| भाण्डारः / पटलः / घटिका | storehouse / screen / clock | `store` / `console` / `clock` |
| प्राप् / स्थापय / अन्विष्य / मुद्रय / प्रेषय | obtain / place / search / print / send | `get` / `put` / `find` / `print` / `send` |
| योगः वियोगः गुणनम् भागः | sum, difference, product, quotient | `+ - * /` |
| सूची कोशः वाक्यम् | list, dictionary, sentence | `list` `dict` `str` |
| प्रत्येकम् चयनम् संकलनम् | each / selection / collection | `map` `filter` `reduce` |
| सत्यम् असत्यम् शून्यम् | true / false / void | `true` `false` `nil` |

The full table is in `verum/sanskrit.py`. `sanskrit <file.vs>` in the shell
renders an English program in Sanskrit. Five tests check that the
vocabulary is total and that Sanskrit and English cells produce identical
results and provenance.

**Typing Sanskrit (S1).** Both shells transliterate from a Latin keyboard
using Harvard-Kyoto, the plain-ASCII convention of Sanskrit scholarship,
with the common spellings accepted too (`aa ii uu sh Sh chh`):

```
shell> lipi on
shell> eval (pratyekam (kAryam (ka) (guNanam ka ka)) (zreNI 5))
=> (0 1 4 9 16)
shell> note dharmo rakSati rakSitaH          -> stores धर्मो रक्षति रक्षितः
shell> lipi paribhASA                        -> परिभाषा
```

Rules: the intent word is never transliterated; English Verum Script words
and anything that already names something in the system (a tag, cell, seat
or program) stay Latin; `:word` forces transliteration even with lipi off;
`\word` forces Latin. With lipi on, the hosted shell also appends Sanskrit
to the kernel's refusals (`division by zero  (शून्येन भागः)`). Five tests
cover round-tripping the whole vocabulary and the shell rules.

**Devanagari on bare metal.** The VGA text mode cannot draw Devanagari, so
the kernel now has a framebuffer console (`metal/kernel/fb.c`): it switches
QEMU's VGA to 800×600×32 through the Bochs VBE ports, finds the linear
framebuffer via PCI, and draws text itself. Devanagari needs real shaping
(conjuncts like क्ष, vowel signs that move before or below their consonant),
which a 100 KiB kernel should not attempt, so `metal/fontgen.py` does it at
build time: it renders an 8×16 ASCII font and every Sanskrit word the kernel
can show (the whole vocabulary plus every Devanagari word in the embedded
cells) with Windows GDI and the Nirmala UI font into `kernel/font.h`. At run
time the console draws ASCII glyph by glyph and Devanagari word by word from
that atlas. Words the atlas has never seen (S2a) are split into aksharas
(syllables: a consonant chain, its vowel sign, and any anusvara or visarga)
and drawn from a second atlas of ~4,900 pre-shaped syllables, with
pixel-accurate advance, so freshly typed or computed Sanskrit renders too;
only a conjunct outside the shaped set falls back to its parts. The
generated `font.h` is committed so Linux CI builds without GDI; the kernel
is ~590 KiB with both atlases.

### One small program, three ways

`bench/primes.py` (plain Python) and `cells/abhajya.sv` (Sanskrit Verum
Script) count the primes below 200 by the same naive trial division.
`python -X utf8 bench/compare.py` runs both and measures; the bare-metal
column comes from `spawn q abhajya`, `send q 200`, `run`, `cells` in QEMU.
All three answer 46 primes, sum 4227.

| | Python | Sanskrit, hosted | Sanskrit, bare metal |
|---|---|---|---|
| code lines (non-comment) | 15 | 10 | 10 |
| source | 764 bytes | 758 chars / 1534 bytes UTF-8 | same |
| wall time, tree-walker | 0.3 ms | 37 ms | 50 ms |
| wall time, **S2b bytecode VM** | — | 38 ms | **20–30 ms** (10 ms timer) |
| peak memory, tree-walker | 1 KiB | 47 KiB | 2 075 KiB |
| peak memory, S2b bytecode VM | — | 7 KiB | ~1.5 MiB for n=200; n=500 now fits (frames pooled, S2c part 1) |
| interpreter steps | n/a | 139 716 | 139 716 |
| runs as | native CPython | cell in a VM inside Python | cell in a VM on the hardware |
| can reach beyond its grant | yes, all of Python | no | no |
| CPU bound | none | fuel | fuel + 50 ms slice |
| memory bound | none | size budget | 8 MiB arena per message |
| result carries provenance | no | yes | yes |
| recorded in a tamper-evident ledger | no | yes | yes, on disk |

Reading the numbers: the Sanskrit program is shorter than the Python one in
characters, and the same length in lines of meaning. Hosted, it runs ~100×
slower than native Python (an interpreter written in an interpreted
language; the bytecode VM gains memory there, not time, because Python's
own call overhead dominates). On bare metal the S2b bytecode VM brings the
tree-walker's 50 ms down to 20 ms: variables resolve at compile time to
frame slots, and references to builtins hit an inline cache instead of a
string search; the timer only resolves 10 ms, so the true figure is
somewhere in 15–25 ms. Memory per message is still large on bare metal
because a cell's arena is only reclaimed between messages, never within
one (S2c, the per-message collector, is next). That is the price of the
right-hand rows: the Python program can read any file and open any socket
on the machine; the cell cannot do anything it was not handed, cannot run
or allocate without limit, and leaves a provable record of what it did.

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

## Prior art

VERUM stands on established ideas; this is where each one comes from.

- **Capabilities, no ambient authority:** Dennis & Van Horn (1966); the
  Cambridge CAP; [KeyKOS](https://wikipedia.com/wiki/KeyKOS) and
  [EROS](https://en.wikipedia.org/wiki/EROS_(microkernel)) (Shapiro);
  seL4; [Fuchsia](https://fuchsia.dev/docs/concepts/principles/secure)
  ("a new process has no capabilities"); CHERI in hardware. VERUM's model is
  textbook KeyKOS lineage.
- **Content-addressed, immutable objects:**
  [Unison](https://www.unison-lang.org/docs/the-big-idea/) (definitions
  named by the hash of their syntax tree), Nix's content-addressed store,
  IPFS, Git.
- **The ledger is the system; boot is replay:**
  [Urbit/Arvo](https://github.com/endplay/omniplay/issues/3) (a
  deterministic event log from which all state is a pure function), and
  event sourcing generally. Urbit is VERUM's closest relative overall.
- **Fuel-bounded execution:** Ethereum gas, Erlang reductions,
  [Wasmtime fuel](https://agentpatterns.ai/security/wasm-sandbox-agent-code-execution/).
- **Programming in Sanskrit:** several projects precede this one, among them
  [Sankode](https://github.com/tovganesh/sankode),
  [Vedic](https://github.com/vedic-lang),
  [VakyaLang](https://dev.to/rajmitra/vakyalang-a-real-programming-language-where-sanskrit-is-the-code-30mj)
  and [Sanskriti](https://github.com/megaketu555/sanskriti). What VERUM adds
  is a Sanskrit surface over a canonical core (same hash, same provenance as
  the English program), on an OS, with a bare-metal Devanagari console.
- **Case-marked arguments (the S3 idea):** Rick Briggs,
  [*Knowledge Representation in Sanskrit and Artificial Intelligence*](https://ojs.aaai.org//index.php/aimagazine/article/view/466)
  (AI Magazine, 1985); Damian Conway's
  [Perligata](https://web.eecs.umich.edu/~imarkov/Perligata.html) (2000),
  Perl in Latin with arguments bound by case ending; and
  [*The Karaka Calling Convention*](https://zenodo.org/records/21916088)
  (2026), six Pāṇinian roles carried morphologically, with a reference
  implementation. S3 will build on that paper and put the new work where the
  gap is: kāraka roles at the operating-system boundary, where the
  instrument role is a capability and the compiler checks roles against
  grants.

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
