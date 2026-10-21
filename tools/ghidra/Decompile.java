// Labels every il2cpp method from polyhost's symbol dump, then decompiles the named
// methods. Run headless:
//   analyzeHeadless <proj> <name> -process libil2cpp.so -noanalysis \
//     -postScript Decompile.java <symbols.tsv> <out.c> <Class$$Method/argc>...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;

import java.io.BufferedReader;
import java.io.FileReader;
import java.io.PrintWriter;
import java.util.HashMap;
import java.util.Map;

public class Decompile extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address base = currentProgram.getImageBase();
        SymbolTable symbols = currentProgram.getSymbolTable();
        Map<String, Address> byName = new HashMap<>();
        boolean labelled = symbols.getLabelOrFunctionSymbols("GameState$$.ctor/0", null).size() > 0;
        try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
            String line;
            while ((line = in.readLine()) != null) {
                String[] f = line.split("\t");
                Address a = base.add(Long.parseLong(f[0], 16));
                String name = f[2].replaceAll("[^A-Za-z0-9_$./<>`,]", "_");
                byName.putIfAbsent(f[2], a);
                if (!labelled) symbols.createLabel(a, name, SourceType.IMPORTED);
            }
        }
        // Give the decompiler real function boundaries for the game's own assemblies, once.
        if (getFunctionAt(byName.get("GameState$$.ctor/0")) == null) {
            try (BufferedReader in = new BufferedReader(new FileReader(args[0]))) {
                String line;
                while ((line = in.readLine()) != null) {
                    String[] f = line.split("\t");
                    if (!f[1].equals("GameLogicAssembly.dll") && !f[1].equals("PolytopiaBackendBase.dll")) continue;
                    Address a = base.add(Long.parseLong(f[0], 16));
                    if (getFunctionAt(a) == null) {
                        disassemble(a);
                        Function fn = createFunction(a, f[2].replaceAll("[^A-Za-z0-9_$./<>`,]", "_"));
                    }
                }
            }
        }
        DecompInterface ifc = new DecompInterface();
        DecompileOptions options = new DecompileOptions();
        options.setMaxPayloadMBytes(1024);
        ifc.setOptions(options);
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[1])) {
            for (int i = 2; i < args.length; i++) {
                Address a = args[i].startsWith("0x") ? base.add(Long.parseLong(args[i].substring(2), 16)) : byName.get(args[i]);
                if (a == null) { out.println("// not found: " + args[i]); continue; }
                Function fn = getFunctionAt(a);
                if (fn == null) {
                    disassemble(a);
                    fn = createFunction(a, null);
                }
                if (fn == null) { out.println("// could not create function: " + args[i]); continue; }
                DecompileResults r = ifc.decompileFunction(fn, 600, monitor);
                out.println("// ===== " + args[i] + " @ " + a);
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
