// Lists (and optionally creates functions in) undisassembled holes in [lo,hi) that sit right after a
// "jr ra; <delay>" epilogue and start with a plausible instruction (not zero padding).
// args: loHex hiHex outFile [apply]
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class CodeHoles extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    long lo = Long.decode(a[0]), hi = Long.decode(a[1]);
    boolean apply = a.length > 3 && a[3].equals("apply");
    Listing listing = currentProgram.getListing();
    int found = 0, made = 0, pass = 0, passFound;
    try (PrintWriter w = new PrintWriter(a[2])) {
     do {
      passFound = 0; pass++;
      long p = lo;
      while (p < hi) {
        Address ad = toAddr(p);
        if (listing.getInstructionContaining(ad) != null || getFunctionContaining(ad) != null) { p += 4; continue; }
        long start = p;
        while (p < hi && listing.getInstructionContaining(toAddr(p)) == null && getFunctionContaining(toAddr(p)) == null) p += 4;
        long end = p;
        boolean afterReturn = start - 8 >= lo && getInt(toAddr(start - 8)) == 0x03e00008;
        // Skip zero alignment padding between the previous epilogue and the next routine.
        while (start < end && getInt(toAddr(start)) == 0) start += 4;
        if (start >= end) continue;
        int first = getInt(toAddr(start));
        if (!afterReturn || end - start < 8) continue;
        found++; passFound++;
        w.println(String.format("0x%x size=0x%x first=%08x", start, end - start, first));
        if (apply) {
          new DisassembleCommand(toAddr(start), null, true).applyTo(currentProgram, monitor);
          if (createFunction(toAddr(start), null) != null) made++;
        }
      }
     } while (apply && passFound > 0 && pass < 200);
    }
    println("passes=" + pass + " holes=" + found + " created=" + made);
  }
}
