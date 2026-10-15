"""Security and correctness tests for VERUM. Run: python -m unittest -v"""

import os
import shutil
import tempfile
import unittest

from verum import vm
from verum.caps import CapError
from verum.kernel import SHELL, Kernel, KernelError
from verum.shell import Shell

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CELLS = os.path.join(HERE, "cells")


def load(name):
    with open(os.path.join(CELLS, name + ".vs"), encoding="utf-8") as f:
        return f.read()


def load_any(name):
    for ext in (".vs", ".sv"):
        p = os.path.join(CELLS, name + ext)
        if os.path.exists(p):
            with open(p, encoding="utf-8") as f:
                return f.read()
    raise FileNotFoundError(name)


class Base(unittest.TestCase):
    def setUp(self):
        self.k = Kernel()                      # in-memory, no persistence
        self.lines = []
        self.sh = Shell(self.k, out=self.lines.append)

    def code(self, name):
        return self.k.put("code", load(name), ["code", name], by=SHELL, op="code")


class TestStore(Base):
    def test_content_addressed_and_immutable(self):
        a = self.k.put("note", "hello", ["note"], SHELL, "note")
        b = self.k.put("note", "hello", ["note"], SHELL, "note")
        c = self.k.put("note", "hello!", ["note"], SHELL, "note")
        self.assertEqual(a, b)                 # same content, same name
        self.assertNotEqual(a, c)              # one byte changed, new name
        self.assertEqual(len(self.k.store), 2)

    def test_ledger_detects_tampering(self):
        self.k.put("note", "x", ["note"], SHELL, "note")
        self.assertTrue(self.k.ledger.verify()[0])
        self.k.ledger.entries[1].event["obj"]["data"] = "evil"
        ok, msg = self.k.ledger.verify()
        self.assertFalse(ok)
        self.assertIn("entry 1", msg)


class TestCapabilities(Base):
    def test_cell_starts_with_nothing(self):
        self.k.spawn("wc", self.code("wordcount"), SHELL)
        self.k.send("wc", "anything", SHELL)
        self.k.run()
        self.assertEqual(self.k.cells["wc"].errors, 1)
        self.assertTrue(any("unbound symbol store" in l for l in self.lines))

    def test_cannot_grant_more_than_you_hold(self):
        self.k.spawn("wc", self.code("wordcount"), SHELL)
        cap = self.k.grant(self.k.shell_caps["store"], SHELL, ["get"], "wc", "store")
        with self.assertRaises(CapError):      # wc tries to widen get -> get,put for a friend
            self.k.caps.attenuate(cap.token, "wc", ["get", "put"], "friend")

    def test_stolen_token_is_useless(self):
        self.k.spawn("wc", self.code("wordcount"), SHELL)
        self.k.spawn("mal", self.code("malware"), SHELL)
        cap = self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "put"], "wc", "store")
        with self.assertRaises(CapError):
            self.k.syscall("mal", cap.token, "get", ["x"])   # right token, wrong holder

    def test_revocation_cascades(self):
        self.k.spawn("a", self.code("shout"), SHELL)
        self.k.spawn("b", self.code("shout"), SHELL)
        parent = self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "put"], "a", "store")
        child = self.k.caps.attenuate(parent.token, "a", ["get"], "b")
        self.k.revoke(parent.token, SHELL)
        self.assertTrue(self.k.caps.get(child.token).revoked)
        with self.assertRaises(CapError):
            self.k.syscall("b", child.token, "get", ["x"])

    def test_cell_cap_targets_one_cell_only(self):
        self.k.spawn("a", self.code("shout"), SHELL)
        self.k.spawn("b", self.code("shout"), SHELL)
        self.k.spawn("c", self.code("shout"), SHELL)
        cap = self.k.mint_cell_cap("b", SHELL)
        self.k.grant(cap.token, SHELL, ["send"], "a", "next")
        tok = self.k.cells["a"].env.vars["next"].token
        self.k.send("b", "hi", "a", tok)       # allowed
        with self.assertRaises(CapError):
            self.k.send("c", "hi", "a", tok)   # same token, different target


