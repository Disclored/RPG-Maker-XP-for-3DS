#!/bin/bash
# ===================================================================
# Aplica o patch dos simbolos ao mruby (~/mruby32), recompila para 3DS
# e copia o resultado para o projeto. Corre no MSYS.
# PRE-REQUISITO: poe 'symbol.c' e 'mruby.h' (descarregados) na pasta
#                onde corres este script.
# ===================================================================
set -e
MRUBY="$HOME/mruby32"
PROJ="$HOME/Desktop/Emulator/mkxp-3ds"
HERE="$(pwd)"

# Verificar pre-requisitos
[ -f "$HERE/symbol.c" ] && [ -f "$HERE/mruby.h" ] || { echo "ERRO: poe 'symbol.c' e 'mruby.h' nesta pasta."; exit 1; }
[ -d "$MRUBY" ] || { echo "ERRO: nao existe $MRUBY"; exit 1; }
[ -d "$PROJ" ] || { echo "ERRO: nao existe $PROJ"; exit 1; }

echo "=== [1/5] Backup + aplicar patch em $MRUBY ==="
cp -f "$MRUBY/src/symbol.c"    "$MRUBY/src/symbol.c.orig"
cp -f "$MRUBY/include/mruby.h" "$MRUBY/include/mruby.h.orig"
cp -f "$HERE/symbol.c"  "$MRUBY/src/symbol.c"
cp -f "$HERE/mruby.h"   "$MRUBY/include/mruby.h"
echo ">> patch aplicado (originais guardados em .orig)"

echo "=== [2/5] Detectar a config de build do 3DS ==="
CONFIG="$(grep -rl "nintendo_3ds" "$MRUBY"/*.rb "$MRUBY"/build_config*.rb 2>/dev/null | head -1)"
if [ -n "$CONFIG" ]; then echo ">> config: $CONFIG"; else echo ">> sem config explicita; vou usar a default (build_config.rb)"; fi

echo "=== [3/5] Limpar o build antigo do 3DS (forcar recompilacao com o header novo) ==="
rm -rf "$MRUBY/build/nintendo_3ds"
echo ">> build/nintendo_3ds apagado"

echo "=== [4/5] Recompilar o mruby para 3DS (pode demorar 1-2 min) ==="
cd "$MRUBY"
if [ -n "$CONFIG" ]; then
  MRUBY_CONFIG="$CONFIG" rake
else
  rake
fi

# Confirmar que saiu a lib
NEWLIB="$MRUBY/build/nintendo_3ds/lib/libmruby.a"
[ -f "$NEWLIB" ] || { echo "ERRO: nao foi gerada $NEWLIB. Cola-me o erro do rake acima."; exit 1; }
echo ">> nova libmruby.a gerada: $NEWLIB"

echo "=== [5/5] Copiar lib + header para o projeto ==="
cp -f "$NEWLIB" "$PROJ/libs/lib/libmruby.a"
cp -f "$MRUBY/include/mruby.h" "$PROJ/libs/include/mruby/mruby.h"
echo ">> copiado para $PROJ/libs/"

echo ""
echo "=============================================================="
echo "MRUBY RECOMPILADO E COPIADO. Agora, no projeto:"
echo "  cd $PROJ"
echo "  make clean && make"
echo "=============================================================="
echo "Reverter mruby (se precisares):"
echo "  cp $MRUBY/src/symbol.c.orig $MRUBY/src/symbol.c"
echo "  cp $MRUBY/include/mruby.h.orig $MRUBY/include/mruby.h"
