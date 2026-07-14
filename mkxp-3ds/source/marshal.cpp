/*
** marshal.cpp — 3DS port, mruby 3.2, no SDL, no funopen
*/
#include "marshal.h"
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/class.h>
#include <mruby/error.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <3ds.h>   /* [MARSHAL-LINEAR] linearAlloc/linearFree p/ o buffer de ficheiro */
#include "profile_3ds.h"

/* ---- logging partilhado com binding_3ds.cpp ---------------------- */
/* g_dbglog e o FILE* aberto em binding_3ds.cpp (sdmc:/mkxp/debug_binding.log).
   printf sozinho NAO chega ao ficheiro de log; escrevemos tambem nele.
   dbglog_stamp() (definido em binding_3ds.cpp) devolve o tempo total/delta para
   o prefixo [t=...s d=...ms], igual ao DBGLOG -> linhas [marshal] tambem
   ficam com timestamp e da' para medir quanto cada load demorou. */
extern FILE *g_dbglog;
extern void  dbglog_stamp(double *out_total_s, double *out_delta_ms);
extern bool  g_log_verbose;   /* [LIMPEZA] espelha $MKXP_VERBOSE */
/* [LIMPEZA 2026-07-05] as linhas por-mapa (Table reconstruido/load OK/dump->port)
 * enchem o log a cada carregamento (SAVE-REPAIR carrega dezenas). Suprimidas salvo
 * $MKXP_VERBOSE. ERROS e NESTED-AUTODEF passam sempre. */
