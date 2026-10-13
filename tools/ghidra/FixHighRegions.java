// One-off fix of the region above the main code end. args: dry|apply
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
public class FixHighRegions extends GhidraScript {
  public void run() throws Exception {
    boolean apply=getScriptArgs()[0].equals("apply"); StringBuilder sb=new StringBuilder();
    // 1. kernel-patch blob: remove functions, clear code, define dwords
    AddressSet blob=new AddressSet(toAddr(0x536000L),toAddr(0x538fffL));
    long bmin=Long.MAX_VALUE,bmax=0; int nf=0;
    for (Function f: currentProgram.getFunctionManager().getFunctions(blob,true)) { nf++; bmin=Math.min(bmin,f.getBody().getMinAddress().getOffset()); bmax=Math.max(bmax,f.getBody().getMaxAddress().getOffset()); }
    for (Instruction i: currentProgram.getListing().getInstructions(blob,true)) { bmin=Math.min(bmin,i.getAddress().getOffset()); bmax=Math.max(bmax,i.getAddress().getOffset()+3); }
    sb.append(String.format("blob: functions=%d code range %x-%x%n",nf,bmin,bmax));
    if (apply && nf+bmax>0) {
      for (Function f: currentProgram.getFunctionManager().getFunctions(blob,true)) removeFunction(f);
      clearListing(toAddr(bmin),toAddr(bmax));
      for (long x=bmin; x<=bmax-3; x+=4) if (getDataAt(toAddr(x))==null) try { createData(toAddr(x),new DWordDataType()); } catch(Exception e){}
    }
    // 2. data tables passed as pointers
    for (long t: new long[]{0x5c1760L,0x5c1820L}) {
      sb.append("table "+Long.toHexString(t)+" insn="+getInstructionAt(toAddr(t))+"\n");
      if (apply) { clearListing(toAddr(t),toAddr(t+7)); createData(toAddr(t),new DWordDataType()); createData(toAddr(t+4),new DWordDataType()); }
    }
    // 3. missing static-initializer functions
    for (long s: new long[]{0x5c1880L,0x5c9410L,0x5c9520L,0x5c9690L,0x5c96a0L,0x5ca180L,0x5ca290L,0x5ca2c0L,0x5ca340L,0x5ca390L}) {
      Function f=getFunctionAt(toAddr(s));
      if (f==null && apply) f=createFunction(toAddr(s),null);
      sb.append(String.format("fn %x -> %s%n",s,f==null?(apply?"CREATE FAILED":"would create"):f.getName()+" size="+f.getBody().getNumAddresses()));
    }
    println((apply?"APPLIED\n":"DRYRUN\n")+sb);
  }
}
