#!/usr/bin/env python3
"""Create a copy of Windows.Foundation.FoundationContract.winmd whose INTERNAL
assembly name is shortened to "Windows.Foundation".

XamlCompiler resolves the bare "Windows.Foundation" assembly ref emitted by
midlrt by matching an assembly whose *internal* name is exactly
"Windows.Foundation". FoundationContract defines precisely the Windows.Foundation
types (IVectorView / IAsyncAction / ...) and none of the Windows.UI.Xaml.* types,
so using it under that name resolves the ref WITHOUT dragging in the union (which
duplicates UniversalApiContract's Windows.UI.* types -> WMC0901).

The name string lives in the #Strings heap. Shortening it in place is safe: we
write the new NUL-terminated string at the same offset and leave the trailing
bytes as unreferenced garbage; no other offset moves.
"""
import os
import shutil
import sys

SRC = r"C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0\Windows.Foundation.FoundationContract\4.0.0.0\Windows.Foundation.FoundationContract.winmd"
DST = r"C:\Users\ted\Desktop\w-music\build\gen\wf2\Windows.Foundation.winmd"

OLD = b"Windows.Foundation.FoundationContract"
NEW = b"Windows.Foundation"

def main() -> int:
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    with open(SRC, "rb") as f:
        data = bytearray(f.read())
    idx = data.find(OLD + b"\x00")
    if idx < 0:
        print("ERROR: old name not found in winmd")
        return 2
    if len(NEW) > len(OLD):
        print("ERROR: new name longer than old")
        return 3
    # write new name + NUL, leave the remaining old bytes untouched (garbage)
    data[idx:idx + len(NEW)] = NEW
    data[idx + len(NEW)] = 0
    with open(DST, "wb") as f:
        f.write(data)
    print(f"patched {SRC}")
    print(f"     -> {DST}  (offset {idx}, '{OLD.decode()}' -> '{NEW.decode()}')")
    return 0

if __name__ == "__main__":
    sys.exit(main())