class TestDelegation(Base):
    DELEG = '''(define (on-message from rights)
                 (call store "delegate" next rights "store")
                 (call next "send" "delegated"))'''

    def setUp(self):
        super().setUp()
        code = self.k.put("code", self.DELEG, ["code", "deleg"], SHELL, "code")
        self.k.spawn("d", code, SHELL)
        self.k.spawn("e", self.code("shout"), SHELL)
        self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "delegate"], "d", "store")
        cap = self.k.mint_cell_cap("e", SHELL)
        self.k.grant(cap.token, SHELL, ["send"], "d", "next")

    def caps_of(self, cell):
        return {label: self.k.caps.get(v.token) for label, v in self.k.cells[cell].env.vars.items()
                if isinstance(v, vm.CapHandle)}

    def test_delegate_narrower_copy(self):
        self.k.send("d", "get", SHELL)
        self.k.run()
        got = self.caps_of("e")["store"]
        self.assertEqual(got.rights, frozenset({"get"}))
        self.assertEqual(got.holder, "e")
        self.assertEqual(self.k.caps.get(got.parent).holder, "d")      # derived from d's own cap

    def test_cannot_delegate_more_than_held(self):
        self.k.send("d", "get,put", SHELL)
        self.k.run()
        self.assertNotIn("store", self.caps_of("e"))
        self.assertTrue(any("cannot grant rights" in l for l in self.lines))

    def test_without_delegate_right(self):
        self.k.revoke(self.caps_of("d")["store"].token, SHELL)
        self.k.grant(self.k.shell_caps["store"], SHELL, ["get"], "d", "store")   # no delegate
        self.k.send("d", "get", SHELL)
        self.k.run()
        self.assertTrue(any("lacks right 'delegate'" in l for l in self.lines))


class TestSeats(Base):
    def test_seat_starts_with_nothing_and_cannot_spawn(self):
        self.k.seat_create("alice", SHELL)
        with self.assertRaises(CapError):
            self.k.spawn("x", self.code("shout"), "alice")
        with self.assertRaises(CapError):
            self.k._cap_of("alice", "store")

    def test_grant_to_seat_and_act_as_seat(self):
        self.k.seat_create("alice", SHELL)
        self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "find"], "alice", "store")
        self.k.switch("alice")
        self.assertEqual(self.k.me, "alice")
        self.assertEqual(self.k.caps.get(self.k.my_caps["store"]).rights, frozenset({"get", "find"}))
        with self.assertRaises(CapError):                       # she cannot put
            self.k.caps.check(self.k.my_caps["store"], "alice", "put")
        self.k.spawn("wc", self.code("wordcount"), SHELL)
        cap = self.k.grant(self.k.my_caps["store"], "alice", ["get"], "wc", "store")   # narrower, from her own
        self.assertEqual(self.k.caps.get(cap.parent).holder, "alice")
        with self.assertRaises(CapError):                       # cannot widen
            self.k.grant(self.k.my_caps["store"], "alice", ["put"], "wc", "store2")

    def test_passphrase(self):
        self.k.seat_create("bob", SHELL, passphrase="hunter2")
        with self.assertRaises(CapError):
            self.k.switch("bob", "wrong")
        with self.assertRaises(CapError):
            self.k.switch("bob")
        self.assertEqual(self.k.me, SHELL)
        self.k.switch("bob", "hunter2")
        self.assertEqual(self.k.me, "bob")
        logins = [e.event for e in self.k.ledger.entries if e.event["type"] == "login"]
        self.assertEqual([e["ok"] for e in logins], [False, False, True])  # failures are audited too
        self.assertNotIn("hunter2", str(self.k.ledger.entries))             # only the salted hash is stored

    def test_seats_survive_replay(self):
        self.k.seat_create("bob", SHELL, passphrase="pw")
        self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], "bob", "console")
        k2 = self.k.fork(len(self.k.ledger.entries))
        self.assertIn("bob", k2.seats)
        self.assertTrue(k2.seats["bob"].check("pw"))
        self.assertFalse(k2.seats["bob"].check("nope"))
        self.assertEqual(k2.seats["bob"].caps["console"], self.k.seats["bob"].caps["console"])


