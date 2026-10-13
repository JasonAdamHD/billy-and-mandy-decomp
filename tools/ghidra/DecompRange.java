// Decompiles every function whose entry lies in [lo,hi) into one file. args: outFile loHex hiHex
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DecompRange extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    long lo = Long.decode(a[1]), hi = Long.decode(a[2]);
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    int n = 0;
    try (PrintWriter w = new PrintWriter(a[0])) {
      for (Function f : currentProgram.getFunctionManager().getFunctions(toAddr(lo), true)) {
        if (f.getEntryPoint().getOffset() >= hi) break;
        DecompileResults r = di.decompileFunction(f, 60, monitor);
        w.println("// ===== " + f.getName(true) + " @ " + f.getEntryPoint());
        w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
        n++;
      }
    }
    di.dispose(); println("functions=" + n);
  }
}
