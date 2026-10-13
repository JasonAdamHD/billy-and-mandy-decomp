// Exports default-named data globals (DAT_*) that are referenced from code, with the names of the functions
// that read/write them, as LLM batches. args: outDir batchSize maxBatches firstNo [statsOnly]
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;
public class ExportGlobals extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs(); String out=a[0]; int bsz=Integer.parseInt(a[1]), maxB=Integer.parseInt(a[2]), bno=Integer.parseInt(a[3]);
    boolean stats=a.length>4;
    SymbolTable st=currentProgram.getSymbolTable(); List<String> blocks=new ArrayList<>(); int total=0, withRefs=0;
    for (Symbol s : st.getAllSymbols(true)) {
      if (s.getSource()!=SourceType.DEFAULT || s.getSymbolType()!=SymbolType.LABEL) continue;
      String n=s.getName(); if (!n.startsWith("DAT_")) continue;
      long off=s.getAddress().getOffset(); if (off<0x100000L || off>=0x2000000L) continue;
      total++;
      Map<String,Integer> fr=new TreeMap<>(); int r=0, w=0;
      for (Reference ref : s.getReferences()) {
        Function f=getFunctionContaining(ref.getFromAddress()); if (f==null) continue;
        fr.merge(f.getName(true),1,Integer::sum);
        if (ref.getReferenceType().isWrite()) w++; else r++;
      }
      if (fr.isEmpty()) continue;
      withRefs++;
      if (stats) continue;
      Data d=getDataAt(s.getAddress());
      StringBuilder sb=new StringBuilder(String.format("0x%08x type=%s reads=%d writes=%d fns:", off, d==null?"?":d.getDataType().getName(), r, w));
      int k=0; for (Map.Entry<String,Integer> e: fr.entrySet()) { if (k++>=12) { sb.append(" ...+"+(fr.size()-12)); break; } sb.append(' ').append(e.getKey()); }
      blocks.add(sb.toString());
    }
    int wb=0;
    if (!stats) for (int i=0;i<blocks.size() && wb<maxB;i+=bsz) {
      try (PrintWriter pw=new PrintWriter(String.format("%s/g%04d.txt",out,bno++))) { for (String b: blocks.subList(i,Math.min(blocks.size(),i+bsz))) pw.println(b); }
      wb++;
    }
    println("DAT_ globals="+total+" referencedFromCode="+withRefs+" batches="+wb+" nextNo="+bno);
  }
}
