// Per-function detail export for tools/re/function_db.py (read-only: changes nothing in the program).
// Arg 0: output directory. Arg 1 (optional): "detail" (default), "fid", or "all".
//
// detail mode writes:
//   detail.tsv        entry, ghidra_cc, params, ret_type, purge, frame_local, frame_params, ret_imm, ecx_first,
//                     edx_first, own_cc, ret_guess, g_reads, g_writes, g_addr, strings, consts, this_offsets,
//                     switches, switch_cases, icalls, vcall_sites, insns
//   globals.tsv       entry, address, reads, writes, addr_taken, sizes     (non-executable memory refs)
//   string_xrefs.tsv  entry, string address, sites
//   consts.tsv        entry, kind (int|float|double), value, count         (<= 24 per function, most frequent)
//   this_offsets.tsv  entry, base (this|arg1), offset, r, w, sizes         ([INFERENCE] linear register tracking)
//   vcalls.tsv        entry, site, slot_off, vptr_off, origin, detail      (indirect calls through a vptr)
//   vft_stores.tsv    entry, site, base, disp, value                       (immediate stores into [reg+disp])
//   switches.tsv      entry, site, cases
// fid mode writes fid_<db>.tsv for vsOlder_x86 and vs2012_x86 (each queried alone):
//   entry, score, matches, status (single|multi_same|multi_diff|below_multi), names, libraries
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.Locale;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.feature.fid.db.FidFile;
import ghidra.feature.fid.db.FidFileManager;
import ghidra.feature.fid.db.FidProgramID;
import ghidra.feature.fid.db.FidQueryService;
import ghidra.feature.fid.service.FidSearchResult;
import ghidra.feature.fid.service.FidService;
import ghidra.feature.fid.service.MatchNameAnalysis;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;
import ghidra.program.model.symbol.StackReference;
import ghidra.program.model.data.StringDataInstance;

