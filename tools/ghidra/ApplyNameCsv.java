// Applies names from a CSV: address,name[,...] (name may be Class::Method). Header line optional.
// Only renames functions that are still unnamed (FUN_/entry_ / DEFAULT) unless arg2 == force.
// Creates functions at addresses with none (if not inside another function).
// args: dry|apply <csvPath(no spaces)> [force]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.util.*;
public class ApplyNameCsv extends GhidraScript {
  boolean unnamed(Function f){ return f.getSymbol().getSource()==SourceType.DEFAULT || f.getName().startsWith("entry") || f.getName().startsWith("FUN_"); }
  public void run() throws Exception {
    String[] a=getScriptArgs();
    boolean apply=a[0].equals("apply"), force=a.length>2 && a[2].equals("force");
    SymbolTable st=currentProgram.getSymbolTable();
    int ok=0, skipNamed=0, bad=0, created=0; StringBuilder sb=new StringBuilder();
    for (String ln : Files.readAllLines(Paths.get(a[1]))) {
      ln=ln.trim(); if (ln.isEmpty() || ln.startsWith("#") || ln.toLowerCase().startsWith("address")) continue;
      String[] e=ln.split(",");
      if (e.length<2) { bad++; continue; }
      String name=e[1].trim();
      if (!name.matches("([A-Za-z_][A-Za-z0-9_]*::)?[A-Za-z_][A-Za-z0-9_]*")) { bad++; sb.append("BADNAME "+ln+"\n"); continue; }
      Address ad;
      try { ad=toAddr(Long.decode(e[0].trim().startsWith("0x")?e[0].trim():"0x"+e[0].trim())); } catch(Exception ex){ bad++; sb.append("BADADDR "+ln+"\n"); continue; }
      Function f=getFunctionAt(ad);
      if (f==null) {
        if (getFunctionContaining(ad)!=null) { bad++; sb.append("INSIDE "+ln+" in "+getFunctionContaining(ad).getName()+"\n"); continue; }
        if (!apply) { created++; continue; }
        if (getInstructionAt(ad)==null) disassemble(ad);
        f=createFunction(ad,null); if (f==null) { bad++; sb.append("CREATEFAIL "+ln+"\n"); continue; }
        created++;
      }
      if (!force && !unnamed(f)) { skipNamed++; continue; }
      String cls=null, meth=name;
      if (name.contains("::")) { cls=name.substring(0,name.indexOf("::")); meth=name.substring(name.indexOf("::")+2); }
      if (apply) {
        try {
          Namespace ns=currentProgram.getGlobalNamespace();
          if (cls!=null) {
            ns=st.getNamespace(cls, currentProgram.getGlobalNamespace());
            if (ns==null) ns=st.createClass(currentProgram.getGlobalNamespace(), cls, SourceType.USER_DEFINED);
          }
          String base=meth; int k=2;
          while (true) {
            boolean clash=false;
            for (Symbol s : st.getSymbols(meth, ns)) if (!s.getAddress().equals(ad)) { clash=true; break; }
            if (!clash) break;
            meth=base+"_"+(k++);
          }
          if (!meth.equals(base)) sb.append("DEDUP "+ln+" -> "+meth+"\n");
          if (cls!=null || force) f.setParentNamespace(ns);
          f.setName(meth, SourceType.USER_DEFINED);
        } catch (Exception ex) { bad++; sb.append("FAIL "+ln+" "+ex.getMessage()+"\n"); continue; }
      }
      ok++;
    }
    println((apply?"APPLIED":"DRYRUN")+" ok="+ok+" created="+created+" skippedAlreadyNamed="+skipNamed+" bad="+bad+"\n"+(sb.length()>4000?sb.substring(0,4000)+"...":sb));
  }
}
