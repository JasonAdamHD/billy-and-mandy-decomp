// Creates functions at every jal target inside [lo,hi) of a raw IOP RAM image. args: loHex hiHex
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import java.util.*;
public class IopSeedFunctions extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    long lo = Long.decode(a[0]), hi = Long.decode(a[1]);
    TreeSet<Long> targets = new TreeSet<>();
    for (long p = lo; p < hi; p += 4) {
      int w = getInt(toAddr(p));
      if ((w >>> 26) == 3) {
        long t = ((p + 4) & 0xf0000000L) | ((long)(w & 0x3ffffff) << 2);
        if (t >= lo && t < hi) targets.add(t);
      }
    }
    int made = 0;
    for (long t : targets) {
      Address ad = toAddr(t);
      if (getFunctionAt(ad) != null) continue;
      new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
      if (createFunction(ad, null) != null) made++;
    }
    println("targets=" + targets.size() + " created=" + made);
  }
}
