"""Emitter core: handler registry, `Op` helper, per-function emission and control flow."""

from __future__ import annotations

import functools
import importlib
import importlib.util
from collections import Counter
from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass, field

from .emit_db import GROUP_OF, GROUPS
from .ppu import Insn
from .ppu_flow import CodeImage, flow_of
from .ppu_format import format_insn

_MASK32 = 0xFFFFFFFF
_MASK64 = 0xFFFFFFFFFFFFFFFF
_SITE_EXAMPLES = 5
_INDENT = "    "

# BO value bits that make a conditional branch unconditional: "ignore CR" and "ignore CTR".
_BO_ALWAYS = 0x14


def u32(v: int) -> str:
    return "0x%08xu" % (v & _MASK32)


def u64(v: int) -> str:
    return "0x%016xull" % (v & _MASK64)


@dataclass(frozen=True)
class EmitEnv:
    names: Mapping[int, str]                        # function entry -> C name ("fn_%08x")
    thunks: Mapping[int, str]                       # import thunk addr -> trampoline ("imp_%08x")
    switch_targets: Mapping[int, tuple[int, ...]]   # bcctr site -> sorted distinct targets
    comments: bool = False


@dataclass
class EmitStats:
    edges: Counter = field(default_factory=Counter)
    unhandled: Counter = field(default_factory=Counter)
    unhandled_sites: dict[str, list[int]] = field(default_factory=dict)
    dynamic_targets: list[tuple[int, int, str]] = field(default_factory=list)
    labels: int = 0
    loop_hooks: int = 0
    functions: int = 0
    instructions: int = 0


class Op:
    """What a handler sees: the decoded instruction plus C-expression helpers."""

    def __init__(self, insn: Insn) -> None:
        spec = insn.spec
        self.insn = insn
        self.addr = insn.addr
        self.word = insn.word
        self.name = spec.name
        self.rc = bool(("rc" in spec.flags and insn.field("Rc"))
                       or ("rc_vc" in spec.flags and insn.field("Rc_vc")))
        self.oe = bool("oe" in spec.flags and insn.field("OE"))
        self.unhandled_key: str | None = None

    def f(self, field_name: str) -> int:
        return self.insn.field(field_name)

    def gpr(self, n: int) -> str:
        return "ctx->gpr[%d]" % n

    def fpr(self, n: int) -> str:
        return "ctx->fpr[%d]" % n

    def vr(self, n: int) -> str:
        return "ctx->vr[%d]" % n

    def ea_d(self, field_name: str = "D") -> str:
        ra, disp = self.f("rA"), self.f(field_name)
        if ra == 0:
            return u32(disp)
        return "(uint32_t)(%s + %s)" % (self.gpr(ra), u64(disp))

    def ea_x(self) -> str:
        ra, rb = self.f("rA"), self.f("rB")
        if ra == 0:
            return "(uint32_t)%s" % self.gpr(rb)
        return "(uint32_t)(%s + %s)" % (self.gpr(ra), self.gpr(rb))

    def ea64_d(self, field_name: str = "D") -> str:
        ra, disp = self.f("rA"), self.f(field_name)
        if ra == 0:
            return u64(disp)
        return "(%s + %s)" % (self.gpr(ra), u64(disp))

    def ea64_x(self) -> str:
        ra, rb = self.f("rA"), self.f("rB")
        if ra == 0:
            return self.gpr(rb)
        return "(%s + %s)" % (self.gpr(ra), self.gpr(rb))

    def record(self, value: str) -> str:
        return "dalRecord(ctx, %s);" % value

    def unsupported(self, reason: str) -> None:
        self.unhandled_key = "%s:%s" % (self.name, reason)
        return None


Handler = Callable[[Op], "list[str] | None"]

_REGISTRY: dict[str, Handler] = {}


def handler(*names: str) -> Callable[[Handler], Handler]:
    for name in names:
        if name not in GROUP_OF:
            raise ValueError("unknown mnemonic %r" % name)
    if len(set(names)) != len(names):
        raise ValueError("mnemonic listed twice in one registration: %s" % ", ".join(names))

    def register(fn: Handler) -> Handler:
        for name in names:
            if name in _REGISTRY:
                raise ValueError("duplicate handler for %r" % name)
        for name in names:
            _REGISTRY[name] = fn
        return fn

    return register


