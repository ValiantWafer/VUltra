import pefile, capstone
pe = pefile.PE(r"J:\SteamLibrary\steamapps\common\Vagante\vagante.exe", fast_load=True)
ib = pe.OPTIONAL_HEADER.ImageBase
text = next(s for s in pe.sections if b'.text' in s.Name)
data = text.get_data(); base_va = ib + text.VirtualAddress
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# reads/writes of [ebp-0x8d] -> disp32 0xffffff73 -> bytes 73 ff ff ff with modrm ..101 (ebp)
needle=b'\x73\xff\xff\xff'
i=0
lo,hi=0x5c23b0,0x5c23b0+10470
while True:
    j=data.find(needle,i)
    if j<0: break
    i=j+1
    for back in range(1,7):
        s=j-back
        for ins in md.disasm(data[s:s+16], base_va+s):
            if '0x8d]' in ins.op_str and 'ebp' in ins.op_str:
                a=ins.address
                if lo<=a<=hi:
                    print(hex(a), ins.mnemonic, ins.op_str)
            break
