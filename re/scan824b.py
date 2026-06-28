import pefile, capstone
pe = pefile.PE(r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe", fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if b'.text' in s.Name)
data = text.get_data(); base_va = ib + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
needle = b'\x24\x08\x00\x00'
i=0; out=[]
while True:
    j = data.find(needle, i)
    if j<0: break
    i=j+1
    # try decode an instruction starting a few bytes back so disp lands here
    for back in range(1,8):
        s=j-back
        if s<0: continue
        for ins in md.disasm(data[s:s+16], base_va+s):
            if '0x824' in ins.op_str:
                out.append((ins.address, ins.mnemonic, ins.op_str))
            break
seen=set()
for a,m,o in out:
    if a in seen: continue
    seen.add(a)
    print(hex(a), m, o)
print("total", len(seen))