class TestSanskrit(Base):
    """संस्कृतलिपि: a Sanskrit program is the same program."""

    def test_vocabulary_is_total(self):
        from verum import sanskrit
        for sk, en in sanskrit.BUILTINS.items():
            self.assertIn(en, vm.BUILTINS, f"{sk} -> {en} is not a builtin")
        for sk, en in sanskrit.FORMS.items():
            if en != "else" and en != "on-message":
                self.assertIn(en, vm.SPECIAL_FORMS, f"{sk} -> {en} is not a special form")

    def test_sanskrit_reads_as_canonical(self):
        it = vm.Interp(lambda *a: None, 10_000)
        env = vm.new_env()
        src = '(परिभाषा (वर्गः क) (गुणनम् क क)) (प्रत्येकम् वर्गः (श्रेणी ४))'
        r = None
        for f in vm.parse(src):
            r = it.eval(f, env)
        self.assertEqual(r, [0, 1, 4, 9])
        self.assertEqual(vm.parse('(यदि सत्यम् १० २०)'), [[vm.Symbol("if"), True, 10, 20]])
        self.assertEqual(vm.parse('(अस्तु ((क ५)) (योगः क १))')[0][0], "let")

    def test_sanskrit_cell_runs_and_matches_english(self):
        note = self.k.put("note", "एकम् द्वे त्रीणि", ["note"], SHELL, "note")
        for name in ("ganana", "wordcount"):
            self.k.spawn(name, self.k.put("code", load_any(name), ["code", name], SHELL, "code"), SHELL)
            self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "put"], name, "store")
            self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], name, "console")
            self.k.send(name, note, SHELL)
        self.k.run()
        self.assertEqual(self.k.cells["ganana"].errors, 0)
        stats = [self.k.store.get(h) for h in self.k.store.find("stats")]
        self.assertEqual(sorted(o.by for o in stats), ["ganana", "wordcount"])
        self.assertEqual({o.data["words"] for o in stats}, {3})           # same answer, same provenance model
        self.assertTrue(any("शब्दाः 3" in l for l in self.lines))

    def test_sanskrit_operation_names(self):
        src = '(परिभाषा (सन्देशे प्रेषकः विषयः) (आह्वानम् पटलः "मुद्रय" "नमस्ते" विषयः))'
        self.k.spawn("s", self.k.put("code", src, ["code"], SHELL, "code"), SHELL)
        self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], "s", "console")
        self.k.send("s", "लोकः", SHELL)
        self.k.run()
        self.assertIn("[s] नमस्ते लोकः", self.lines)
        self.assertEqual(self.k.cells["s"].errors, 0)

    def test_round_trip_display(self):
        from verum import sanskrit
        en = load("wordcount")
        sk = sanskrit.to_sanskrit(en)
        self.assertIn("परिभाषा", sk)
        self.assertIn("आह्वानम् भाण्डारः", sk)
        self.assertEqual(vm.parse(sk), vm.parse(en))                       # identical programs


