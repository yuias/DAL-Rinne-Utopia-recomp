// Pre-analysis script: set the TOC register and seed known function entries.
//
// Arguments: <toc hex | none> [<function address hex> ...]
//
// The ELF sections are unnamed, so Ghidra cannot locate the TOC base (r2 in the PPC64 ELFv1
// ABI) by itself. Without it, TOC-relative addressing is not resolved during analysis.
//
// @category PS3

import java.math.BigInteger;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.ProgramContext;
import ghidra.program.model.mem.MemoryBlock;

public class Ps3Prepare extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: Ps3Prepare.java <toc hex | none> [<function address hex> ...]");
            throw new IllegalArgumentException("missing arguments");
        }

        if (!args[0].equalsIgnoreCase("none")) {
            setToc(Long.decode(args[0]));
        } else {
            println("TOC not given; r2 left unset");
        }

        int created = 0;
        for (int i = 1; i < args.length; i++) {
            if (seedFunction(Long.decode(args[i]))) {
                created++;
            }
        }
        println("seeded " + created + " of " + (args.length - 1) + " functions");
    }

    private void setToc(long toc) throws Exception {
        Register r2 = currentProgram.getLanguage().getRegister("r2");
        if (r2 == null) {
            printerr("warning: language has no register r2; TOC not set");
            return;
        }
        ProgramContext context = currentProgram.getProgramContext();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isExecute()) {
                continue;
            }
            context.setValue(r2, block.getStart(), block.getEnd(), BigInteger.valueOf(toc));
            println(String.format("r2 = 0x%08x over %s [%s - %s]", toc, block.getName(),
                    block.getStart(), block.getEnd()));
        }
    }

    private boolean seedFunction(long offset) {
        Address addr = toAddr(offset);
        try {
            if (!disassemble(addr)) {
                printerr(String.format("warning: cannot disassemble seed 0x%08x", offset));
                return false;
            }
            if (createFunction(addr, null) == null) {
                printerr(String.format("warning: cannot create function at 0x%08x", offset));
                return false;
            }
            println(String.format("function at 0x%08x", offset));
            return true;
        } catch (Exception e) {
            printerr(String.format("warning: seed 0x%08x failed: %s", offset, e));
            return false;
        }
    }
}
