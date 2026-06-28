import sys
from dia import create_datasource, mod

SymTagFunction = 5
SymTagData = 7
SymTagEnum = 12
SymTagPublicSymbol = 10
SymTagNull = 0
nsfCaseInsensitive = 8  # actually nsCaseInsensitive=1; we'll use regex-free contains via enumeration

PDB = r"J:\SteamLibrary\steamapps\common\Vagante\vagante.pdb"

ds = create_datasource()
ds.loadDataFromPdb(PDB)
session = ds.openSession()
g = session.globalScope

def enum_children(sym, tag=SymTagNull, name=None, flags=0):
    e = sym.findChildren(tag, name, flags)
    n = e.Count
    out = []
    for i in range(n):
        out.append(e.Item(i))
    return out

cmd = sys.argv[1] if len(sys.argv) > 1 else "enum"

if cmd == "enum":
    want = sys.argv[2] if len(sys.argv) > 2 else "SpellType"
    enums = enum_children(g, SymTagEnum, None, 0)
    for en in enums:
        if en.name and want in en.name:
            print("ENUM:", en.name)
            for m in enum_children(en, SymTagData, None, 0):
                try:
                    print("  %5d  %s" % (m.value, m.name))
                except Exception as ex:
                    print("  ?      %s (%s)" % (m.name, ex))

elif cmd == "enumof":
    # fast: find enum by exact name, list members
    for nm in sys.argv[2:]:
        e = g.findChildren(SymTagEnum, nm, 0)
        for i in range(e.Count):
            en = e.Item(i)
            print("ENUM:", en.name)
            for m in enum_children(en, SymTagData, None, 0):
                try: print("  %5d  %s" % (m.value, m.name))
                except Exception: print("  ?      %s" % m.name)

elif cmd == "findenum":
    # find any enum whose members contain a needle
    needle = sys.argv[2].upper()
    enums = enum_children(g, SymTagEnum, None, 0)
    for en in enums:
        members = enum_children(en, SymTagData, None, 0)
        names = [m.name for m in members if m.name]
        if any(needle in n.upper() for n in names):
            print("ENUM:", en.name, "(%d members)" % len(members))
            for m in members:
                try:
                    print("  %5d  %s" % (m.value, m.name))
                except Exception:
                    print("  ?      %s" % m.name)
            print()

elif cmd == "enums":
    # list all enum type names
    for en in enum_children(g, SymTagEnum, None, 0):
        if en.name:
            print(en.name)

elif cmd == "udt":
    # dump data members (offset, name) of a class/struct, optionally near an offset
    SymTagUDT = 11
    target_name = sys.argv[2]
    near = int(sys.argv[3], 0) if len(sys.argv) > 3 else None
    udts = enum_children(g, SymTagUDT, target_name, 0)
    for u in udts:
        members = enum_children(u, SymTagData, None, 0)
        rows = []
        for m in members:
            try:
                off = m.offset
                rows.append((off, m.name))
            except Exception:
                pass
        rows.sort()
        print("UDT %s (%d data members)" % (u.name, len(rows)))
        for off, nm in rows:
            if near is None or abs(off - near) <= 0x10:
                mark = " <<<" if near is not None and off == near else ""
                print("  +0x%-4x %s%s" % (off, nm, mark))
        break

elif cmd == "name":
    # fast exact / prefix name lookup via findChildren
    for needle in sys.argv[2:]:
        e = g.findChildren(SymTagNull, needle, 0)  # 0 = exact, case-sensitive
        if e.Count == 0:
            # try undecorated-name compare flag (nsfUndecoratedName=2) is not name; just report none
            print("(no exact match for %s)" % needle)
        for i in range(e.Count):
            s = e.Item(i)
            try:
                print("rva=0x%08x len=%s tag=%d  %s" % (s.relativeVirtualAddress, getattr(s,'length','?'), s.symTag, s.name))
            except Exception as ex:
                print("match %s (%s)" % (s.name, ex))

elif cmd == "byaddr":
    for a in sys.argv[2:]:
        rva = int(a, 0)
        sym = session.findSymbolByRVA(rva, SymTagFunction)
        if sym:
            print("rva=0x%08x -> 0x%08x len=%d  %s" % (rva, sym.relativeVirtualAddress, sym.length, sym.name))
        else:
            print("rva=0x%08x -> (no function symbol)" % rva)

elif cmd == "func":
    needle = sys.argv[2]
    funcs = enum_children(g, SymTagFunction, None, 0)
    for f in funcs:
        if f.name and needle in f.name:
            print("FUNC rva=0x%08x len=%d  %s" % (f.relativeVirtualAddress, f.length, f.name))
    pubs = enum_children(g, SymTagPublicSymbol, None, 0)
    for p in pubs:
        if p.name and needle in p.name:
            print("PUB  rva=0x%08x  %s" % (p.relativeVirtualAddress, p.name))
