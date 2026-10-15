"""संस्कृतलिपि - the Sanskrit surface of Verum Script.

Verum Script is a Lisp, so its syntax is only parentheses; everything else is
a word. This table gives every keyword, builtin, capability name and
operation a Sanskrit name. The reader maps a Sanskrit symbol to its canonical
form while parsing, so a program written in Sanskrit and the same program
written in English are literally the same program to the kernel: same hash,
same provenance, same capabilities.

Devanagari digits (०-९) are read as numbers. Strings are left alone.

    (परिभाषा (सन्देशे प्रेषकः विषयः)
      (अस्तु ((वस्तु (आह्वानम् भाण्डारः "प्राप्" विषयः)))
        (आह्वानम् पटलः "मुद्रय" "शब्दाः:" (परिमाणम् (विभज (गृहाण वस्तु "data") " ")))))
"""

from __future__ import annotations

from typing import Dict

# ---- special forms -------------------------------------------------------------
FORMS: Dict[str, str] = {
    "परिभाषा":   "define",      # paribhāṣā   - definition
    "कार्यम्":    "lambda",      # kāryam      - a function, a work
    "अस्तु":     "let",         # astu        - let it be
    "यदि":       "if",          # yadi        - if
    "विकल्पः":    "cond",        # vikalpaḥ    - alternatives
    "अन्यथा":    "else",        # anyathā     - otherwise
    "क्रमेण":     "do",          # krameṇa     - in sequence
    "च":         "and",         # ca          - and
    "वा":        "or",          # vā          - or
    "उद्धृतम्":   "quote",       # uddhṛtam    - quoted
    "आह्वानम्":   "call",        # āhvānam     - invocation (the only effect)
    "सन्देशे":    "on-message",  # sandeśe     - upon a message
}

# ---- builtins --------------------------------------------------------------------
BUILTINS: Dict[str, str] = {
    "योगः": "+", "वियोगः": "-", "गुणनम्": "*", "भागः": "/", "शेषफलम्": "mod",
    "समम्": "=", "न्यूनम्": "<", "अधिकम्": ">", "न्यूनसमम्": "<=", "अधिकसमम्": ">=", "न": "not",
    "शून्यम्?": "nil?", "संख्या?": "number?", "वाक्यम्?": "string?", "सूची?": "list?", "कोशः?": "dict?",
    "सूची": "list", "प्रथमः": "head", "शेषः": "tail", "स्थानम्": "nth", "परिमाणम्": "len",
    "योजय": "append", "पुरतः": "cons", "श्रेणी": "range", "विपर्ययः": "reverse", "क्रमय": "sort",
    "कोशः": "dict", "गृहाण": "get", "कुञ्चिकाः": "keys", "संयोजय": "assoc",
    "वाक्यम्": "str", "विभज": "split", "संयुज": "join", "बृहत्": "upper", "लघु": "lower", "अन्तर्गतम्": "contains",
    "खण्डः": "slice", "दोषः": "error",
    "प्रत्येकम्": "map", "चयनम्": "filter", "संकलनम्": "reduce",
}

# ---- the standard capability labels and handler parameters ---------------------
NAMES: Dict[str, str] = {
    "भाण्डारः": "store",     # bhāṇḍāraḥ  - storehouse
    "पटलः":    "console",   # paṭalaḥ    - screen
    "घटिका":   "clock",     # ghaṭikā    - clock
    "यन्त्रम्": "kernel",    # yantram    - the machine
    "अग्रिमः":  "next",      # agrimaḥ    - the next one
    "प्रेषकः":  "from",      # preṣakaḥ   - sender
    "विषयः":   "body",      # viṣayaḥ    - subject matter
}

# ---- operation names inside (call cap "op" ...) --------------------------------
OPS: Dict[str, str] = {
    "प्राप्":   "get",      # prāp     - obtain
    "स्थापय":  "put",      # sthāpaya - place
    "अन्विष्य": "find",     # anviṣya  - search
    "वंशः":    "lineage",  # vaṃśaḥ   - lineage
    "मुद्रय":  "print",    # mudraya  - print
    "अधुना":   "now",      # adhunā   - now
    "प्रेषय":  "send",     # preṣaya  - send
    "समर्पय":  "delegate", # samarpaya- hand over
    "कोष्ठाः":  "cells",    # koṣṭhāḥ  - the cells
}

LITERALS: Dict[str, str] = {"सत्यम्": "true", "असत्यम्": "false", "शून्यम्": "nil"}

SYMBOLS: Dict[str, str] = {**FORMS, **BUILTINS, **NAMES, **LITERALS}

DIGITS = str.maketrans("०१२३४५६७८९", "0123456789")


def canonical_symbol(token: str) -> str:
    """Map a Sanskrit symbol (or Devanagari number) to its canonical spelling; others unchanged."""
    if token in SYMBOLS:
        return SYMBOLS[token]
    t = token.translate(DIGITS)
    return t if t != token and t.lstrip("-").isdigit() else token


def canonical_op(op: str) -> str:
    return OPS.get(op, op)


def to_sanskrit(src: str) -> str:
    """Best-effort rendering of an English Verum Script program in Sanskrit (for display)."""
    rev = {v: k for k, v in SYMBOLS.items()}
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"':
            j = src.find('"', i + 1)
            j = n if j < 0 else j
            out.append(src[i:j + 1]); i = j + 1
        elif c == ';':
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(src[i:j]); i = j
        elif c.isspace() or c in "()":
            out.append(c); i += 1
        else:
            j = i
            while j < n and not src[j].isspace() and src[j] not in "();":
                j += 1
            tok = src[i:j]
            out.append(rev.get(tok, tok)); i = j
    return "".join(out)
