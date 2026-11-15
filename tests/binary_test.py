"""Load the actual library and query its Metamod ABI without starting a server."""
import ctypes
import pathlib
import sys

library = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
factory = library.CreateInterface
factory.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
factory.restype = ctypes.c_void_p
status = ctypes.c_int(-1)
plugin = factory(b"ISmmPlugin", ctypes.byref(status))
assert plugin and status.value == 0
vtable = ctypes.cast(plugin, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
api = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)(vtable[0])(plugin)
assert api == 18, api
assert not factory(b"UnsupportedInterface", ctypes.byref(status))
print("Linux library: CreateInterface and Metamod API 18: passed")
