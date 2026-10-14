"""Extract and disassemble the VU1 microprograms embedded in SLUS_214.51 (native renderer research).

Usage: python -I tools/vu1_programs.py <path to SLUS_214.51> <output dir>

Each program is a descriptor in the ELF data section: name[16] ("VU1..."), +0x10 pointer to a VIF packet,
+0x14 packet size in quadwords, +0x16 entry/load address, +0x18 u16[16] entry table. The packet is
STCYCL/STMASK/STMOD followed by MPG chunks. Writes asm_<NAME>.txt per program plus programs.txt.
"""
import os
import re
import struct
import sys

ELF_TEXT_BASE = 0xFFF00  # virtual address = file offset + this for the loaded segment


def extract(elf):
    data = open(elf, "rb").read()
    names = sorted((m.start() + ELF_TEXT_BASE, m.group().decode())
                   for m in re.finditer(rb"VU1[A-Z0-9_]*", data) if m.start() + ELF_TEXT_BASE < 0x540000)
    progs = {}
    for addr, name in names:
        off = addr - ELF_TEXT_BASE
        if off + 0x38 > len(data):
            continue
        ptr, = struct.unpack_from("<I", data, off + 16)
        cnt, load = struct.unpack_from("<HH", data, off + 20)
        table = struct.unpack_from("<16H", data, off + 24)
        if not (0x100000 <= ptr < 0x600000) or cnt == 0:
            continue
        pk = data[ptr - ELF_TEXT_BASE:ptr - ELF_TEXT_BASE + cnt * 16]
        pos, mem, events = 0, {}, []
        while pos + 4 <= len(pk):
            w = struct.unpack_from("<I", pk, pos)[0]
            pos += 4
            cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
            if cmd == 0x4A:
                n = num or 256
                events.append(("MPG", imm, n))
                for i in range(n):
                    mem[imm + i] = pk[pos + i * 8:pos + i * 8 + 8]
                pos += n * 8
            elif cmd == 0x20:
                pos += 4
            elif cmd in (0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x14, 0x15, 0x17):
                events.append((hex(cmd), imm))
            else:
                break
        if mem:
            progs[name] = dict(addr=addr, ptr=ptr, cnt=cnt, load=load, table=table, events=events, mem=mem)
    return progs


BC="xyzw"
def dest(i):
    s=""
    for b,c in ((24,"x"),(23,"y"),(22,"z"),(21,"w")):
        if i>>b&1: s+=c
    return s
def up(u):
    ft=u>>16&31; fs=u>>11&31; fd=u>>6&31; op=u&63; ds=dest(u)
    fl=("[I]" if u>>31 else "")+("[E]" if u>>30&1 else "")+("[M]" if u>>29&1 else "")+("[D]" if u>>28&1 else "")+("[T]" if u>>27&1 else "")
    def f3(n,bc=None):
        s=f"{n}.{ds} vf{fd:02d}, vf{fs:02d}, vf{ft:02d}"
        return s
    def bcs(n,fd_=True):
        b=BC[u&3]
        return f"{n}{b}.{ds} "+(f"vf{fd:02d}, " if fd_ else "")+f"vf{fs:02d}, vf{ft:02d}{b}"
    names={0:"ADD",4:"SUB",8:"MADD",12:"MSUB",16:"MAX",20:"MINI",24:"MUL"}
    if op<0x1c:
        return bcs(names[op&0x1c]),fl
    t={0x1c:"MULq",0x1d:"MAXi",0x1e:"MULi",0x1f:"MINIi",0x20:"ADDq",0x21:"MADDq",0x22:"ADDi",0x23:"MADDi",0x24:"SUBq",0x25:"MSUBq",0x26:"SUBi",0x27:"MSUBi"}
    if op in t:
        n=t[op]; 
        if n.endswith("q"): return f"{n[:-1]}.{ds} vf{fd:02d}, vf{fs:02d}, Q",fl
        if n.endswith("i"): return f"{n[:-1]}.{ds} vf{fd:02d}, vf{fs:02d}, I",fl
        if n=="MULq": pass
    t2={0x28:"ADD",0x29:"MADD",0x2a:"MUL",0x2b:"MAX",0x2c:"SUB",0x2d:"MSUB",0x2e:"OPMSUB",0x2f:"MINI"}
    if op in t2:
        if op==0x2e: return f"OPMSUB.xyz vf{fd:02d}, vf{fs:02d}, vf{ft:02d}",fl
        return f3(t2[op]),fl
    if op>=0x3c:
        s=(u&3)|((u>>4)&0x7c)
        if s<0x10:
            return bcs({0:"ADDA",4:"SUBA",8:"MADDA",12:"MSUBA"}[s&0xc],False).replace(" vf","ACC, vf",1) if False else f"{ {0:'ADDA',4:'SUBA',8:'MADDA',12:'MSUBA'}[s&0xc]}{BC[s&3]}.{ds} ACC, vf{fs:02d}, vf{ft:02d}{BC[s&3]}",fl
        if 0x10<=s<0x18:
            n=["ITOF0","ITOF4","ITOF12","ITOF15","FTOI0","FTOI4","FTOI12","FTOI15"][s-0x10]
            return f"{n}.{ds} vf{ft:02d}, vf{fs:02d}",fl
        if 0x18<=s<0x1c: return f"MULA{BC[s&3]}.{ds} ACC, vf{fs:02d}, vf{ft:02d}{BC[s&3]}",fl
        m={0x1c:"MULAq",0x1d:"ABS",0x1e:"MULAi",0x1f:"CLIP",0x20:"ADDAq",0x21:"MADDAq",0x22:"ADDAi",0x23:"MADDAi",0x24:"SUBAq",0x25:"MSUBAq",0x26:"SUBAi",0x27:"MSUBAi",0x28:"ADDA",0x29:"MADDA",0x2a:"MULA",0x2c:"SUBA",0x2d:"MSUBA",0x2e:"OPMULA",0x2f:"NOP"}
        n=m.get(s,"UP?%x"%s)
        if n=="NOP": return "NOP",fl
        if n=="ABS": return f"ABS.{ds} vf{ft:02d}, vf{fs:02d}",fl
        if n=="CLIP": return f"CLIP.xyz vf{fs:02d}xyz, vf{ft:02d}w",fl
        if n=="OPMULA": return f"OPMULA.xyz ACC, vf{fs:02d}, vf{ft:02d}",fl
        if n.endswith("q"): return f"{n[:-1]}.{ds} ACC, vf{fs:02d}, Q",fl
        if n.endswith("i"): return f"{n[:-1]}.{ds} ACC, vf{fs:02d}, I",fl
        return f"{n}.{ds} ACC, vf{fs:02d}, vf{ft:02d}",fl
    return "UP?%02x"%op,fl
