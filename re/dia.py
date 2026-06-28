"""Minimal DIA (Debug Interface Access) driver using msdia140.dll without registration."""
import comtypes, comtypes.client
from comtypes import GUID, IUnknown, COMMETHOD, HRESULT
import ctypes
from ctypes import POINTER, byref, c_void_p, c_wchar_p
from ctypes.wintypes import DWORD, ULONG

MSDIA = r"C:\Program Files\dotnet\sdk\9.0.101\TestHostNetFramework\x64\msdia140.dll"
CLSID_DiaSource = GUID("{E6756135-1E65-4D17-8576-610761398C3C}")

# Generate comtypes wrappers from the typelib embedded in msdia140.dll
mod = comtypes.client.GetModule(MSDIA)

import ctypes
IID_IClassFactory = GUID("{00000001-0000-0000-C000-000000000046}")

class IClassFactory(IUnknown):
    _iid_ = IID_IClassFactory
    _methods_ = [
        COMMETHOD([], HRESULT, "CreateInstance",
                  (['in'], POINTER(IUnknown), "pUnkOuter"),
                  (['in'], POINTER(GUID), "riid"),
                  (['out'], POINTER(c_void_p), "ppv")),
        COMMETHOD([], HRESULT, "LockServer", (['in'], ctypes.c_int, "fLock")),
    ]

def create_datasource():
    dll = ctypes.OleDLL(MSDIA)
    dll.DllGetClassObject.argtypes = [POINTER(GUID), POINTER(GUID), POINTER(c_void_p)]
    pcf = c_void_p()
    dll.DllGetClassObject(byref(CLSID_DiaSource), byref(IID_IClassFactory), byref(pcf))
    factory = ctypes.cast(pcf, POINTER(IClassFactory))
    pobj = factory.CreateInstance(None, mod.IDiaDataSource._iid_)
    ds = ctypes.cast(pobj, POINTER(mod.IDiaDataSource))
    return ds

if __name__ == "__main__":
    import sys
    pdb = sys.argv[1] if len(sys.argv) > 1 else r"J:\SteamLibrary\steamapps\common\Vagante\vagante.pdb"
    ds = create_datasource()
    ds.loadDataFromPdb(pdb)
    session = ds.openSession()
    print("PDB loaded. globalScope name:", session.globalScope.name)
    print("loadAddress:", session.loadAddress)
