"""Builders for synthetic big-endian ELF images used by the unit tests."""

from __future__ import annotations

import struct
from dataclasses import dataclass

PT_LOAD = 1

_SHT_STRTAB = 3


@dataclass
class FixtureSegment:
    p_type: int
    vaddr: int
    data: bytes
    memsz: int | None = None
    flags: int = 4
    align: int = 0x10


@dataclass
class FixtureSection:
    name: str
    sh_type: int
    addr: int
    size: int
    flags: int = 0


def _align(value: int, alignment: int = 16) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def _ident(ei_class: int, osabi: int) -> bytes:
    return bytes([0x7F, 0x45, 0x4C, 0x46, ei_class, 2, 1, osabi]) + bytes(8)


def _layout_segments(segments: list[FixtureSegment], data_start: int) -> list[int]:
    offsets = []
    pos = data_start
    for seg in segments:
        pos = _align(pos)
        offsets.append(pos)
        pos += len(seg.data)
    return offsets


def build_elf64_be(*, entry: int, segments: list[FixtureSegment],
                   sections: list[FixtureSection] | None = None,
                   e_type: int = 2, machine: int = 21, osabi: int = 0x66) -> bytes:
    ehsize, phentsize, shentsize = 64, 56, 64
    phnum = len(segments)
    offsets = _layout_segments(segments, ehsize + phentsize * phnum)

    buf = bytearray(ehsize + phentsize * phnum)
    for seg, off in zip(segments, offsets):
        buf.extend(bytes(off - len(buf)))
        buf.extend(seg.data)

    shoff = shnum = shstrndx = 0
    if sections is not None:
        # Name offsets into .shstrtab; the leading NUL gives the null section "".
        strtab = bytearray(b"\0")
        name_offs = []
        for sec in sections:
            if sec.name:
                name_offs.append(len(strtab))
                strtab.extend(sec.name.encode("ascii") + b"\0")
            else:
                name_offs.append(0)
        strtab_name = len(strtab)
        strtab.extend(b".shstrtab\0")

        strtab_off = _align(len(buf))
        buf.extend(bytes(strtab_off - len(buf)))
        buf.extend(strtab)
        shoff = _align(len(buf))
        buf.extend(bytes(shoff - len(buf)))
        shnum = len(sections) + 2
        shstrndx = shnum - 1
        buf.extend(bytes(shentsize * shnum))
        missing_off = len(buf) + 0x1000

        def section_offset(addr: int) -> int:
            for seg, off in zip(segments, offsets):
                if seg.p_type == PT_LOAD and seg.vaddr <= addr < seg.vaddr + len(seg.data):
                    return off + (addr - seg.vaddr)
            return missing_off

        rows = [struct.pack(">IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)]
        for sec, name_off in zip(sections, name_offs):
            rows.append(struct.pack(">IIQQQQIIQQ", name_off, sec.sh_type, sec.flags,
                                    sec.addr, section_offset(sec.addr), sec.size,
                                    0, 0, 1, 0))
        rows.append(struct.pack(">IIQQQQIIQQ", strtab_name, _SHT_STRTAB, 0, 0,
                                strtab_off, len(strtab), 0, 0, 1, 0))
        buf[shoff:shoff + shentsize * shnum] = b"".join(rows)

    ehdr = struct.pack(">16sHHIQQQIHHHHHH", _ident(2, osabi), e_type, machine, 1, entry,
                       ehsize if phnum else 0, shoff, 0, ehsize, phentsize, phnum,
                       shentsize, shnum, shstrndx)
    buf[0:ehsize] = ehdr
    for i, (seg, off) in enumerate(zip(segments, offsets)):
        memsz = seg.memsz if seg.memsz is not None else len(seg.data)
        phdr = struct.pack(">IIQQQQQQ", seg.p_type, seg.flags, off, seg.vaddr, seg.vaddr,
                           len(seg.data), memsz, seg.align)
        buf[ehsize + i * phentsize:ehsize + (i + 1) * phentsize] = phdr
    return bytes(buf)


def build_elf32_be(*, entry: int, segments: list[FixtureSegment],
                   e_type: int = 2, machine: int = 23) -> bytes:
    ehsize, phentsize = 52, 32
    phnum = len(segments)
    offsets = _layout_segments(segments, ehsize + phentsize * phnum)

    buf = bytearray(ehsize + phentsize * phnum)
    for seg, off in zip(segments, offsets):
        buf.extend(bytes(off - len(buf)))
        buf.extend(seg.data)

    ehdr = struct.pack(">16sHHIIIIIHHHHHH", _ident(1, 0), e_type, machine, 1, entry,
                       ehsize if phnum else 0, 0, 0, ehsize, phentsize, phnum, 40, 0, 0)
    buf[0:ehsize] = ehdr
    for i, (seg, off) in enumerate(zip(segments, offsets)):
        memsz = seg.memsz if seg.memsz is not None else len(seg.data)
        phdr = struct.pack(">IIIIIIII", seg.p_type, off, seg.vaddr, seg.vaddr,
                           len(seg.data), memsz, seg.flags, seg.align)
        buf[ehsize + i * phentsize:ehsize + (i + 1) * phentsize] = phdr
    return bytes(buf)