def simm11(x): x&=0x7ff; return x-0x800 if x&0x400 else x
def lo(l,pc):
    if l==0x8000033c or l==0: return "NOP"
    op=l>>25; ft=l>>16&31; fs=l>>11&31; it=ft; is_=fs; id_=l>>6&31; ds=dest(l)
    imm=l&0x7ff; si=simm11(l)
    imm15=((l>>21&0xf)<<11)|imm
    if op==0: return f"LQ.{ds} vf{ft:02d}, {si}(vi{is_:02d})"
    if op==1: return f"SQ.{ds} vf{fs:02d}, {si}(vi{it:02d})"
    if op==4: return f"ILW.{ds} vi{it:02d}, {si}(vi{is_:02d})"
    if op==5: return f"ISW.{ds} vi{it:02d}, {si}(vi{is_:02d})"
    if op==8: return f"IADDIU vi{it:02d}, vi{is_:02d}, {imm15}"
    if op==9: return f"ISUBIU vi{it:02d}, vi{is_:02d}, {imm15}"
    imm24=l&0xffffff
    if op==0x10: return f"FCEQ vi01, 0x{imm24:x}"
    if op==0x11: return f"FCSET 0x{imm24:x}"
    if op==0x12: return f"FCAND vi01, 0x{imm24:x}"
    if op==0x13: return f"FCOR vi01, 0x{imm24:x}"
    if op==0x14: return f"FSEQ vi{it:02d}, 0x{(l>>10&0x800)|imm:x}"
    if op==0x15: return f"FSSET 0x{(l>>10&0x800)|imm:x}"
    if op==0x16: return f"FSAND vi{it:02d}, 0x{(l>>10&0x800)|imm:x}"
    if op==0x17: return f"FSOR vi{it:02d}, 0x{(l>>10&0x800)|imm:x}"
    if op==0x18: return f"FMEQ vi{it:02d}, vi{is_:02d}"
    if op==0x1a: return f"FMAND vi{it:02d}, vi{is_:02d}"
    if op==0x1b: return f"FMOR vi{it:02d}, vi{is_:02d}"
    if op==0x1c: return f"FCGET vi{it:02d}"
    t=(pc+1+si)&0x7ff
    if op==0x20: return f"B @{t}"
    if op==0x21: return f"BAL vi{it:02d}, @{t}"
    if op==0x24: return f"JR vi{is_:02d}"
    if op==0x25: return f"JALR vi{it:02d}, vi{is_:02d}"
    if op==0x28: return f"IBEQ vi{it:02d}, vi{is_:02d}, @{t}"
    if op==0x29: return f"IBNE vi{it:02d}, vi{is_:02d}, @{t}"
    if op==0x2c: return f"IBLTZ vi{is_:02d}, @{t}"
    if op==0x2d: return f"IBGTZ vi{is_:02d}, @{t}"
    if op==0x2e: return f"IBLEZ vi{is_:02d}, @{t}"
    if op==0x2f: return f"IBGEZ vi{is_:02d}, @{t}"
    if op==0x40:
        f=l&63
        if f==0x30: return f"IADD vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f==0x31: return f"ISUB vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f==0x32:
            im=l>>6&31; im=im-32 if im&16 else im
            return f"IADDI vi{it:02d}, vi{is_:02d}, {im}"
        if f==0x34: return f"IAND vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f==0x35: return f"IOR vi{id_:02d}, vi{is_:02d}, vi{it:02d}"
        if f>=0x3c:
            s=(l&3)|((l>>4)&0x7c)
            fsf=l>>21&3; ftf=l>>23&3
            m={0x30:f"MOVE.{ds} vf{ft:02d}, vf{fs:02d}",0x31:f"MR32.{ds} vf{ft:02d}, vf{fs:02d}",
               0x34:f"LQI.{ds} vf{ft:02d}, (vi{is_:02d}++)",0x35:f"SQI.{ds} vf{fs:02d}, (vi{it:02d}++)",
               0x36:f"LQD.{ds} vf{ft:02d}, (--vi{is_:02d})",0x37:f"SQD.{ds} vf{fs:02d}, (--vi{it:02d})",
               0x38:f"DIV Q, vf{fs:02d}{BC[fsf]}, vf{ft:02d}{BC[ftf]}",0x39:f"SQRT Q, vf{ft:02d}{BC[ftf]}",
               0x3a:f"RSQRT Q, vf{fs:02d}{BC[fsf]}, vf{ft:02d}{BC[ftf]}",0x3b:"WAITQ",
               0x3c:f"MTIR vi{it:02d}, vf{fs:02d}{BC[fsf]}",0x3d:f"MFIR.{ds} vf{ft:02d}, vi{is_:02d}",
               0x3e:f"ILWR.{ds} vi{it:02d}, (vi{is_:02d})",0x3f:f"ISWR.{ds} vi{it:02d}, (vi{is_:02d})",
               0x40:f"RNEXT.{ds} vf{ft:02d}",0x41:f"RGET.{ds} vf{ft:02d}",0x42:f"RINIT R, vf{fs:02d}{BC[fsf]}",0x43:f"RXOR R, vf{fs:02d}{BC[fsf]}",
               0x64:f"MFP.{ds} vf{ft:02d}, P",0x68:f"XTOP vi{it:02d}",0x69:f"XITOP vi{it:02d}",0x6c:f"XGKICK vi{is_:02d}",
               0x70:f"ESADD P, vf{fs:02d}",0x71:f"ERSADD P, vf{fs:02d}",0x72:f"ELENG P, vf{fs:02d}",0x73:f"ERLENG P, vf{fs:02d}",
               0x74:f"EATANxy P, vf{fs:02d}",0x75:f"EATANxz P, vf{fs:02d}",0x76:f"ESUM P, vf{fs:02d}",0x77:f"ERSQRT P, vf{fs:02d}{BC[fsf]}",
               0x78:f"ESQRT P, vf{fs:02d}{BC[fsf]}",0x79:f"ESIN P, vf{fs:02d}{BC[fsf]}",0x7a:f"ERCPR P, vf{fs:02d}{BC[fsf]}",0x7b:"WAITP",
               0x7c:f"EATAN P, vf{fs:02d}{BC[fsf]}",0x7d:f"EEXP P, vf{fs:02d}{BC[fsf]}"}
            return m.get(s,"LO1? %x"%s)
        return "LO0? f=%x"%f
    return "LO? op=%x %08x"%(op,l)