static inline bool mz_suppressed(const char *fmt){
    if (g_log_verbose || !fmt) return false;
    static const char *BL[] = {
        "[marshal] Table", "[marshal] load OK", "[marshal] dump->port",
        "[PROFILE|marshal]", 0
    };
    for (int i=0; BL[i]; i++) if (strstr(fmt, BL[i])) return true;
    return false;
}
#define MZLOG(fmt, ...) do { \
        if (mz_suppressed(fmt)) break; \
        double __ts_t, __ts_d; dbglog_stamp(&__ts_t, &__ts_d); \
        printf("[t=%8.3fs d=%7.1fms] " fmt, __ts_t, __ts_d, ##__VA_ARGS__); \
        if (g_dbglog) { fprintf(g_dbglog, "[t=%8.3fs d=%7.1fms] " fmt, __ts_t, __ts_d, ##__VA_ARGS__); fflush(g_dbglog); } \
    } while(0)

/* contadores de diagnostico, reiniciados a cada load completo */
static int s_tbl_built = 0;
static int s_udef_seen = 0;

/* ============================================================================
 * CACHE DE RESOLUCAO DE CLASSE — a correcao do gargalo dos 219s.
 * ----------------------------------------------------------------------------
 * O profiling provou que class_from_path() consumia 219 dos 220 segundos do
 * CommonEvents (e ~97% do tempo de cada mapa). Causa: cada um dos 113 mil
 * objetos obrigava a resolver a sua classe via mrb_const_get + mrb_intern, e
 * no 3DS isso e' patologicamente lento (~1.9ms cada, provavelmente por cair no
 * const_missing custom do Essentials). Como quase todos os objetos sao da MESMA
 * classe (ex: RPG::EventCommand), resolver repetidamente e' desperdicio puro.
 *
 * SOLUCAO: cache nome-da-classe -> RClass*. Resolve cada classe UMA vez; as
 * 113 mil chamadas seguintes sao lookups O(1) num std::unordered_map.
 *
 * IMPORTANTE — isto e' GENERICO, nao especifico do Pokemon: a chave e' o nome
 * lido do PROPRIO ficheiro marshal (RPG::Map, RPG::Actor, RPG::Skill, RPG::Enemy,
 * RPG::Troop, GameData::X, etc). Acelera QUALQUER jogo RPG Maker XP, porque TODOS
 * serializam as mesmas classes RPG::* repetidas milhares de vezes.
 *
 * SEGURANCA (emulador): o cache guarda ponteiros RClass* que so' sao validos
 * dentro de UMA sessao mruby. Se o mruby for recriado (reiniciar/trocar de jogo),
 * os ponteiros antigos ficam invalidos. Defesa dupla:
 *   1. Owner-check: o cache esta' ligado a um mrb_state*; se mudar, limpa-se.
 *   2. Validacao de tipo: antes de devolver um hit, confirma que o ponteiro
 *      ainda aponta para uma classe/modulo valido (MRB_TT_CLASS/MODULE/SCLASS).
 *      Se a memoria foi reutilizada por outra coisa (ex: close+open no mesmo
 *      endereco), o tipo nao bate -> re-resolve em vez de devolver lixo.
 * As duas juntas tornam o cache seguro mesmo que um dia o emulador reinicie o
 * mruby sem fechar a aplicacao. */
static std::unordered_map<std::string, struct RClass*> s_class_cache;
static mrb_state *s_class_cache_owner = 0;

/* Simbolos das ivars de Table, pre-computados uma vez por sessao (ver
 * build_table_from_dump). Em escopo de ficheiro para o reset os limpar quando o
 * mruby e' recriado -- senao um mrb_sym de uma sessao antiga poderia nao bater
 * na nova (a symbol table e' reconstruida). */
static mrb_sym s_tbl_sym_x=0, s_tbl_sym_y=0, s_tbl_sym_z=0, s_tbl_sym_data=0;

/* Chamavel do exterior (binding) para limpar o cache ao (re)criar o mruby. */
extern "C" void marshal_class_cache_reset(void){
    s_class_cache.clear();
    s_class_cache_owner = 0;
    s_tbl_sym_x = s_tbl_sym_y = s_tbl_sym_z = s_tbl_sym_data = 0;  /* re-internar na nova sessao */
}

static inline void class_cache_check_owner(mrb_state *mrb){
    if (s_class_cache_owner != mrb){
        s_class_cache.clear();          /* mruby diferente -> ponteiros antigos invalidos */
        s_class_cache_owner = mrb;
    }
}

/* Confirma que um RClass* em cache ainda e' uma classe/modulo valido. Defesa
 * contra reutilizacao de memoria entre sessoes. */
static inline bool class_ptr_still_valid(struct RClass *c){
    if (!c) return false;
    enum mrb_vtype tt = (enum mrb_vtype)((struct RBasic*)c)->tt;
    return tt == MRB_TT_CLASS || tt == MRB_TT_MODULE || tt == MRB_TT_SCLASS;
}
/* Posto a 1 por write_value quando encontra um tipo que nao sabe serializar
 * (MRB_TT_DATA, etc). marshalDumpInt devolve-o para o cache decidir NAO gravar
 * um ficheiro que nao faz round-trip (senao gravava lixo -> EOF ao reler). */
static int s_dump_incomplete = 0;

/* ---- Exception --------------------------------------------------- */
struct MarshalException {
    enum Type { ArgumentError, TypeError, IOError, MKXPError };
    Type type; char msg[256];
    MarshalException(Type t, const char *fmt, ...) : type(t) {
        va_list ap; va_start(ap,fmt); vsnprintf(msg,sizeof(msg),fmt,ap); va_end(ap);
    }
};
static void raiseMrbExc(mrb_state *mrb, const MarshalException &e) {
    const char *n;
    switch(e.type){
        case MarshalException::ArgumentError: n="ArgumentError"; break;
        case MarshalException::TypeError:     n="TypeError";     break;
        case MarshalException::IOError:       n="IOError";       break;
        default:                              n="RuntimeError";  break;
    }
    mrb_raise(mrb, mrb_class_get(mrb,n), e.msg);
}

/* ---- Growable write buffer --------------------------------------- */
struct WriteBuf {
    std::vector<char> data;
    void writeByte(int8_t b)               { data.push_back(b); }
    void writeData(const char *p, int len) { data.insert(data.end(),p,p+len); }
};

/* ---- Read buffer (const view) ------------------------------------ */
struct ReadBuf {
    const char *data; size_t size; size_t pos;
    ReadBuf(const char *d, size_t s) : data(d), size(s), pos(0) {}
    int8_t readByte() {
        if (pos >= size) throw MarshalException(MarshalException::ArgumentError,"dump format error (EOF)");
        return (int8_t)(unsigned char)data[pos++];
    }
    void readData(char *dest, int len) {
        if (pos+len > size) throw MarshalException(MarshalException::ArgumentError,"dump format error (EOF)");
        memcpy(dest, data+pos, len); pos += len;
    }
};

/* ---- mrb_value hash/eq ------------------------------------------ */
struct MrbVHash {
    size_t operator()(mrb_value v) const {
        return std::hash<uintptr_t>()((uintptr_t)mrb_ptr(v));
    }
};
struct MrbVEq {
    bool operator()(mrb_value a, mrb_value b) const {
        if (mrb_type(a)!=mrb_type(b)) return false;
        switch(mrb_type(a)){
            case MRB_TT_TRUE:    return true;
            case MRB_TT_FALSE:
            case MRB_TT_INTEGER: return mrb_integer(a)==mrb_integer(b);
            case MRB_TT_SYMBOL:  return mrb_symbol(a)==mrb_symbol(b);
            case MRB_TT_FLOAT:   return mrb_float(a)==mrb_float(b);
            default:             return mrb_ptr(a)==mrb_ptr(b);
        }
    }
};

template<typename T, typename H=std::hash<T>>
struct LinkBuf {
    std::unordered_map<T,int,H> map; std::vector<T> vec;
    bool contains(T v)    const { return map.count(v)>0; }
    bool containsIdx(size_t i)  { return i<vec.size(); }
    int  lookupIdx(T v)         { return map[v]; }
    T    lookup(size_t i)       { return vec[i]; }
    int  add(T v){ int i=(int)vec.size(); map[v]=i; vec.push_back(v); return i; }
};
struct ObjLinkBuf {
    std::unordered_map<mrb_value,int,MrbVHash,MrbVEq> map; std::vector<mrb_value> vec;
    bool contains(mrb_value v) const { return map.count(v)>0; }
    bool containsIdx(size_t i)       { return i<vec.size(); }
    int  lookupIdx(mrb_value v)      { return map[v]; }
    mrb_value lookup(size_t i)       { return vec[i]; }
    int  add(mrb_value v){ int i=(int)vec.size(); map[v]=i; vec.push_back(v); return i; }
    /* [UCLASS] troca o ULTIMO slot (a base recem-registada) pelo objeto promovido,
     * para TYPE_LINKs subsequentes apontarem a subclasse e nao a base descartada. */
    void replace_last(mrb_value oldv, mrb_value newv){
        if(!vec.empty() && MrbVEq()(vec.back(), oldv)){
            int i=(int)vec.size()-1;
            map.erase(oldv); map[newv]=i; vec[i]=newv;
        }
    }
};

/* ---- Type tags --------------------------------------------------- */
#define MARSHAL_MAJOR 4
#define MARSHAL_MINOR 8
#define TYPE_NIL     '0'
#define TYPE_TRUE    'T'
#define TYPE_FALSE   'F'
#define TYPE_FIXNUM  'i'
#define TYPE_OBJECT  'o'
#define TYPE_USERDEF 'u'
#define TYPE_FLOAT   'f'
#define TYPE_STRING  '"'
#define TYPE_ARRAY   '['
#define TYPE_HASH    '{'
#define TYPE_IVAR    'I'
#define TYPE_LINK    '@'
#define TYPE_SYMBOL  ':'
#define TYPE_SYMLINK ';'
#define TYPE_UCLASS  'C'   /* [UCLASS 2026-07-02] subclasse de String/Array/Hash (PkmnAnimations) */

static char gpbuf[512];

/* ---- Contexts ---------------------------------------------------- */
struct WriteCtx {
    WriteBuf       *wb;
    mrb_state      *mrb;
    mrb_int         limit;
    LinkBuf<mrb_sym> symbols;
    ObjLinkBuf       objects;
    void writeByte(int8_t b)               { wb->writeByte(b); }
    void writeData(const char *p, int len) { wb->writeData(p,len); }
};
struct ReadCtx {
    ReadBuf        *rb;
    mrb_state      *mrb;
    LinkBuf<mrb_sym> symbols;
    ObjLinkBuf       objects;
    int8_t readByte()              { return rb->readByte(); }
    void readData(char *d, int n)  { rb->readData(d,n); }
};

/* ====== READ SIDE ================================================= */
static int read_fixnum(ReadCtx *c){
    int8_t h=c->readByte();
    if(h==0) return 0;
    if(h>5) return h-5;
    if(h<-4) return h+5;
    bool pos=(h>0); int8_t len=pos?h:(int8_t)(-h);
    int8_t fill=pos?0:(int8_t)0xFF;
    int8_t n1=0,n2=fill,n3=fill,n4=fill;
    n1=c->readByte();
    if(len>=2){ n2=c->readByte(); }
    if(len>=3){ n3=c->readByte(); }
    if(len>=4){ n4=c->readByte(); }
    return (uint8_t)n1|((uint8_t)n2<<8)|((uint8_t)n3<<16)|((uint8_t)n4<<24);
}
/* [SANITY 2026-07-02] comprimentos lidos do stream teem de caber no que RESTA do
 * ficheiro. Sem isto, um stream desalinhado (ex: type-tag desconhecido engolido)
 * produzia um len gigante -> mrb_malloc enorme -> raise 'allocation failure' via
 * longjmp que SALTAVA o catch C++ (o 'Animations: allocation failure' do log).
 * Com o guard, vira MarshalException limpa com posicao exata. */
static void check_len(ReadCtx *c, long n, const char *what){
    long rem = (long)(c->rb->size - c->rb->pos);
    if(n < 0 || n > rem)
        throw MarshalException(MarshalException::MKXPError,
            "%s: length insano %ld (restam %ld bytes) -- stream desalinhado", what, n, rem);
}
static float read_float(ReadCtx *c){
    int len=read_fixnum(c); check_len(c,len,"float");
    if(len>(int)sizeof(gpbuf)-1) throw MarshalException(MarshalException::MKXPError,"float: len>%d",(int)sizeof(gpbuf)-1);
    c->readData(gpbuf,len); gpbuf[len]='\0';
    return strtof(gpbuf,0);
}
static char *read_str_raw(ReadCtx *c){
    int len=read_fixnum(c); check_len(c,len,"str_raw");
    if(len>(int)sizeof(gpbuf)-1) throw MarshalException(MarshalException::MKXPError,"str_raw: len>%d",(int)sizeof(gpbuf)-1);
    c->readData(gpbuf,len); gpbuf[len]='\0'; return gpbuf;
}
static mrb_value read_str_val(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    check_len(c,len,"string");
    PROF_BEGIN(PROF_MARSHAL_STRING);
    mrb_value s=mrb_str_new(mrb,0,len);
    c->readData(RSTR_PTR(RSTRING(s)),len); RSTR_PTR(RSTRING(s))[len]='\0';
    PROF_END(PROF_MARSHAL_STRING);
    return s;
}
/* Lê string sem registar no object pool — usado por read_ivar que regista o slot */
static mrb_value read_str_val_notrack(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    check_len(c,len,"string(ivar)");
    mrb_value s=mrb_str_new(mrb,0,len);
    c->readData(RSTR_PTR(RSTRING(s)),len); RSTR_PTR(RSTRING(s))[len]='\0';
    return s;
}
static mrb_value read_value(ReadCtx *c);
static struct RClass *class_from_path(mrb_state *mrb, mrb_value path);   /* [UCLASS] fwd */
static mrb_value read_userdef(ReadCtx *c);                               /* [IU-USERDEF] fwd */
static mrb_value read_array(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    check_len(c,len,"array");   /* cada elemento ocupa >=1 byte */
    mrb_value a=mrb_ary_new_capa(mrb,len); c->objects.add(a);
    for(int i=0;i<len;i++) mrb_ary_set(mrb,a,i,read_value(c));
    return a;
}
static mrb_value read_hash(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    check_len(c,len,"hash");
    mrb_value h=mrb_hash_new_capa(mrb,len); c->objects.add(h);
    for(int i=0;i<len;i++){
        mrb_value k=read_value(c), v=read_value(c); mrb_hash_set(mrb,h,k,v);
    }
    return h;
}
/* [UCLASS 2026-07-02] TYPE_UCLASS 'C': instancia de SUBCLASSE de String/Array/Hash
 * (ex: PBAnimations < Array em PkmnAnimations.rxdata -- o load falhava com
 * "IVAR: inner type desconhecido 'C'" e $data_animations/BattleAnims ficavam nil,
 * o que por sua vez fazia chegar um Integer a Sprite#bitmap= ([BMP|UNWRAP])).
 * Layout CRuby: 'C' <symbol classe> <valor base>. Estrategia: ler a base e TENTAR
 * promover para a subclasse via Classe.new + concat/merge!/replace; se a classe
 * nao existir ou o initialize exigir args, devolve a BASE (Array/Hash/String) --
 * suficiente para consumidores que so' iteram. Pool: a base regista-se a si
 * propria (read_array/read_hash/read_str_val fazem add) tal como no CRuby o
 * objeto UCLASS ocupa 1 slot. */
static mrb_value read_uclass(ReadCtx *c){
    mrb_state *mrb=c->mrb;
    mrb_value klass_path=read_value(c);            /* symbol ou symlink */
    int8_t base_t=c->readByte();
    mrb_value base;
    switch(base_t){
        case TYPE_ARRAY:  base=read_array(c);   break;
        case TYPE_HASH:   base=read_hash(c);    break;
        case TYPE_STRING: base=read_str_val(c); c->objects.add(base); break;
        default:
            throw MarshalException(MarshalException::MKXPError,
                "UCLASS: base type inesperado '\\x%02x'", (unsigned char)base_t);
    }
    /* [UCLASS] NAO promovemos para a subclasse. Tentar mrb_obj_new(subclasse) era
     * PERIGOSO: se o initialize da subclasse (ou um ivar da subclasse-de-Array)
     * rebentar com "cannot set instance variable" (MRB_TT_ARRAY nao tem iv_tbl
     * neste mruby), o raise faz LONGJMP e salta este codigo C++ -> o erro escapa
     * ate' ao load do jogo (provado: BattleAnims 'cannot set instance variable').
     * A base (Array/Hash/String) ja' contem TODOS os dados; os consumidores de
     * PkmnAnimations iteram a colecao, por isso a base crua chega. (klass_path e'
     * lido e descartado -> so' perdemos o TIPO exato da subclasse, nunca os dados.) */
    (void)klass_path;
    return base;
}
static mrb_sym read_symbol(ReadCtx *c){
    char *raw=read_str_raw(c);
    PROF_BEGIN(PROF_MARSHAL_SYMBOL);
    mrb_sym s=mrb_intern_cstr(c->mrb,raw);
    PROF_END(PROF_MARSHAL_SYMBOL);
    c->symbols.add(s); return s;
}
static mrb_sym read_symlink(ReadCtx *c){
    int i=read_fixnum(c);
    if(!c->symbols.containsIdx(i)) throw MarshalException(MarshalException::ArgumentError,"bad symlink");
    return c->symbols.lookup(i);
}
static mrb_value read_link(ReadCtx *c){
    int i=read_fixnum(c);
    if(!c->objects.containsIdx(i)) throw MarshalException(MarshalException::ArgumentError,"unlinked");
    return c->objects.lookup(i);
}
static mrb_value read_ivar(ReadCtx *c){
    /* O CRuby regista o IVAR wrapper como object link, nao o objecto interno.
       Por isso lemos o inner sem registar e registamos aqui, EXCEPTO para
       tipos que ja se auto-registam no pool (ARRAY, HASH) -- nesses casos
       nao chamamos c->objects.add() uma segunda vez ou o pool fica deslocado.
       Os ivars internos sao frequentemente de encoding (:E, :encoding) -- ignorados. */
    int8_t inner_type=c->readByte();
    mrb_value obj;
    bool already_tracked=false;

    if(inner_type==TYPE_STRING){
        /* read_str_val_notrack nao regista -- nos registamos abaixo */
        obj=read_str_val_notrack(c);
    } else if(inner_type==TYPE_ARRAY){
        /* read_array ja chama c->objects.add() internamente */
        obj=read_array(c);
        already_tracked=true;
    } else if(inner_type==TYPE_HASH){
        /* read_hash ja chama c->objects.add() internamente */
        obj=read_hash(c);
        already_tracked=true;
    } else if(inner_type==TYPE_UCLASS){
        /* [UCLASS] IVAR(UCLASS(...)) e' exatamente o topo do PkmnAnimations.rxdata
         * (I C :PBAnimations [...]). read_uclass regista no pool internamente. */
        obj=read_uclass(c);
        already_tracked=true;
    } else if(inner_type==TYPE_USERDEF){
        /* [IU-USERDEF 2026-07-08] 'Iu' = IVAR(USERDEF): o messages.dat (2.9MB, a
         * fonte de TODOS os nomes de item/move/especie) tem 400x
         *   I u :OrderedHash <payload> ivars{ :@keys => [...] }
         * (CRuby anexa o ivar @keys ao resultado do _load). Sem este ramo, o 1o
         * OrderedHash lancava "IVAR: inner type desconhecido 'u'" -> o load inteiro
         * abortava -> @@messages vazio -> pbGetMessage=0 -> nomes "0" + cascata de
         * erros de tipo na batalha. Ordem de pool = CRuby: o resultado do _load
         * regista 1 slot (abaixo, via !already_tracked); os valores dos ivars
         * registam-se depois no loop de descarte (read_value f-los-a). O @keys
         * descartado e' redundante: OrderedHash._load ja' preenche @keys via []=. */
        obj=read_userdef(c);
    } else {
        switch(inner_type){
            case TYPE_NIL:    obj=mrb_nil_value();  break;
            case TYPE_TRUE:   obj=mrb_true_value(); break;
            case TYPE_FALSE:  obj=mrb_false_value(); break;
            case TYPE_FIXNUM: obj=mrb_int_value(c->mrb,read_fixnum(c)); break;
            default:
                /* Tipo genuinamente desconhecido dentro de IVAR -- nao ha forma
                   segura de saltar os bytes sem conhecer o layout. Abortar para
                   evitar deslocamento de stream silencioso. */
                throw MarshalException(MarshalException::MKXPError,
                    "IVAR: inner type desconhecido '\\x%02x'", (unsigned char)inner_type);
        }
    }

    if(!already_tracked) c->objects.add(obj);

    int n=read_fixnum(c);
    /* [IVAR-SANITY2 2026-07-11] o read_object ja' guardava a sua contagem de
     * ivars; este read_ivar (tag 'I') NAO -> um n gigante por desync criava
     * milhoes de objetos de lixo no loop ate' o check_len de um read aninhado
     * disparar (candidato ao churn de 1.5M). Cada par ocupa >=2 bytes. */
    if(n < 0 || (long)n*2 > (long)(c->rb->size - c->rb->pos))
        throw MarshalException(MarshalException::MKXPError,
            "IVAR: ivar-count insano %d (restam %ld bytes) -- stream desalinhado",
            n, (long)(c->rb->size - c->rb->pos));
    for(int i=0;i<n;i++){
        /* ler e descartar -- normalmente :E => true (encoding UTF-8) */
        read_value(c); read_value(c);
    }
    return obj;
}
static struct RClass *class_from_path(mrb_state *mrb, mrb_value path){
    char *s;
    if(mrb_symbol_p(path)) s=(char*)mrb_sym_name(mrb,mrb_symbol(path));
    else if(mrb_string_p(path)) s=(char*)RSTRING_PTR(path);
    else throw MarshalException(MarshalException::ArgumentError,"bad class path");
    if(s[0]=='"') s++;

    /* CACHE: o nome da classe (ex: "RPG::EventCommand") repete-se em milhares de
     * objetos. Resolver uma vez e reutilizar evita os ~1.9ms/obj do const_get no
     * 3DS. Construir a chave ate' ao terminador ('"' ou fim). */
    class_cache_check_owner(mrb);
    const char *name_end = s;
    while(*name_end && *name_end!='"') name_end++;
    std::string key(s, (size_t)(name_end - s));
    auto it = s_class_cache.find(key);
    if(it != s_class_cache.end() && class_ptr_still_valid(it->second))
        return it->second;   /* HIT valido: O(1), sem const_get */

    /* MISS: resolver via const_get (caro no 3DS, mas so' 1x por classe). */
    char *p=s,*b=s;
    struct RClass *outer = mrb->object_class;      /* namespace corrente */
    mrb_value klass = mrb_obj_value(outer);
    while(*p&&*p!='"'){
        while(*p&&*p!=':'&&*p!='"') p++;
        mrb_sym csym = mrb_intern(mrb,b,(size_t)(p-b));
        mrb_value seg = mrb_nil_value();
        /* [NESTED-AUTODEF 2026-07-05] RAIZ do "allocation failure" de System(10KB)
         * e Animations(43KB): compat_stubs define RPG::System e RPG::AudioFile mas
         * NAO as classes ANINHADAS (RPG::System::Words/TestBattler,
         * RPG::Animation::Frame/Timing). mrb_const_get numa aninhada inexistente caia
         * no const_missing do probe -> devolvia um valor NAO-classe -> mrb_obj_ptr ->
         * mrb_obj_alloc(klass) com cls->tt invalido -> raise "allocation failure" pura
         * (gc.c:561), abortando o load inteiro (ficava $data_system/$data_animations
         * nil). Fix: usar mrb_const_defined (NAO dispara const_missing) e, se a
         * aninhada nao existir, DEFINI-LA vazia sob o namespace atual -- os scripts
         * ja' fazem RPG::Animation::Timing.new a contar que exista. */
        if(mrb_const_defined(mrb, klass, csym)){
            seg = mrb_const_get(mrb, klass, csym);
        }
        enum mrb_vtype st = (enum mrb_vtype)mrb_type(seg);
        if(st != MRB_TT_CLASS && st != MRB_TT_MODULE && st != MRB_TT_SCLASS){
            /* inexistente OU nao-classe: cria classe vazia sob 'outer' (se ja' existia
             * como nao-classe, cai para Object para nao rebentar no define). */
            if(mrb_const_defined(mrb, klass, csym)){
                seg = mrb_obj_value(mrb->object_class);   /* raro: const existe mas nao e' classe */
            }else{
                std::string leaf(b,(size_t)(p-b));
                struct RClass *nc = mrb_define_class_under(mrb, outer, leaf.c_str(), mrb->object_class);
                MZLOG("[marshal] NESTED-AUTODEF: classe aninhada '%.*s' nao existia -> definida vazia sob namespace (evita 'allocation failure')\n",
                      (int)(p-b), b);
                seg = mrb_obj_value(nc);
            }
        }
        klass = seg;
        outer = mrb_class_ptr(klass);
        if(p[0]==':'){ if(p[1]!=':') return 0; p+=2; b=p; }
    }
    struct RClass *result=(struct RClass*)mrb_obj_ptr(klass);
    s_class_cache[key]=result;   /* guardar para as proximas milhares de vezes */
    return result;
}
static mrb_value read_object(ReadCtx *c){
    mrb_state *mrb=c->mrb;
    mrb_value class_path=read_value(c);
    PROF_BEGIN(PROF_MARSHAL_CLASS);
    struct RClass *klass=class_from_path(mrb,class_path);
    PROF_END(PROF_MARSHAL_CLASS);
    if(!klass){
        /* [SAVE-CLASS-BUG 2026-07-03] Classe nao resolvida -> fallback Object =
         * objeto com ivars certos mas SEM metodos (Game_Map.map_id -> method_missing
         * -> 0 -> mapa 0 no load, ecra preto). O aviso ia p/ printf (consola,
         * INVISIVEL). Agora vai para o LOG com a classe exata que falha. */
        const char *cname="<unknown>";
        if(mrb_symbol_p(class_path))      cname=mrb_sym_name(mrb,mrb_symbol(class_path));
        else if(mrb_string_p(class_path)) cname=RSTRING_PTR(class_path);
        MZLOG("[Marshal] CLASSE-NAO-RESOLVIDA: '%s' -> Object (metodos partidos! raiz do load preto)\n", cname);
        klass=mrb->object_class;
    }
    PROF_BEGIN(PROF_MARSHAL_ALLOC);
    /* [ALLOC-DIAG 2026-07-05] mrb_obj_alloc(MRB_TT_OBJECT, klass) levanta o mruby
     * "allocation failure" quando a classe tem instance_tt != OBJECT (classe nativa
     * MRB_TT_DATA/etc). Foi o que abateu Animations(43KB)+System(10KB) enquanto
     * ficheiros de 1.8MB carregam OK -> NAO era memoria, era a classe. Logamos o
     * nome+tt+posicao exatos e caimos para Object (ivars certos, sem crash) em vez
     * de rebentar o load inteiro (deixava $data_system/$data_animations a nil). */
    {
        enum mrb_vtype _itt = (enum mrb_vtype)MRB_INSTANCE_TT(klass);
        if(_itt != MRB_TT_OBJECT && _itt != MRB_TT_FALSE){
            const char *cn = mrb_class_name(mrb,klass);
            MZLOG("[marshal] ALLOC-NATIVE-TT: classe '%s' instance_tt=%d (nao OBJECT) @pos=%d/%d -> fallback Object (era o 'allocation failure')\n",
                  cn?cn:"<?>", (int)_itt, (int)c->rb->pos, (int)c->rb->size);
            klass = mrb->object_class;
        }
    }
    mrb_value obj=mrb_obj_value(mrb_obj_alloc(mrb,MRB_TT_OBJECT,klass));
    PROF_END(PROF_MARSHAL_ALLOC);
    c->objects.add(obj); int n=read_fixnum(c);
    /* [IVAR-SANITY] contagem de ivars tem de caber no que resta (cada par ocupa
     * >=2 bytes); um n gigante aqui denuncia um desync antes do alloc. */
    if(n < 0 || (long)n*2 > (long)(c->rb->size - c->rb->pos))
        throw MarshalException(MarshalException::MKXPError,
            "objeto '%s': ivar-count insano %d (restam %ld bytes) -- stream desalinhado",
            (mrb_class_name(mrb,klass)?mrb_class_name(mrb,klass):"<?>"), n,
            (long)(c->rb->size - c->rb->pos));
    for(int i=0;i<n;i++){
        mrb_value nm=read_value(c), vl=read_value(c);
        if(mrb_symbol_p(nm)){
            PROF_BEGIN(PROF_MARSHAL_IVSET);
            mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_symbol(nm),vl);
            PROF_END(PROF_MARSHAL_IVSET);
        }
    }
    return obj;
}
/* ---- helpers para ler inteiros little-endian de um buffer cru ------ */
static int32_t le_i32(const unsigned char *p){
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1]<<8) |
                     ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24));
}
static int16_t le_i16(const unsigned char *p){
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8));
}
static double le_f64(const unsigned char *p){
    uint64_t b =  (uint64_t)p[0]        | ((uint64_t)p[1]<<8)  |
                 ((uint64_t)p[2]<<16)   | ((uint64_t)p[3]<<24) |
                 ((uint64_t)p[4]<<32)   | ((uint64_t)p[5]<<40) |
                 ((uint64_t)p[6]<<48)   | ((uint64_t)p[7]<<56);
    double d; memcpy(&d,&b,sizeof(d)); return d;
}

