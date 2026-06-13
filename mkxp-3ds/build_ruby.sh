#!/bin/bash
set -e
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
PREFIX=$DEVKITARM/bin/arm-none-eabi
RUBY_SRC=${RUBY_SRC:-$HOME/joiplay-ruby}
OUT=$HOME/mkxp-3ds/ruby
export PATH="$DEVKITARM/bin:$PATH"
CC="$PREFIX-gcc" ; CXX="$PREFIX-g++" ; AR="$PREFIX-ar" ; RANLIB="$PREFIX-ranlib"
CFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
        -O1 -ffunction-sections -fdata-sections \
        -D__3DS__ -DARM_32BIT -DNO_FORK -DNO_EXEC \
        -I$DEVKITPRO/libctru/include"
cd "$RUBY_SRC"
echo "[1/3] Configuring Ruby 1.8.7 for 3DS..."
./configure \
    --host=arm-none-eabi --target=arm-none-eabi --prefix="$OUT" \
    --disable-shared --enable-static --disable-install-doc \
    --without-tcl --without-tk --without-dbm --without-gdbm \
    --without-readline --without-curses --without-dl \
    CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" \
    CFLAGS="$CFLAGS" LDFLAGS="-L$DEVKITPRO/libctru/lib" \
    ac_cv_func_fork=no ac_cv_func_vfork=no \
    ac_cv_func_popen=no ac_cv_func_pclose=no \
    ac_cv_have_decl_sys_nerr=no ac_cv_sizeof_off_t=4
echo "[2/3] Building libruby.a..."
make -j$(nproc) MAKEFLAGS= libruby-static.a
echo "[3/3] Installing..."
mkdir -p "$OUT/include/ruby" "$OUT/lib"
cp libruby-static.a "$OUT/lib/libruby.a"
cp ruby.h "$OUT/include/"
cp -r .ext/include/arm-none-eabi/ruby "$OUT/include/" 2>/dev/null || true
cp config.h "$OUT/include/ruby/" 2>/dev/null || true
echo "=== Ruby built at: $OUT ==="