def disassemble(prog, out):
    mem, lines, targets = prog["mem"], [], set()
    for a in sorted(mem):
        l, u = struct.unpack("<II", mem[a])
        txt = f"LOI {struct.unpack('<f', struct.pack('<I', l))[0]:g}" if u >> 31 else lo(l, a)
        if "@" in txt and u >> 31 == 0:
            targets.add(int(txt.split("@")[1]))
        t, fl = up(u)
        lines.append((a, f"{fl:7s} {t:46s} | {txt}"))
    with open(out, "w") as f:
        for a, s in lines:
            f.write(("L%d:\n" % a if a in targets else "") + f"{a:4d}  {s}\n")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    progs = extract(sys.argv[1])
    os.makedirs(sys.argv[2], exist_ok=True)
    with open(os.path.join(sys.argv[2], "programs.txt"), "w") as f:
        for name, p in progs.items():
            f.write(f"{name:16s} desc=0x{p['addr']:06x} packet=0x{p['ptr']:06x} qwc={p['cnt']} load={p['load']} "
                    f"instr={min(p['mem'])}..{max(p['mem'])} entries={[e for e in p['table'] if e]}\n")
            disassemble(p, os.path.join(sys.argv[2], f"asm_{name}.txt"))
    print(f"{len(progs)} programs written to {sys.argv[2]}")
