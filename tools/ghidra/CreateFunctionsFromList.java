// Creates functions at the listed addresses (one 0xADDR per line) when the address is an instruction start
// not already inside a function. Processes in address order so earlier bodies don't swallow later starts
// that are referenced. args: dry|apply listPath
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.nio.file.*;
import java.util.*;
public class CreateFunctionsFromList extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); boolean apply=a[0].equals("apply");
    TreeSet<Long> addrs=new TreeSet<>();
    for (String ln: Files.readAllLines(Paths.get(a[1]))) { ln=ln.trim(); if(!ln.isEmpty()) addrs.add(Long.decode(ln)); }
    int made=0, inside=0, noInsn=0, fail=0; long bytes=0;
    for (long x: addrs) {
      Address ad=toAddr(x);
      if (getFunctionAt(ad)!=null) continue;
      if (getFunctionContaining(ad)!=null) { inside++; continue; }
      if (getInstructionAt(ad)==null) { noInsn++; continue; }
      if (!apply) { made++; continue; }
      Function f=createFunction(ad,null);
      if (f==null) fail++; else { made++; bytes+=f.getBody().getNumAddresses(); }
    }
    println((apply?"APPLIED":"DRYRUN")+" created="+made+" bytes="+bytes+" nowInsideOther="+inside+" noInstruction="+noInsn+" failed="+fail);
  }
}