def load_handlers() -> dict[str, Handler]:
    """Import every existing `emit_ops_<group>` module and return the registry."""
    package = __package__ or "ps3recomp"
    for group in GROUPS:
        if group == "control":
            continue
        module_name = "%s.emit_ops_%s" % (package, group)
        if importlib.util.find_spec(module_name) is None:
            continue
        importlib.import_module(module_name)
        for name, fn in _REGISTRY.items():
            if fn.__module__ == module_name and GROUP_OF[name] != group:
                raise ValueError("%s registers %r, which belongs to group %s"
                                 % (module_name, name, GROUP_OF[name]))
    return dict(_REGISTRY)


@functools.lru_cache(maxsize=1)
def _default_handlers() -> dict[str, Handler]:
    return load_handlers()


def _always(insn: Insn) -> bool:
    return insn.field("BO") & _BO_ALWAYS == _BO_ALWAYS


class _FunctionEmitter:
    """Collects the statements of one function and the labels they reference."""

    def __init__(self, image: CodeImage, addrs: list[int], env: EmitEnv, stats: EmitStats,
                 handlers: Mapping[str, Handler]) -> None:
        self.image = image
        self.addrs = addrs
        self.owned = frozenset(addrs)
        self.env = env
        self.stats = stats
        self.handlers = handlers
        self.targeted: set[int] = set()

    def count_unhandled(self, key: str, site: int) -> None:
        self.stats.unhandled[key] += 1
        sites = self.stats.unhandled_sites.setdefault(key, [])
        if len(sites) < _SITE_EXAMPLES:
            sites.append(site)

    def unhandled_stmt(self, key: str, site: int, word: int) -> list[str]:
        self.count_unhandled(key, site)
        return ["DAL_UNHANDLED(%s, %s);" % (u32(site), u32(word))]

    # Direct transfer targets are classified one of: intra, function, import, dynamic.
    def classify(self, target: int, *, allow_intra: bool) -> str:
        if allow_intra and target in self.owned:
            return "intra"
        if target in self.env.names:
            return "function"
        if target in self.env.thunks:
            return "import"
        return "dynamic"

    def callee(self, kind: str, target: int) -> str:
        return (self.env.names if kind == "function" else self.env.thunks)[target]

    def tail_stmt(self, site: int, target: int, edge: str, dynamic_kind: str) -> list[str]:
        """Transfer to `target` that never returns here (b, bc, fall-through, switch case)."""
        kind = self.classify(target, allow_intra=edge != "fallthrough_tails")
        if kind == "intra":
            self.targeted.add(target)
            stmt = "goto L_%08x;" % target
            if target <= site:
                self.stats.loop_hooks += 1
                return ["{ DAL_LOOP(%s); %s }" % (u32(site), stmt)]
            return [stmt]
        if kind == "dynamic":
            self.stats.dynamic_targets.append((site, target, dynamic_kind))
            self.stats.edges["fallthrough_tails" if edge == "fallthrough_tails"
                             else "tails_dynamic"] += 1
            return ["DAL_TAIL_DYN(%s, %s);" % (u32(site), u32(target))]
        if edge == "fallthrough_tails":
            self.stats.edges[edge] += 1
        else:
            self.stats.edges["tails_direct" if kind == "function" else "tails_import"] += 1
        return ["DAL_TAIL(%s);" % self.callee(kind, target)]

    def call_stmt(self, site: int, target: int) -> list[str]:
        ret = u32(site + 4)
        kind = self.classify(target, allow_intra=False)
        if kind == "dynamic":
            self.stats.dynamic_targets.append((site, target, "call"))
            self.stats.edges["calls_dynamic_const"] += 1
            return ["DAL_CALL_DYN(%s, %s, %s);" % (u32(site), u32(target), ret)]
        self.stats.edges["calls_direct" if kind == "function" else "calls_import"] += 1
        return ["DAL_CALL(%s, %s);" % (self.callee(kind, target), ret)]

    def indirect_stmt(self, site: int, reg: str, link: bool) -> list[str]:
        if link:
            self.stats.edges["calls_indirect"] += 1
            return ["DAL_CALL_DYN(%s, ctx->%s, %s);" % (u32(site), reg, u32(site + 4))]
        self.stats.edges["tails_dynamic"] += 1
        return ["DAL_TAIL_DYN(%s, ctx->%s);" % (u32(site), reg)]

    def control(self, op: Op) -> list[str] | None:
        """Statements for a control-group instruction, or None when it is not supported."""
        insn, site, name = op.insn, op.addr, op.name
        link = bool(insn.field("LK")) if name in ("b", "bc", "bclr", "bcctr") else False
        if name in ("b", "bc"):
            flow = flow_of(insn)
            if name == "bc" and not _always(insn):
                return None
            if flow.kind == "seq":
                # get-PC idiom `bcl 20,31,$+4`: only LR is written.
                return ["ctx->lr = %s;" % u32(site + 4)]
            if link:
                return self.call_stmt(site, flow.target)
            return self.tail_stmt(site, flow.target, "tails", "tail")
        if name in ("bclr", "bcctr"):
            if not _always(insn):
                return None
            if name == "bclr":
                if link:
                    return self.indirect_stmt(site, "lr", True)
                self.stats.edges["returns"] += 1
                return ["return;"]
            return self.indirect_stmt(site, "ctr", link)
        if name == "sc":
            self.stats.edges["syscalls"] += 1
            return ["DAL_SYSCALL(%s);" % u32(site)]
        if name in ("tw", "twi", "td", "tdi") and insn.field("TO") == 31:
            self.stats.edges["traps"] += 1
            return ["DAL_TRAP(%s);" % u32(site)]
        return None

    def instruction(self, addr: int) -> list[str]:
        image = self.image
        if not image.valid(addr):
            return self.unhandled_stmt("invalid", addr, image.word(addr))
        insn = image.insn(addr)
        op = Op(insn)
        if op.name in ("b", "bc", "bclr", "bcctr", "sc", "tw", "twi", "td", "tdi"):
            lines = self.control(op)
            if lines is None:
                return self.unhandled_stmt(op.unhandled_key or op.name, addr, op.word)
            return lines
        fn = self.handlers.get(op.name)
        lines = None if fn is None else fn(op)
        if lines is None:
            return self.unhandled_stmt(op.unhandled_key or op.name, addr, op.word)
        if len(lines) > 1:
            # Handler temporaries (t0, t1, ...) stay local to the instruction.
            return ["{"] + [_INDENT + line for line in lines] + ["}"]
        return lines

    def fall_through(self, addr: int) -> list[str]:
        image = self.image
        if not image.valid(addr) or not flow_of(image.insn(addr)).falls_through:
            return []
        nxt = addr + 4
        if nxt in self.owned:
            return []
        return self.tail_stmt(addr, nxt, "fallthrough_tails", "fallthrough")


