// Lists runs of instructions not inside any function within [lo,hi): start, count, refs to start, first insn.
// args: loHex hiHex
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
public class LooseRuns extends GhidraScript {
  public void run() throws Exception {
    long lo=Long.decode(getScriptArgs()[0]), hi=Long.decode(getScriptArgs()[1]);
    StringBuilder sb=new StringBuilder(); Instruction runStart=null; int n=0; long prev=-1;
    for (Instruction i : currentProgram.getListing().getInstructions(new AddressSet(toAddr(lo),toAddr(hi-1)),true)) {
      if (getFunctionContaining(i.getAddress())!=null) continue;
      long a=i.getAddress().getOffset();
      if (runStart!=null && a!=prev+4) { emit(sb,runStart,n); runStart=null; }
      if (runStart==null) { runStart=i; n=0; }
      n++; prev=a;
    }
    if (runStart!=null) emit(sb,runStart,n);
    println(sb.toString());
  }
  void emit(StringBuilder sb, Instruction s, int n) {
    sb.append(s.getAddress()+" n="+n+" refs="+getReferencesTo(s.getAddress()).length+" : "+s+"\n");
  }
}
