// Prints the address and 32-bit value of named symbols. args: name...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
public class ReadSym extends GhidraScript {
  public void run() throws Exception {
    StringBuilder sb=new StringBuilder();
    for (String n: getScriptArgs()) for (Symbol s: currentProgram.getSymbolTable().getSymbols(n))
      sb.append(n+" @ "+s.getAddress()+" = 0x"+Integer.toHexString(getInt(s.getAddress()))+"\n");
    println(sb.toString());
  }
}
