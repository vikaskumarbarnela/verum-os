"""Capabilities: the only form of authority in VERUM.

There is no root, no administrator, no user accounts, no permission bits.
A principal (the shell, or a cell) can do exactly what the capabilities it
holds allow - nothing more. A capability is an unforgeable token that names:

    kind     what it reaches       ("store", "console", "clock", "cell", "kernel")
    target   which instance        (a cell id for kind "cell", else None)
    rights   which operations      (subset of the kind's rights)
    holder   who may present it    (checked on every use - a leaked token is useless)

Capabilities can only be *attenuated* (narrowed), never widened. Revoking a
capability revokes everything derived from it. Because authority is never
ambient, there is nothing for an attacker to escalate into.
"""

from __future__ import annotations

import secrets
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional


# "delegate" is valid on every kind but kernel: its holder may hand a narrower
# copy of that capability to a cell it is linked to.
RIGHTS: Dict[str, FrozenSet[str]] = {
    "store":   frozenset({"get", "put", "find", "lineage", "delegate"}),
    "console": frozenset({"print", "delegate"}),
    "clock":   frozenset({"now", "delegate"}),
    "cell":    frozenset({"send", "delegate"}),
    "kernel":  frozenset({"spawn", "mint", "revoke", "cells"}),
}


@dataclass
class Cap:
    token: str
    kind: str
    rights: FrozenSet[str]
    holder: str
    target: Optional[str] = None
    parent: Optional[str] = None
    revoked: bool = False
    children: List[str] = field(default_factory=list)

    def describe(self) -> str:
        tgt = f"->{self.target}" if self.target else ""
        state = " (REVOKED)" if self.revoked else ""
        return f"{self.kind}{tgt}[{','.join(sorted(self.rights))}] held by {self.holder}{state}"


class CapError(Exception):
    pass


class CapTable:
    def __init__(self) -> None:
        self._caps: Dict[str, Cap] = {}

    def mint(self, kind: str, rights, holder: str, target: Optional[str] = None,
             token: Optional[str] = None, parent: Optional[str] = None) -> Cap:
        if kind not in RIGHTS:
            raise CapError(f"unknown capability kind {kind!r}")
        rights = frozenset(rights)
        if not rights <= RIGHTS[kind]:
            raise CapError(f"rights {sorted(rights - RIGHTS[kind])} do not exist for kind {kind!r}")
        token = token or secrets.token_hex(16)
        cap = Cap(token=token, kind=kind, rights=rights, holder=holder, target=target, parent=parent)
        self._caps[token] = cap
        if parent:
            self._caps[parent].children.append(token)
        return cap

    def attenuate(self, token: str, by: str, rights, new_holder: str,
                  new_token: Optional[str] = None) -> Cap:
        """Derive a narrower capability for someone else. Never wider."""
        src = self.check(token, by)
        rights = frozenset(rights)
        if not rights <= src.rights:
            raise CapError(f"cannot grant rights {sorted(rights - src.rights)} you do not hold")
        return self.mint(src.kind, rights, new_holder, target=src.target, token=new_token, parent=token)

    def revoke(self, token: str, by: str) -> List[str]:
        """Holder of a cap, or holder of any ancestor, may revoke it and all its descendants."""
        cap = self._caps.get(token)
        if cap is None:
            raise CapError("no such capability")
        if not self._is_ancestor_holder(cap, by):
            raise CapError(f"{by} does not control this capability")
        return self._revoke_tree(cap)

    def _is_ancestor_holder(self, cap: Cap, who: str) -> bool:
        cur: Optional[Cap] = cap
        while cur is not None:
            if cur.holder == who:
                return True
            cur = self._caps.get(cur.parent) if cur.parent else None
        return False

    def _revoke_tree(self, cap: Cap) -> List[str]:
        out = []
        stack = [cap]
        while stack:
            c = stack.pop()
            if not c.revoked:
                c.revoked = True
                out.append(c.token)
            stack.extend(self._caps[ch] for ch in c.children)
        return out

    def check(self, token: str, holder: str, right: Optional[str] = None) -> Cap:
        """The gate every syscall passes through."""
        cap = self._caps.get(token) if isinstance(token, str) else None
        if cap is None:
            raise CapError("invalid capability")
        if cap.revoked:
            raise CapError("capability has been revoked")
        if cap.holder != holder:
            # A token stolen from another principal is worthless.
            raise CapError("capability is not held by the caller")
        if right is not None and right not in cap.rights:
            raise CapError(f"capability lacks right {right!r} (has {sorted(cap.rights)})")
        return cap

    def get(self, token: str) -> Optional[Cap]:
        return self._caps.get(token)

    def held_by(self, holder: str) -> List[Cap]:
        return [c for c in self._caps.values() if c.holder == holder]

    def all(self) -> List[Cap]:
        return list(self._caps.values())