/*
** RPG::Table — reconstrucao nativa a partir do dump userdef do RGSS.
**
** Em CRuby/RGSS, Table e serializado via Table#_dump como uma string binaria:
**   [dim, xsize, ysize, zsize, total]  (5 x int32 little-endian)
**   seguido de `total` celulas (int16 little-endian cada).
**
** O mruby do 3DS NAO define Table._load, por isso o caminho normal de
** userdef (mrb_funcall ..._load) lancava TypeError ("class needs '_load'"),
** fazendo load_data falhar e o jogo cair em RPG::Tileset/Map vazios -> ecra
** preto. Aqui construimos o objecto Table directamente, preenchendo os ivars
** (@x,@y,@z,@data) tal como compat_stubs.h os espera. @data e um Array plano
** indexado por  z*@x*@y + y*@x + x  -- exactamente a ordem do stream RGSS
** (x varia primeiro, depois y, depois z), por isso basta empilhar por ordem.
*/
static mrb_value build_table_from_dump(mrb_state *mrb, struct RClass *klass, mrb_value data){
    const unsigned char *p=(const unsigned char*)RSTRING_PTR(data);
    mrb_int len=RSTRING_LEN(data);
    if(len<20) throw MarshalException(MarshalException::ArgumentError,
        "Table._load: dump demasiado curto (%d bytes)",(int)len);

    int32_t xsize=le_i32(p+4);
    int32_t ysize=le_i32(p+8);
    int32_t zsize=le_i32(p+12);
    int32_t total=le_i32(p+16);

    /* clamp defensivo: as dimensoes nunca devem ser <1 (compat_stubs faz o mesmo) */
    if(xsize<1) xsize=1;
    if(ysize<1) ysize=1;
    if(zsize<1) zsize=1;
    if(total<0) total=0;

    /* numero de celulas realmente presentes no buffer (int16 cada, a partir do offset 20) */
    mrb_int avail=(len-20)/2;
    if((mrb_int)total>avail) total=(int32_t)avail;

    mrb_value obj=mrb_obj_value(mrb_obj_alloc(mrb,MRB_TT_OBJECT,klass));
    int arena=mrb_gc_arena_save(mrb);
    mrb_value arr=mrb_ary_new_capa(mrb,total>0?total:1);
    const unsigned char *cell=p+20;
    /* ROBUSTEZ V31: para Tables GRANDES (ex.: passabilidade de tilesets gigantes,
     * xsize 8144), o preenchimento celula-a-celula com mrb_ary_set podia deixar o
     * array com comprimento/valores errados no runtime do 3DS (perdiamos as
     * passabilidades/prioridades de IDs altos -> andar nas paredes + camadas
     * trocadas). Aqui garantimos o comprimento com mrb_ary_resize UMA vez e depois
     * escrevemos DIRETAMENTE no buffer (RARRAY_PTR): zero GC durante o fill e zero
     * realocacao, por isso o ponteiro mantem-se valido. Inteiros sao imediatos
     * (WORD_BOXING) -> nao precisam de write-barrier. */
    if(total>0){
        mrb_ary_resize(mrb,arr,total);
        mrb_value *ap=RARRAY_PTR(arr);
        for(int32_t i=0;i<total;i++){
            int16_t v=le_i16(cell); cell+=2;
            ap[i]=mrb_int_value(mrb,(mrb_int)v);
        }
    }
    /* Simbolos das ivars pre-computados UMA vez por sessao (ver declaracao em
     * escopo de ficheiro + reset em marshal_class_cache_reset). build_table_from_dump
     * e' chamado centenas de vezes (Animations.rxdata: 667 tables); evitar
     * re-internar "@x"/"@y"/"@z"/"@data" a cada chamada poupa milhares de
     * mrb_intern no 3DS. */
    if(!s_tbl_sym_x){
        s_tbl_sym_x   =mrb_intern_lit(mrb,"@x");
        s_tbl_sym_y   =mrb_intern_lit(mrb,"@y");
        s_tbl_sym_z   =mrb_intern_lit(mrb,"@z");
        s_tbl_sym_data=mrb_intern_lit(mrb,"@data");
    }
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),s_tbl_sym_x,   mrb_int_value(mrb,xsize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),s_tbl_sym_y,   mrb_int_value(mrb,ysize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),s_tbl_sym_z,   mrb_int_value(mrb,zsize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),s_tbl_sym_data,arr);
    mrb_gc_arena_restore(mrb,arena);
    s_tbl_built++;
    /* [LOG-LEAN] era `<=3 || total>4096` -> os mapas tem dezenas de Tables >4096
     * cells, todas logadas por-tabela (flood de boot). So' as 2 primeiras. */
    if(s_tbl_built<=2)
        MZLOG("[marshal] Table#%d reconstruido: x=%d y=%d z=%d cells=%d len_arr=%d (buf=%d bytes)\n",
              s_tbl_built,(int)xsize,(int)ysize,(int)zsize,(int)total,(int)RARRAY_LEN(arr),(int)len);
    return obj;
}