public class ExportFunctionDetail extends GhidraScript {
    Listing listing;
    ReferenceManager refMgr;

    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        File dir = new File(a.length > 0 ? a[0] : ".");
        String mode = a.length > 1 ? a[1] : "detail";
        dir.mkdirs();
        listing = currentProgram.getListing();
        refMgr = currentProgram.getReferenceManager();
        if (mode.equals("detail") || mode.equals("all")) {
            detail(dir);
        }
        if (mode.equals("fid") || mode.equals("all")) {
            fid(dir, "vsOlder_x86");
            fid(dir, "vs2012_x86");
        }
    }

    // ---------------------------------------------------------------- FID
    void fid(File dir, String base) throws Exception {
        FidFileManager mgr = FidFileManager.getInstance();
        Map<FidFile, Boolean> saved = new HashMap<>();
        FidFile chosen = null;
        for (FidFile ff : mgr.getFidFiles()) {
            saved.put(ff, ff.isActive());
            if (ff.getBaseName().equals(base)) {
                chosen = ff;
            }
        }
        if (chosen == null) {
            printerr("FID: " + base + " not installed");
            return;
        }
        FidService service = new FidService();
        int n = 0, single = 0;
        try {
            for (FidFile ff : mgr.getFidFiles()) {
                ff.setActive(ff == chosen);
            }
            FidProgramID pid = new FidProgramID(currentProgram, false);
            try (FidQueryService q = service.openFidQueryService(pid, false);
                 PrintWriter w = new PrintWriter(new File(dir, "fid_" + base + ".tsv"), "UTF-8")) {
                w.println("entry\tscore\tmatches\tstatus\tnames\tlibraries");
                List<FidSearchResult> res = service.processProgram(currentProgram, q,
                    service.getDefaultScoreThreshold(), monitor);
                TreeMap<Address, String> rows = new TreeMap<>();
                MatchNameAnalysis na = new MatchNameAnalysis();
                for (FidSearchResult r : res) {
                    if (r.function.isThunk() || r.matches.isEmpty()) {
                        continue;
                    }
                    na.analyzeNames(r.matches, currentProgram, monitor);
                    float score = na.getOverallScore();
                    String status;
                    if (na.numNames() == 1) {
                        status = "single";
                    }
                    else if (na.getMostOptimisticCount() == 1) {
                        status = "multi_same";
                    }
                    else {
                        status = score >= service.getDefaultMultiNameThreshold() ? "multi_diff" : "below_multi";
                    }
                    na.analyzeLibraries(r.matches, 10, monitor);
                    rows.put(r.function.getEntryPoint(), String.format(Locale.ROOT, "%.2f\t%d\t%s\t%s\t%s", score,
                        r.matches.size(), status, join(na.getNameIterator(), 4), join(na.getLibraryIterator(), 3)));
                    if (status.equals("single")) {
                        single++;
                    }
                }
                for (var e : rows.entrySet()) {
                    w.println(e.getKey() + "\t" + e.getValue());
                    n++;
                }
            }
        }
        finally {
            for (var e : saved.entrySet()) {
                e.getKey().setActive(e.getValue());
            }
        }
        println("FID " + base + ": " + n + " functions matched, " + single + " single-name");
    }

    static String join(Iterator<String> it, int cap) {
        TreeSet<String> s = new TreeSet<>();
        while (it.hasNext()) {
            s.add(clean(it.next()));
        }
        StringBuilder b = new StringBuilder();
        int i = 0;
        for (String x : s) {
            if (i++ == cap) {
                b.append("|...");
                break;
            }
            if (b.length() > 0) {
                b.append('|');
            }
            b.append(x);
        }
        return b.toString();
    }

    // ---------------------------------------------------------------- detail
    static boolean isSeg(Register r) {
        String n = r.getName();
        return n.equals("FS") || n.equals("GS") || n.equals("ES") || n.equals("DS") || n.equals("SS") || n.equals("CS");
    }

    static String base(Register r) {
        Register b = r.getBaseRegister();
        return b == null ? r.getName() : b.getName();
    }

    /** [reg + disp] operand: returns {baseRegName, disp} or null. */
    static Object[] baseDisp(Instruction ins, int op) {
        int t = ins.getOperandType(op);
        if (!OperandType.isDynamic(t)) {
            return null;
        }
        Register reg = null;
        long disp = 0;
        int scalars = 0;
        for (Object o : ins.getOpObjects(op)) {
            if (o instanceof Register r) {
                if (isSeg(r)) {
                    return null;
                }
                if (reg != null) {
                    return null;
                }
                reg = r;
            }
            else if (o instanceof Scalar s) {
                scalars++;
                disp = s.getSignedValue();
            }
        }
        if (reg == null || scalars > 1) {
            return null;
        }
        return new Object[] { base(reg), disp };
    }

    /** Memory access size in bytes from the instruction's p-code: LOAD/STORE (register-relative operands) or a
     *  RAM varnode (absolute operands). x86 instructions have one explicit memory operand. */
    static String size(Instruction ins, int op) {
        int sz = 0;
        for (PcodeOp p : ins.getPcode()) {
            if (p.getOpcode() == PcodeOp.LOAD && p.getOutput() != null) {
                sz = Math.max(sz, p.getOutput().getSize());
            }
            else if (p.getOpcode() == PcodeOp.STORE && p.getNumInputs() > 2) {
                sz = Math.max(sz, p.getInput(2).getSize());
            }
            else {
                if (p.getOutput() != null && p.getOutput().isAddress()) {
                    sz = Math.max(sz, p.getOutput().getSize());
                }
                for (int i = 0; i < p.getNumInputs(); i++) {
                    if (p.getInput(i).isAddress()) {
                        sz = Math.max(sz, p.getInput(i).getSize());
                    }
                }
            }
        }
        return sz == 0 ? "?" : Integer.toString(sz);
    }

    /** Absolute memory operand [addr] (no register): its address, else null. */
    static Address absAddr(Instruction ins, int op) {
        if (!ins.getDefaultOperandRepresentation(op).contains("[")) {
            return null;
        }
        for (Object o : ins.getOpObjects(op)) {
            if (o instanceof Register r && !isSeg(r)) {
                return null;
            }
        }
        for (Reference r : ins.getOperandReferences(op)) {
            if (r.getToAddress().isMemoryAddress() && !r.isStackReference()) {
                return r.getToAddress();
            }
        }
        return null;
    }

    static boolean touches(Object[] objs, String reg) {
        for (Object o : objs) {
            if (o instanceof Register r && base(r).equals(reg)) {
                return true;
            }
        }
        return false;
    }

    static boolean selfClear(Instruction ins) {
        String m = ins.getMnemonicString();
        if (!(m.equals("XOR") || m.equals("SUB")) || ins.getNumOperands() != 2) {
            return false;
        }
        Register r0 = ins.getRegister(0), r1 = ins.getRegister(1);
        return r0 != null && r0.equals(r1);
    }

    /** First use of a register along the fall-through path from the entry: read|write|call|none|jmp. */
    String firstUse(Function f, String reg) {
        Instruction ins = listing.getInstructionAt(f.getEntryPoint());
        for (int i = 0; ins != null && i < 200; i++) {
            String m = ins.getMnemonicString();
            boolean push = m.equals("PUSH") && ins.getRegister(0) != null && base(ins.getRegister(0)).equals(reg);
            if (!push) {
                boolean rd = touches(ins.getInputObjects(), reg) && !selfClear(ins);
                boolean wr = touches(ins.getResultObjects(), reg);
                if (rd) {
                    return "read";
                }
                if (wr) {
                    return "write";
                }
            }
            if (ins.getFlowType().isCall()) {
                return "call";
            }
            if (ins.getFlowType().isTerminal()) {
                return "none";
            }
            if (ins.getFlowType().isJump() && !ins.getFlowType().isConditional()) {
                Address[] fl = ins.getFlows();
                if (fl.length != 1 || !f.getBody().contains(fl[0])) {
                    return "jmp";
                }
                ins = listing.getInstructionAt(fl[0]);
                continue;
            }
            Address ft = ins.getFallThrough();
            ins = ft == null ? null : listing.getInstructionAt(ft);
        }
        return "none";
    }

    boolean writesReg(Instruction ins, String reg) {
        if (ins.getFlowType().isCall() && (reg.equals("EAX") || reg.equals("ECX") || reg.equals("EDX"))) {
            return true;
        }
        return touches(ins.getResultObjects(), reg);
    }

    /** Previous instruction on the straight-line path, or null at the entry / a merge point. */
    /** f may be null for code outside any Ghidra function (e.g. undefined dynamic-initialiser stubs). */
    Instruction prevStraight(Function f, Instruction ins) {
        if (f != null && ins.getMinAddress().equals(f.getEntryPoint())) {
            return null;
        }
        for (Reference r : refMgr.getReferencesTo(ins.getMinAddress())) {
            if (r.getReferenceType().isJump()) {
                return null;
            }
        }
        Instruction p = listing.getInstructionBefore(ins.getMinAddress());
        if (p == null || p.getFallThrough() == null || !p.getFallThrough().equals(ins.getMinAddress())
            || (f != null && !f.getBody().contains(p.getMinAddress()))) {
            return null;
        }
        return p;
    }

    /** Direct CALL, or unconditional JMP to another function's entry (tail call). */
    boolean isDirectTransfer(Instruction ins, Function f) {
        if (ins.getFlowType().isComputed() || ins.getFlows().length != 1) {
            return false;
        }
        if (ins.getFlowType().isCall()) {
            return true;
        }
        Address t = ins.getFlows()[0];
        return ins.getFlowType().isJump() && !ins.getFlowType().isConditional() && getFunctionAt(t) != null
            && (f == null || !f.getBody().contains(t));
    }

    /** Global object loaded into ECX by MOV ECX, imm / LEA ECX, [abs] (writable memory), else null. */
    Address objOf(Instruction d) {
        if (d == null || d.getNumOperands() != 2 || d.getRegister(0) == null || !base(d.getRegister(0)).equals("ECX")) {
            return null;
        }
        String m = d.getMnemonicString();
        Address ob = null;
        if (m.equals("LEA")) {
            ob = absAddr(d, 1);
        }
        else if (m.equals("MOV") && !d.getDefaultOperandRepresentation(1).contains("[")
            && !OperandType.isRegister(d.getOperandType(1)) && d.getScalar(1) != null) {
            long v = d.getScalar(1).getUnsignedValue();
            ob = v >= 0x401000 ? toAddr(v) : null;
        }
        MemoryBlock b = ob == null ? null : currentProgram.getMemory().getBlock(ob);
        return b != null && b.isWrite() ? ob : null;
    }

    static final int MERGE = 0;


    /** Walk back from 'from' (exclusive) to the defining instruction of reg; returns def or null; sets why. */
    Instruction findDef(Function f, Instruction from, String reg, String[] why, String storeBase, long[] storeVal) {
        Instruction p = from;
        for (int i = 0; i < 64; i++) {
            Instruction q = prevStraight(f, p);
            if (q == null) {
                why[0] = f != null && p.getMinAddress().equals(f.getEntryPoint()) ? "entry" : "merge";
                return null;
            }
            p = q;
            if (storeBase != null && p.getMnemonicString().equals("MOV") && p.getNumOperands() == 2) {
                Object[] bd = baseDisp(p, 0);
                if (bd != null && bd[0].equals(storeBase) && (Long) bd[1] == storeVal[1]) {
                    Scalar s = p.getScalar(1);
                    if (s != null && OperandType.isScalar(p.getOperandType(1))) {
                        storeVal[0] = s.getUnsignedValue();
                        why[0] = "store";
                        return p;
                    }
                }
            }
            if (writesReg(p, reg)) {
                why[0] = "def";
                return p;
            }
        }
        why[0] = "far";
        return null;
    }

    /** Origin of the object register: kind + detail. */
    String[] origin(Function f, Instruction from, String reg, long vptrOff) {
        String cur = reg;
        Instruction at = from;
        for (int hop = 0; hop < 6; hop++) {
            String[] why = new String[1];
            long[] sv = new long[] { 0, vptrOff };
            Instruction d = findDef(f, at, cur, why, cur, sv);
            if (d == null) {
                if (why[0].equals("entry")) {
                    return new String[] { cur.equals("ECX") ? "this" : "entry_reg", cur };
                }
                if (cur.equals("ESI") || cur.equals("EDI") || cur.equals("EBX") || cur.equals("EBP")) {
                    // Callee-saved register written exactly once in the whole function, by MOV reg, ECX with ECX
                    // still the entry value: it holds 'this' on every path (calls preserve it; POP only at exits).
                    Instruction only = null;
                    int writes = 0;
                    for (Instruction w : listing.getInstructions(f.getBody(), true)) {
                        String wm = w.getMnemonicString();
                        if (!wm.equals("POP") && !wm.equals("PUSH") && touches(w.getResultObjects(), cur)) {
                            writes++;
                            only = w;
                        }
                    }
                    if (writes == 1 && only.getMnemonicString().equals("MOV") && only.getRegister(1) != null
                        && OperandType.isRegister(only.getOperandType(1)) && base(only.getRegister(1)).equals("ECX")) {
                        String[] w2 = new String[1];
                        if (findDef(f, only, "ECX", w2, null, null) == null && w2[0].equals("entry")) {
                            return new String[] { "this", cur + ":single-def" };
                        }
                    }
                }
                return new String[] { why[0], cur };
            }
            if (why[0].equals("store")) {
                return new String[] { "store", String.format("%08x", sv[0]) };
            }
            String m = d.getMnemonicString();
            if (d.getFlowType().isCall()) {
                Address[] fl = d.getFlows();
                if (cur.equals("EAX") && fl.length == 1) {
                    return new String[] { "retof", fl[0].toString() };
                }
                return new String[] { "clobbered", cur };
            }
            if (m.equals("MOV") && d.getNumOperands() == 2) {
                Register src = d.getRegister(1);
                if (src != null && OperandType.isRegister(d.getOperandType(1))) {
                    cur = base(src);
                    at = d;
                    continue;
                }
                for (Reference r : d.getOperandReferences(1)) {
                    if (r instanceof StackReference sr) {
                        return new String[] { sr.getStackOffset() > 0 ? "param" : "local",
                            Integer.toString(sr.getStackOffset()) };
                    }
                    if (r.getToAddress().isMemoryAddress()) {
                        boolean load = d.getDefaultOperandRepresentation(1).contains("[");
                        return new String[] { load ? "globalptr" : "globalobj", r.getToAddress().toString() };
                    }
                }
                Object[] bd = baseDisp(d, 1);
                if (bd != null) {
                    return new String[] { "field", bd[0] + "+" + bd[1] };
                }
                Scalar s = d.getScalar(1);
                if (s != null) {
                    return new String[] { "globalobj", String.format("%08x", s.getUnsignedValue()) };
                }
            }
            if (m.equals("LEA")) {
                for (Reference r : d.getOperandReferences(1)) {
                    if (r instanceof StackReference sr) {
                        return new String[] { "stackobj", Integer.toString(sr.getStackOffset()) };
                    }
                    if (r.getToAddress().isMemoryAddress()) {
                        return new String[] { "globalobj", r.getToAddress().toString() };
                    }
                }
                Object[] bd = baseDisp(d, 1);
                if (bd != null) {
                    return new String[] { "subobj", bd[0] + "+" + bd[1] };
                }
            }
            return new String[] { "other", m };
        }
        return new String[] { "far", cur };
    }

    void detail(File dir) throws Exception {
        Register ecx = currentProgram.getRegister("ECX");
        int nf = 0, nv = 0;
        try (PrintWriter wd = new PrintWriter(new File(dir, "detail.tsv"), "UTF-8");
             PrintWriter wg = new PrintWriter(new File(dir, "globals.tsv"), "UTF-8");
             PrintWriter ws = new PrintWriter(new File(dir, "string_xrefs.tsv"), "UTF-8");
             PrintWriter wc = new PrintWriter(new File(dir, "consts.tsv"), "UTF-8");
             PrintWriter wt = new PrintWriter(new File(dir, "this_offsets.tsv"), "UTF-8");
             PrintWriter wv = new PrintWriter(new File(dir, "vcalls.tsv"), "UTF-8");
             PrintWriter wvs = new PrintWriter(new File(dir, "vft_stores.tsv"), "UTF-8");
             PrintWriter wsw = new PrintWriter(new File(dir, "switches.tsv"), "UTF-8");
             PrintWriter wps = new PrintWriter(new File(dir, "ptr_stores.tsv"), "UTF-8");
             PrintWriter woc = new PrintWriter(new File(dir, "obj_calls.tsv"), "UTF-8")) {
            wd.println("entry\tghidra_cc\tparams\tret_type\tpurge\tframe_local\tframe_params\tret_imm\tecx_first\t"
                + "edx_first\town_cc\tret_guess\tg_reads\tg_writes\tg_addr\tstrings\tconsts\tthis_offsets\tswitches\t"
                + "switch_cases\ticalls\tvcall_sites\tinsns\tret_value");
            wps.println("entry\tsite\tglobal\torigin\tdetail");
            woc.println("entry\tsite\tobject\tcallee");
            wg.println("entry\taddress\treads\twrites\taddr_taken\tsizes");
            ws.println("entry\tstring\tsites");
            wc.println("entry\tkind\tvalue\tcount");
            wt.println("entry\tbase\toffset\tr\tw\tsizes");
            wv.println("entry\tsite\tslot_off\tvptr_off\torigin\tdetail");
            wvs.println("entry\tsite\tbase\tdisp\tvalue");
            wsw.println("entry\tsite\tcases");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Function f = it.next();
                String e = f.getEntryPoint().toString();
                if (f.isThunk()) {
                    wd.printf("%s\t%s\t%d\t%s\t%d\t\t\t\t\t\tthunk\t\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t1\t%n", e,
                        f.getCallingConventionName(), f.getParameterCount(), clean(f.getReturnType().getName()),
                        f.getStackPurgeSize());
                    nf++;
                    continue;
                }
                String ecxFirst = firstUse(f, "ECX");
                String edxFirst = firstUse(f, "EDX");
                TreeSet<Long> retImm = new TreeSet<>();
                TreeSet<String> retVals = new TreeSet<>();
                Map<String, Integer> retKinds = new TreeMap<>();
                Map<Address, int[]> glob = new TreeMap<>();
                Map<Address, TreeSet<String>> globSz = new HashMap<>();
                Map<Address, Integer> strs = new TreeMap<>();
                Map<String, Integer> consts = new HashMap<>();
                Map<String, int[]> offs = new TreeMap<>();
                Map<String, TreeSet<String>> offSz = new HashMap<>();
                boolean thisCall = ecxFirst.equals("read") || f.getCallingConventionName().equals("__thiscall");
                Map<String, String> tags = new HashMap<>();
                if (thisCall) {
                    tags.put("ECX", "this");
                }
                int switches = 0, cases = 0, icalls = 0, vsites = 0, insns = 0;
                Instruction prev = null;
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    insns++;
                    String m = ins.getMnemonicString();
                    // return value guess from the instruction before each RET (skipping epilogue noise)
                    if (m.equals("RET")) {
                        Scalar s = ins.getNumOperands() > 0 ? ins.getScalar(0) : null;
                        retImm.add(s == null ? 0L : s.getUnsignedValue());
                        Instruction p = listing.getInstructionBefore(ins.getMinAddress());
                        for (int k = 0; p != null && k < 6; k++) {
                            String pm = p.getMnemonicString();
                            boolean noise = pm.equals("POP") || pm.equals("LEAVE")
                                || (pm.equals("ADD") && p.getRegister(0) != null && base(p.getRegister(0)).equals("ESP"))
                                || (pm.equals("MOV") && p.getRegister(0) != null && base(p.getRegister(0)).equals("ESP"))
                                || pm.equals("FSTP") && p.getRegister(0) != null;
                            if (!noise) {
                                break;
                            }
                            p = listing.getInstructionBefore(p.getMinAddress());
                        }
                        String k = "void";
                        if (p != null) {
                            String pm = p.getMnemonicString();
                            if (p.getFlowType().isCall()) k = "call";
                            else if (pm.startsWith("FLD") || pm.startsWith("FILD")) k = "float";
                            else if (touches(p.getResultObjects(), "EAX")) k = "int";
                        }
                        retKinds.merge(k, 1, Integer::sum);
                        // constant / global-load return value (singleton getters)
                        String rv = "?";
                        if (p != null && p.getMnemonicString().equals("MOV") && p.getRegister(0) != null
                            && OperandType.isRegister(p.getOperandType(0)) && p.getRegister(0).getName().equals("EAX")) {
                            Address ga2 = absAddr(p, 1);
                            if (ga2 != null) {
                                rv = "load:" + ga2;
                            }
                            else if (OperandType.isScalar(p.getOperandType(1)) && p.getScalar(1) != null) {
                                rv = String.format("imm:%08x", p.getScalar(1).getUnsignedValue());
                            }
                        }
                        retVals.add(rv);
                    }
                    // switch tables and indirect calls
                    if (ins.getFlowType().isComputed()) {
                        if (ins.getFlowType().isJump()) {
                            TreeSet<Address> t = new TreeSet<>();
                            for (Reference r : ins.getReferencesFrom()) {
                                if (r.getReferenceType().isFlow()) {
                                    t.add(r.getToAddress());
                                }
                            }
                            if (t.size() > 1) {
                                switches++;
                                cases += t.size();
                                wsw.printf("%s\t%s\t%d%n", e, ins.getMinAddress(), t.size());
                            }
                        }
                        else if (ins.getFlowType().isCall()) {
                            icalls++;
                            String vreg = null;
                            long slot = -1;
                            Instruction from = ins;
                            Object[] bd = baseDisp(ins, 0);
                            if (bd != null && !bd[0].equals("ESP") && !bd[0].equals("EBP")) {
                                vreg = (String) bd[0];
                                slot = (Long) bd[1];
                            }
                            else if (ins.getRegister(0) != null && OperandType.isRegister(ins.getOperandType(0))) {
                                String[] why = new String[1];
                                Instruction d = findDef(f, ins, base(ins.getRegister(0)), why, null, null);
                                if (d != null && d.getMnemonicString().equals("MOV")) {
                                    Object[] b2 = baseDisp(d, 1);
                                    if (b2 != null && !b2[0].equals("ESP") && !b2[0].equals("EBP")) {
                                        vreg = (String) b2[0];
                                        slot = (Long) b2[1];
                                        from = d;
                                    }
                                }
                            }
                            if (vreg != null && slot >= 0 && slot < 0x1000 && slot % 4 == 0) {
                                String[] why = new String[1];
                                Instruction d = findDef(f, from, vreg, why, null, null);
                                if (d != null && d.getMnemonicString().equals("MOV")) {
                                    Object[] b3 = baseDisp(d, 1);
                                    if (b3 != null && !b3[0].equals("ESP") && !b3[0].equals("EBP")) {
                                        long vptrOff = (Long) b3[1];
                                        String[] o = origin(f, d, (String) b3[0], vptrOff);
                                        wv.printf("%s\t%s\t%d\t%d\t%s\t%s%n", e, ins.getMinAddress(), slot, vptrOff,
                                            o[0], o[1]);
                                        vsites++;
                                        nv++;
                                    }
                                    else if (absAddr(d, 1) != null) {
                                        wv.printf("%s\t%s\t%d\t0\tglobalobj\t%s%n", e, ins.getMinAddress(), slot,
                                            absAddr(d, 1));
                                        vsites++;
                                        nv++;
                                    }
                                }
                            }
                        }
                    }
                    // memory references (globals, strings, float constants)
                    boolean fp = m.startsWith("F") || m.endsWith("SS") || m.endsWith("SD") || m.endsWith("PS")
                        || m.contains("SS2") || m.contains("SD2");
                    for (int op = 0; op < ins.getNumOperands(); op++) {
                        for (Reference r : ins.getOperandReferences(op)) {
                            Address to = r.getToAddress();
                            if (!to.isMemoryAddress() || r.getReferenceType().isFlow()) {
                                continue;
                            }
                            MemoryBlock b = currentProgram.getMemory().getBlock(to);
                            if (b == null || b.isExecute()) {
                                continue;
                            }
                            RefType t = r.getReferenceType();
                            int[] g = glob.computeIfAbsent(to, k -> new int[3]);
                            if (t.isRead()) g[0]++;
                            if (t.isWrite()) g[1]++;
                            if (!t.isRead() && !t.isWrite()) g[2]++;
                            if (t.isRead() || t.isWrite()) {
                                globSz.computeIfAbsent(to, k -> new TreeSet<>()).add(size(ins, op));
                            }
                            Data dd = getDataAt(to);
                            if (dd != null && StringDataInstance.isString(dd)) {
                                strs.merge(to, 1, Integer::sum);
                            }
                            if (fp && t.isRead() && !b.isWrite()) {
                                String sz = size(ins, op);
                                try {
                                    if (sz.equals("8")) {
                                        double v = Double.longBitsToDouble(currentProgram.getMemory().getLong(to));
                                        consts.merge("double\t" + fmt(v), 1, Integer::sum);
                                    }
                                    else if (sz.equals("4") || sz.equals("16")) {
                                        float v = Float.intBitsToFloat(currentProgram.getMemory().getInt(to));
                                        consts.merge("float\t" + fmt(v), 1, Integer::sum);
                                    }
                                }
                                catch (Exception ex) {
                                    // unreadable: skip
                                }
                            }
                        }
                        int ot = ins.getOperandType(op);
                        if (OperandType.isScalar(ot) && !OperandType.isDynamic(ot)
                            && ins.getOperandReferences(op).length == 0) {
                            Scalar s = ins.getScalar(op);
                            Register r0 = ins.getRegister(0);
                            boolean stackAdj = r0 != null && (base(r0).equals("ESP") || base(r0).equals("EBP"));
                            if (s != null && !stackAdj && !m.equals("RET")) {
                                long v = s.getSignedValue();
                                if (v > 256 || v < -256) {
                                    consts.merge("int\t" + String.format("0x%x", s.getUnsignedValue()), 1, Integer::sum);
                                }
                            }
                        }
                        // struct offsets on this / first stack parameter
                        Object[] bd = baseDisp(ins, op);
                        if (bd != null && tags.containsKey(bd[0]) && !m.equals("LEA")) {
                            long d = (Long) bd[1];
                            if (d >= 0 && d < 0x10000) {
                                String key = tags.get(bd[0]) + "\t" + String.format("%06x", d);
                                int[] c = offs.computeIfAbsent(key, k -> new int[2]);
                                RefType t = ins.getOperandRefType(op);
                                if (t.isRead()) c[0]++;
                                if (t.isWrite()) c[1]++;
                                offSz.computeIfAbsent(key, k -> new TreeSet<>()).add(size(ins, op));
                            }
                        }
                    }
                    // stores into absolute globals: vftable immediates (static object init) and object pointers
                    if (m.equals("MOV") && ins.getNumOperands() == 2) {
                        Address gdst = absAddr(ins, 0);
                        if (gdst != null && currentProgram.getMemory().getBlock(gdst).isWrite()) {
                            int ot1 = ins.getOperandType(1);
                            if (!ins.getDefaultOperandRepresentation(1).contains("[") && !OperandType.isRegister(ot1)
                                && ins.getScalar(1) != null) {
                                long v = ins.getScalar(1).getUnsignedValue();
                                MemoryBlock b = v >= 0x401000 ? currentProgram.getMemory().getBlock(toAddr(v)) : null;
                                if (b != null && !b.isWrite() && !b.isExecute()) {
                                    wvs.printf("%s\t%s\tabs:%s\t0\t%08x%n", e, ins.getMinAddress(), gdst, v);
                                }
                                wps.printf("%s\t%s\t%s\timm\t%08x%n", e, ins.getMinAddress(), gdst, v);
                            }
                            else if (OperandType.isRegister(ot1) && ins.getRegister(1) != null) {
                                String[] o = origin(f, ins, base(ins.getRegister(1)), -1);
                                wps.printf("%s\t%s\t%s\t%s\t%s%n", e, ins.getMinAddress(), gdst, o[0], o[1]);
                            }
                        }
                    }
                    // direct calls on a global object: MOV ECX, imm (or LEA ECX, [abs]) then CALL / tail JMP target
                    if (isDirectTransfer(ins, f)) {
                        Address oc = objOf(findDef(f, ins, "ECX", new String[1], null, null));
                        if (oc != null) {
                            woc.printf("%s\t%s\t%s\t%s%n", e, ins.getMinAddress(), oc, ins.getFlows()[0]);
                        }
                    }
                    // immediate stores into [reg+disp] (vftable candidates are filtered by function_db.py)
                    if (m.equals("MOV") && ins.getNumOperands() == 2 && OperandType.isScalar(ins.getOperandType(1))) {
                        Object[] bd = baseDisp(ins, 0);
                        Scalar s = ins.getScalar(1);
                        if (bd != null && s != null) {
                            long v = s.getUnsignedValue();
                            MemoryBlock b = currentProgram.getMemory().getBlock(toAddr(v));
                            if (v >= 0x401000 && b != null && !b.isWrite() && !b.isExecute()) {
                                String bk = tags.getOrDefault(bd[0], (String) bd[0]);
                                for (Reference r : ins.getOperandReferences(0)) {
                                    if (r instanceof StackReference sr) {
                                        bk = "stack" + sr.getStackOffset();
                                    }
                                }
                                wvs.printf("%s\t%s\t%s\t%d\t%08x%n", e, ins.getMinAddress(), bk, (Long) bd[1], v);
                            }
                        }
                    }
                    // register tag propagation for this/arg1 tracking ([INFERENCE] address-order, kill on write)
                    String newTag = null;
                    Register dst = ins.getNumOperands() == 2 && OperandType.isRegister(ins.getOperandType(0))
                        ? ins.getRegister(0) : null;
                    if (m.equals("MOV") && dst != null) {
                        Register src = OperandType.isRegister(ins.getOperandType(1)) ? ins.getRegister(1) : null;
                        if (src != null) {
                            newTag = tags.get(base(src));
                        }
                        else if (!thisCall) {
                            for (Reference r : ins.getOperandReferences(1)) {
                                if (r instanceof StackReference sr && sr.getStackOffset() == 4) {
                                    newTag = "arg1";
                                }
                            }
                        }
                    }
                    if (ins.getFlowType().isCall()) {
                        tags.remove("EAX");
                        tags.remove("ECX");
                        tags.remove("EDX");
                    }
                    for (Object o : ins.getResultObjects()) {
                        if (o instanceof Register r) {
                            tags.remove(base(r));
                        }
                    }
                    if (newTag != null && dst != null) {
                        tags.put(base(dst), newTag);
                    }
                    prev = ins;
                }
                for (var g : glob.entrySet()) {
                    int[] c = g.getValue();
                    TreeSet<String> sz = globSz.get(g.getKey());
                    wg.printf("%s\t%s\t%d\t%d\t%d\t%s%n", e, g.getKey(), c[0], c[1], c[2],
                        sz == null ? "" : String.join(";", sz));
                }
                for (var s : strs.entrySet()) {
                    ws.printf("%s\t%s\t%d%n", e, s.getKey(), s.getValue());
                }
                List<Map.Entry<String, Integer>> cl = new ArrayList<>(consts.entrySet());
                cl.sort((x, y) -> x.getValue().equals(y.getValue()) ? x.getKey().compareTo(y.getKey())
                    : y.getValue() - x.getValue());
                for (int i = 0; i < cl.size() && i < 24; i++) {
                    wc.printf("%s\t%s\t%d%n", e, cl.get(i).getKey(), cl.get(i).getValue());
                }
                for (var o : offs.entrySet()) {
                    int[] c = o.getValue();
                    wt.printf("%s\t%s\t%d\t%d\t%s%n", e, o.getKey(), c[0], c[1], String.join(";", offSz.get(o.getKey())));
                }
                int gr = 0, gw = 0, ga = 0;
                for (int[] c : glob.values()) {
                    if (c[0] > 0) gr++;
                    if (c[1] > 0) gw++;
                    if (c[2] > 0) ga++;
                }
                long purgeOwn = retImm.isEmpty() ? -1 : retImm.last();
                String ownCc;
                boolean ecxR = ecxFirst.equals("read"), edxR = edxFirst.equals("read");
                if (ecxR && edxR) ownCc = "fastcall";
                else if (ecxR) ownCc = "thiscall";
                else if (purgeOwn > 0) ownCc = "stdcall";
                else if (purgeOwn == 0) ownCc = "cdecl";
                else ownCc = "noret";
                String rg = retKinds.isEmpty() ? "" : retKinds.entrySet().stream()
                    .max((x, y) -> x.getValue().equals(y.getValue()) ? y.getKey().compareTo(x.getKey())
                        : x.getValue() - y.getValue()).get().getKey();
                StringBuilder ri = new StringBuilder();
                for (long v : retImm) {
                    if (ri.length() > 0) ri.append(';');
                    ri.append(v);
                }
                String retValue = retVals.size() == 1 && !retVals.first().equals("?") ? retVals.first() : "";
                wd.printf("%s\t%s\t%d\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s%n",
                    e, f.getCallingConventionName(), f.getParameterCount(), clean(f.getReturnType().getName()),
                    f.getStackPurgeSize(), f.getStackFrame().getLocalSize(), f.getStackFrame().getParameterSize(),
                    ri, ecxFirst, edxFirst, ownCc, rg, gr, gw, ga, strs.size(), Math.min(cl.size(), 24), offs.size(),
                    switches, cases, icalls, vsites, insns, retValue);
                nf++;
            }
            // Code outside any Ghidra function (static-object initialiser / atexit stubs reached through the CRT
            // _initterm table): global-object calls and absolute vftable stores only.
            int orphan = 0;
            for (Instruction ins : listing.getInstructions(true)) {
                monitor.checkCancelled();
                if (getFunctionContaining(ins.getMinAddress()) != null) {
                    continue;
                }
                orphan++;
                if (isDirectTransfer(ins, null)) {
                    Address oc = objOf(findDef(null, ins, "ECX", new String[1], null, null));
                    if (oc != null) {
                        woc.printf("-\t%s\t%s\t%s%n", ins.getMinAddress(), oc, ins.getFlows()[0]);
                    }
                }
                if (ins.getMnemonicString().equals("MOV") && ins.getNumOperands() == 2 && absAddr(ins, 0) != null
                    && !ins.getDefaultOperandRepresentation(1).contains("[") && ins.getScalar(1) != null
                    && !OperandType.isRegister(ins.getOperandType(1))) {
                    long v = ins.getScalar(1).getUnsignedValue();
                    MemoryBlock b = v >= 0x401000 ? currentProgram.getMemory().getBlock(toAddr(v)) : null;
                    if (b != null && !b.isWrite() && !b.isExecute()) {
                        wvs.printf("-\t%s\tabs:%s\t0\t%08x%n", ins.getMinAddress(), absAddr(ins, 0), v);
                    }
                }
            }
            println("ExportFunctionDetail: " + orphan + " instructions outside functions scanned");
        }
        println("ExportFunctionDetail: " + nf + " functions, " + nv + " vptr call sites -> " + dir);
    }

    static String fmt(double v) {
        if (Double.isNaN(v) || Double.isInfinite(v)) {
            return Double.toString(v);
        }
        return String.format(Locale.ROOT, "%.7g", v);
    }

    static String clean(String s) {
        return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }
}
