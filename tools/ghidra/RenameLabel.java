// Renames the primary symbol at an address (label or function). args: addrHex newName
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
public class RenameLabel extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    Symbol s = getSymbolAt(toAddr(Long.decode(a[0])));
    if (s == null) { println("no symbol"); return; }
    String old = s.getName(true);
    s.setName(a[1], SourceType.USER_DEFINED);
    println(old + " -> " + s.getName(true));
  }
}
