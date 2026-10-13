// Names unnamed functions from "Class::Method ..." diagnostic strings they reference.
// args: dry|apply
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.util.*;
import java.util.regex.*;
import java.io.*;
public class QualStrNames extends GhidraScript {
  boolean unnamed(Function f){ return f.getSymbol().getSource()==SourceType.DEFAULT || f.getName().startsWith("entry"); }
  public void run() throws Exception {
    boolean apply = getScriptArgs().length>0 && getScriptArgs()[0].equals("apply");
    Pattern p=Pattern.compile("^([A-Za-z_][A-Za-z0-9_]*)::(~?[A-Za-z_][A-Za-z0-9_]*)");
    Map<Function, Set<String>> props=new HashMap<>();
    for (Data d : currentProgram.getListing().getDefinedData(true)) {
      if (!(d.getValue() instanceof String)) continue;
      if (d.getAddress().getOffset()>=0x20000000L) continue;
      Matcher m=p.matcher(((String)d.getValue()).trim()); if(!m.find()) continue;
      String q=m.group(1)+"::"+m.group(2);
      for (Reference r : getReferencesTo(d.getAddress())) {
        Function f=getFunctionContaining(r.getFromAddress()); if(f==null) continue;
        props.computeIfAbsent(f,k->new TreeSet<>()).add(q);
      }
    }
    // shared single-proposal names -> overload suffixes in address order
    Map<String,List<Function>> byName=new TreeMap<>();
    StringBuilder conf=new StringBuilder(), out=new StringBuilder();
    for (Map.Entry<Function,Set<String>> e: props.entrySet()) {
      if (!unnamed(e.getKey())) continue;
      if (e.getValue().size()>1) { conf.append(e.getKey().getEntryPoint()+" "+e.getValue()+"\n"); continue; }
      byName.computeIfAbsent(e.getValue().iterator().next(),k->new ArrayList<>()).add(e.getKey());
    }
    SymbolTable st=currentProgram.getSymbolTable(); int applied=0, total=0;
    for (Map.Entry<String,List<Function>> e: byName.entrySet()) {
      List<Function> fs=e.getValue(); fs.sort(Comparator.comparing(Function::getEntryPoint));
      String[] parts=e.getKey().split("::");
      for (int i=0;i<fs.size();i++) {
        Function f=fs.get(i); String meth=parts[1].replace("~","Destructor_")+(fs.size()>1?"_"+(i+1):"");
        total++; out.append(f.getEntryPoint()+","+parts[0]+"::"+meth+"\n");
        if (apply) {
          Namespace ns=st.getNamespace(parts[0], currentProgram.getGlobalNamespace());
          if (ns==null) ns=st.createClass(currentProgram.getGlobalNamespace(), parts[0], SourceType.USER_DEFINED);
          f.setParentNamespace(ns); f.setName(meth, SourceType.USER_DEFINED); applied++;
        }
      }
    }
    try (PrintWriter w=new PrintWriter("C:/Users/jason/Desktop/Billy and Mandy Recomp/PS2Recomp/names/auto_qualified_strings.csv")) { w.print("address,name\n"+out); }
    println("assignable="+total+" applied="+applied+"\nMULTI-PROPOSAL (manual):\n"+conf);
  }
}
