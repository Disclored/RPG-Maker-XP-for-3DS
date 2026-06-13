#!/bin/bash
set -e
echo "Installing devkitPro 3DS dependencies..."
/opt/devkitpro/msys2/usr/bin/pacman.exe -S --noconfirm \
    3ds-dev \
    3ds-zlib \
    3ds-libpng \
    3ds-libogg \
    3ds-libvorbisidec \
    3ds-freetype
echo "Done."
