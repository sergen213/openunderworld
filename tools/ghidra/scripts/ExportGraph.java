// Export the call graph and per-function reference facts as TSV, for tools/re/function_db.py.
// Arg 0: output directory. Writes:
//   functions_full.tsv  entry, name, namespace, source, size, min, max, thunk, thunk_target, fid, ranges
//   edges.tsv           caller, callee, kind (call|jump|thunk), sites
//   func_refs.tsv       entry, data_refs, data_reads, data_writes, string_refs, vftable_refs, ptr_refs_to
//   data_symbols.tsv    address, name, source, block (non-default symbols outside executable blocks)
//   blocks.tsv          name, start, end, rwx
// Read-only: changes nothing in the program.
// @category OpenUnderworld
import java.io.File;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.Bookmark;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportGraph extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File dir = new File(getScriptArgs().length > 0 ? getScriptArgs()[0] : ".");
        dir.mkdirs();
        Set<Address> fidAddrs = new HashSet<>();
        var bit = currentProgram.getBookmarkManager().getBookmarksIterator();
        while (bit.hasNext()) {
            Bookmark b = bit.next();
            if (b.getCategory() != null && b.getCategory().contains("Function ID")) {
                fidAddrs.add(b.getAddress());
            }
        }
        try (PrintWriter wb = new PrintWriter(new File(dir, "blocks.tsv"), "UTF-8")) {
            wb.println("name\tstart\tend\trwx");
            for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
                wb.printf("%s\t%s\t%s\t%s%s%s%n", b.getName(), b.getStart(), b.getEnd(),
                    b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-");
            }
        }
        int nf = 0, ne = 0;
        try (PrintWriter wf = new PrintWriter(new File(dir, "functions_full.tsv"), "UTF-8");
             PrintWriter we = new PrintWriter(new File(dir, "edges.tsv"), "UTF-8");
             PrintWriter wr = new PrintWriter(new File(dir, "func_refs.tsv"), "UTF-8")) {
            wf.println("entry\tname\tnamespace\tsource\tsize\tmin\tmax\tthunk\tthunk_target\tfid\tranges");
            we.println("caller\tcallee\tkind\tsites");
            wr.println("entry\tdata_refs\tdata_reads\tdata_writes\tstring_refs\tvftable_refs\tptr_refs_to");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                monitor.checkCancelled();
                Function f = it.next();
                StringBuilder ranges = new StringBuilder();
                for (AddressRange r : f.getBody()) {
                    if (ranges.length() > 0) {
                        ranges.append(';');
                    }
                    ranges.append(r.getMinAddress()).append('-').append(r.getMaxAddress());
                }
                Function tt = f.isThunk() ? f.getThunkedFunction(false) : null;
                Symbol s = f.getSymbol();
                wf.printf("%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s%n", f.getEntryPoint(), clean(f.getName()),
                    clean(f.getParentNamespace().getName(true)), s.getSource(), f.getBody().getNumAddresses(),
                    f.getBody().getMinAddress(), f.getBody().getMaxAddress(), f.isThunk() ? "1" : "0",
                    tt == null ? "" : tt.getEntryPoint().toString(), fidAddrs.contains(f.getEntryPoint()) ? "1" : "0",
                    ranges);
                nf++;
                Map<Address, int[]> callees = new TreeMap<>();
                Set<Address> data = new HashSet<>();
                int reads = 0, writes = 0, strs = 0, vft = 0;
                for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                    for (Reference r : ins.getReferencesFrom()) {
                        Address to = r.getToAddress();
                        RefType t = r.getReferenceType();
                        if (t.isFlow()) {
                            Function g = getFunctionAt(to);
                            if (g == null || g == f) {
                                continue;
                            }
                            int kind = t.isCall() ? 0 : 1;
                            callees.computeIfAbsent(g.getEntryPoint(), k -> new int[2])[kind]++;
                        }
                        else if (to.isMemoryAddress()) {
                            Function g = getFunctionAt(to);
                            if (g != null) {
                                continue; // function pointer taken; counted via ptr_refs_to of callee
                            }
                            MemoryBlock b = currentProgram.getMemory().getBlock(to);
                            if (b == null || b.isExecute()) {
                                continue;
                            }
                            data.add(to);
                            if (t.isRead()) {
                                reads++;
                            }
                            if (t.isWrite()) {
                                writes++;
                            }
                            Data d = getDataAt(to);
                            if (d != null && StringDataInstance.isString(d)) {
                                strs++;
                            }
                            Symbol ps = getSymbolAt(to);
                            if (ps != null && ps.getName().contains("vftable")) {
                                vft++;
                            }
                        }
                    }
                }
                if (f.isThunk() && tt != null) {
                    we.printf("%s\t%s\tthunk\t1%n", f.getEntryPoint(), tt.getEntryPoint());
                    ne++;
                }
                for (var e : callees.entrySet()) {
                    int[] c = e.getValue();
                    if (c[0] > 0) {
                        we.printf("%s\t%s\tcall\t%d%n", f.getEntryPoint(), e.getKey(), c[0]);
                        ne++;
                    }
                    if (c[1] > 0) {
                        we.printf("%s\t%s\tjump\t%d%n", f.getEntryPoint(), e.getKey(), c[1]);
                        ne++;
                    }
                }
                int ptrTo = 0;
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    if (!r.getReferenceType().isFlow()) {
                        ptrTo++;
                    }
                }
                wr.printf("%s\t%d\t%d\t%d\t%d\t%d\t%d%n", f.getEntryPoint(), data.size(), reads, writes, strs, vft, ptrTo);
            }
        }
        int nd = 0;
        try (PrintWriter wd = new PrintWriter(new File(dir, "data_symbols.tsv"), "UTF-8")) {
            wd.println("address\tname\tsource\tblock");
            SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
            while (si.hasNext()) {
                Symbol s = si.next();
                Address a = s.getAddress();
                if (!a.isMemoryAddress() || s.getSource() == SourceType.DEFAULT) {
                    continue;
                }
                MemoryBlock b = currentProgram.getMemory().getBlock(a);
                if (b == null || b.isExecute()) {
                    continue;
                }
                wd.printf("%s\t%s\t%s\t%s%n", a, clean(s.getName(true)), s.getSource(), b.getName());
                nd++;
            }
        }
        println("ExportGraph: " + nf + " functions, " + ne + " edges, " + nd + " data symbols, " + fidAddrs.size()
            + " FID bookmarks -> " + dir);
    }

    static String clean(String s) {
        return s == null ? "" : s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }
}