class TestLipi(Base):
    """Harvard-Kyoto transliteration: what you type on a Latin keyboard becomes Devanagari."""

    def test_round_trip_vocabulary(self):
        from verum import sanskrit, translit
        for word in sanskrit.SYMBOLS:
            latin = translit.to_latin(word)
            self.assertEqual(translit.to_devanagari(latin), word, f"{word} -> {latin} does not round-trip")

    def test_common_spellings(self):
        from verum.translit import to_devanagari as d
        self.assertEqual(d("paribhASA"), "परिभाषा")
        self.assertEqual(d("paribhaaShaa"), "परिभाषा")
        self.assertEqual(d("saMskRta"), "संस्कृत")
        self.assertEqual(d("kSetram"), "क्षेत्रम्")
        self.assertEqual(d("200"), "२००")
        self.assertEqual(d("rAm"), "राम्")           # trailing consonant takes a halant

    def test_shell_lipi_mode_runs_a_sanskrit_program(self):
        self.sh.do("lipi on")
        self.sh.do('eval (paribhASA (vargaH ka) (guNanam ka ka)) (pratyekam vargaH (zreNI 4))')
        self.assertIn("=> (0 1 4 9)", self.lines)
        self.sh.do("lipi off")
        self.sh.do("eval (len (list :rAma))")           # :word works even with lipi off
        self.assertTrue(any("unbound symbol राम" in l for l in self.lines))

    def test_english_words_stay_english_in_lipi_mode(self):
        self.sh.do("lipi on")
        self.sh.do("note dharmo rakSati rakSitaH")      # `note` stays, the text becomes Devanagari
        h = self.k.store.find("note")[0]
        self.assertEqual(self.k.store.get(h).data, "धर्मो रक्षति रक्षितः")
        self.sh.do("eval (+ 1 2)")                      # English builtins untouched
        self.assertIn("=> 3", self.lines)

    def test_sanskrit_error_hint(self):
        self.sh.do("lipi on")
        self.sh.do("eval (/ 1 0)")
        self.assertTrue(any("शून्येन भागः" in l for l in self.lines))


class TestDebugger(Base):
    """D1: breakpoints, stepping, inspection and profiling on the bytecode engine."""

    SRC = ('(define (square x)\n'
           '  (* x x))\n'
           '(define (on-message from body)\n'
           '  (let ((a (square body))\n'
           '        (b (+ body 1)))\n'
           '    (call console "print" a b)))\n')

    def setUp(self):
        super().setUp()
        self.k.spawn("d", self.k.put("code", self.SRC, ["code"], SHELL, "code"), SHELL)
        self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], "d", "console")

    def test_breakpoint_pauses_and_inspect_shows_slots(self):
        self.sh.do("break d 5")
        self.k.send("d", 7, SHELL)
        self.sh.do("run")
        self.assertTrue(any("paused in cell d at line 5" in l for l in self.lines), self.lines)
        self.sh.do("inspect")
        joined = "\n".join(self.lines)
        self.assertIn("from", joined)
        self.assertIn("a                49", joined)          # (square 7) already bound; b not yet
        self.sh.do("continue")
        self.assertIn("[d] 49 8", self.lines)
        self.assertFalse(self.k.debugger.paused)

    def test_step_moves_one_line(self):
        self.sh.do("break d 4")
        self.k.send("d", 3, SHELL)
        self.sh.do("run")
        self.assertTrue(any("at line 4" in l for l in self.lines))
        self.sh.do("step")
        self.assertTrue(any("at line 2" in l for l in self.lines))   # into square
        self.sh.do("continue")
        self.assertIn("[d] 9 4", self.lines)

    def test_profile_counts_instructions_per_function(self):
        self.sh.do("profile on")
        self.k.send("d", 5, SHELL)
        self.sh.do("run")
        prof = self.k.debugger.profile
        self.assertIn("square", prof)
        self.assertIn("on-message", prof)
        self.assertGreater(prof["on-message"], prof["square"])

    def test_error_shows_source_line(self):
        src = '(define (on-message from body)\n  (+ 1\n     (nope)))\n'
        self.k.spawn("e", self.k.put("code", src, ["code"], SHELL, "code"), SHELL)
        self.k.send("e", 1, SHELL)
        self.k.run()
        joined = "\n".join(self.lines)
        self.assertIn("unbound symbol nope at line 3", joined)
        self.assertIn("line 3: (nope)))", joined)


