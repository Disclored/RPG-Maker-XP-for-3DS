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

/* ---- logging partilhado com binding_3ds.cpp ---------------------- */
/* g_dbglog e o FILE* aberto em binding_3ds.cpp (sdmc:/mkxp/debug_binding.log).
   printf sozinho NAO chega ao ficheiro de log; escrevemos tambem nele. */
extern FILE *g_dbglog;
#define MZLOG(fmt, ...) do { \
        printf(fmt, ##__VA_ARGS__); \
        if (g_dbglog) { fprintf(g_dbglog, fmt, ##__VA_ARGS__); fflush(g_dbglog); } \
    } while(0)

/* contadores de diagnostico, reiniciados a cada load completo */
static int s_tbl_built = 0;
static int s_udef_seen = 0;

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
static float read_float(ReadCtx *c){
    int len=read_fixnum(c); c->readData(gpbuf,len); gpbuf[len]='\0';
    return strtof(gpbuf,0);
}
static char *read_str_raw(ReadCtx *c){
    int len=read_fixnum(c); c->readData(gpbuf,len); gpbuf[len]='\0'; return gpbuf;
}
static mrb_value read_str_val(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    mrb_value s=mrb_str_new(mrb,0,len);
    c->readData(RSTR_PTR(RSTRING(s)),len); RSTR_PTR(RSTRING(s))[len]='\0';
    return s;
}
/* Lê string sem registar no object pool — usado por read_ivar que regista o slot */
static mrb_value read_str_val_notrack(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    mrb_value s=mrb_str_new(mrb,0,len);
    c->readData(RSTR_PTR(RSTRING(s)),len); RSTR_PTR(RSTRING(s))[len]='\0';
    return s;
}
static mrb_value read_value(ReadCtx *c);
static mrb_value read_array(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    mrb_value a=mrb_ary_new_capa(mrb,len); c->objects.add(a);
    for(int i=0;i<len;i++) mrb_ary_set(mrb,a,i,read_value(c));
    return a;
}
static mrb_value read_hash(ReadCtx *c){
    mrb_state *mrb=c->mrb; int len=read_fixnum(c);
    mrb_value h=mrb_hash_new_capa(mrb,len); c->objects.add(h);
    for(int i=0;i<len;i++){
        mrb_value k=read_value(c), v=read_value(c); mrb_hash_set(mrb,h,k,v);
    }
    return h;
}
static mrb_sym read_symbol(ReadCtx *c){
    mrb_sym s=mrb_intern_cstr(c->mrb,read_str_raw(c)); c->symbols.add(s); return s;
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
    char *p=s,*b=s; mrb_value klass=mrb_obj_value(mrb->object_class);
    while(*p&&*p!='"'){
        while(*p&&*p!=':'&&*p!='"') p++;
        klass=mrb_const_get(mrb,klass,mrb_intern(mrb,b,p-b));
        if(p[0]==':'){ if(p[1]!=':') return 0; p+=2; b=p; }
    }
    return (struct RClass*)mrb_obj_ptr(klass);
}
static mrb_value read_object(ReadCtx *c){
    mrb_state *mrb=c->mrb;
    mrb_value class_path=read_value(c);
    struct RClass *klass=class_from_path(mrb,class_path);
    if(!klass){
        /* Classe nao definida — usa Object como fallback e loga aviso */
        const char *cname="<unknown>";
        if(mrb_symbol_p(class_path))      cname=mrb_sym_name(mrb,mrb_symbol(class_path));
        else if(mrb_string_p(class_path)) cname=RSTRING_PTR(class_path);
        printf("[Marshal] WARN: classe '%s' nao definida -- a usar Object como fallback\n", cname);
        klass=mrb->object_class;
    }
    mrb_value obj=mrb_obj_value(mrb_obj_alloc(mrb,MRB_TT_OBJECT,klass));
    c->objects.add(obj); int n=read_fixnum(c);
    for(int i=0;i<n;i++){
        mrb_value nm=read_value(c), vl=read_value(c);
        if(mrb_symbol_p(nm))
            mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_symbol(nm),vl);
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
    for(int32_t i=0;i<total;i++){
        int16_t v=le_i16(cell); cell+=2;
        mrb_ary_set(mrb,arr,i,mrb_int_value(mrb,(mrb_int)v));
    }
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_intern_lit(mrb,"@x"),   mrb_int_value(mrb,xsize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_intern_lit(mrb,"@y"),   mrb_int_value(mrb,ysize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_intern_lit(mrb,"@z"),   mrb_int_value(mrb,zsize));
    mrb_obj_iv_set(mrb,mrb_obj_ptr(obj),mrb_intern_lit(mrb,"@data"),arr);
    mrb_gc_arena_restore(mrb,arena);
    s_tbl_built++;
    if(s_tbl_built<=3)
        MZLOG("[marshal] Table#%d reconstruido: x=%d y=%d z=%d cells=%d (buf=%d bytes)\n",
              s_tbl_built,(int)xsize,(int)ysize,(int)zsize,(int)total,(int)len);
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
    MZLOG("[marshal] userdef '%s' via _load (%d bytes)\n",cname?cname:"?",(int)RSTRING_LEN(data));
    return mrb_funcall(mrb,mrb_obj_value(klass),"_load",1,data);
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
        case MRB_TT_OBJECT:
            c->objects.add(v);
            if(mrb_obj_respond_to(mrb,mrb_obj_ptr(v)->c,mrb_intern_lit(mrb,"_dump"))){
                c->writeByte(TYPE_USERDEF); write_userdef(c,v);
            } else {
                c->writeByte(TYPE_OBJECT); write_object(c,v);
            }
            break;
        default: printf("Marshal: unwritable type %d\n",(int)mrb_type(v)); break;
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
void marshalDumpInt(mrb_state *mrb, FILE *fp, mrb_value val){
    WriteBuf wb; WriteCtx c; c.wb=&wb; c.mrb=mrb; c.limit=100;
    write_header(&c); write_value(&c,val);
    fwrite(wb.data.data(),1,wb.data.size(),fp);
}

mrb_value marshalLoadInt(mrb_state *mrb, FILE *fp){
    /* read whole file into buffer */
    fseek(fp,0,SEEK_END); long sz=ftell(fp); fseek(fp,0,SEEK_SET);
    std::vector<char> buf(sz);
    fread(buf.data(),1,sz,fp);
    ReadBuf rb(buf.data(),sz); ReadCtx c; c.rb=&rb; c.mrb=mrb;
    s_tbl_built=0; s_udef_seen=0;
    /* try/catch para registar EXATAMENTE onde o marshal falha (tipo + posicao).
     * Sem isto, a MarshalException propagava-se como excecao C++ pura e o erro
     * detalhado nunca chegava ao log -- so se via "marshal falhou" sem causa.
     * Agora convertemos em erro mruby (como mrb_marshal_load faz) e logamos o
     * ponto de falha, para poder corrigir a causa raiz. */
    try {
        verify_header(&c);
        mrb_value v=read_value(&c);
        MZLOG("[marshal] load OK (%ld bytes): userdefs=%d tables=%d top_type=%d\n",
              sz,s_udef_seen,s_tbl_built,(int)mrb_type(v));
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
    WriteBuf wb; WriteCtx c; c.wb=&wb; c.mrb=mrb; c.limit=limit;
    try{ write_header(&c); write_value(&c,val); }
    catch(const MarshalException &e){ raiseMrbExc(mrb,e); }
    return mrb_str_new(mrb,wb.data.data(),(mrb_int)wb.data.size());
}

static mrb_value mrb_marshal_load(mrb_state *mrb, mrb_value /*self*/){
    mrb_value port; mrb_get_args(mrb,"o",&port);
    if(!mrb_string_p(port))
        mrb_raise(mrb,mrb_class_get(mrb,"ArgumentError"),"Marshal.load: expected String");
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