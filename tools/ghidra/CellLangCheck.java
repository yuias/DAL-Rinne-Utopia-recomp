// Language check script: disassemble a table of words, run short instruction sequences in the
// emulator and decompile a sample function, all in the language of the imported program.
//
// Arguments: <request json path> <result json path>
//
// Failures of single words, cases or the sample are data in the result, not aborts. Only a bad
// request or an unwritable result ends the script with an exception.
//
// @category PS3

import java.io.File;
import java.io.IOException;
import java.io.Reader;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.emulator.EmulatorHelper;
import ghidra.app.script.GhidraScript;
import ghidra.program.disassemble.Disassembler;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class CellLangCheck extends GhidraScript {
    private static final int DECOMPILE_TIMEOUT_SECONDS = 60;
    private static final int INSTRUCTION_BYTES = 4;
    // Data area the cases work in; zeroed before each case so the cases do not see each other.
    private static final long DATA_START = 0x20000;
    private static final int DATA_SIZE = 0x100;

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 2) {
            printerr("usage: CellLangCheck.java <request json> <result json>");
            throw new IllegalArgumentException("expected 2 arguments");
        }
        JsonObject request;
        try (Reader reader = Files.newBufferedReader(new File(args[0]).toPath(),
                StandardCharsets.UTF_8)) {
            request = JsonParser.parseReader(reader).getAsJsonObject();
        }

        JsonObject result = new JsonObject();
        result.addProperty("language", currentProgram.getLanguageID().getIdAsString());
        result.add("table", runTable(request.getAsJsonArray("table")));
        result.add("cases", runCases(request.getAsJsonArray("cases")));
        result.add("decompile", runDecompile(request.get("decompile").getAsString()));
        writeResult(new File(args[1]), result);
        println("checked " + currentProgram.getLanguageID());
    }

    private JsonArray runTable(JsonArray addrs) throws Exception {
        Disassembler disassembler = Disassembler.getDisassembler(currentProgram, monitor, null);
        JsonArray table = new JsonArray();
        for (JsonElement element : addrs) {
            monitor.checkCancelled();
            Address addr = toAddr(Long.decode(element.getAsString()));
            JsonObject entry = new JsonObject();
            entry.addProperty("addr", element.getAsString());
            entry.add("mnemonic", null);
            entry.add("operands", null);
            try {
                // Restricted to the word and without flow: a branch must not pull in other words.
                disassembler.disassemble(addr,
                        new AddressSet(addr, addr.add(INSTRUCTION_BYTES - 1)), false);
                Instruction insn = currentProgram.getListing().getInstructionAt(addr);
                if (insn != null) {
                    String name = insn.getMnemonicString();
                    String text = insn.toString();
                    if (text.startsWith(name)) {
                        text = text.substring(name.length());
                    }
                    entry.addProperty("mnemonic", name);
                    entry.addProperty("operands", text.trim());
                }
            } catch (Exception e) {
                // Left as null: the word does not decode in this language.
            }
            table.add(entry);
        }
        return table;
    }

    private JsonArray runCases(JsonArray cases) throws Exception {
        JsonArray results = new JsonArray();
        for (JsonElement element : cases) {
            monitor.checkCancelled();
            results.add(runCase(element.getAsJsonObject()));
        }
        return results;
    }

    // EmulatorHelper is deprecated for removal since Ghidra 12.1 but still works; PcodeEmulator is
    // its replacement if it is ever removed.
    @SuppressWarnings("removal")
    private JsonObject runCase(JsonObject request) {
        JsonObject entry = new JsonObject();
        entry.addProperty("name", request.get("name").getAsString());
        entry.add("error", null);
        JsonObject regs = new JsonObject();
        JsonArray mem = new JsonArray();
        entry.add("regs", regs);
        entry.add("mem", mem);

        EmulatorHelper emulator = null;
        try {
            emulator = new EmulatorHelper(currentProgram);
            emulator.writeMemory(toAddr(DATA_START), new byte[DATA_SIZE]);
            for (JsonElement m : request.getAsJsonArray("mem")) {
                JsonObject block = m.getAsJsonObject();
                emulator.writeMemory(toAddr(Long.decode(block.get("addr").getAsString())),
                        fromHex(block.get("hex").getAsString()));
            }
            for (Map.Entry<String, JsonElement> reg : request.getAsJsonObject("regs").entrySet()) {
                emulator.writeRegister(reg.getKey(), parseValue(reg.getValue().getAsString()));
            }
            emulator.writeRegister(emulator.getPCRegister(),
                    Long.decode(request.get("addr").getAsString()));

            int count = request.get("count").getAsInt();
            for (int i = 0; i < count; i++) {
                if (!emulator.step(monitor)) {
                    String reason = emulator.getLastError();
                    entry.addProperty("error", "step " + i + " failed: "
                            + (reason == null ? "no message" : reason));
                    break;
                }
            }

            if (entry.get("error").isJsonNull()) {
                for (JsonElement name : request.getAsJsonArray("read_regs")) {
                    String reg = name.getAsString();
                    BigInteger value = emulator.readRegister(reg);
                    Register register = currentProgram.getLanguage().getRegister(reg);
                    // The helper may return a signed value for a register with the top bit set.
                    BigInteger mask = BigInteger.ONE.shiftLeft(register.getBitLength())
                            .subtract(BigInteger.ONE);
                    regs.addProperty(reg, "0x" + value.and(mask).toString(16));
                }
                for (JsonElement m : request.getAsJsonArray("read_mem")) {
                    JsonObject block = m.getAsJsonObject();
                    byte[] bytes = emulator.readMemory(
                            toAddr(Long.decode(block.get("addr").getAsString())),
                            block.get("len").getAsInt());
                    JsonObject out = new JsonObject();
                    out.addProperty("addr", block.get("addr").getAsString());
                    out.addProperty("hex", toHex(bytes));
                    mem.add(out);
                }
            }
        } catch (Exception e) {
            entry.addProperty("error", e.toString());
        } finally {
            if (emulator != null) {
                emulator.dispose();
            }
        }
        return entry;
    }

    private JsonObject runDecompile(String addrText) {
        JsonObject out = new JsonObject();
        out.addProperty("completed", false);
        out.addProperty("error", "");
        out.addProperty("c", "");
        DecompInterface decompiler = new DecompInterface();
        try {
            Address addr = toAddr(Long.decode(addrText));
            // With flow, so the sample is disassembled up to its blr.
            if (!disassemble(addr)) {
                out.addProperty("error", "cannot disassemble the sample at " + addr);
                return out;
            }
            Function function = createFunction(addr, null);
            if (function == null) {
                out.addProperty("error", "cannot create a function at " + addr);
                return out;
            }
            if (!decompiler.openProgram(currentProgram)) {
                out.addProperty("error", "cannot open the decompiler: " + decompiler.getLastMessage());
                return out;
            }
            DecompileResults results = decompiler.decompileFunction(function,
                    DECOMPILE_TIMEOUT_SECONDS, monitor);
            String message = results.getErrorMessage();
            out.addProperty("completed", results.decompileCompleted());
            out.addProperty("error", message == null ? "" : message);
            if (results.getDecompiledFunction() != null) {
                out.addProperty("c", results.getDecompiledFunction().getC());
            }
        } catch (Exception e) {
            out.addProperty("completed", false);
            out.addProperty("error", e.toString());
        } finally {
            decompiler.dispose();
        }
        return out;
    }

    private static BigInteger parseValue(String text) {
        return text.startsWith("0x") || text.startsWith("0X")
                ? new BigInteger(text.substring(2), 16) : new BigInteger(text);
    }

    private static byte[] fromHex(String text) {
        if (text.length() % 2 != 0) {
            throw new IllegalArgumentException("odd hex length");
        }
        byte[] bytes = new byte[text.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(text.substring(2 * i, 2 * i + 2), 16);
        }
        return bytes;
    }

    private static String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(String.format("%02x", b & 0xff));
        }
        return sb.toString();
    }

    private static void writeResult(File file, JsonObject result) throws IOException {
        File parent = file.getAbsoluteFile().getParentFile();
        if (parent != null) {
            parent.mkdirs();
        }
        Gson gson = new GsonBuilder().serializeNulls().disableHtmlEscaping().create();
        Files.write(file.toPath(), gson.toJson(result).getBytes(StandardCharsets.UTF_8));
    }
}
