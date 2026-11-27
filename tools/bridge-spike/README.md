# Memory bridge spike

Proof that a Wine builtin DLL with a Unix half loads under Proton Experimental and maps a memfd
over Wine-reserved address space (2026-11-26: 1.50 µs per map+unmap under Proton, 1.63 µs under
system Wine 11.22). See docs/memory-bridge-plan.md.

    gcc -O2 -shared -fPIC -o bridge.so bridge_unix.c
    x86_64-w64-mingw32-gcc -O2 -shared -o bridge.dll bridge_pe.c && winebuild --builtin bridge.dll
    x86_64-w64-mingw32-gcc -O2 -o load.exe load.c
    # bridge.dll and bridge.so side by side with load.exe, then under Proton's wine:
    WINEPREFIX=... "$PROTON/files/bin/wine" ./load.exe
