"""Content-addressed object store and hash-chained ledger.

There are no files and no paths in VERUM. An object is immutable and is named
by the SHA-256 of its canonical JSON encoding. Changing a single byte changes
the name, so nothing can be silently tampered with. Every object records its
provenance: which cell created it, by which operation, from which inputs.

The ledger is the single source of truth for the whole system: an append-only
list of events, each hashed together with the hash of its predecessor. The
kernel's in-memory state is nothing more than a replay of the ledger.
"""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional


def canonical(value: Any) -> str:
    """Deterministic JSON encoding: same value -> same bytes -> same hash."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def digest(value: Any) -> str:
    return hashlib.sha256(canonical(value).encode("utf-8")).hexdigest()


# --------------------------------------------------------------------------- objects


@dataclass(frozen=True)
class Obj:
    kind: str                      # "note", "code", "stats", ... free-form
    data: Any                      # JSON-like payload
    tags: tuple                    # how humans find it (there are no paths)
    by: str                        # principal that created it ("shell", a cell id)
    op: str                        # operation that produced it ("note", "run", ...)
    inputs: tuple                  # hashes of objects it was derived from
    tick: int                      # logical time of creation

    def to_dict(self) -> Dict[str, Any]:
        return {
            "kind": self.kind,
            "data": self.data,
            "tags": list(self.tags),
            "by": self.by,
            "op": self.op,
            "inputs": list(self.inputs),
            "tick": self.tick,
        }

    @staticmethod
    def from_dict(d: Dict[str, Any]) -> "Obj":
        return Obj(
            kind=d["kind"],
            data=d["data"],
            tags=tuple(d["tags"]),
            by=d["by"],
            op=d["op"],
            inputs=tuple(d["inputs"]),
            tick=d["tick"],
        )

    @property
    def hash(self) -> str:
        return digest(self.to_dict())


class Store:
    """Immutable objects indexed by hash and by tag."""

    def __init__(self) -> None:
        self._objects: Dict[str, Obj] = {}
        self._by_tag: Dict[str, List[str]] = {}

    def put(self, obj: Obj) -> str:
        h = obj.hash
        if h in self._objects:          # identical content already exists: same name
            return h
        self._objects[h] = obj
        for t in obj.tags:
            self._by_tag.setdefault(t, []).append(h)
        return h

    def get(self, h: str) -> Optional[Obj]:
        return self._objects.get(h)

    def resolve(self, prefix: str) -> Optional[str]:
        """Allow humans to type a short unique prefix instead of 64 hex chars."""
        if prefix in self._objects:
            return prefix
        matches = [h for h in self._objects if h.startswith(prefix)]
        return matches[0] if len(matches) == 1 else None

    def find(self, tag: str) -> List[str]:
        return list(self._by_tag.get(tag, []))

    def tags(self) -> Dict[str, int]:
        return {t: len(hs) for t, hs in sorted(self._by_tag.items())}

    def __len__(self) -> int:
        return len(self._objects)

    def __iter__(self) -> Iterable[str]:
        return iter(self._objects)

    def lineage(self, h: str, depth: int = 0, seen=None) -> List[tuple]:
        """Provenance tree: [(depth, hash, obj), ...] walking back through inputs."""
        seen = seen if seen is not None else set()
        out: List[tuple] = []
        obj = self.get(h)
        if obj is None or h in seen:
            return out
        seen.add(h)
        out.append((depth, h, obj))
        for i in obj.inputs:
            out.extend(self.lineage(i, depth + 1, seen))
        return out


# --------------------------------------------------------------------------- ledger


@dataclass
class Entry:
    seq: int
    prev: str
    event: Dict[str, Any]
    hash: str = field(default="")

    def compute_hash(self) -> str:
        return digest({"seq": self.seq, "prev": self.prev, "event": self.event})


GENESIS = "0" * 64


class Ledger:
    """Append-only, hash-chained event log, optionally persisted as JSON lines."""

    def __init__(self, path: Optional[str] = None) -> None:
        self.path = path
        self.entries: List[Entry] = []

    @property
    def head(self) -> str:
        return self.entries[-1].hash if self.entries else GENESIS

    def append(self, event: Dict[str, Any]) -> Entry:
        e = Entry(seq=len(self.entries), prev=self.head, event=event)
        e.hash = e.compute_hash()
        self.entries.append(e)
        if self.path:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(canonical({"seq": e.seq, "prev": e.prev, "event": e.event, "hash": e.hash}) + "\n")
        return e

    def verify(self) -> tuple[bool, str]:
        prev = GENESIS
        for i, e in enumerate(self.entries):
            if e.seq != i:
                return False, f"entry {i}: sequence number is {e.seq}"
            if e.prev != prev:
                return False, f"entry {i}: broken chain (prev mismatch)"
            if e.compute_hash() != e.hash:
                return False, f"entry {i}: content does not match its hash"
            prev = e.hash
        return True, f"{len(self.entries)} entries, chain intact, head {self.head[:12]}"

    @staticmethod
    def load(path: str) -> "Ledger":
        led = Ledger(path=None)
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    d = json.loads(line)
                    led.entries.append(Entry(seq=d["seq"], prev=d["prev"], event=d["event"], hash=d["hash"]))
        led.path = path
        return led
