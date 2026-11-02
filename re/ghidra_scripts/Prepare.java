// Ghidra headless pre-script (runs before auto-analysis).
//  - turns on "Decompiler Parameter ID" so Watcom register arguments
//    (EAX, EDX, EBX, ECX) are recovered as real parameters
//  - creates functions at every code address stored in the data object
//    (C++ vtables, callback tables) — taken from `ledump --fixups`
//
// -preScript Prepare.java <fixups.txt>
//@category Ascendancy
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.mem.MemoryBlock;

import java.io.BufferedReader;
import java.io.FileReader;
import java.util.TreeSet;

public class Prepare extends GhidraScript {
  @Override
  public void run() throws Exception {
    setAnalysisOption(currentProgram, "Decompiler Parameter ID", "true");

    String[] args = getScriptArgs();
    if (args.length == 0) return;
    MemoryBlock text = currentProgram.getMemory().getBlock(".text");
    TreeSet<Long> targets = new TreeSet<>();
    try (BufferedReader r = new BufferedReader(new FileReader(args[0]))) {
      String line;
      while ((line = r.readLine()) != null) {
        // "<src> <type> -> <dst>"
        String[] p = line.trim().split("\\s+");
        if (p.length < 4) continue;
        long src = Long.parseLong(p[0], 16), dst = Long.parseLong(p[3], 16);
        Address s = toAddr(src), d = toAddr(dst);
        if (!text.contains(s) && text.contains(d)) targets.add(dst);
      }
    }
    int made = 0;
    for (long t : targets) {
      Address a = toAddr(t);
      new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
      if (getFunctionAt(a) == null &&
          new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) made++;
    }
    println("Prepare: " + targets.size() + " code pointers in data, " + made + " functions created");
  }
}
