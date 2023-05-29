# zsx
Zsx compression

## Configure
install tcc

copy cabinet.def in tcc/lib directory

install fasm

copy cabinet.inc in fasm/include directory

## Compile (Windows 8+)
open sfx.asm with fasmw and compile

    tcc bin2c.c -o bin2c.exe

    bin2c sfx.exe sfx.c sfx

    tcc win32.c -lcabinet -o zsx.exe


## Test (Compress)
    zsx.exe enwik9.1024


## Test (Decompress)
    enwik9.1024.exe