/*
** RPG::Color / RPG::Tone — reconstrucao nativa.
**
** Color#_dump e Tone#_dump produzem 4 doubles little-endian (32 bytes):
**   Color: red, green, blue, alpha
**   Tone:  red, green, blue, gray
** Nenhum tem _load no mruby do 3DS (compat_stubs so define initialize),
** por isso um Tone dentro de, p.ex., um comando "Tint Screen" de um evento
** em MapXXX.rxdata fazia o load_data inteiro rebentar.
** NOTA: Color e Tone sao MRB_TT_DATA (C struct nativo); NAO se pode usar
** mrb_obj_alloc(MRB_TT_OBJECT). Invocamos mrb_obj_new com os 4 floats para
** que o initialize nativo aloque o struct correctamente.
*/
static mrb_value build_colortone_from_dump(mrb_state *mrb, struct RClass *klass,
                                           mrb_value data, bool is_tone){
    const unsigned char *p=(const unsigned char*)RSTRING_PTR(data);
    mrb_int len=RSTRING_LEN(data);
    double v[4]={0,0,0,0};
    for(int i=0;i<4 && (mrb_int)(i*8+8)<=len;i++) v[i]=le_f64(p+i*8);
    /* Color e Tone sao classes nativas MRB_TT_DATA (C struct) -- NAO usar
       mrb_obj_alloc(MRB_TT_OBJECT) pois isso causa "allocation failure".
       mrb_obj_new invoca o initialize nativo que aloca correctamente o struct. */
    mrb_value args[4]={
        mrb_float_value(mrb,(mrb_float)v[0]),
        mrb_float_value(mrb,(mrb_float)v[1]),
        mrb_float_value(mrb,(mrb_float)v[2]),
        mrb_float_value(mrb,(mrb_float)v[3])
    };
    return mrb_obj_new(mrb,klass,4,args);
}