class TestBrowser(Base):
    """The VERUM browser: पत्र pages are objects; links resolve to content; scripts run only by consent."""

    def setUp(self):
        super().setUp()
        self.k.spawn("wc", self.code("wordcount"), SHELL)     # so `allow` has code to run? no: pages name code objects
        for name in ("home", "about", "sanskrit"):
            self.sh.do(f'publish "{os.path.join(HERE, "pages", name + ".patra")}"')
        self.k.put("code", load_any("ganana"), ["code", "ganana"], SHELL, "code")

    def test_parse(self):
        from verum import patra
        p = patra.parse("# T\n\npara one\nstill one\n\n* a\n=> abc123 go\n@cell ganana store[get] console[print]\n")
        self.assertEqual(p.title, "T")
        kinds = [k for k, _ in p.blocks]
        self.assertEqual(kinds, ["h1", "p", "li", "link"])
        self.assertEqual(p.blocks[1][1], "para one still one")
        self.assertEqual(p.links[0].target, "abc123")
        self.assertEqual(p.script.caps, [("store", ["get"]), ("console", ["print"])])

    def test_page_is_an_object_with_provenance(self):
        h = self.k.store.find("home")[0]
        o = self.k.store.get(h)
        self.assertEqual(o.kind, "patra")
        self.assertEqual(o.by, SHELL)
        self.assertIn("patra", o.tags)

    def test_navigation(self):
        self.sh.do("open page:home")
        self.assertTrue(any("HOME" in l for l in self.lines))
        self.sh.do("follow 1")                          # -> about
        self.assertTrue(any("This is not a web browser" in l for l in self.lines))
        self.sh.do("back")
        self.assertTrue(any("link(s)" in l for l in self.lines[-1:]))
        self.sh.do("follow 3")                          # tag:note -> a synthesized index
        self.assertTrue(any("objects tagged note" in l.lower() for l in self.lines))

    def test_broken_link_resolves_to_nothing_not_something_else(self):
        self.sh.do("open deadbeef")
        self.assertTrue(any("refused: no unique object" in l for l in self.lines))

    def test_allow_runs_the_cell_with_only_declared_caps(self):
        self.sh.do("open page:sanskrit")
        self.sh.do("allow")
        name = next(n for n in self.k.cells if n.startswith("page-"))
        cell = self.k.cells[name]
        caps = {label: self.k.caps.get(v.token) for label, v in cell.env.vars.items() if isinstance(v, vm.CapHandle)}
        self.assertEqual(set(caps), {"store", "console"})
        self.assertEqual(caps["store"].rights, frozenset({"get", "put"}))
        self.assertNotIn("kernel", caps)
        self.assertEqual(cell.errors, 0)
        self.assertTrue(any("शब्दाः" in l for l in self.lines))        # it counted the page's words
        stats = self.k.store.find("stats")
        self.assertEqual(self.k.store.get(stats[0]).by, name)        # provenance names the page's cell


class TestMalware(Base):
    def setUp(self):
        super().setUp()
        self.k.spawn("mal", self.code("malware"), SHELL)
        self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], "mal", "console")

    def attack(self, n):
        self.k.send("mal", n, SHELL)
        self.k.run()
        return "\n".join(self.lines)

    def test_forged_token(self):
        self.assertIn("first argument must be a capability", self.attack(1))

    def test_wrong_right(self):
        self.assertIn("lacks right 'get'", self.attack(2))

    def test_infinite_loop_is_bounded(self):
        self.assertIn("fuel exhausted", self.attack(3))

    def test_memory_bomb_is_bounded(self):
        out = self.attack(4)
        self.assertTrue("size budget" in out or "fuel exhausted" in out, out)

    def test_stack_bomb_is_bounded(self):
        self.assertIn("call depth exceeded", self.attack(5))

    def test_kernel_survives_all_attacks(self):
        for n in range(1, 6):
            self.attack(n)
        self.assertEqual(self.k.cells["mal"].errors, 5)
        self.assertTrue(self.k.ledger.verify()[0])
        self.assertEqual(self.k.store.find("note"), [])   # it never touched the store


