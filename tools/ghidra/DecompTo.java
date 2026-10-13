// Decompiles the functions containing the given addresses into a file. args: outFile addr1 [addr2 ...]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DecompTo extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    try (PrintWriter w=new PrintWriter(a[0])) {
      for (int i=1;i<a.length;i++) {
        Function f=getFunctionContaining(toAddr(Long.decode(a[i])));
        if (f==null) { w.println("// no function at "+a[i]); continue; }
        DecompileResults r=di.decompileFunction(f,60,monitor);
        w.println("// ===== "+f.getName(true)+" @ "+f.getEntryPoint()+" (query "+a[i]+")");
        w.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
    di.dispose(); println("ok");
  }
}