static mrb_value read_userdef(ReadCtx *c){
    mrb_state *mrb=c->mrb;
    struct RClass *klass=class_from_path(mrb,read_value(c));
    /* Lemos SEMPRE a string de dados primeiro para manter o stream alinhado,
       independentemente de existir _load ou de tratarmos a classe nativamente. */
    mrb_value data=read_str_val(c);

    /* RPG::Table (e o alias Table de topo) nao tem _load no mruby do 3DS:
       reconstruimos nativamente a partir do dump binario. */
    const char *cname=klass?mrb_class_name(mrb,klass):0;
    s_udef_seen++;
    if(cname && strcmp(cname,"Table")==0)
        return build_table_from_dump(mrb,klass,data);
    if(cname && strcmp(cname,"Color")==0)
        return build_colortone_from_dump(mrb,klass,data,false);
    if(cname && strcmp(cname,"Tone")==0)
        return build_colortone_from_dump(mrb,klass,data,true);

    /* Qualquer outra classe userdef precisa de _load. Isto identifica o
       proximo ponto de falha com precisao em vez de um TypeError generico. */
    if(!mrb_obj_respond_to(mrb,mrb_class(mrb,mrb_obj_value(klass)),mrb_intern_cstr(mrb,"_load"))){
        MZLOG("[marshal] ERRO: userdef '%s' sem _load (data=%d bytes) -> load vai falhar\n",
              cname?cname:"?",(int)RSTRING_LEN(data));
        throw MarshalException(MarshalException::TypeError,"class needs '_load'");
    }
    /* [LOG-LEAN IU] 400x OrderedHash no messages.dat inundariam o log: 2 primeiros. */
    static int s_udef_load_logged=0;
    if(s_udef_load_logged<2){ s_udef_load_logged++;
        MZLOG("[marshal] userdef '%s' via _load (%d bytes)\n",cname?cname:"?",(int)RSTRING_LEN(data)); }
    mrb_value r=mrb_funcall(mrb,mrb_obj_value(klass),"_load",1,data);
    /* [IU-USERDEF] se o _load Ruby levantar (ex.: Marshal.load aninhado falha), o
     * mrb_funcall devolve lixo com mrb->exc posto -- SEM este check o valor errado
     * entrava no pool e o load "passava" com dados corrompidos silenciosos. */
    if(mrb->exc){
        mrb_value ex=mrb_obj_value(mrb->exc); mrb->exc=0;
        mrb_value msg=mrb_funcall(mrb,ex,"message",0);
        MZLOG("[marshal] ERRO: '%s'._load levantou: %s\n",cname?cname:"?",
              mrb_string_p(msg)?RSTRING_PTR(msg):"?");
        throw MarshalException(MarshalException::TypeError,
              "'%s'._load raised",cname?cname:"?");
    }
    return r;
}
static mrb_value read_value(ReadCtx *c){
    mrb_state *mrb=c->mrb; int arena=mrb_gc_arena_save(mrb);
    int8_t type=c->readByte(); mrb_value v=mrb_nil_value();
    switch(type){
        case TYPE_NIL:     v=mrb_nil_value();   break;
        case TYPE_TRUE:    v=mrb_true_value();  break;
        case TYPE_FALSE:   v=mrb_false_value(); break;
        case TYPE_FIXNUM:  v=mrb_int_value(mrb,read_fixnum(c)); break;
        /* CRuby Marshal 4.8 REGISTA floats no object link table (r_entry).
           Tem de ser registado para que back-references (@) posteriores
           continuem alinhadas com o pool de objectos. */
        case TYPE_FLOAT:   v=mrb_float_value(mrb,read_float(c)); c->objects.add(v); break;
        case TYPE_STRING:  v=read_str_val(c); c->objects.add(v); break;
        case TYPE_ARRAY:   v=read_array(c);  break;
        case TYPE_HASH:    v=read_hash(c);   break;
        case TYPE_SYMBOL:  v=mrb_symbol_value(read_symbol(c));  break;
        case TYPE_SYMLINK: v=mrb_symbol_value(read_symlink(c)); break;
        case TYPE_OBJECT:  v=read_object(c); break;
        case TYPE_IVAR:    v=read_ivar(c);   break;
        case TYPE_LINK:    v=read_link(c);   break;
        case TYPE_UCLASS:  v=read_uclass(c); break;   /* [UCLASS] regista-se internamente */
        case TYPE_USERDEF: v=read_userdef(c); c->objects.add(v); break;
        default:
            MZLOG("[marshal] ERRO: tipo nao suportado '%c' (0x%02x) @pos=%d/%d\n",
                  (char)type,(unsigned char)type,(int)c->rb->pos,(int)c->rb->size);
            throw MarshalException(MarshalException::MKXPError,"unsupported type '%c'",(char)type);
    }
    mrb_gc_arena_restore(mrb,arena); return v;
}

