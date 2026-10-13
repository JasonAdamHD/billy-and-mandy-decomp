// Scans data (0x512400-0x5f8d80, plus code-region literal pools) for 32-bit values pointing into the code range
// [0x100000,0x512400) that are 4-byte aligned, are NOT a function entry, and are NOT inside a function.
// Writes candidate addresses (one per line) and prints a summary. args: outFile [apply]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import java.io.*;
import java.util.*;
public class FindCodePtrTargets extends GhidraScript {
  long codePtr(Memory mem, long x) {
    try { long v=mem.getInt(toAddr(x))&0xffffffffL; return (v>=0x100000L && v<0x512400L && (v&3)==0) ? v : -1; } catch(Exception e){ return -1; }
  }
  public void run() throws Exception {
    String[] a=getScriptArgs(); boolean apply=a.length>1 && a[1].equals("apply");
    Memory mem=currentProgram.getMemory(); FunctionManager fm=currentProgram.getFunctionManager();
    TreeMap<Long,Integer> cands=new TreeMap<>();
    Set<Long> inTable=new HashSet<>();
    for (long x=0x512400L; x<0x5f8d80L; x+=4) {
      long v=codePtr(mem,x); if (v<0) continue;
      boolean neighbour=codePtr(mem,x-4)>=0 || codePtr(mem,x+4)>=0;
      Address t=toAddr(v);
      if (fm.getFunctionAt(t)!=null || fm.getFunctionContaining(t)!=null) continue;
      cands.merge(v,1,Integer::sum);
      if (neighbour) inTable.add(v);
    }
    int insn=0, undef=0, created=0, failed=0;
    try (PrintWriter w=new PrintWriter(a[0])) {
      for (Map.Entry<Long,Integer> e: cands.entrySet()) {
        Address t=toAddr(e.getKey());
        boolean isInsn=getInstructionAt(t)!=null;
        if (isInsn) insn++; else undef++;
        // only treat as code if the bytes look like a function start: instruction already there, or a prologue word
        int word=mem.getInt(t);
        boolean prologue=(word & 0xffff0000)==0x27bd0000 && (short)(word&0xffff)<0;
        boolean table=inTable.contains(e.getKey());
        if (!isInsn && !prologue && !table) continue;
        w.println(String.format("0x%08x refs=%d %s", e.getKey(), e.getValue(), isInsn?"insn":(prologue?"prologue":"table")));
        if (apply) {
          if (!isInsn) disassemble(t);
          Function f=createFunction(t,null);
          if (f!=null) created++; else failed++;
        }
      }
    }
    println("candidates="+cands.size()+" alreadyInsn="+insn+" undefined="+undef+(apply?" created="+created+" failed="+failed:""));
  }
}
