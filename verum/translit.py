"""लिपि - Latin-keyboard transliteration into Devanagari (and back).

Scheme: Harvard-Kyoto, the plain-ASCII convention used in Sanskrit scholarship,
plus the common spellings people type anyway (aa ii uu for long vowels,
sh/Sh for श/ष, chh for छ, RRi for ऋ).

    paribhASA   -> परिभाषा        saMskRta  -> संस्कृत
    astu        -> अस्तु          AhvAnam   -> आह्वानम्
    zabdAH      -> शब्दाः         sUcI      -> सूची

Vowels:      a A i I u U R RR lR e ai o au       (aa ii uu RRi also accepted)
Consonants:  k kh g gh G  c ch j jh J  T Th D Dh N  t th d dh n  p ph b bh m
             y r l v  z S s h             (sh = z, Sh = S, chh = ch)
Marks:       M anusvara (ं)   H visarga (ः)   ~ candrabindu (ँ)   ' avagraha (ऽ)
Digits:      0-9 -> ०-९
A consonant not followed by a vowel gets a virama (halant); write the vowel
`a` explicitly when you mean it:  rAma -> राम,  rAm -> राम्.
"""

from __future__ import annotations

from typing import Dict, List, Tuple

VOWELS: Dict[str, Tuple[str, str]] = {        # independent form, dependent sign (matra)
    "a": ("अ", ""), "A": ("आ", "ा"), "aa": ("आ", "ा"),
    "i": ("इ", "ि"), "I": ("ई", "ी"), "ii": ("ई", "ी"),
    "u": ("उ", "ु"), "U": ("ऊ", "ू"), "uu": ("ऊ", "ू"),
    "R": ("ऋ", "ृ"), "RRi": ("ऋ", "ृ"), "RR": ("ॠ", "ॄ"), "lR": ("ऌ", "ॢ"),
    "e": ("ए", "े"), "ai": ("ऐ", "ै"), "o": ("ओ", "ो"), "au": ("औ", "ौ"),
}

CONSONANTS: Dict[str, str] = {
    "k": "क", "kh": "ख", "g": "ग", "gh": "घ", "G": "ङ",
    "c": "च", "ch": "छ", "chh": "छ", "j": "ज", "jh": "झ", "J": "ञ",
    "T": "ट", "Th": "ठ", "D": "ड", "Dh": "ढ", "N": "ण",
    "t": "त", "th": "थ", "d": "द", "dh": "ध", "n": "न",
    "p": "प", "ph": "फ", "b": "ब", "bh": "भ", "m": "म",
    "y": "य", "r": "र", "l": "ल", "v": "व", "w": "व",
    "z": "श", "sh": "श", "S": "ष", "Sh": "ष", "s": "स", "h": "ह",
    "L": "ळ", "kS": "क्ष", "kSh": "क्ष", "jJ": "ज्ञ",
}

MARKS: Dict[str, str] = {"M": "ं", "H": "ः", "~": "ँ", "'": "ऽ", "|": "।", "||": "॥"}
DIGITS = str.maketrans("0123456789", "०१२३४५६७८९")
VIRAMA = "्"

_KEYS = sorted(set(VOWELS) | set(CONSONANTS) | set(MARKS), key=len, reverse=True)


def to_devanagari(text: str) -> str:
    """Transliterate one word or line. Characters outside the scheme pass through."""
    out: List[str] = []
    i, n = 0, len(text)
    pending_consonant = False               # the previous output was a consonant awaiting its vowel
    while i < n:
        hit = None
        for k in _KEYS:
            if text.startswith(k, i):
                hit = k
                break
        if hit is None:
            if pending_consonant:
                out.append(VIRAMA)
                pending_consonant = False
            c = text[i]
            out.append(c.translate(DIGITS) if c.isdigit() else c)
            i += 1
            continue
        if hit in CONSONANTS:
            if pending_consonant:
                out.append(VIRAMA)
            out.append(CONSONANTS[hit])
            pending_consonant = True
        elif hit in VOWELS:
            independent, sign = VOWELS[hit]
            if pending_consonant:
                out.append(sign)            # "a" has no sign: the inherent vowel
                pending_consonant = False
            else:
                out.append(independent)
        else:                               # marks
            if pending_consonant:
                out.append(VIRAMA)
                pending_consonant = False
            out.append(MARKS[hit])
        i += len(hit)
    if pending_consonant:
        out.append(VIRAMA)
    return "".join(out)


_REV_V = {v[0]: k for k, v in VOWELS.items() if len(k) <= 2 and k not in ("aa", "ii", "uu")}
_REV_SIGN = {v[1]: k for k, v in VOWELS.items() if v[1] and k not in ("aa", "ii", "uu", "RRi")}
_REV_C = {v: k for k, v in CONSONANTS.items() if k not in ("chh", "sh", "Sh", "w", "kS", "kSh", "jJ")}
_REV_M = {v: k for k, v in MARKS.items() if k != "||"}
_REV_D = str.maketrans("०१२३४५६७८९", "0123456789")


def to_latin(text: str) -> str:
    """Devanagari back to Harvard-Kyoto (for showing what to type)."""
    out: List[str] = []
    chars = list(text)
    i = 0
    while i < len(chars):
        c = chars[i]
        if c in _REV_C:
            nxt = chars[i + 1] if i + 1 < len(chars) else ""
            if nxt == VIRAMA:
                out.append(_REV_C[c]); i += 2
            elif nxt in _REV_SIGN:
                out.append(_REV_C[c] + _REV_SIGN[nxt]); i += 2
            else:
                out.append(_REV_C[c] + "a"); i += 1
        elif c in _REV_V:
            out.append(_REV_V[c]); i += 1
        elif c in _REV_M:
            out.append(_REV_M[c]); i += 1
        else:
            out.append(c.translate(_REV_D)); i += 1
    return "".join(out)


def is_devanagari(text: str) -> bool:
    return any("ऀ" <= ch <= "ॿ" for ch in text)


# --------------------------------------------------------------------------- shell helper

def transliterate_line(line: str, keep: set) -> str:
    """Transliterate a shell line: tokens in `keep` (shell intents, English Verum Script words,
    capability labels) stay as they are; `:word` forces transliteration; `\\word` forces literal;
    everything else that is purely Latin letters is transliterated, including inside strings."""
    out: List[str] = []
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c == '"':                                      # string: transliterate its content
            j = line.find('"', i + 1)
            j = n if j < 0 else j
            out.append('"' + to_devanagari(line[i + 1:j]) + '"')
            i = j + 1
            continue
        if c.isspace() or c in "()":
            out.append(c); i += 1
            continue
        j = i
        while j < n and not line[j].isspace() and line[j] not in "()\"":
            j += 1
        tok = line[i:j]
        if tok.startswith(":"):
            out.append(to_devanagari(tok[1:]))
        elif tok.startswith("\\"):
            out.append(tok[1:])
        elif tok in keep or not tok.replace("'", "").replace("~", "").isalpha() or is_devanagari(tok):
            out.append(tok)
        else:
            out.append(to_devanagari(tok))
        i = j
    return "".join(out)


if __name__ == "__main__":
    import sys
    data = sys.stdin.read() if len(sys.argv) < 2 else open(sys.argv[1], encoding="utf-8").read()
    sys.stdout.reconfigure(encoding="utf-8")
    print(to_devanagari(data), end="")
