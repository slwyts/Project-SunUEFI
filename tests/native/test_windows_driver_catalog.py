"""Check package membership with Windows' SHA-256 catalog hash for each file."""
import ctypes
from ctypes import wintypes
from contextlib import ExitStack
import msvcrt
from pathlib import Path
import sys


def check(catalog, files):
    trust = ctypes.WinDLL('wintrust', use_last_error=True)
    handle, word, flag = wintypes.HANDLE, wintypes.DWORD, wintypes.BOOL
    ptr = ctypes.c_void_p

    def api(name, result, *arguments):
        function = getattr(trust, name)
        function.restype, function.argtypes = result, arguments
        return function

    acquire = api('CryptCATAdminAcquireContext2', flag, ctypes.POINTER(handle), ptr,
                  wintypes.LPCWSTR, ptr, word)
    release = api('CryptCATAdminReleaseContext', flag, handle, word)
    calculate = api('CryptCATAdminCalcHashFromFileHandle2', flag, handle, handle,
                    ctypes.POINTER(word), ptr, word)
    open_catalog = api('CryptCATOpen', handle, wintypes.LPCWSTR, word, ptr, word, word)
    close_catalog = api('CryptCATClose', flag, handle)
    member = api('CryptCATGetMemberInfo', ptr, handle, wintypes.LPCWSTR)

    with ExitStack() as cleanup:
        context = handle()
        if not acquire(ctypes.byref(context), None, 'SHA256', None, 0):
            raise ctypes.WinError(ctypes.get_last_error())
        cleanup.callback(release, context, 0)
        cat = open_catalog(str(catalog.resolve()), 4, None, 0, 0)  # CRYPTCAT_OPEN_EXISTING
        if cat in (None, handle(-1).value):
            raise ctypes.WinError(ctypes.get_last_error())
        cleanup.callback(close_catalog, cat)
        for path in files:
            digest = (ctypes.c_ubyte * 32)()
            length = word(len(digest))
            with path.open('rb') as file:
                # A PE catalog digest excludes fields such as the signature
                # directory. A plain SHA-256 of the SYS file is not equivalent.
                if not calculate(context, msvcrt.get_osfhandle(file.fileno()),
                                 ctypes.byref(length), digest, 0):
                    raise ctypes.WinError(ctypes.get_last_error())
            tag = bytes(digest).hex().upper()
            if length.value != len(digest) or not member(cat, tag):
                raise ValueError(f'{path.name} is not in {catalog.name}: {tag}')
            print(f'{catalog.name}: {path.name} {tag}')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit('Usage: test_windows_driver_catalog.py CATALOG FILE [FILE ...]')
    check(Path(sys.argv[1]), map(Path, sys.argv[2:]))