/* ====== WRITE SIDE ================================================ */
static void write_fixnum(WriteCtx *c, int v){
    if(v==0){c->writeByte(0);return;}
    if(v>0&&v<123){c->writeByte((int8_t)(v+5));return;}
    if(v<0&&v>-124){c->writeByte((int8_t)(v-5));return;}
    int8_t len;
    if(v>0){ if(v<=0x7F)len=1; else if(v<=0x7FFF)len=2; else if(v<=0x7FFFFF)len=3; else len=4; }
    else   { if(v>=(int)0xFFFFFF80)len=-1; else if(v>=(int)0xFFFF8000)len=-2; else if(v>=(int)0xFF800000)len=-3; else len=-4; }
    c->writeByte(len);
    if(len>=1||len<=-1) c->writeByte((v&0x000000FF)>>0);
    if(len>=2||len<=-2) c->writeByte((v&0x0000FF00)>>8);
    if(len>=3||len<=-3) c->writeByte((v&0x00FF0000)>>16);
    if(len==4||len==-4) c->writeByte((int8_t)((v&0xFF000000)>>24));
}
static void write_float(WriteCtx *c, float v){
    sprintf(gpbuf,"%.16g",v); int len=strlen(gpbuf);
    write_fixnum(c,len); c->writeData(gpbuf,len);
}
static void write_str_raw(WriteCtx *c, const char *s){
    int len=strlen(s); write_fixnum(c,len); c->writeData(s,len);
}
static void write_str_val(WriteCtx *c, mrb_value s){
    int len=RSTRING_LEN(s); write_fixnum(c,len); c->writeData(RSTRING_PTR(s),len);
}
static void write_value(WriteCtx *c, mrb_value v);
static void write_array(WriteCtx *c, mrb_value a){
    int len=(int)RARRAY_LEN(a); write_fixnum(c,len);
    for(int i=0;i<len;i++) write_value(c,mrb_ary_entry(a,i));
}
static void write_hash(WriteCtx *c, mrb_value h){
    mrb_value keys=mrb_hash_keys(c->mrb,h);
    int len=(int)RARRAY_LEN(keys); write_fixnum(c,len);
    for(int i=0;i<len;i++){
        mrb_value k=mrb_ary_entry(keys,i);
        write_value(c,k); write_value(c,mrb_hash_get(c->mrb,h,k));
    }
}
static void write_sym(WriteCtx *c, mrb_value sym){
    mrb_int len; const char *p=mrb_sym_name_len(c->mrb,mrb_symbol(sym),&len);
    write_str_raw(c,p);
}
static void write_object(WriteCtx *c, mrb_value obj){
    mrb_state *mrb=c->mrb; struct RObject *o=mrb_obj_ptr(obj);
    write_value(c,mrb_str_intern(mrb,mrb_class_path(mrb,o->c)));
    mrb_value ivs=mrb_funcall(mrb,obj,"instance_variables",0);
    int n=(int)RARRAY_LEN(ivs); write_fixnum(c,n);
    for(int i=0;i<n;i++){
        mrb_value nm=mrb_ary_entry(ivs,i);
        write_value(c,nm); write_value(c,mrb_obj_iv_get(mrb,o,mrb_symbol(nm)));
    }
}
static void write_userdef(WriteCtx *c, mrb_value obj){
    mrb_state *mrb=c->mrb; struct RObject *o=mrb_obj_ptr(obj);
    write_value(c,mrb_str_intern(mrb,mrb_class_path(mrb,o->c)));
    write_str_val(c,mrb_funcall(mrb,obj,"_dump",0));
}

/* ── Serializacao de Table/Color/Tone para o cache ──────────────────────────
 * Estes objectos foram reconstruidos nativamente (build_table_from_dump /
 * build_colortone_from_dump) como MRB_TT_OBJECT com ivars, SEM _dump. Para o
 * cache fazer round-trip (e os 16 mapas + CommonEvents lerem do cache rapido
 * em vez da fonte -> 0.4 fps ao entrar no mapa), gravamos de volta o formato
 * userdef binario IGUAL ao que o leitor espera. */
static void le_put_i32(WriteCtx *c, int32_t v){
    unsigned char b[4]={(unsigned char)(v&0xFF),(unsigned char)((v>>8)&0xFF),
                        (unsigned char)((v>>16)&0xFF),(unsigned char)((v>>24)&0xFF)};
    c->writeData((const char*)b,4);
}
static void le_put_i16(WriteCtx *c, int16_t v){
    unsigned char b[2]={(unsigned char)(v&0xFF),(unsigned char)((v>>8)&0xFF)};
    c->writeData((const char*)b,2);
}
static int32_t iv_i32(mrb_state *mrb, mrb_value obj, const char *name, int32_t dflt){
    mrb_value v=mrb_iv_get(mrb,obj,mrb_intern_cstr(mrb,name));
    if(mrb_integer_p(v)) return (int32_t)mrb_integer(v);
    if(mrb_float_p(v))   return (int32_t)mrb_float(v);
    return dflt;
}
/* Table#_dump: [dim,xsize,ysize,zsize,total] (5x int32 LE) + total x int16 LE.
 * Escrevemos como TYPE_USERDEF: classpath + string binaria. */
static void write_table_userdef(WriteCtx *c, mrb_value obj){
    mrb_state *mrb=c->mrb; struct RObject *o=mrb_obj_ptr(obj);
    c->writeByte(TYPE_USERDEF);
    write_value(c,mrb_str_intern(mrb,mrb_class_path(mrb,o->c)));
    int32_t xs=iv_i32(mrb,obj,"@x",1), ys=iv_i32(mrb,obj,"@y",1), zs=iv_i32(mrb,obj,"@z",1);
    if(xs<1)xs=1; if(ys<1)ys=1; if(zs<1)zs=1;
    mrb_value data=mrb_iv_get(mrb,obj,mrb_intern_lit(mrb,"@data"));
    int32_t total=0;
    if(mrb_array_p(data)) total=(int32_t)RARRAY_LEN(data);
    int32_t expect=xs*ys*zs;
    int dim = (zs>1)?3:((ys>1)?2:1);
    /* string de dados: 20 bytes header + total*2 */
    int32_t bodylen=20+total*2;
    write_fixnum(c,bodylen);                 /* tamanho da string userdef */
    le_put_i32(c,dim); le_put_i32(c,xs); le_put_i32(c,ys); le_put_i32(c,zs);
    le_put_i32(c, (total>0)?total:expect);
    for(int32_t i=0;i<total;i++){
        mrb_value cell=mrb_ary_entry(data,i);
        int16_t cv = mrb_integer_p(cell)?(int16_t)mrb_integer(cell):0;
        le_put_i16(c,cv);
    }
}
/* Color/Tone#_dump: 4 doubles LE (32 bytes): red,green,blue,alpha/gray.
 * Os valores podem estar em ivars (MRB_TT_OBJECT) OU no struct nativo
 * (MRB_TT_DATA) -- neste caso lemos via metodos red/green/blue/alpha. */
