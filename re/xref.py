import sys, pefile, struct
EXE = r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = open(EXE, "rb").read()

# map sections
secs = [(s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Name) for s in pe.sections]
def rva_of_fileoff(fo):
    for va, vs, pr, sr, nm in secs:
        if pr <= fo < pr + sr:
            return va + (fo - pr)
    return None
def fileoff_of_rva(rva):
    return pe.get_offset_from_rva(rva)

needle = sys.argv[1].encode() + b"\x00"
i = data.find(needle)
while i != -1:
    srva = rva_of_fileoff(i)
    sva = base + srva if srva is not None else None
    print("string at fileoff=0x%x rva=0x%x va=0x%x : %r" % (i, srva or 0, sva or 0, needle))
    if sva:
        # search for absolute references to this VA (push imm32 / mov ... , imm32)
        token = struct.pack("<I", sva)
        j = data.find(token)
        cnt = 0
        while j != -1 and cnt < 20:
            jrva = rva_of_fileoff(j)
            if jrva is not None:
                print("   ref at fileoff=0x%x rva=0x%x va=0x%x  ctxt=%s" % (j, jrva, base+jrva, data[j-1:j+4].hex()))
                cnt += 1
            j = data.find(token, j+1)
    i = data.find(needle, i+1)
