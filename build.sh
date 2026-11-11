#!/usr/bin/env bash
# Builds RoCheatsMonitor.exe (64-bit Windows) with MinGW-w64.
#   Debian/Ubuntu:  sudo apt install g++-mingw-w64-x86-64 python3-pil
#   An MSYS2 MINGW64 shell on Windows should also work with CXX=g++ WINDRES=windres (not tried).
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-x86_64-w64-mingw32-g++}"
WINDRES="${WINDRES:-x86_64-w64-mingw32-windres}"

mkdir -p obj
[ -f res/app.ico ] || python3 tools/make_icon.py res/app.ico

(cd res && "$WINDRES" -i app.rc -O coff -o ../obj/app.res.o)

"$CXX" -std=c++17 -O2 -Wall -Wextra \
    -municode -mwindows -static -s \
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
    src/main.cpp obj/app.res.o \
    -o RoCheatsMonitor.exe \
    -lwinhttp -lshell32 -ladvapi32 -luser32 -lgdi32

ls -l RoCheatsMonitor.exe