def _comment_text(text: str) -> str:
    # A trailing backslash would splice the next line into the comment.
    return "".join(c if 0x20 <= ord(c) < 0x7F and c != "\\" else "?" for c in text)


def emit_function(image: CodeImage, entry: int, addrs: Iterable[int], env: EmitEnv,
                  stats: EmitStats, *, name: str, title: str | None = None,
                  handlers: Mapping[str, Handler] | None = None) -> str:
    addr_list = sorted(addrs)
    em = _FunctionEmitter(image, addr_list, env, stats,
                          _default_handlers() if handlers is None else handlers)
    blocks: list[tuple[int, list[str]]] = []
    for addr in addr_list:
        blocks.append((addr, em.instruction(addr) + em.fall_through(addr)))

    out: list[str] = []
    if title:
        out.append("// %s" % _comment_text(title))
    out.append("DAL_PPU_FUNC(%s) {" % name)
    out.append(_INDENT + "DAL_ENTER(%s);" % u32(entry))
    for addr, lines in blocks:
        if addr in em.targeted:
            out.append("L_%08x:" % addr)
        if env.comments:
            out.append(_INDENT + "// %s: %08x  %s"
                       % (u32(addr)[:-1], image.word(addr), format_insn(image.insn(addr), True)))
        out.extend(_INDENT + line for line in lines)
    out.append("}")

    stats.functions += 1
    stats.instructions += len(addr_list)
    stats.labels += len(em.targeted)
    return "\n".join(out) + "\n"
