import pefile, capstone
pe = pefile.PE(r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe", fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if b'.text' in s.Name)
data = text.get_data(); base_va = ib + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail=True
hits=[]
for ins in md.disasm(data, base_va):
    if '0x824' in ins.op_str:
        hits.append((ins.address, ins.mnemonic, ins.op_str))
for a,m,o in hits:
    print(hex(a), m, o)
print("total", len(hits))
