// Lists functions at/above a start address with code-likeness evidence, and summarizes instructions
// outside functions in that range. args: startHex endHex outFile
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;
public class SurveyHighCode extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); long lo=Long.decode(a[0]), hi=Long.decode(a[1]);
    AddressSet set=new AddressSet(toAddr(lo),toAddr(hi-1));
    int nf=0;
    try (PrintWriter w=new PrintWriter(a[2])) {
      for (Function f : currentProgram.getFunctionManager().getFunctions(set,true)) {
        nf++;
        int insn=0, jrra=0, bad=0, prologue=0, zeroWords=0;
        for (Instruction i : currentProgram.getListing().getInstructions(f.getBody(),true)) {
          insn++; String m=i.getMnemonicString();
          if (i.toString().startsWith("jr ra")) jrra++;
          if (m.startsWith("_") || m.equals("syscall") || m.equals("break")) bad++;
          if (i.toString().startsWith("addiu sp,sp,-")) prologue++;
          try { if (i.getInt(0)==0) zeroWords++; } catch(Exception e){}
        }
        int codeCallers=0, dataRefs=0;
        for (Reference r : getReferencesTo(f.getEntryPoint())) {
          if (r.getReferenceType().isCall() || r.getReferenceType().isJump()) { Function c=getFunctionContaining(r.getFromAddress()); if (c!=null && c!=f) codeCallers++; }
          else dataRefs++;
        }
        w.printf("%s size=%d insn=%d jrra=%d prologue=%d bad=%d nops=%d callers=%d dataRefs=%d name=%s%n",
          f.getEntryPoint(), f.getBody().getNumAddresses(), insn, jrra, prologue, bad, zeroWords, codeCallers, dataRefs, f.getName(true));
      }
    }
    // undefined-but-disassembled instructions outside functions
    int loose=0; for (Instruction i : currentProgram.getListing().getInstructions(set,true)) if (getFunctionContaining(i.getAddress())==null) loose++;
    println("functions="+nf+" looseInstructions="+loose);
  }
}
