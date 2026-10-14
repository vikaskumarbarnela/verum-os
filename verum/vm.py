"""Verum Script - the only language cells may run in.

Why a custom language instead of native code or Python?  Because a cell must
be *unable*, not merely forbidden, to reach anything it was not handed. This
interpreter has:

    * no access to Python objects, modules, files, sockets, or memory
    * no mutable state outside the store (no set!, no globals that persist)
    * a fuel budget: every evaluation step costs 1; running out is an error
    * a size budget: strings/lists above the limit are refused
    * exactly one door to the outside world: (call <capability> "op" args...)

Syntax is a small Lisp:

    (define (on-message from body) ...)       ; the cell's entry point
    (let ((x 1) (y 2)) (+ x y))
    (if c a b)  (cond (c1 e1) (c2 e2) (else e))
    (lambda (a b) ...)   (do e1 e2 ...)   (and ...)   (or ...)   (quote x)
    (dict "k" 1 "j" 2)   (get d "k")   (keys d)   (assoc d "k" v)
    (list 1 2 3) (head l) (tail l) (nth l i) (len l) (append l1 l2) (range n)
    (map f l) (filter f l) (reduce f init l)
    (str a b ...) (split s sep) (join l sep) (upper s) (lower s) (contains s sub)
    (call store "get" h)  ; the only side-effect primitive
"""

from __future__ import annotations

from typing import Any, Callable, Dict, List, Optional


class VMError(Exception):
    pass


class OutOfFuel(VMError):
    pass


MAX_SIZE = 200_000     # characters in a string / elements in a list
MAX_DEPTH = 150        # nested calls


# --------------------------------------------------------------------------- reader


class Symbol(str):
    __slots__ = ()

    def __repr__(self) -> str:
        return str.__str__(self)


def tokenize(src: str) -> List[str]:
    tokens: List[str] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c.isspace():
            i += 1
        elif c == ";":
            while i < n and src[i] != "\n":
                i += 1
        elif c in "()":
            tokens.append(c)
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and src[j] != '"':
                if src[j] == "\\" and j + 1 < n:
                    j += 1
                    buf.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(src[j], src[j]))
                else:
                    buf.append(src[j])
                j += 1
            if j >= n:
                raise VMError("unterminated string")
            tokens.append('"' + "".join(buf))
            i = j + 1
        else:
            j = i
            while j < n and not src[j].isspace() and src[j] not in "();":
                j += 1
            tokens.append(src[i:j])
            i = j
    return tokens


def parse(src: str) -> List[Any]:
    tokens = tokenize(src)
    forms = []
    pos = 0

    def read() -> Any:
        nonlocal pos
        if pos >= len(tokens):
            raise VMError("unexpected end of input")
        t = tokens[pos]
        pos += 1
        if t == "(":
            lst = []
            while pos < len(tokens) and tokens[pos] != ")":
                lst.append(read())
            if pos >= len(tokens):
                raise VMError("missing )")
            pos += 1
            return lst
        if t == ")":
            raise VMError("unexpected )")
        if t.startswith('"'):
            return t[1:]
        if t == "true":
            return True
        if t == "false":
            return False
        if t == "nil":
            return None
        try:
            return int(t)
        except ValueError:
            pass
        try:
            return float(t)
        except ValueError:
            pass
        return Symbol(t)

    while pos < len(tokens):
        forms.append(read())
    return forms


# --------------------------------------------------------------------------- values


class Closure:
    __slots__ = ("params", "body", "env", "name")

    def __init__(self, params: List[str], body: List[Any], env: "Env", name: str = "lambda") -> None:
        self.params, self.body, self.env, self.name = params, body, env, name

    def __repr__(self) -> str:
        return f"<fn {self.name}/{len(self.params)}>"


class CapHandle:
    """Opaque: code can pass it to (call ...) and nothing else. Its token is never printable."""
    __slots__ = ("token", "label")

    def __init__(self, token: str, label: str) -> None:
        self.token, self.label = token, label

    def __repr__(self) -> str:
        return f"<cap {self.label}>"


class Env:
    __slots__ = ("vars", "outer")

    def __init__(self, vars: Optional[Dict[str, Any]] = None, outer: Optional["Env"] = None) -> None:
        self.vars = vars or {}
        self.outer = outer

    def lookup(self, name: str) -> Any:
        e: Optional[Env] = self
        while e is not None:
            if name in e.vars:
                return e.vars[name]
            e = e.outer
        raise VMError(f"unbound symbol {name!r}")


def show(v: Any) -> str:
    """Human-readable rendering used by the console and the shell."""
    if v is None:
        return "nil"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if isinstance(v, str):
        return v
    if isinstance(v, list):
        return "(" + " ".join(show(x) for x in v) + ")"
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}: {show(x)}" for k, x in v.items()) + "}"
    return repr(v)


# --------------------------------------------------------------------------- builtins


def _guard(v: Any) -> Any:
    if isinstance(v, (str, list, dict)) and len(v) > MAX_SIZE:
        raise VMError(f"value exceeds size budget ({MAX_SIZE})")
    return v


