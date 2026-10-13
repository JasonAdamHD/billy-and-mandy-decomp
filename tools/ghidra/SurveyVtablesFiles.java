// Survey: (1) runs of >=3 consecutive function-entry pointers in data (vtable candidates)
// (2) functions referencing "*.cpp" filename strings.
// Writes vtables.txt and filefuncs.txt into the given output dir.
// args: outDir(no spaces)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class SurveyVtablesFiles extends GhidraScript {
  public void run() throws Exception {
    String out=getScriptArgs()[0];
    Memory mem=currentProgram.getMemory(); FunctionManager fm=currentProgram.getFunctionManager();
    long lo=0x500000L, hi=0x5f8d80L; // data area after code (code ends ~0x512xxx)
    // find code end: highest function entry
    long codeEnd=0; for (Function f: fm.getFunctions(true)) codeEnd=Math.max(codeEnd,f.getBody().getMaxAddress().getOffset());
    lo=Math.min(lo,codeEnd);
    int vt=0, entries=0;
    try (PrintWriter w=new PrintWriter(out+"/vtables.txt")) {
      long a=lo; List<Long> run=new ArrayList<>(); long runStart=0;
      while (a<hi) {
        int v; try { v=mem.getInt(toAddr(a)); } catch(Exception ex){ v=0; }
        long uv=v&0xffffffffL;
        Function f = (uv>=0x100000L && uv<codeEnd) ? fm.getFunctionAt(toAddr(uv)) : null;
        if (f!=null) { if (run.isEmpty()) runStart=a; run.add(uv); }
        else {
          if (run.size()>=3) {
            vt++; entries+=run.size();
            StringBuilder sb=new StringBuilder(String.format("VT %08x n=%d refs=%d:", runStart, run.size(), getReferencesTo(toAddr(runStart)).length));
            for (long x: run) sb.append(' ').append(String.format("%x=%s", x, fm.getFunctionAt(toAddr(x)).getName(true)));
            w.println(sb);
          }
          run.clear();
        }
        a+=4;
      }
    }
    // file strings
    Map<String,Set<String>> fileFuncs=new TreeMap<>();
    for (Data d : currentProgram.getListing().getDefinedData(true)) {
      if (!(d.getValue() instanceof String)) continue;
      if (d.getAddress().getOffset()>=0x20000000L) continue;
      String s=((String)d.getValue()).trim();
      if (!s.matches("(?i)[A-Za-z0-9_./\\\\]+\\.(cpp|c|h|hpp)")) continue;
      for (Reference r : getReferencesTo(d.getAddress())) {
        Function f=getFunctionContaining(r.getFromAddress()); if (f==null) continue;
        fileFuncs.computeIfAbsent(s,k->new TreeSet<>()).add(f.getEntryPoint()+"="+f.getName(true));
      }
    }
    int ff=0;
    try (PrintWriter w=new PrintWriter(out+"/filefuncs.txt")) {
      for (Map.Entry<String,Set<String>> e: fileFuncs.entrySet()) { ff+=e.getValue().size(); w.println(e.getKey()+" ("+e.getValue().size()+"): "+String.join(" ",e.getValue())); }
    }
    println("codeEnd="+Long.toHexString(codeEnd)+" vtables="+vt+" vtEntries="+entries+" files="+fileFuncs.size()+" fileFuncRefs="+ff);
  }
}
