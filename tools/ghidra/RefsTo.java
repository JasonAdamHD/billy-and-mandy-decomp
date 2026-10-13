// Lists all references to an address (from addr, type, containing function). args: addrHex
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
public class RefsTo extends GhidraScript {
  public void run() throws Exception {
    StringBuilder sb=new StringBuilder();
    for (String s: getScriptArgs()) {
      for (Reference r: getReferencesTo(toAddr(Long.decode(s)))) {
        Function f=getFunctionContaining(r.getFromAddress());
        sb.append(s+" <- "+r.getFromAddress()+" "+r.getReferenceType()+" "+(f==null?"(data)":f.getName(true))+"\n");
      }
    }
    println(sb.toString());
  }
}
