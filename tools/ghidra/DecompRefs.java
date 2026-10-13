// Decompiles every function that references the given address into a file. args: outFile addrHex
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class DecompRefs extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    Set<Function> fs=new LinkedHashSet<>();
    for (Reference r: getReferencesTo(toAddr(Long.decode(a[1])))) { Function f=getFunctionContaining(r.getFromAddress()); if (f!=null) fs.add(f); }
    try (PrintWriter w=new PrintWriter(a[0])) {
      for (Function f: fs) {
        DecompileResults r=di.decompileFunction(f,60,monitor);
        w.println("// ===== "+f.getName(true)+" @ "+f.getEntryPoint());
        w.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
    di.dispose(); println("functions="+fs.size());
  }
}
