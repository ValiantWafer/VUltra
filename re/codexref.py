import sys, pefile, struct
EXE = r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(EXE, "rb").read()
text = None
for s in pe.sections:
    if s.Name.rstrip(b"\x00") == b".text":
        text = s
to = int(sys.argv[1], 0)  # target VA
tsva = to
# scan whole file for E8/E9 rel32 whose target == to
results = []
for op in (0xE8, 0xE9):
    i = 0
    while True:
        i = data.find(bytes([op]), i)
        if i < 0: break
        # need 4 bytes after
        if i+5 <= len(data):
            rel = struct.unpack_from("<i", data, i+1)[0]
            # compute instruction VA
            # map fileoff->rva
            rva = pe.get_rva_from_offset(i)
            if rva is not None:
                insn_va = base + rva
                tgt = (insn_va + 5 + rel) & 0xffffffff
                if tgt == to:
                    results.append((insn_va, "call" if op==0xE8 else "jmp"))
        i += 1
print("xrefs to 0x%08x: %d" % (to, len(results)))
for va, kind in sorted(results):
    print("  0x%08x  %s" % (va, kind))
