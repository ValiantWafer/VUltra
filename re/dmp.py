import struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else r'J:/SteamLibrary/steamapps/common/Vagante/vagante_20260621_200314.dmp'
d = open(path, 'rb').read()
assert d[:4] == b'MDMP', d[:4]
nstreams, dirrva = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(nstreams):
    st, dsize, drva = struct.unpack_from('<III', d, dirrva + i * 12)
    streams[st] = (dsize, drva)

dsize, drva = streams[6]  # exception stream
exc_off = drva + 8
code, flags, rec, addr = struct.unpack_from('<IIQQ', d, exc_off)
nparams = struct.unpack_from('<I', d, exc_off + 24)[0]
params = [struct.unpack_from('<Q', d, exc_off + 32 + 8 * k)[0] for k in range(min(nparams, 15))]
print('exception code : 0x%08X (%s)' % (code, {0xC0000005: 'ACCESS_VIOLATION', 0xC000001D: 'ILLEGAL_INSTRUCTION', 0x80000003: 'BREAKPOINT'}.get(code, '?')))
print('fault address  : 0x%016X' % addr)
print('params         :', [hex(p) for p in params])
if code == 0xC0000005 and len(params) >= 2:
    print('  access type  :', {0: 'read', 1: 'write', 8: 'execute'}.get(params[0], params[0]), 'at 0x%X' % params[1])

dsize, drva = streams[4]  # module list
nmod = struct.unpack_from('<I', d, drva)[0]
mo = drva + 4
fault_rva = None
for i in range(nmod):
    base, size = struct.unpack_from('<QI', d, mo)
    nameRva = struct.unpack_from('<I', d, mo + 20)[0]
    ln = struct.unpack_from('<I', d, nameRva)[0]
    name = d[nameRva + 4:nameRva + 4 + ln].decode('utf-16-le')
    short = name.replace('\\', '/').split('/')[-1]
    if base <= addr < base + size:
        fault_rva = addr - base
        print('fault module   : %s  base=0x%X  RVA=0x%X' % (short, base, fault_rva))
    mo += 108
if fault_rva is None:
    print('fault address not in any module')
