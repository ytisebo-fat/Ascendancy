// Ghidra headless post-script: export the function list and the decompiled C
// of every function, for reverse-engineering the original game code.
//
// analyzeHeadless <proj_dir> <proj_name> -import ascend.elf \
//     -scriptPath re/ghidra_scripts -postScript ExportAll.java <outdir>
//
// Output (in <outdir>):
//   functions.tsv   address, name, size, callers, callees, convention
//   decomp.c        decompiled C for every function, in address order
//   strings.tsv     defined strings with the functions that reference them
//@category Ascendancy
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.data.StringDataInstance;

import java.io.File;
import java.io.PrintWriter;
import java.util.*;

public class ExportAll extends GhidraScript {
  // Watcom asserts pass a source file name ("..\\cosmos.cpp") and a line
  // number to one handler. The handler is the function that most of these
  // strings are passed to; every function using such a string gets the
  // module name as prefix: FUN_00012345 -> cosmos_00012345.
  private void labelFromAsserts(Listing listing, FunctionManager fm) throws Exception {
    Map<Function, String> module = new HashMap<>();
    for (Data d : listing.getDefinedData(true)) {
      if (!d.hasStringValue()) continue;
      String s = StringDataInstance.getStringDataInstance(d).getStringValue();
      if (s == null || !s.startsWith("..\\") || !s.endsWith(".cpp")) continue;
      String mod = s.substring(3, s.length() - 4).toLowerCase();
      for (Reference r : getReferencesTo(d.getAddress())) {
        Function f = fm.getFunctionContaining(r.getFromAddress());
        if (f != null) module.putIfAbsent(f, mod);
      }
    }
    int renamed = 0;
    for (Map.Entry<Function, String> e : module.entrySet()) {
      Function f = e.getKey();
      if (!f.getName().startsWith("FUN_")) continue;
      f.setName(e.getValue() + "_" + f.getEntryPoint().toString(), ghidra.program.model.symbol.SourceType.ANALYSIS);
      renamed++;
    }
    println("labelled " + renamed + " functions with their source module");
  }

  @Override
  public void run() throws Exception {
    String[] args = getScriptArgs();
    File outDir = new File(args.length > 0 ? args[0] : ".");
    outDir.mkdirs();

    Listing listing = currentProgram.getListing();
    FunctionManager fm = currentProgram.getFunctionManager();
    labelFromAsserts(listing, fm);

    try (PrintWriter tsv = new PrintWriter(new File(outDir, "functions.tsv"))) {
      tsv.println("address\tname\tsize\tcallers\tcallees\tconvention\tstack_purge");
      for (Function f : fm.getFunctions(true)) {
        tsv.printf("%s\t%s\t%d\t%d\t%d\t%s\t%d%n", f.getEntryPoint(), f.getName(),
            f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size(),
            f.getCalledFunctions(monitor).size(), f.getCallingConventionName(),
            f.getStackPurgeSize());
      }
    }

    try (PrintWriter st = new PrintWriter(new File(outDir, "strings.tsv"))) {
      for (Data d : listing.getDefinedData(true)) {
        if (!d.hasStringValue()) continue;
        String s = StringDataInstance.getStringDataInstance(d).getStringValue();
        if (s == null) continue;
        Set<String> users = new TreeSet<>();
        for (Reference r : getReferencesTo(d.getAddress())) {
          Function f = fm.getFunctionContaining(r.getFromAddress());
          users.add(f == null ? r.getFromAddress().toString() : f.getName());
        }
        st.printf("%s\t%s\t%s%n", d.getAddress(), s.replace("\t", "\\t").replace("\n", "\\n")
            .replace("\r", "\\r"), String.join(",", users));
      }
    }

    DecompInterface di = new DecompInterface();
    DecompileOptions opts = new DecompileOptions();
    di.setOptions(opts);
    di.openProgram(currentProgram);
    int n = 0, failed = 0;
    try (PrintWriter c = new PrintWriter(new File(outDir, "decomp.c"))) {
      for (Function f : fm.getFunctions(true)) {
        if (monitor.isCancelled()) break;
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        c.printf("// ===== %s @ %s =====%n", f.getName(), f.getEntryPoint());
        if (r != null && r.decompileCompleted()) {
          c.println(r.getDecompiledFunction().getC());
        } else {
          c.println("// decompilation failed: " + (r == null ? "null" : r.getErrorMessage()));
          failed++;
        }
        if (++n % 250 == 0) println("decompiled " + n);
      }
    }
    di.dispose();
    println("exported " + n + " functions (" + failed + " failed) to " + outDir);
  }
}
