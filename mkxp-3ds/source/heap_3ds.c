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
 *   ATUALIZAÇÃO (pool do mruby DESLIGADO): com USE_POOL_ALLOCATOR=0 o pool
 *   deixou de existir e libertou ~28 MB ao heap malloc (era o pool que enchia o
 *   malloc e causava o OOM acima). Por isso o equilíbrio mudou: o malloc passou
 *   a ter folga ampla e os tilesets GIGANTES (ex.: 'Gen4 Outside Route'
 *   256x29632 = 14,8 MB em 4444) é que deixaram de caber — o buffer 4444 tenta
 *   malloc e depois linearAlloc (bitmap_binding: ambos + GC) e falhava com só
 *   ~10 MB de linear livre. Subimos o linear 24 -> 32 MB: nas cenas de jogo as
 *   texturas usam ~7-14 MB, deixando ~18-25 MB livres no linear -> o buffer de
 *   14,8 MB passa a caber no fallback linear. O malloc perde 8 MB mas, com os
 *   ~28 MB libertados pelo pool, fica ainda com ~+20 MB de margem sobre o antigo
 *   ponto de OOM. Reversível: se o log mostrar OOM de malloc, voltar a 24.
 *
 *   Genérico e bom para hardware 3DS real (FCRAM pequena): prioriza dar ao
 *   malloc — onde vive o interpretador Ruby — a maior fatia possível.
 * ============================================================================ */
#include <3ds.h>

/* Reserva fixa para o heap linear (texturas GPU + buffers de decode de bitmaps
 * gigantes 4444). Resto -> heap malloc. Subido de 24 p/ 32 porque o pool do
 * mruby foi desligado (libertou ~28 MB ao malloc), permitindo dar mais ao linear
 * sem reintroduzir o OOM de malloc.
 *
 * ATUALIZAÇÃO 2 (magenta em mapas com vizinho): mapas como 'Gardenia Town' teem
 * um mapa LIGADO (connected) cujo spriteset carrega o SEU proprio tileset gigante
 * (ex.: 'Gen4 Outside Route' 256x29632 = 15,1 MB) ao mesmo tempo que o do mapa
 * principal ('Gen4 Outside Town' 256x31040 = 15,9 MB). Os DOIS juntos = ~31 MB
 * NAO cabiam nos 32 MB -> expulsavam-se mutuamente -> re-decode do .png falhava
 * por FRAGMENTACAO (log: "need=15520KB linear_livre=16045KB" = ha' espaco total
 * mas nao contiguo) -> tiles magenta. Subir 32 -> 44 deixa os DOIS tilesets
 * gigantes residentes em simultaneo (sem eviccao -> sem re-decode -> sem magenta),
 * com ~9 MB de folga para o resto (sprites + @layer0). O malloc perde 12 MB mas,
 * com os ~28 MB libertados pelo pool, mantem margem sobre o ponto de OOM.
 * Reversível: se o log mostrar OOM de malloc (falha num malloc pequeno), baixar
 * para 40 ou 36. */
#define LINEAR_HEAP_MB 44u

/* Símbolo "weak" do libctru: ao definir um valor forte aqui, o libctru reserva
 * exatamente este tamanho para o heap linear e dá todo o restante ao malloc. */
u32 __ctru_linear_heap_size = LINEAR_HEAP_MB * 1024u * 1024u;

/* NOTA: NÃO definimos __ctru_heap_size de propósito. Deixá-lo a 0 (default do
 * libctru) faz o malloc receber TODA a memória APPLICATION que sobra depois da
 * reserva linear acima — incluindo memória estendida quando disponível. */