def _num(v: Any, op: str) -> Any:
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise VMError(f"{op}: expected a number, got {show(v)}")
    return v


def _lst(v: Any, op: str) -> list:
    if not isinstance(v, list):
        raise VMError(f"{op}: expected a list, got {show(v)}")
    return v


def _s(v: Any, op: str) -> str:
    if not isinstance(v, str):
        raise VMError(f"{op}: expected a string, got {show(v)}")
    return v


def _fail(msg: str):
    raise VMError(msg)


class HigherOrder:
    """A builtin that needs the interpreter to call back into Verum closures."""
    __slots__ = ("f",)

    def __init__(self, f: Callable) -> None:
        self.f = f


def _map(it: "Interp", f, l):
    return _guard([it.apply(f, [x]) for x in _lst(l, "map")])


def _filter(it: "Interp", f, l):
    return [x for x in _lst(l, "filter") if it.apply(f, [x])]


def _reduce(it: "Interp", f, init, l):
    acc = init
    for x in _lst(l, "reduce"):
        acc = it.apply(f, [acc, x])
    return acc


def _arith(op: str, fn: Callable) -> Callable:
    def f(*args):
        if not args:
            raise VMError(f"{op}: needs arguments")
        acc = _num(args[0], op)
        for a in args[1:]:
            acc = fn(acc, _num(a, op))
        return acc
    return f


def _div(a, b):
    if b == 0:
        raise VMError("division by zero")
    return a / b if isinstance(a, float) or isinstance(b, float) else a // b


def _cmp(op: str, fn: Callable) -> Callable:
    def f(a, b):
        return fn(_num(a, op), _num(b, op))
    return f


def _dict(*kv):
    if len(kv) % 2:
        raise VMError("dict: odd number of arguments")
    return _guard({_s(kv[i], "dict"): kv[i + 1] for i in range(0, len(kv), 2)})


def _get(d, k, default=None):
    if isinstance(d, dict):
        return d.get(_s(k, "get"), default)
    if isinstance(d, list):
        i = _num(k, "get")
        return d[i] if 0 <= i < len(d) else default
    raise VMError(f"get: cannot index {show(d)}")


def _nth(l, i):
    l = _lst(l, "nth")
    i = _num(i, "nth")
    if not 0 <= i < len(l):
        raise VMError(f"nth: index {i} out of range")
    return l[i]


BUILTINS: Dict[str, Callable] = {
    "+": _arith("+", lambda a, b: a + b),
    "-": _arith("-", lambda a, b: a - b),
    "*": _arith("*", lambda a, b: a * b),
    "/": _arith("/", _div),
    "mod": lambda a, b: _num(a, "mod") % _num(b, "mod") if b != 0 else _fail("mod by zero"),
    "=": lambda a, b: a == b,
    "<": _cmp("<", lambda a, b: a < b),
    ">": _cmp(">", lambda a, b: a > b),
    "<=": _cmp("<=", lambda a, b: a <= b),
    ">=": _cmp(">=", lambda a, b: a >= b),
    "not": lambda a: not a,
    "nil?": lambda a: a is None,
    "number?": lambda a: isinstance(a, (int, float)) and not isinstance(a, bool),
    "string?": lambda a: isinstance(a, str),
    "list?": lambda a: isinstance(a, list),
    "dict?": lambda a: isinstance(a, dict),
    "list": lambda *a: _guard(list(a)),
    "head": lambda l: _lst(l, "head")[0] if l else None,
    "tail": lambda l: _lst(l, "tail")[1:],
    "nth": _nth,
    "len": lambda v: len(v) if isinstance(v, (str, list, dict)) else _fail("len: not sized"),
    "map": HigherOrder(_map),
    "filter": HigherOrder(_filter),
    "reduce": HigherOrder(_reduce),
    "append": lambda *ls: _guard([x for l in ls for x in _lst(l, "append")]),
    "cons": lambda x, l: _guard([x] + _lst(l, "cons")),
    "range": lambda n: _guard(list(range(_num(n, "range")))),
    "reverse": lambda l: list(reversed(_lst(l, "reverse"))),
    "sort": lambda l: sorted(_lst(l, "sort"), key=lambda x: (str(type(x)), x)),
    "dict": _dict,
    "get": _get,
    "keys": lambda d: list(d.keys()) if isinstance(d, dict) else _fail("keys: not a dict"),
    "assoc": lambda d, k, v: _guard({**d, _s(k, "assoc"): v}) if isinstance(d, dict) else _fail("assoc: not a dict"),
    "str": lambda *a: _guard("".join(show(x) for x in a)),
    "split": lambda s, sep=" ": _guard([w for w in _s(s, "split").split(_s(sep, "split")) if w]),
    "join": lambda l, sep="": _guard(_s(sep, "join").join(show(x) for x in _lst(l, "join"))),
    "upper": lambda s: _s(s, "upper").upper(),
    "lower": lambda s: _s(s, "lower").lower(),
    "trim": lambda s: _s(s, "trim").strip(),
    "contains": lambda s, sub: _s(sub, "contains") in _s(s, "contains"),
    "slice": lambda v, a, b=None: v[_num(a, "slice"):(None if b is None else _num(b, "slice"))],
    "error": lambda msg: _fail(show(msg)),
}


