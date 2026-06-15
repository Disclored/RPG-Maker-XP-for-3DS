/* ============================================================================
 * heap_3ds.c  --  Configuração do split de FCRAM (heap linear vs heap malloc)
 * ----------------------------------------------------------------------------
 * PORQUÊ
 *   O 3DS divide a memória da aplicação (FCRAM) em duas zonas geridas pelo
 *   libctru:
 *     • heap LINEAR  -> linearAlloc()  -> usado pelas TEXTURAS (citro3d/GPU).
 *     • heap MALLOC  -> malloc()/new   -> usado pelo POOL do mruby e por TODOS
 *                                          os b->pixels (cópia RGBA em CPU de
 *                                          cada Bitmap, necessária como fonte
 *                                          de blt) + objetos Ruby.
 *
 *   O diagnóstico [SUPER] provou que, depois dos consertos de dispose/VRAM:
 *     - O heap LINEAR NUNCA esgota: pico de uso ~18,5 MB, com ~12-14 MB sempre
 *       livres (logo o libctru estava a reservar ~30 MB para o linear).
 *     - O heap MALLOC ESGOTOU: "[POOL|GROW] malloc(32768) FALHOU (OOM)" durante
 *       um diálogo longo. O pool do mruby (cresceu a ~1,5M slots) + os pixels
 *       duplicados encheram o malloc, e o GC entrou em thrash (contribui p/ as
 *       quedas de FPS).
 *
 *   Ou seja: o linear está SOBRE-dimensionado e o malloc está SUB-dimensionado.
 *   Esta correção reequilibra: reserva um valor FIXO e generoso para o linear
 *   (texturas) e entrega TODO o resto da FCRAM ao malloc — que é o heap que
 *   realmente faltava memória.
 *
 * COMO
 *   O libctru expõe estes símbolos "weak" que a app pode redefinir. Ao definir
 *   apenas __ctru_linear_heap_size (símbolo forte), o __system_allocateHeaps()
 *   do libctru passa a reservar exatamente esse valor para o heap linear e
 *   atribui automaticamente TODO o restante da região APPLICATION ao heap
 *   malloc (porque __ctru_heap_size fica a 0 = "usa o resto"). Em consolas/
 *   ambientes com memória estendida, o "resto" inclui essa memória extra de
 *   forma transparente — sem precisar de .cia/RSF.
 *
 * SEGURANÇA / ESCOLHA DO VALOR (24 MB linear)
 *   - Pico medido de texturas = 18,5 MB  ->  24 MB dá ~5,5 MB de folga.
 *   - A contagem de texturas estabiliza (o dispose imediato mantém o churn
 *     equilibrado: +82/-75 por intervalo), não cresce sem limite.
 *   - REDE DE SEGURANÇA: se alguma cena futura exceder 24 MB de linear, o
 *     caminho de overflow-para-VRAM (display_3ds.cpp) já desvia texturas
 *     pequenas (<=512 KB) para os 4 MB de VRAM (atualmente a 0), e qualquer
 *     falha fica registada como falhas_tex no log [SUPER] — visível, não fatal.
 *   - Reduzir o linear de ~30 -> 24 MB liberta ~6 MB para o malloc, margem
 *     ampla sobre o ponto de OOM (que falhou num único malloc de 32 KB, ou
 *     seja, estava no limite). Combina com o full_gc no freeze (graphics_
 *     binding.cpp) que baixa o pico de objetos vivos do pool.
 *
 *   Valor ajustável: se o log voltar a mostrar falhas_tex>0 (linear apertado),
 *   subir LINEAR_HEAP_MB; se voltar a haver OOM de malloc, baixá-lo.
 *
 *   Genérico e bom para hardware 3DS real (FCRAM pequena): prioriza dar ao
 *   malloc — onde vive o interpretador Ruby — a maior fatia possível.
 * ============================================================================ */
#include <3ds.h>

/* Reserva fixa para o heap linear (texturas GPU). Resto -> heap malloc. */
#define LINEAR_HEAP_MB 24u

/* Símbolo "weak" do libctru: ao definir um valor forte aqui, o libctru reserva
 * exatamente este tamanho para o heap linear e dá todo o restante ao malloc. */
u32 __ctru_linear_heap_size = LINEAR_HEAP_MB * 1024u * 1024u;

/* NOTA: NÃO definimos __ctru_heap_size de propósito. Deixá-lo a 0 (default do
 * libctru) faz o malloc receber TODA a memória APPLICATION que sobra depois da
 * reserva linear acima — incluindo memória estendida quando disponível. */
