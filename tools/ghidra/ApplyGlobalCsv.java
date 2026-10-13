// Renames default DAT_ labels from CSV address,name[,conf]. args: dry|apply csvPath
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.nio.file.*;
public class ApplyGlobalCsv extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); boolean apply=a[0].equals("apply");
    SymbolTable st=currentProgram.getSymbolTable(); int ok=0, skip=0, bad=0, dedup=0;
    for (String ln : Files.readAllLines(Paths.get(a[1]))) {
      ln=ln.trim(); if (ln.isEmpty()) continue; String[] e=ln.split(",");
      if (e.length<2 || !e[1].trim().matches("[A-Za-z_][A-Za-z0-9_]*")) { bad++; continue; }
      Address ad; try { ad=toAddr(Long.decode(e[0].trim())); } catch(Exception x){ bad++; continue; }
      Symbol s=st.getPrimarySymbol(ad);
      if (s==null || s.getSource()!=SourceType.DEFAULT) { skip++; continue; }
      String base=e[1].trim(), n=base; int k=2;
      while (!st.getGlobalSymbols(n).isEmpty()) n=base+"_"+(k++);
      if (!n.equals(base)) dedup++;
      if (apply) { try { s.setName(n, SourceType.USER_DEFINED); } catch(Exception x){ bad++; continue; } }
      ok++;
    }
    println((apply?"APPLIED":"DRYRUN")+" ok="+ok+" skipped="+skip+" bad="+bad+" dedup="+dedup);
  }
}
