"""S2b - the bytecode compiler and virtual machine for Verum Script.

The tree-walking interpreter in vm.py is the reference. This module compiles
the same forms into a flat instruction list and runs them in a tight loop:

    * variables are resolved at compile time to (depth, slot): a frame is a
      list of slots with a static link to the frame it was created in, so a
      reference costs a few list indexings instead of a string search through
      a chain of environments
    * fuel is charged per instruction; the same check point reads the
      preempt/step flag, so time slices, breakpoints and single-stepping are
      all one mechanism
    * every instruction carries the source line it came from, so errors say
      where ("... at line 7") and a debugger can map back to the text

The security model is untouched: the only effect is still CALLCAP, which
hands the capability handle, operation and arguments to the kernel's syscall
function exactly as sf_call did.
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional

from . import vm
from .vm import BUILTINS, CapHandle, HigherOrder, VMError, OutOfFuel, Symbol, MAX_DEPTH

# ---------------------------------------------------------------- instruction set
CONST, LOCAL, SETLOCAL, GLOBAL, SETGLOBAL, JUMP, JUMPIFNOT, JUMPIF, POP, DUP, \
    CALL, CALLCAP, CLOSURE, RET = range(14)
OPNAMES = ["CONST", "LOCAL", "SETLOCAL", "GLOBAL", "SETGLOBAL", "JUMP", "JUMPIFNOT", "JUMPIF", "POP", "DUP",
           "CALL", "CALLCAP", "CLOSURE", "RET"]


class Code:
    __slots__ = ("name", "nparams", "nslots", "instrs", "consts", "codes", "lines", "slotnames")

    def __init__(self, name: str, nparams: int) -> None:
        self.name = name
        self.nparams = nparams
        self.nslots = nparams
        self.instrs: List[tuple] = []      # (op, a, b)
        self.lines: List[int] = []         # source line per instruction
        self.consts: List[Any] = []
        self.codes: List[Code] = []
        self.slotnames: List[str] = []     # slot -> variable name, for the debugger's `inspect`

    def emit(self, op: int, a: Any = 0, b: Any = 0, line: int = 0) -> int:
        self.instrs.append((op, a, b))
        self.lines.append(line)
        return len(self.instrs) - 1

    def patch(self, at: int, target: int) -> None:
        op, _, b = self.instrs[at]
        self.instrs[at] = (op, target, b)

    def const(self, v: Any) -> int:
        self.consts.append(v)
        return len(self.consts) - 1

    def disassemble(self) -> str:
        out = [f"{self.name}/{self.nparams} slots={self.nslots}"]
        for i, ((op, a, b), ln) in enumerate(zip(self.instrs, self.lines)):
            arg = ""
            if op == CONST:
                arg = repr(self.consts[a])
            elif op in (LOCAL, SETLOCAL):
                arg = f"depth={a} slot={b}"
            elif op in (GLOBAL, SETGLOBAL):
                arg = self.consts[a]
            elif op in (JUMP, JUMPIFNOT, JUMPIF):
                arg = f"-> {a}"
            elif op in (CALL, CALLCAP):
                arg = f"argc={a}"
            elif op == CLOSURE:
                arg = self.codes[a].name
            out.append(f"  {i:4} L{ln:<3} {OPNAMES[op]:10} {arg}")
        for c in self.codes:
            out.append(c.disassemble())
        return "\n".join(out)


class Closure:
    __slots__ = ("code", "parent")

    def __init__(self, code: Code, parent: Optional["Frame"]) -> None:
        self.code, self.parent = code, parent

    def __repr__(self) -> str:
        return f"<fn {self.code.name}/{self.code.nparams}>"


class Frame:
    __slots__ = ("slots", "parent")

    def __init__(self, n: int, parent: Optional["Frame"]) -> None:
        self.slots = [None] * n
        self.parent = parent


# ---------------------------------------------------------------- compiler

class Scope:
    """Compile-time names of one function frame: name -> slot. Blocks (let) push shadowing maps."""

    def __init__(self, code: Code, parent: Optional["Scope"]) -> None:
        self.code = code
        self.parent = parent
        self.blocks: List[Dict[str, int]] = [{}]

    def lookup(self, name: str):
        for b in reversed(self.blocks):
            if name in b:
                return b[name]
        return None

    def declare(self, name: str) -> int:
        slot = self.code.nslots
        self.code.nslots += 1
        self.blocks[-1][name] = slot
        self.code.slotnames.append(name)
        return slot


class Compiler:
    def __init__(self, positions: Optional[Dict[int, int]] = None) -> None:
        self.positions = positions or {}

    def line(self, x: Any) -> int:
        return self.positions.get(id(x), 0)

    def compile_module(self, forms: List[Any]) -> Code:
        code = Code("<module>", 0)
        scope = Scope(code, None)
        scope.module = True                              # type: ignore[attr-defined]
        for i, f in enumerate(forms):
            self.expr(f, scope, code)
            if i < len(forms) - 1:
                code.emit(POP)
        if not forms:
            code.emit(CONST, code.const(None))
        code.emit(RET)
        return code

    def compile_expr(self, form: Any) -> Code:
        return self.compile_module([form])

    # -- variables: (depth, slot) through the static chain, else global
    def resolve(self, name: str, scope: Scope):
        depth, s = 0, scope
        while s is not None:
            if not getattr(s, "module", False):
                slot = s.lookup(name)
                if slot is not None:
                    return depth, slot
                depth += 1
            s = s.parent
        return None

    def expr(self, x: Any, scope: Scope, code: Code) -> None:
        ln = self.line(x) or getattr(self, "cur_line", 0)   # atoms inherit the enclosing form's line
        if isinstance(x, list) and ln:
            self.cur_line = ln
        if isinstance(x, Symbol):
            r = self.resolve(x, scope)
            if r is None:
                code.emit(GLOBAL, code.const(str(x)), 0, ln)
            else:
                code.emit(LOCAL, r[0], r[1], ln)
            return
        if not isinstance(x, list):
            code.emit(CONST, code.const(x), 0, ln)
            return
        if not x:
            code.emit(CONST, code.const([]), 0, ln)
            return
        head = x[0]
        if isinstance(head, Symbol) and head in vm.SPECIAL_FORMS:
            getattr(self, "sf_" + head)(x, scope, code, ln)
            return
        self.expr(head, scope, code)
        for a in x[1:]:
            self.expr(a, scope, code)
        code.emit(CALL, len(x) - 1, 0, ln)

    def body(self, forms: List[Any], start: int, scope: Scope, code: Code) -> None:
        if start >= len(forms):
            code.emit(CONST, code.const(None))
            return
        for i in range(start, len(forms)):
            self.expr(forms[i], scope, code)
            if i < len(forms) - 1:
                code.emit(POP)

    # -- special forms
    def sf_quote(self, x, scope, code, ln):
        if len(x) != 2:
            raise VMError("quote: one argument")
        code.emit(CONST, code.const(x[1]), 0, ln)

    def sf_if(self, x, scope, code, ln):
        if len(x) not in (3, 4):
            raise VMError("if: (if cond then [else])")
        self.expr(x[1], scope, code)
        j1 = code.emit(JUMPIFNOT, 0, 0, ln)
        self.expr(x[2], scope, code)
        j2 = code.emit(JUMP, 0, 0, ln)
        code.patch(j1, len(code.instrs))
        if len(x) == 4:
            self.expr(x[3], scope, code)
        else:
            code.emit(CONST, code.const(None))
        code.patch(j2, len(code.instrs))

    def sf_cond(self, x, scope, code, ln):
        ends = []
        for clause in x[1:]:
            if not isinstance(clause, list) or not clause:
                raise VMError("cond: bad clause")
            test = clause[0]
            if isinstance(test, Symbol) and test == "else":
                self.body(clause, 1, scope, code)
                ends.append(code.emit(JUMP, 0, 0, ln))
                break
            self.expr(test, scope, code)
            jn = code.emit(JUMPIFNOT, 0, 0, ln)
            self.body(clause, 1, scope, code)
            ends.append(code.emit(JUMP, 0, 0, ln))
            code.patch(jn, len(code.instrs))
        else:
            code.emit(CONST, code.const(None))
        for e in ends:
            code.patch(e, len(code.instrs))

    def sf_do(self, x, scope, code, ln):
        self.body(x, 1, scope, code)

    def sf_and(self, x, scope, code, ln):
        if len(x) == 1:
            code.emit(CONST, code.const(True))
            return
        ends = []
        for i, a in enumerate(x[1:]):
            self.expr(a, scope, code)
            if i < len(x) - 2:
                code.emit(DUP)
                ends.append(code.emit(JUMPIFNOT, 0, 0, ln))
                code.emit(POP)
        for e in ends:
            code.patch(e, len(code.instrs))

    def sf_or(self, x, scope, code, ln):
        if len(x) == 1:
            code.emit(CONST, code.const(False))
            return
        ends = []
        for i, a in enumerate(x[1:]):
            self.expr(a, scope, code)
            if i < len(x) - 2:
                code.emit(DUP)
                ends.append(code.emit(JUMPIF, 0, 0, ln))
                code.emit(POP)
        for e in ends:
            code.patch(e, len(code.instrs))

    def sf_let(self, x, scope, code, ln):
        if len(x) < 3 or not isinstance(x[1], list):
            raise VMError("let: (let ((name value) ...) body...)")
        if getattr(scope, "module", False):
            # a let at module level: run it inside a thunk so its slots have a frame
            thunk = self.lambda_code([], x, 2, scope, "let", prelude=x[1])
            code.emit(CLOSURE, self.add_code(code, thunk), 0, ln)
            code.emit(CALL, 0, 0, ln)
            return
        scope.blocks.append({})
        for binding in x[1]:
            if not (isinstance(binding, list) and len(binding) == 2 and isinstance(binding[0], Symbol)):
                raise VMError("let: bad binding")
            self.expr(binding[1], scope, code)
            slot = scope.declare(binding[0])
            code.emit(SETLOCAL, 0, slot, self.line(binding) or ln)   # the binding's own line
        self.body(x, 2, scope, code)
        scope.blocks.pop()

    def sf_lambda(self, x, scope, code, ln):
        if len(x) < 3 or not isinstance(x[1], list):
            raise VMError("lambda: (lambda (params) body...)")
        fn = self.lambda_code([str(p) for p in x[1]], x, 2, scope, "lambda")
        code.emit(CLOSURE, self.add_code(code, fn), 0, ln)

    def sf_define(self, x, scope, code, ln):
        if len(x) < 3:
            raise VMError("define: (define name value) or (define (name params) body...)")
        target = x[1]
        if isinstance(target, list):
            if not target or not isinstance(target[0], Symbol):
                raise VMError("define: bad function head")
            name = str(target[0])
            slot = None if getattr(scope, "module", False) else (scope.lookup(name) if scope.lookup(name) is not None else scope.declare(name))
            fn = self.lambda_code([str(p) for p in target[1:]], x, 2, scope, name)
            code.emit(CLOSURE, self.add_code(code, fn), 0, ln)
        else:
            if not isinstance(target, Symbol):
                raise VMError("define: name must be a symbol")
            name = str(target)
            slot = None if getattr(scope, "module", False) else (scope.lookup(name) if scope.lookup(name) is not None else scope.declare(name))
            self.expr(x[2], scope, code)
        if slot is None:
            code.emit(SETGLOBAL, code.const(name), 0, ln)
        else:
            code.emit(SETLOCAL, 0, slot, ln)
        code.emit(CONST, code.const(None))

    def sf_call(self, x, scope, code, ln):
        if len(x) < 3:
            raise VMError('call: (call <capability> "op" args...)')
        for a in x[1:]:
            self.expr(a, scope, code)
        code.emit(CALLCAP, len(x) - 3, 0, ln)

    # -- helpers
    def add_code(self, code: Code, child: Code) -> int:
        code.codes.append(child)
        return len(code.codes) - 1

    def lambda_code(self, params: List[str], forms: List[Any], start: int, scope: Scope, name: str, prelude=None) -> Code:
        fn = Code(name, len(params))
        inner = Scope(fn, scope)
        for i, p in enumerate(params):
            inner.blocks[0][p] = i
            fn.slotnames.append(p)
        if prelude is not None:                           # module-level let: bindings become slots of the thunk
            for binding in prelude:
                if not (isinstance(binding, list) and len(binding) == 2 and isinstance(binding[0], Symbol)):
                    raise VMError("let: bad binding")
                self.expr(binding[1], inner, fn)
                slot = inner.declare(binding[0])
                fn.emit(SETLOCAL, 0, slot)
        self.body(forms, start, inner, fn)
        fn.emit(RET)
        return fn


# ---------------------------------------------------------------- virtual machine

class VM:
    """Runs compiled code. One instance per cell run, like vm.Interp; the same fields
    (fuel, used, depth, syscall) so the kernel treats both engines alike."""

    def __init__(self, syscall: Callable[[CapHandle, str, list], Any], fuel: int,
                 globals_: Optional[Dict[str, Any]] = None) -> None:
        self.syscall = syscall
        self.fuel = fuel
        self.used = 0
        self.depth = 0
        self.globals: Dict[str, Any] = globals_ if globals_ is not None else {}
        self.trace: Optional[Callable[[Code, int, list, Frame], None]] = None   # debugger hook per instruction
        self.preempt = False                                                  # set by a timer/stepper; checked per instruction
        self._line = 0

    # the old Interp API, so HigherOrder builtins (map/filter/reduce) and the kernel can call back
    def apply(self, fn: Any, args: list) -> Any:
        if isinstance(fn, Closure):
            return self.call_closure(fn, args)
        if isinstance(fn, vm.Closure):                   # a closure from the tree-walker (mixed use)
            return vm.Interp(self.syscall, self.fuel - self.used).apply(fn, args)
        if isinstance(fn, HigherOrder):
            try:
                return fn.f(self, *args)
            except TypeError as e:
                raise VMError(f"bad arguments: {e}")
        if callable(fn):
            try:
                return fn(*args)
            except VMError:
                raise
            except TypeError as e:
                raise VMError(f"bad arguments: {e}")
            except (IndexError, KeyError, ValueError) as e:
                raise VMError(str(e) or type(e).__name__)
        raise VMError(f"not callable: {vm.show(fn)}")

    def call_closure(self, fn: Closure, args: list) -> Any:
        code = fn.code
        if len(args) != code.nparams:
            raise VMError(f"{code.name}: expected {code.nparams} args, got {len(args)}")
        frame = Frame(code.nslots, fn.parent)
        frame.slots[:len(args)] = args
        self.depth += 1
        if self.depth > MAX_DEPTH:
            raise VMError("call depth exceeded")
        try:
            return self.execute(code, frame)
        finally:
            self.depth -= 1

    def run_module(self, code: Code) -> Any:
        """Execute a module body: defines land in self.globals."""
        return self.execute(code, Frame(code.nslots, None))

    def where(self) -> str:
        return f" at line {self._line}" if self._line else ""

    def execute(self, code: Code, frame: Frame) -> Any:
        instrs = code.instrs
        lines = code.lines
        consts = code.consts
        stack: list = []
        pc = 0
        fuel = self.fuel
        try:
            while True:
                self.used += 1
                if self.used > fuel:
                    raise OutOfFuel(f"fuel exhausted after {fuel} steps")
                if self.preempt:
                    self.preempt = False
                    if self.trace:
                        self.trace(code, pc, stack, frame)
                op, a, b = instrs[pc]
                self._line = lines[pc]
                pc += 1
                if op == LOCAL:
                    f = frame
                    for _ in range(a):
                        f = f.parent
                    v = f.slots[b]
                    if v is None and b >= code.nparams and f is frame:
                        pass                              # a declared-but-unset slot reads as nil
                    stack.append(v)
                elif op == CONST:
                    stack.append(consts[a])
                elif op == CALL:
                    args = stack[len(stack) - a:] if a else []
                    del stack[len(stack) - a:]
                    fn = stack.pop()
                    if type(fn) is Closure:               # fast path: no apply/call_closure hops
                        c2 = fn.code
                        if a != c2.nparams:
                            raise VMError(f"{c2.name}: expected {c2.nparams} args, got {a}")
                        f2 = Frame(c2.nslots, fn.parent)
                        f2.slots[:a] = args
                        self.depth += 1
                        if self.depth > MAX_DEPTH:
                            raise VMError("call depth exceeded")
                        try:
                            stack.append(self.execute(c2, f2))
                        finally:
                            self.depth -= 1
                    else:
                        stack.append(self.apply(fn, args))
                elif op == GLOBAL:
                    name = consts[a]
                    if name in self.globals:
                        stack.append(self.globals[name])
                    elif name in BUILTINS:
                        stack.append(BUILTINS[name])
                    else:
                        raise VMError(f"unbound symbol {name}")
                elif op == JUMPIFNOT:
                    if not stack.pop():
                        pc = a
                elif op == JUMP:
                    pc = a
                elif op == POP:
                    stack.pop()
                elif op == SETLOCAL:
                    f = frame
                    for _ in range(a):
                        f = f.parent
                    f.slots[b] = stack.pop()
                elif op == RET:
                    return stack.pop() if stack else None
                elif op == CALLCAP:
                    args = stack[len(stack) - a:] if a else []
                    del stack[len(stack) - a:]
                    opname = stack.pop()
                    cap = stack.pop()
                    if not isinstance(cap, CapHandle):
                        raise VMError(f"call: first argument must be a capability, got {vm.show(cap)}")
                    if not isinstance(opname, str):
                        raise VMError("call: operation must be a string")
                    self.used += 10                       # syscalls are expensive
                    stack.append(self.syscall(cap, opname, args))
                elif op == CLOSURE:
                    stack.append(Closure(code.codes[a], frame))
                elif op == DUP:
                    stack.append(stack[-1])
                elif op == JUMPIF:
                    if stack.pop():
                        pc = a
                elif op == SETGLOBAL:
                    self.globals[consts[a]] = stack.pop()
                else:
                    raise VMError(f"bad instruction {op}")
        except VMError as e:
            if not getattr(e, "located", False):
                e.args = (f"{e.args[0]}{self.where()}",) if e.args else (self.where(),)
                e.located = True                          # type: ignore[attr-defined]
            raise
        except RecursionError:
            raise VMError("call depth exceeded" + self.where())


# ---------------------------------------------------------------- convenience

def compile_source(src: str) -> Code:
    positions: Dict[int, int] = {}
    forms = vm.parse(src, positions)
    return Compiler(positions).compile_module(forms)


def run_source(src: str, syscall=lambda *a: None, fuel: int = 100_000, globals_: Optional[Dict[str, Any]] = None) -> Any:
    m = VM(syscall, fuel, globals_)
    return m.run_module(compile_source(src))