static double ct_component(mrb_state *mrb, mrb_value obj, const char *iv, const char *meth){
    mrb_value v=mrb_iv_get(mrb,obj,mrb_intern_cstr(mrb,iv));
    if(mrb_float_p(v)) return mrb_float(v);
    if(mrb_integer_p(v)) return (double)mrb_integer(v);
    /* sem ivar: tentar metodo nativo */
    if(mrb_respond_to(mrb,obj,mrb_intern_cstr(mrb,meth))){
        mrb_value r=mrb_funcall(mrb,obj,meth,0);
        if(mrb_float_p(r)) return mrb_float(r);
        if(mrb_integer_p(r)) return (double)mrb_integer(r);
    }
    return 0.0;
}
static void write_colortone_userdef(WriteCtx *c, mrb_value obj){
    mrb_state *mrb=c->mrb; struct RObject *o=mrb_obj_ptr(obj);
    c->writeByte(TYPE_USERDEF);
    write_value(c,mrb_str_intern(mrb,mrb_class_path(mrb,o->c)));
    double vals[4];
    vals[0]=ct_component(mrb,obj,"@red","red");
    vals[1]=ct_component(mrb,obj,"@green","green");
    vals[2]=ct_component(mrb,obj,"@blue","blue");
    vals[3]=ct_component(mrb,obj,"@alpha","alpha");
    /* Tone usa 'gray' no 4o campo em vez de alpha; tentar se alpha=0 */
    if(vals[3]==0.0 && mrb_respond_to(mrb,obj,mrb_intern_cstr(mrb,"gray"))){
        mrb_value g=mrb_funcall(mrb,obj,"gray",0);
        if(mrb_float_p(g)) vals[3]=mrb_float(g);
        else if(mrb_integer_p(g)) vals[3]=(double)mrb_integer(g);
    }
    write_fixnum(c,32);                      /* 4 doubles */
    for(int i=0;i<4;i++){
        unsigned char b[8]; memcpy(b,&vals[i],8);
        c->writeData((const char*)b,8);
    }
}
static void write_value(WriteCtx *c, mrb_value v){
    mrb_state *mrb=c->mrb; int arena=mrb_gc_arena_save(mrb);
    if(c->objects.contains(v)){
        c->writeByte(TYPE_LINK); write_fixnum(c,c->objects.lookupIdx(v));
        mrb_gc_arena_restore(mrb,arena); return;
    }
    switch(mrb_type(v)){
        case MRB_TT_TRUE:    c->writeByte(TYPE_TRUE); break;
        case MRB_TT_FALSE:   c->writeByte(mrb_integer(v)?TYPE_FALSE:TYPE_NIL); break;
        case MRB_TT_INTEGER: c->writeByte(TYPE_FIXNUM); write_fixnum(c,(int)mrb_integer(v)); break;
        /* CRuby Marshal 4.8 REGISTA floats no object link table. Registamos
           tambem aqui para que os indices do pool coincidam com os do leitor
           (read_value) e o round-trip dump/load se mantenha consistente. */
        case MRB_TT_FLOAT:   c->objects.add(v); c->writeByte(TYPE_FLOAT); write_float(c,(float)mrb_float(v)); break;
        case MRB_TT_STRING:  c->objects.add(v); c->writeByte(TYPE_STRING); write_str_val(c,v); break;
        case MRB_TT_ARRAY:   c->objects.add(v); c->writeByte(TYPE_ARRAY);  write_array(c,v); break;
        case MRB_TT_HASH:    c->objects.add(v); c->writeByte(TYPE_HASH);   write_hash(c,v); break;
        case MRB_TT_SYMBOL:
            if(c->symbols.contains(mrb_symbol(v))){
                c->writeByte(TYPE_SYMLINK); write_fixnum(c,c->symbols.lookupIdx(mrb_symbol(v)));
            } else {
                c->writeByte(TYPE_SYMBOL); write_sym(c,v); c->symbols.add(mrb_symbol(v));
            }
            break;
        case MRB_TT_CLASS:
        case MRB_TT_OBJECT: {
            c->objects.add(v);
            const char *cn = mrb_obj_classname(mrb, v);
            if (cn && strcmp(cn,"Table")==0) {
                write_table_userdef(c, v);
            } else if (cn && (strcmp(cn,"Color")==0 || strcmp(cn,"Tone")==0)) {
                write_colortone_userdef(c, v);
            } else if (mrb_obj_respond_to(mrb,mrb_obj_ptr(v)->c,mrb_intern_lit(mrb,"_dump"))) {
                c->writeByte(TYPE_USERDEF); write_userdef(c,v);
            } else {
                c->writeByte(TYPE_OBJECT); write_object(c,v);
            }
            break;
        }
        default: {
            /* Color/Tone (e por vezes Table) sao reconstruidos como classes
             * nativas MRB_TT_DATA (struct C), NAO MRB_TT_OBJECT. Por isso nao
             * caem no case acima -- caem aqui. Detetamos pela classe e gravamos
             * o userdef na mesma. Sem isto, qualquer mapa com Tone (todos menos
             * os mais simples) tinha o cache IGNORADO. */
            const char *cn = mrb_obj_classname(mrb, v);
            if (cn && strcmp(cn,"Table")==0) {
                c->objects.add(v);
                write_table_userdef(c, v);
                break;
            } else if (cn && (strcmp(cn,"Color")==0 || strcmp(cn,"Tone")==0)) {
                c->objects.add(v);
                write_colortone_userdef(c, v);
                break;
            }
            /* Tipo nao-serializavel (outro MRB_TT_DATA). O write nao sabe
             * grava-lo. Marcamos o dump como sujo (o cache de rxdata ignora) MAS
             * [MARSHAL-PORT 2026-07-02] escrevemos NIL no lugar: antes nao se
             * escrevia NADA -> o stream ficava mal-formado e um SAVE que contivesse
             * um destes objetos (ex: um TT_DATA esquecido dentro de $PokemonGlobal)
             * ficava corrompido/ilegivel. Com nil, o save perde SO' esse campo mas
             * carrega -- e o log diz exatamente que tipo foi. */
            s_dump_incomplete = 1;
            /* [SAVE-DIAG 2026-07-03] printf ia so' p/ a consola (INVISIVEL no log!)
             * -> os saves esqueleticos de 101 bytes nunca mostraram porque. MZLOG
             * poe no debug_binding.log a CLASSE exata que nao serializa. */
            {
                const char *cn2 = mrb_obj_classname(mrb, v);
                MZLOG("[marshal] AVISO: tipo nao-serializavel tt=%d classe=%s -> escrito nil (save fica incompleto)\n",
                      (int)mrb_type(v), cn2 ? cn2 : "?");
            }
            c->writeByte(TYPE_NIL);
            break;
        }
    }
    mrb_gc_arena_restore(mrb,arena);
}
static void write_header(WriteCtx *c){ c->writeByte(MARSHAL_MAJOR); c->writeByte(MARSHAL_MINOR); }
static void verify_header(ReadCtx *c){
    int8_t maj=c->readByte(), min=c->readByte();
    if(maj!=MARSHAL_MAJOR||min!=MARSHAL_MINOR)
        throw MarshalException(MarshalException::TypeError,"incompatible marshal format");
}

/* ====== PUBLIC FILE* API ========================================== */
bool marshalDumpInt(mrb_state *mrb, FILE *fp, mrb_value val){
    WriteBuf wb; WriteCtx c; c.wb=&wb; c.mrb=mrb; c.limit=100;
    s_dump_incomplete = 0;
    write_header(&c); write_value(&c,val);
    fwrite(wb.data.data(),1,wb.data.size(),fp);
    return s_dump_incomplete == 0;   /* true = dump integro, seguro para cache */
}

mrb_value marshalLoadInt(mrb_state *mrb, FILE *fp){
    /* read whole file into buffer.
     * [MARSHAL-LINEAR 2026-07-11] era std::vector (malloc): com o heap saturado
     * pelos dados EBDX residentes, Map*.rxdata ~1MB nao alocava -> "marshal
     * falhou em Map194/139/043" + 21s de GC-thrash + mundo preto (log 11:35).
     * Buffer TRANSIENTE -> malloc com fallback linearAlloc (dezenas de MB
     * livres), mesmo padrao do [M2READ-LINEAR]/[LIBPNG-LINEAR]. */
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    if (sz <= 0) return mrb_nil_value();
    bool buf_linear = false;
    char *bufp = (char*)malloc((size_t)sz);
    if (!bufp) { bufp = (char*)linearAlloc((size_t)sz); if (bufp) buf_linear = true; }
    if (!bufp) {
        MZLOG("[marshal] FALHA: sem memoria p/ buffer de %ld bytes (malloc+linear)\n", sz);
        return mrb_nil_value();
    }
    struct BufGuard {
        char *p; bool lin;
        ~BufGuard() { if (p) { if (lin) linearFree(p); else free(p); } }
    } guard { bufp, buf_linear };
    fread(bufp,1,sz,fp);
    ReadBuf rb(bufp,sz); ReadCtx c; c.rb=&rb; c.mrb=mrb;
    s_tbl_built=0; s_udef_seen=0;
    /* try/catch para registar EXATAMENTE onde o marshal falha (tipo + posicao).
     * Sem isto, a MarshalException propagava-se como excecao C++ pura e o erro
     * detalhado nunca chegava ao log -- so se via "marshal falhou" sem causa.
     * Agora convertemos em erro mruby (como mrb_marshal_load faz) e logamos o
     * ponto de falha, para poder corrigir a causa raiz.
     * NOTA: a aceleracao do GC (anti-O(n^2)) e' feita ATIVANDO O GC GERACIONAL
     * uma vez no arranque (mrb_full_gc + generational), NAO desligando o GC nem
     * mexendo no arena aqui -- desligar o GC durante o read corrompia o array
     * de topo no 3DS (arena overflow com GC off). */
    try {
        verify_header(&c);
        /* PROFILING: capturar contadores ANTES do parse para medir o que ESTE
         * ficheiro custou (objetos criados e frees=GC durante o parse). */
        uint64_t prof_objs0  = g_prof_objs_created;
        uint64_t prof_frees0 = g_prof_frees;
        /* Reset das zonas internas (alloc/ivset/class) para medir SO' este
         * ficheiro -- assim o breakdown diz onde foi o tempo deste parse. */
        prof_zone_reset(PROF_MARSHAL_ALLOC);
        prof_zone_reset(PROF_MARSHAL_IVSET);
        prof_zone_reset(PROF_MARSHAL_CLASS);
        prof_zone_reset(PROF_MARSHAL_SYMBOL);
        prof_zone_reset(PROF_MARSHAL_STRING);
        uint64_t prof_t0     = prof_tick();

        mrb_value v=read_value(&c);

        double   parse_ms    = prof_ticks_to_ms(prof_tick() - prof_t0);
        uint64_t objs_made   = g_prof_objs_created - prof_objs0;
        uint64_t frees_made  = g_prof_frees        - prof_frees0;
        g_prof_bytes_parsed += (uint64_t)sz;

        /* [LOG-LEAN] so' ficheiros grandes (>64KB) — os pequenos (.dat de config,
         * saves triviais) eram 1 linha/ficheiro de boot sem valor diagnostico. */
        if (sz > 64*1024)
            MZLOG("[marshal] load OK (%ld bytes): userdefs=%d tables=%d top_type=%d\n",
                  sz,s_udef_seen,s_tbl_built,(int)mrb_type(v));
        /* Relatorio de profiling SO' para ficheiros grandes (>50KB) -- os
         * pequenos nao interessam e poluiriam o log. Mostra ms/byte, ms/obj e
         * o BREAKDOWN (alloc vs ivset vs class) que localiza os ~2ms/objeto. */
        if (sz > 30000) {   /* baixado de 50KB p/ 30KB: apanha o Animations.rxdata (43KB),
                             * a maior anomalia do log (14s p/ 43KB). Assim o BREAKDOWN
                             * diz no 3DS real onde vao esses 14s (alloc/ivset/class/leitura). */
            prof_report_marshal("(ficheiro)", (uint64_t)sz, objs_made, frees_made, parse_ms);
        }
        return v;
    } catch(const MarshalException &e){
        MZLOG("[marshal] FALHA @pos=%d/%ld apos %d tables, %d userdefs: %s\n",
              (int)rb.pos,sz,s_tbl_built,s_udef_seen,e.msg);
        raiseMrbExc(mrb,e);   /* converte em erro mruby (mrb->exc fica setado) */
        return mrb_nil_value();
    }
}

