// Prints naming progress stats.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
public class NamingStats extends GhidraScript {
  boolean unnamed(Function f){ String n=f.getName(); return f.getSymbol().getSource()==SourceType.DEFAULT || n.startsWith("entry") || n.startsWith("FUN_"); }
  public void run() throws Exception {
    int total=0, un=0, ready=0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      total++; if (!unnamed(f)) continue; un++;
      boolean ok=true; for (Function c: f.getCalledFunctions(monitor)) if (c!=f && unnamed(c)) { ok=false; break; }
      if (ok) ready++;
    }
    println("total="+total+" unnamed="+un+" ready="+ready);
  }
}
