import pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
EXE = r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(EXE, "rb").read()
text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
tstart = text.PointerToRawData
tend = tstart + text.SizeOfRawData
md = Cs(CS_ARCH_X86, CS_MODE_32)

# find occurrences of disp32 == 0x00000280 (bytes 80 02 00 00) inside .text
needle = b"\x80\x02\x00\x00"
i = tstart
hits = []
while True:
    i = data.find(needle, i, tend)
    if i < 0: break
    hits.append(i)
    i += 1

print("raw 0x280-disp candidates in .text:", len(hits))
# disassemble a small window ending near each hit to recover the actual instruction accessing [reg+0x280]
seen = set()
for h in hits:
    # try to align: decode starting a few bytes before so the instruction with disp lands on h
    for back in range(1, 8):
        startoff = h - back
        va = base + pe.get_rva_from_offset(startoff)
        code = data[startoff:startoff+16]
        ok = False
        for ins in md.disasm(code, va):
            if "0x280" in ins.op_str and ("+ 0x280" in ins.op_str):
                key = ins.address
                if key not in seen:
                    seen.add(key)
                    print("0x%08X  %-22s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))
                ok = True
                break
        if ok:
            break
print("distinct [reg+0x280] instructions:", len(seen))
