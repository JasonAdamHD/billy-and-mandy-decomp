// Finds functions that address an absolute global via lui hi / (lw|sw|addiu) lo pairs, then decompiles them.
// args: outFile addrHex
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;
public class FindAbsUses extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); long target=Long.decode(a[1]);
    long lo=(short)(target & 0xffff); long hi=((target - lo) >> 16) & 0xffff;
    Set<Function> fs=new LinkedHashSet<>(); StringBuilder sites=new StringBuilder();
    for (Instruction i: currentProgram.getListing().getInstructions(true)) {
      String m=i.getMnemonicString().replace("_","");
      if (!(m.equals("lw")||m.equals("sw")||m.equals("addiu")||m.equals("ld")||m.equals("sd")||m.equals("lwu"))) continue;
      boolean hit=false;
      for (int k=0;k<i.getNumOperands() && !hit;k++) for (Object o: i.getOpObjects(k)) if (o instanceof Scalar && ((Scalar)o).getSignedValue()==lo) hit=true;
      if (!hit) continue;
      // look back up to 8 instructions for lui reg,hi
      Instruction p=i.getPrevious(); boolean ok=false;
      for (int k=0;k<8 && p!=null && !ok;k++,p=p.getPrevious())
        if (p.getMnemonicString().replace("_","").equals("lui")) for (Object o: p.getOpObjects(1)) if (o instanceof Scalar && (((Scalar)o).getUnsignedValue()&0xffff)==hi) ok=true;
      if (!ok) continue;
      Function f=getFunctionContaining(i.getAddress());
      sites.append(i.getAddress()+" "+i+" in "+(f==null?"?":f.getName(true))+"\n");
      if (f!=null) fs.add(f);
    }
    DecompInterface di=new DecompInterface(); di.openProgram(currentProgram);
    try (PrintWriter w=new PrintWriter(a[0])) {
      w.println("/* sites:\n"+sites+"*/");
      for (Function f: fs) {
        DecompileResults r=di.decompileFunction(f,60,monitor);
        w.println("// ===== "+f.getName(true)+" @ "+f.getEntryPoint());
        w.println(r.decompileCompleted()? r.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
    di.dispose(); println("functions="+fs.size());
  }
}