/* ====== MRB BINDINGS ============================================== */
static mrb_value mrb_marshal_dump(mrb_state *mrb, mrb_value /*self*/){
    mrb_value val, port=mrb_nil_value(); mrb_int limit=100;
    mrb_get_args(mrb,"o|oi",&val,&port,&limit);
    /* [MARSHAL-PORT 2026-07-02] semantica CRuby: dump(obj, limit) OU dump(obj, io, limit).
     * O SaveData do Essentials grava com File.open('wb'){|f| Marshal.dump(dados, f)} --
     * antes o port era IGNORADO (devolvia so' a string) -> o save ficava VAZIO. */
    if(mrb_integer_p(port)){ limit=mrb_integer(port); port=mrb_nil_value(); }
    WriteBuf wb; WriteCtx c; c.wb=&wb; c.mrb=mrb; c.limit=limit;
    try{ write_header(&c); write_value(&c,val); }
    catch(const MarshalException &e){ raiseMrbExc(mrb,e); }
    mrb_value str = mrb_str_new(mrb,wb.data.data(),(mrb_int)wb.data.size());
    if(!mrb_nil_p(port)){
        /* [SAVE-0B-FIX 2026-07-03] NAO usar mrb_respond_to: neste ambiente o
         * respond_to? e' NAO-FIAVEL (ja' partiu varios fixes; aqui fazia o dump
         * levantar "port sem #write" DEPOIS do File.open('wb') ter truncado o
         * ficheiro -> save de 0 bytes -> load rebenta -> sem "Continuar").
         * Chamada DIRETA com guarda de excecao: se port.write nao existir, o
         * mrb_funcall poe mrb->exc e nos convertemos em erro claro no log. */
        mrb_funcall(mrb,port,"write",1,str);
        if(mrb->exc){
            mrb_value exc = mrb_obj_value(mrb->exc); mrb->exc = 0;
            mrb_value m = mrb_funcall(mrb, exc, "message", 0);
            MZLOG("[marshal] ERRO dump->port.write falhou: %s (save ficaria 0 bytes)\n",
                  mrb_string_p(m) ? RSTRING_PTR(m) : "?");
            mrb_raise(mrb,mrb_class_get(mrb,"ArgumentError"),
                      "Marshal.dump: port.write falhou");
        }
        MZLOG("[marshal] dump->port OK (%d bytes escritos no IO)\n",(int)wb.data.size());
        /* [SAVE-DIAG] raio-X do save: se o topo e' um Hash (compile_save_hash do
         * Essentials), listar CHAVE=CLASSE de cada valor. Um save de 101 bytes com
         * valores nil/em falta aponta imediatamente a peca que nao serializa. */
        if(mrb_hash_p(val) && wb.data.size() < 65536){
            mrb_value keys = mrb_hash_keys(mrb, val);
            mrb_int n = RARRAY_LEN(keys);
            char kb[512]; int ko = 0;
            for(mrb_int i = 0; i < n && ko < (int)sizeof(kb) - 64; i++){
                mrb_value k = mrb_ary_ref(mrb, keys, i);
                mrb_value v2 = mrb_hash_get(mrb, val, k);
                mrb_value ks = mrb_funcall(mrb, k, "to_s", 0);
                if(mrb->exc){ mrb->exc = 0; continue; }
                ko += snprintf(kb + ko, sizeof(kb) - ko, " %s=%s",
                               mrb_string_p(ks) ? RSTRING_PTR(ks) : "?",
                               mrb_obj_classname(mrb, v2));
            }
            MZLOG("[marshal] SAVE-RAIOX: %d chaves:%s\n",(int)n, kb);
        }
        return port;                       /* CRuby devolve o proprio port */
    }
    /* [SAVE-TRACE] dumps grandes SEM porta = provavel caminho de save via
     * File.write(path, Marshal.dump(data)). Loga p/ mapear o fluxo real. */
    if (wb.data.size() > 4096)
        MZLOG("[marshal] dump->STRING %d bytes (sem porta; caller grava por File.write?)\n",
              (int)wb.data.size());
    return str;
}

static mrb_value mrb_marshal_load(mrb_state *mrb, mrb_value /*self*/){
    mrb_value port; mrb_get_args(mrb,"o",&port);
    /* [MARSHAL-PORT] aceitar IO/File alem de String (CRuby): le' a partir da posicao
     * ATUAL e avanca-a exatamente o consumido por UM documento -> suporta o padrao
     * do SaveData "save_data << Marshal.load(file) until file.eof?" (saves pre-v19). */
    if(!mrb_string_p(port)){
        {
            /* [SAVE-0B-FIX 2026-07-03] idem dump: chamada DIRETA em vez de
             * mrb_respond_to (nao-fiavel). pos e' opcional: falhar nao invalida. */
            mrb_value pos0 = mrb_funcall(mrb,port,"pos",0);
            bool has_pos = !mrb->exc && mrb_integer_p(pos0);
            if(mrb->exc) mrb->exc = 0;
            mrb_value data = mrb_funcall(mrb,port,"read",0);
            if(mrb->exc || !mrb_string_p(data)){
                mrb->exc = 0;
                mrb_raise(mrb,mrb_class_get(mrb,"ArgumentError"),
                          "Marshal.load: port.read nao devolveu String");
            }
            ReadBuf rb2(RSTRING_PTR(data),(size_t)RSTRING_LEN(data));
            ReadCtx c2; c2.rb=&rb2; c2.mrb=mrb;
            mrb_value val2=mrb_nil_value();
            try{ verify_header(&c2); val2=read_value(&c2); }
            catch(const MarshalException &e){ raiseMrbExc(mrb,e); return mrb_nil_value(); }
            if(has_pos && mrb_integer_p(pos0)){
                mrb_funcall(mrb,port,"pos=",1,
                            mrb_int_value(mrb,mrb_integer(pos0)+(mrb_int)rb2.pos));
                if(mrb->exc) mrb->exc = 0;   /* pos= falhar nao invalida o load */
            }
            return val2;
        }
        mrb_raise(mrb,mrb_class_get(mrb,"ArgumentError"),"Marshal.load: expected String/IO");
    }
    ReadBuf rb(RSTRING_PTR(port),(size_t)RSTRING_LEN(port));
    ReadCtx c; c.rb=&rb; c.mrb=mrb;
    mrb_value val=mrb_nil_value();
    try{ verify_header(&c); val=read_value(&c); }
    catch(const MarshalException &e){ raiseMrbExc(mrb,e); }
    return val;
}

void marshalBindingInit(mrb_state *mrb){
    RClass *mod=mrb_define_module(mrb,"Marshal");
    mrb_define_module_function(mrb,mod,"dump",mrb_marshal_dump,MRB_ARGS_REQ(1)|MRB_ARGS_OPT(2));
    mrb_define_module_function(mrb,mod,"load",mrb_marshal_load,MRB_ARGS_REQ(1));
}