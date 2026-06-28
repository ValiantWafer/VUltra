import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe"
pe = pefile.PE(EXE, fast_load=True)
image_base = pe.OPTIONAL_HEADER.ImageBase

def rva_to_off(rva):
    return pe.get_offset_from_rva(rva)

def read(rva, n):
    off = rva_to_off(rva)
    with open(EXE, "rb") as f:
        f.seek(off)
        return f.read(n)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

def disasm(rva, length):
    code = read(rva, length)
    for ins in md.disasm(code, image_base + rva):
        print("0x%08x  %-22s %s %s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))

if __name__ == "__main__":
    rva = int(sys.argv[1], 0)
    length = int(sys.argv[2], 0)
    print("image_base=0x%08x  rva=0x%08x  va=0x%08x  len=%d" % (image_base, rva, image_base+rva, length))
    disasm(rva, length)
