// Ghidra headless script: m2emulator 1.1a's default control layouts, read off
// EMULATOR.EXE's disassembly (Pinboard #1347; docs/m2emulator-controls.md).
// Run it through m2emu_controls.py, which checks the EXE and formats the result.
//
// Each layout is a global CInputPorts subclass (RTTI _<x>_Inputs) filled by a
// constructor that calls the list insert, 0x4519d0, once an entry, with the
// entry built on the stack just before: word (port | mask << 8), dword default
// key, byte type, char *name, at [ESI], ESI = LEA [ESP + n]. A store may write
// part of a slot (a byte into the word keeps the last entry's mask), so the
// stack is modelled byte by byte. A game's descriptor holds its name at +0x04,
// its title at +0x10 and its layout object at +0x14.
//@category m2-hle2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
import java.util.regex.*;

public class M2emuControls extends GhidraScript {
    static final long LIST_INSERT = 0x4519d0L;
    static final Pattern STACK = Pattern.compile("MOV (byte|word|dword) ptr \\[ESP \\+ (0x[0-9a-f]+)\\],(\\S+)");
    static final Pattern ABS = Pattern.compile("MOV (?:dword ptr )?\\[(0x[0-9a-f]+)\\],(\\S+)");
    static final Pattern LEA = Pattern.compile("LEA (E[A-Z]+),\\[ESP \\+ (0x[0-9a-f]+)\\]");
    static final Pattern MOVI = Pattern.compile("MOV (E[A-Z]+),(\\S+)");
    static final Pattern XOR = Pattern.compile("XOR (E[A-Z]+),(E[A-Z]+)");
    static final Pattern ORM1 = Pattern.compile("OR (E[A-Z]+),0xffffffff");
    static final Map<String, String> SUB = Map.ofEntries(
        Map.entry("AL", "EAX"), Map.entry("AX", "EAX"), Map.entry("BL", "EBX"), Map.entry("BX", "EBX"),
        Map.entry("CL", "ECX"), Map.entry("CX", "ECX"), Map.entry("DL", "EDX"), Map.entry("DX", "EDX"),
        Map.entry("SI", "ESI"), Map.entry("DI", "EDI"), Map.entry("BP", "EBP"));

    /* A source operand's value: an immediate, or a register holding one; null if unknown. */
    static Long val(String s, Map<String, Long> regs) {
        s = SUB.getOrDefault(s, s);
        if (regs.containsKey(s)) return regs.get(s);
        try {
            if (s.startsWith("0x")) return Long.parseLong(s.substring(2), 16);
            if (s.startsWith("-0x")) return (-Long.parseLong(s.substring(3), 16)) & 0xffffffffL;
            return Long.parseLong(s) & 0xffffffffL;
        } catch (NumberFormatException e) { return null; }
    }

    static long rd(Map<Long, Long> m, long off, int size) {
        long v = 0;
        for (int b = 0; b < size; b++) v |= m.getOrDefault(off + b, 0L) << (8 * b);
        return v;
    }

    /* Register tracking common to both walks; true if the instruction was a register write it understood. */
    static boolean trackRegs(String t, Map<String, Long> regs) {
        Matcher m;
        if ((m = XOR.matcher(t)).matches() && m.group(1).equals(m.group(2))) { regs.put(m.group(1), 0L); return true; }
        if ((m = ORM1.matcher(t)).matches()) { regs.put(m.group(1), 0xffffffffL); return true; }
        if ((m = MOVI.matcher(t)).matches()) {
            Long v = val(m.group(2), regs);
            if (v == null) regs.remove(m.group(1)); else regs.put(m.group(1), v);
            return true;
        }
        if (t.startsWith("CALL ")) { regs.remove("EAX"); regs.remove("ECX"); regs.remove("EDX"); return true; }
        String[] p = t.split("[ ,]");
        if (p.length > 1) regs.remove(SUB.getOrDefault(p[1], p[1]));
        return false;
    }