# --------------------------------------------------------------------------- evaluator


class Interp:
    """One interpreter instance per cell run. `syscall` is the single door out."""

    def __init__(self, syscall: Callable[[CapHandle, str, list], Any], fuel: int) -> None:
        self.syscall = syscall
        self.fuel = fuel
        self.used = 0
        self.depth = 0

    def burn(self, n: int = 1) -> None:
        self.used += n
        if self.used > self.fuel:
            raise OutOfFuel(f"fuel exhausted after {self.fuel} steps")

    def eval(self, x: Any, env: Env) -> Any:
        self.burn()
        if isinstance(x, Symbol):
            return env.lookup(x)
        if not isinstance(x, list):
            return x
        if not x:
            return []
        head = x[0]
        if isinstance(head, Symbol):
            sf = getattr(self, "sf_" + head.replace("-", "_"), None) if head in SPECIAL_FORMS else None
            if sf is not None:
                return sf(x, env)
        fn = self.eval(head, env)
        args = [self.eval(a, env) for a in x[1:]]
        return self.apply(fn, args)

    def apply(self, fn: Any, args: list) -> Any:
        if isinstance(fn, Closure):
            if len(args) != len(fn.params):
                raise VMError(f"{fn.name}: expected {len(fn.params)} args, got {len(args)}")
            self.depth += 1
            if self.depth > MAX_DEPTH:
                raise VMError("call depth exceeded")
            try:
                local = Env(dict(zip(fn.params, args)), fn.env)
                result = None
                for form in fn.body:
                    result = self.eval(form, local)
                return result
            finally:
                self.depth -= 1
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
        raise VMError(f"not callable: {show(fn)}")

    # ---- special forms (names registered in SPECIAL_FORMS)

    def sf_quote(self, x, env):
        return x[1]

    def sf_if(self, x, env):
        if len(x) not in (3, 4):
            raise VMError("if: (if cond then [else])")
        if self.eval(x[1], env):
            return self.eval(x[2], env)
        return self.eval(x[3], env) if len(x) == 4 else None

    def sf_cond(self, x, env):
        for clause in x[1:]:
            if not isinstance(clause, list) or not clause:
                raise VMError("cond: bad clause")
            if clause[0] == "else" or self.eval(clause[0], env):
                result = None
                for form in clause[1:]:
                    result = self.eval(form, env)
                return result
        return None

    def sf_do(self, x, env):
        result = None
        for form in x[1:]:
            result = self.eval(form, env)
        return result

    def sf_and(self, x, env):
        v = True
        for form in x[1:]:
            v = self.eval(form, env)
            if not v:
                return v
        return v

    def sf_or(self, x, env):
        v = False
        for form in x[1:]:
            v = self.eval(form, env)
            if v:
                return v
        return v

    def sf_let(self, x, env):
        if len(x) < 3 or not isinstance(x[1], list):
            raise VMError("let: (let ((name value) ...) body...)")
        local = Env({}, env)
        for binding in x[1]:
            if not (isinstance(binding, list) and len(binding) == 2 and isinstance(binding[0], Symbol)):
                raise VMError("let: bad binding")
            local.vars[binding[0]] = self.eval(binding[1], local)
        result = None
        for form in x[2:]:
            result = self.eval(form, local)
        return result

    def sf_lambda(self, x, env):
        if len(x) < 3 or not isinstance(x[1], list):
            raise VMError("lambda: (lambda (params) body...)")
        return Closure([str(p) for p in x[1]], x[2:], env)

    def sf_define(self, x, env):
        if len(x) < 3:
            raise VMError("define: (define name value) or (define (name params) body...)")
        target = x[1]
        if isinstance(target, list):           # (define (f a b) body...)
            name = str(target[0])
            env.vars[name] = Closure([str(p) for p in target[1:]], x[2:], env, name)
            return None
        env.vars[str(target)] = self.eval(x[2], env)
        return None

    def sf_call(self, x, env):
        """(call <cap> "op" args...) - the only way to affect the world."""
        if len(x) < 3:
            raise VMError('call: (call <capability> "op" args...)')
        cap = self.eval(x[1], env)
        if not isinstance(cap, CapHandle):
            raise VMError(f"call: first argument must be a capability, got {show(cap)}")
        op = self.eval(x[2], env)
        args = [self.eval(a, env) for a in x[3:]]
        self.burn(10)                         # syscalls are expensive
        return self.syscall(cap, _s(op, "call"), args)


SPECIAL_FORMS = {"quote", "if", "cond", "do", "and", "or", "let", "lambda", "define", "call"}


def new_env(bindings: Optional[Dict[str, Any]] = None) -> Env:
    base = Env(dict(BUILTINS))
    return Env(dict(bindings or {}), base)


def load_module(src: str, env: Env, syscall, fuel: int) -> Interp:
    """Evaluate top-level forms (defines) of a cell's code into env."""
    it = Interp(syscall, fuel)
    for form in parse(src):
        it.eval(form, env)
    return it