class TestWorkflow(Base):
    def test_pipeline_with_provenance(self):
        note = self.k.put("note", "one two three", ["note"], SHELL, "note")
        self.k.spawn("wc", self.code("wordcount"), SHELL)
        self.k.spawn("shout", self.code("shout"), SHELL)
        for cell in ("wc", "shout"):
            self.k.grant(self.k.shell_caps["store"], SHELL, ["get", "put"], cell, "store")
        self.k.grant(self.k.shell_caps["console"], SHELL, ["print"], "wc", "console")
        cap = self.k.mint_cell_cap("wc", SHELL)
        self.k.grant(cap.token, SHELL, ["send"], "shout", "next")

        self.k.send("shout", note, SHELL)
        self.k.run()

        stats = self.k.store.find("stats")
        self.assertEqual(len(stats), 1)
        self.assertEqual(self.k.store.get(stats[0]).data["words"], 3)
        chain = [o.kind for _, _, o in self.k.store.lineage(stats[0])]
        self.assertEqual(chain, ["stats", "note", "note"])          # stats <- SHOUTED <- original
        self.assertEqual(self.k.store.get(stats[0]).by, "wc")


class TestTimeTravel(unittest.TestCase):
    def test_persist_replay_and_fork(self):
        tmp = tempfile.mkdtemp()
        try:
            d = os.path.join(tmp, "state")
            k = Kernel(d)
            k.put("note", "first", ["note"], SHELL, "note")
            seq_after_first = len(k.ledger.entries)
            k.put("note", "second", ["note"], SHELL, "note")
            k.spawn("wc", k.put("code", load("wordcount"), ["code"], SHELL, "code"), SHELL)

            k2 = Kernel(d)                     # reboot = replay
            self.assertEqual(len(k2.store), 3)
            self.assertIn("wc", k2.cells)
            self.assertEqual(k2.ledger.head, k.ledger.head)
            self.assertEqual(set(k2.shell_caps.values()), set(k.shell_caps.values()))

            f = k2.fork(seq_after_first, os.path.join(tmp, "fork"))
            self.assertEqual(len(f.store), 1)
            self.assertEqual(f.cells, {})
            f.put("note", "alternate history", ["note"], SHELL, "note")
            self.assertEqual(len(Kernel(d).store), 3)           # original untouched
            self.assertEqual(len(Kernel(os.path.join(tmp, "fork")).store), 2)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class TestVM(unittest.TestCase):
    def run_src(self, src):
        it = vm.Interp(lambda *a: None, 10_000)
        env = vm.new_env()
        r = None
        for f in vm.parse(src):
            r = it.eval(f, env)
        return r

    def test_language_basics(self):
        self.assertEqual(self.run_src("(+ 1 2 3)"), 6)
        self.assertEqual(self.run_src('(define (f x) (* x x)) (map f (list 1 2 3))'), [1, 4, 9])
        self.assertEqual(self.run_src('(reduce + 0 (range 5))'), 10)
        self.assertEqual(self.run_src('(get (assoc (dict "a" 1) "b" 2) "b")'), 2)
        self.assertEqual(self.run_src('(cond ((= 1 2) "no") (else "yes"))'), "yes")

    def test_no_python_leaks(self):
        for bad in ("(__import__ \"os\")", "(getattr 1 \"x\")", "(eval 1)", "(open \"x\")"):
            with self.assertRaises(vm.VMError):
                self.run_src(bad)


if __name__ == "__main__":
    unittest.main()