    String cstr(long va) throws Exception {
        if (va == 0 || !currentProgram.getMemory().contains(toAddr(va))) return null;
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 160; i++) {
            byte c = getByte(toAddr(va + i));
            if (c == 0) break;
            b.append((char) (c & 0xff));
        }
        return b.toString();
    }

    static String js(String s) {
        if (s == null) return "null";
        return "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
    }

    static String text(Instruction i) { return i.toString().replace(", ", ","); }

    Address insert;
    PrintWriter w;
    Map<Long, String> classOf = new LinkedHashMap<>();
    int fi = 0, gi = 0;

    static void storeStack(Matcher m, Map<String, Long> regs, Map<Long, Long> stack) {
        Long v = val(m.group(3), regs);
        long off = Long.decode(m.group(2));
        int size = m.group(1).equals("byte") ? 1 : m.group(1).equals("word") ? 2 : 4;
        for (int b = 0; b < size; b++) {
            if (v == null) stack.remove(off + b); else stack.put(off + b, (v >> (8 * b)) & 0xff);
        }
    }

    boolean callsInsert(String t, Instruction i) {
        return t.startsWith("CALL ") && i.getFlows().length > 0 && i.getFlows()[0].equals(insert);
    }

    /* One list entry, built on the stack at [ESI] before the insert call. */
    String entryRow(Map<Long, Long> stack, Map<String, Long> lea) throws Exception {
        long base = lea.getOrDefault("ESI", -1L);
        long word = rd(stack, base, 2), key = rd(stack, base + 4, 4);
        long type = rd(stack, base + 8, 1), name = rd(stack, base + 12, 4);
        return String.format("{\"port\": %d, \"mask\": %d, \"key\": %d, \"type\": %d, \"name\": %s}",
            word & 0xff, word >> 8, key, type, js(cstr(name)));
    }

    /* A constructor's entries, in call order; its stores to globals go in abs. */
    List<String> layoutRows(Function f, Map<Long, Long> abs) throws Exception {
        Map<String, Long> regs = new HashMap<>();
        Map<Long, Long> stack = new HashMap<>();
        Map<String, Long> lea = new HashMap<>();
        List<String> rows = new ArrayList<>();
        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            String t = text(i);
            Matcher m;
            if ((m = STACK.matcher(t)).matches()) {
                storeStack(m, regs, stack);
            } else if ((m = ABS.matcher(t)).matches()) {
                Long v = val(m.group(2), regs);
                if (v != null) abs.put(Long.decode(m.group(1)), v);
            } else if ((m = LEA.matcher(t)).matches()) {
                lea.put(m.group(1), Long.decode(m.group(2)));
            } else if (callsInsert(t, i)) {
                rows.add(entryRow(stack, lea));
                regs.remove("EAX"); regs.remove("ECX"); regs.remove("EDX");
            } else {
                if (!trackRegs(t, regs)) lea.remove(t.split("[ ,]").length > 1 ? t.split("[ ,]")[1] : "");
            }
        }
        return rows;
    }

    boolean isVftable(Symbol s) { return s != null && s.getName().equals("vftable"); }

    void layout(Function f) throws Exception {
        Map<Long, Long> abs = new LinkedHashMap<>();
        List<String> rows = layoutRows(f, abs);
        /* The object: the first global given a vftable here; its class, the last vftable's RTTI namespace. */
        long obj = -1; String cls = null;
        for (Map.Entry<Long, Long> e : abs.entrySet()) {
            Symbol s = getSymbolAt(toAddr(e.getValue()));
            if (!isVftable(s)) continue;
            if (obj < 0) obj = e.getKey();
            if (e.getKey() == obj) cls = s.getParentNamespace().getName();
        }
        classOf.put(obj, cls);
        w.printf("%s\"0x%x\": {\"ctor\": \"0x%x\", \"class\": %s, \"entries\": [%n  %s]}%n", fi++ > 0 ? "," : "",
            obj, f.getEntryPoint().getOffset(), js(cls), String.join(",\n  ", rows));
    }

    /* Descriptors given a layout object at + 0x14 in f; every store to a global goes in abs. */
    List<Long> gameStores(Function f, Map<Long, Long> abs) {
        Map<String, Long> regs = new HashMap<>();
        List<Long> descs = new ArrayList<>();
        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(), true)) {
            String t = text(i);
            Matcher m = ABS.matcher(t);
            if (!m.matches()) {
                trackRegs(t, regs);
                continue;
            }
            Long v = val(m.group(2), regs);
            long a = Long.decode(m.group(1));
            if (v == null) { abs.remove(a); continue; }
            abs.put(a, v);
            if (classOf.containsKey(v)) descs.add(a - 0x14);
        }
        return descs;
    }

    void games(Function f) throws Exception {
        Map<Long, Long> abs = new HashMap<>();
        for (long d : gameStores(f, abs)) {
            String name = cstr(abs.getOrDefault(d + 4, 0L));
            if (name == null) continue;
            w.printf("%s{\"name\": %s, \"title\": %s, \"descriptor\": \"0x%x\", \"list\": \"0x%x\", \"class\": %s}%n",
                gi++ > 0 ? "," : "", js(name), js(cstr(abs.getOrDefault(d + 0x10, 0L))), d,
                abs.get(d + 0x14), js(classOf.get(abs.get(d + 0x14))));
        }
    }

    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        insert = toAddr(LIST_INSERT);
        TreeSet<Function> ctors = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        for (Reference r : getReferencesTo(insert)) {
            Function f = getFunctionContaining(r.getFromAddress());
            if (f != null && r.getReferenceType().isCall()) ctors.add(f);
        }
        w = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out), "UTF-8"));
        w.println("{\"lists\": {");
        for (Function f : ctors) layout(f);
        w.println("}, \"games\": [");
        /* Games: a store of a layout object at descriptor + 0x14. */
        TreeSet<Function> regFns = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
        for (long obj : classOf.keySet())
            for (Reference r : getReferencesTo(toAddr(obj))) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null && !ctors.contains(f)) regFns.add(f);
            }
        for (Function f : regFns) games(f);
        w.println("]}");
        w.close();
        println("M2emuControls: " + ctors.size() + " layouts, " + gi + " games -> " + out);
    }
}
