// Dumps unnamed functions (address order) into batch files for LLM naming.
// args: outDir(no spaces) startAddrHex batchSize maxBatches firstBatchNumber [ready]
// ready = only functions whose callees are all named (bottom-up waves)
// Each batch file: header + per-function block (context + decompiled C, truncated).
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;
public class ExportBatches extends GhidraScript {
  static final int MAX_LINES=70;
  boolean unnamed(Function f){ String n=f.getName(); return f.getSymbol().getSource()==SourceType.DEFAULT || n.startsWith("entry") || n.startsWith("FUN_"); }
  TreeMap<Long,String> anchors=new TreeMap<>();
  Map<Long,List<String>> vtSlots=new HashMap<>();
  String fileOf(long a){
    Map.Entry<Long,String> p=anchors.floorEntry(a), n=anchors.ceilingEntry(a);
    if (p!=null && p.getKey()==a) return p.getValue()+" (direct)";
    if (p!=null && n!=null && p.getValue().equals(n.getValue())) return p.getValue()+" (inside)";
    return (p==null?"?":"after "+p.getValue())+" / "+(n==null?"?":"before "+n.getValue());
  }
  public void run() throws Exception {
    String[] a=getScriptArgs();
    String out=a[0]; long start=Long.decode(a[1]); int bsz=Integer.parseInt(a[2]); int maxB=Integer.parseInt(a[3]); int bno=Integer.parseInt(a[4]); boolean ready=a.length>5 && a[5].equals("ready");
    FunctionManager fm=currentProgram.getFunctionManager(); Memory mem=currentProgram.getMemory();
    // anchors from *.cpp strings
    for (Data d : currentProgram.getListing().getDefinedData(true)) {
      if (!(d.getValue() instanceof String) || d.getAddress().getOffset()>=0x20000000L) continue;
      String s=((String)d.getValue()).trim();
      if (!s.matches("(?i)[A-Za-z0-9_./\\\\]+\\.(cpp|c)")) continue;
      for (Reference r : getReferencesTo(d.getAddress())) { Function f=getFunctionContaining(r.getFromAddress()); if (f!=null) anchors.put(f.getEntryPoint().getOffset(), s.replaceAll(".*[/\\\\]","")); }
    }
    // vtable slots
    long codeEnd=0x512400L;
    for (long x=0x512400L; x<0x5f8d80L; x+=4) {
      List<Long> run=new ArrayList<>(); long rs=x;
      while (x<0x5f8d80L) { long v; try{ v=mem.getInt(toAddr(x))&0xffffffffL; }catch(Exception e){break;} if (v>=0x100000L && v<0x512400L && fm.getFunctionAt(toAddr(v))!=null) { run.add(v); x+=4; } else break; }
      if (run.size()>=3) for (int i=0;i<run.size();i++) vtSlots.computeIfAbsent(run.get(i),k->new ArrayList<>()).add(String.format("VT_%06x[%d/%d]",rs,i,run.size()));
    }
    DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    if (a.length>6 && a[5].equals("list")) {
      // export the functions listed (one 0xADDR per line) regardless of naming state, in file order
      List<Function> pick=new ArrayList<>();
      for (String ln : java.nio.file.Files.readAllLines(java.nio.file.Paths.get(a[6]))) {
        ln=ln.trim(); if (ln.isEmpty()) continue;
        Function f=fm.getFunctionAt(toAddr(Long.decode(ln.split(",")[0]))); if (f!=null) pick.add(f);
      }
      int w=0; for (int i=0;i<pick.size() && w<maxB;i+=bsz) { writeBatch(out,bno++,pick.subList(i,Math.min(pick.size(),i+bsz)),di); w++; }
      di.dispose();
      println("list: functions="+pick.size()+" batches="+w+" nextBatchNo="+bno);
      return;
    }
    if (a.length>5 && a[5].equals("rank")) {
      // pick the maxB*bsz unnamed functions with the fewest unnamed callees, emit in address order
      List<Function> all=new ArrayList<>(); Map<Function,Integer> cnt=new HashMap<>();
      for (Function f: fm.getFunctions(true)) {
        if (!unnamed(f) || f.getEntryPoint().getOffset()>=0x5f8d80L) continue;
        int n=0; for (Function c: f.getCalledFunctions(monitor)) if (c!=f && unnamed(c)) n++;
        all.add(f); cnt.put(f,n);
      }
      all.sort(Comparator.comparingInt((Function f)->cnt.get(f)).thenComparing(Function::getEntryPoint));
      List<Function> pick=new ArrayList<>(all.subList(0,Math.min(all.size(),maxB*bsz)));
      pick.sort(Comparator.comparing(Function::getEntryPoint));
      int w=0; for (int i=0;i<pick.size();i+=bsz) { writeBatch(out,bno++,pick.subList(i,Math.min(pick.size(),i+bsz)),di); w++; }
      di.dispose();
      println("rank: unnamed="+all.size()+" batches="+w+" nextBatchNo="+bno+" maxUnnamedCalleesPicked="+(pick.isEmpty()?0:cnt.get(all.get(pick.size()-1))));
      return;
    }
    FunctionIterator it=fm.getFunctions(toAddr(start),true);
    int written=0; long last=start; List<Function> cur=new ArrayList<>();
    while (it.hasNext() && written<maxB && !monitor.isCancelled()) {
      Function f=it.next(); if (!unnamed(f)) continue;
      if (f.getEntryPoint().getOffset()>=0x5f8d80L) continue;
      if (ready) { boolean ok=true; for (Function c: f.getCalledFunctions(monitor)) if (c!=f && unnamed(c)) { ok=false; break; } if (!ok) continue; }
      cur.add(f);
      if (cur.size()==bsz) { writeBatch(out,bno++,cur,di); written++; last=f.getEntryPoint().getOffset(); cur.clear(); }
    }
    if (!cur.isEmpty() && written<maxB) { writeBatch(out,bno++,cur,di); written++; last=cur.get(cur.size()-1).getEntryPoint().getOffset(); }
    di.dispose();
    println("batches="+written+" nextBatchNo="+bno+" resumeAfter=0x"+Long.toHexString(last+4));
  }
  void writeBatch(String out,int bno,List<Function> fs,DecompInterface di) throws Exception {
    try (PrintWriter w=new PrintWriter(String.format("%s/b%04d.txt",out,bno))) {
      w.println("# batch "+bno+" functions="+fs.size());
      for (Function f: fs) {
        long ea=f.getEntryPoint().getOffset();
        w.printf("\n### 0x%08x %s size=%d file=%s%n", ea, f.getName(true), f.getBody().getNumAddresses(), fileOf(ea));
        List<String> vs=vtSlots.get(ea); if (vs!=null) w.println("vtable: "+String.join(" ",vs.subList(0,Math.min(6,vs.size()))));
        Set<String> callers=new TreeSet<>(); for (Function c: f.getCallingFunctions(monitor)) { callers.add(c.getName(true)); if (callers.size()>=8) break; }
        Set<String> callees=new TreeSet<>(); for (Function c: f.getCalledFunctions(monitor)) callees.add(c.getName(true));
        w.println("callers("+f.getSymbol().getReferenceCount()+"): "+String.join(", ",callers));
        if (!callees.isEmpty()) w.println("callees: "+String.join(", ",callees));
        DecompileResults r=di.decompileFunction(f,20,monitor);
        String c=(r!=null && r.decompileCompleted())? r.getDecompiledFunction().getC() : "<decompile failed>";
        String[] lines=c.split("\n"); int n=0; StringBuilder sb=new StringBuilder();
        for (String l: lines) { if (l.trim().isEmpty()) continue; if (n++>=MAX_LINES) { sb.append("  ... ("+(lines.length-MAX_LINES)+" more lines)\n"); break; } sb.append(l).append('\n'); }
        w.print(sb);
      }
    }
  }
}
