# Zip Imager - Zip Image Creator

Windows program (C++ / Win32 API, no dependencies) to create and edit raw disk images of
Iomega Zip 100, 250 and 750 media (FAT16, optional MBR like real Zip disks).

**Version:** 1.0.0

## Features
- Simple GUI: add files/folders, new folder, rename, delete, capacity bar
- Drag & drop from Windows Explorer
- New / Open / Save / Save As (`.img`, `.ima`, `.dsk`, `.bin`)
- Long file name support, volume label, opens existing FAT16 Zip images

## Build (MinGW)
```
g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ ZipImager.cpp -o ZipImager.exe -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32
```
For Windows 95/98/ME use an old MinGW toolchain (mingw.org, GCC 3.x-4.x).
