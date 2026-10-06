"""Syntax normalisation between Ghidra's listing text and our decoder's text."""

from __future__ import annotations

import re
from collections.abc import Callable
from functools import lru_cache

from .ppu import Insn
from .ppu_format import format_insn

Token = str | int

_MEM_OPERAND = re.compile(r"^(.*)\((r\d+)\)$")
_INT = re.compile(r"^-?(?:0x[0-9a-f]+|\d+)$")
_INT_MODULI = tuple(1 << k for k in (16, 32, 64))

# Per normalised Ghidra mnemonic: rewrite into the form we print. Each entry must be a pure
# syntax change.
GHIDRA_REWRITES: dict[str, Callable[[tuple[Token, ...]], tuple[str, tuple[Token, ...]]]] = {}

# (our raw spec name or None if we cannot decode, normalised Ghidra mnemonic or None if Ghidra
# cannot decode) -> reason. Ghidra-side limitations only, never our bugs.
KNOWN_GHIDRA_DIFFERENCES: dict[tuple[str | None, str | None], str] = {}


def normalize_mnemonic(mnemonic: str) -> str:
    mnemonic = mnemonic.lower()
    # Branch prediction hint, e.g. "beq+".
    if mnemonic.endswith(("+", "-")):
        mnemonic = mnemonic[:-1]
    return mnemonic


def _parse_int(text: str) -> int:
    negative = text.startswith("-")
    body = text[1:] if negative else text
    value = int(body, 16) if body.startswith("0x") else int(body, 10)
    return -value if negative else value


def _token(text: str) -> Token:
    return _parse_int(text) if _INT.match(text) else text


def _drop_cr0(tokens: tuple[Token, ...]) -> tuple[Token, ...]:
    return tokens[1:] if tokens and tokens[0] == "cr0" else tokens


@lru_cache(maxsize=1 << 16)
def normalize(mnemonic: str, operands: str | None) -> tuple[str, tuple[Token, ...]]:
    tokens: list[Token] = []
    text = "".join((operands or "").lower().split())
    for part in text.split(",") if text else ():
        m = _MEM_OPERAND.match(part)
        if m:
            if m.group(1):
                tokens.append(_token(m.group(1)))
            tokens.append(m.group(2))
        else:
            tokens.append(_token(part))
    return normalize_mnemonic(mnemonic), _drop_cr0(tuple(tokens))


def _ints_equal(a: int, b: int) -> bool:
    # Ghidra prints some fields sign-extended or zero-extended, we print them as encoded.
    return a == b or any(a % m == b % m for m in _INT_MODULI)


def _tokens_equal(a: tuple[Token, ...], b: tuple[Token, ...]) -> bool:
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if isinstance(x, int) and isinstance(y, int):
            if not _ints_equal(x, y):
                return False
        elif x != y:
            return False
    return True


def _normalize_text(text: str) -> tuple[str, tuple[Token, ...]]:
    mnemonic, _, operands = text.partition(" ")
    return normalize(mnemonic, operands)


def ghidra_form(mnemonic: str, operands: str | None) -> tuple[str, tuple[Token, ...]]:
    name, tokens = normalize(mnemonic, operands)
    rewrite = GHIDRA_REWRITES.get(name)
    if rewrite is not None:
        name, tokens = rewrite(tokens)
        tokens = _drop_cr0(tokens)
    return name, tokens


def compare(insn: Insn, ghidra_mnemonic: str, ghidra_operands: str | None) -> str:
    """Returns "match", "mnemonic_mismatch" or "operand_mismatch" for a decoded `insn`."""
    g_name, g_tokens = ghidra_form(ghidra_mnemonic, ghidra_operands)
    same_mnemonic = False
    for simplify in (True, False):
        name, tokens = _normalize_text(format_insn(insn, simplify))
        if name != g_name:
            continue
        if _tokens_equal(tokens, g_tokens):
            return "match"
        same_mnemonic = True
    return "operand_mismatch" if same_mnemonic else "mnemonic_mismatch"
