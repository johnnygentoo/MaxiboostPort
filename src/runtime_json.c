/* libSceJson2 (sce::Json): JSON values, arrays, strings and the parser, for what a game reads from
 * a server. MAXIBOOST ON parses its server configuration with it (version, maintenance, the add-on
 * schedule, ...: see runtime_net's local answers); the offline stubs it replaces failed every parse.
 *
 * The game owns the objects it declares and calls the constructors on its own memory; each holds
 * at most a pointer of ours, sized as MAXIBOOST ON's frames leave room for (guest+0xee920):
 *   Value: type (u32), payload (u64: boolean, integer, unsigned, real, or a node)   16 bytes
 *   String, Array: a node pointer                                                   8 bytes
 *   Array::iterator: the element (a Value inside the array's node)                 8 bytes
 * Nodes are reference counted: copies share them, the game only reads. Types as the SDK's
 * ValueType: 0 null, 1 boolean, 2 integer, 3 unsigned integer, 4 real, 5 string, 6 array,
 * 7 object. A number without sign, fraction or exponent is an unsigned integer. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { J_NULL, J_BOOLEAN, J_INTEGER, J_UINTEGER, J_REAL, J_STRING, J_ARRAY, J_OBJECT };
#define JSON_ERROR_PARSE ((int32_t)0x80920001)
#define JSON_ERROR_MEMORY ((int32_t)0x80920002)

typedef struct JNode JNode;
typedef struct { uint32_t type, reserved; union { uint64_t u; int64_t i; double d; JNode *node; } v; } Value;
typedef struct { JNode *node; } Handle; /* sce::Json::String, sce::Json::Array */
typedef struct { Value *at; } Iterator;
_Static_assert(sizeof(Value)==16,"sce::Json::Value layout");

struct JNode {
    int refs;
    uint32_t kind;          /* J_STRING, J_ARRAY or J_OBJECT */
    Handle self;            /* what getString/getArray return a reference to */
    size_t count, capacity; /* array elements or object members */
    Value *values;
    char **keys;            /* objects */
    char *text;             /* strings (NUL-terminated, may hold NULs: length is count) */
};

static JNode *node_new(uint32_t kind) {
    JNode *n=calloc(1,sizeof(*n));
    if (!n) return NULL;
    n->refs=1; n->kind=kind; n->self.node=n;
    return n;
}
static JNode *node_ref(JNode *n) { if (n) __atomic_add_fetch(&n->refs,1,__ATOMIC_RELAXED); return n; }
static void value_release(Value *v);
static void node_unref(JNode *n) {
    if (!n || __atomic_sub_fetch(&n->refs,1,__ATOMIC_ACQ_REL)) return;
    for (size_t i=0;i<n->count && n->values;++i) value_release(&n->values[i]);
    for (size_t i=0;i<n->count && n->keys;++i) free(n->keys[i]);
    free(n->values); free(n->keys); free(n->text); free(n);
}
static int has_node(const Value *v) { return v->type>=J_STRING && v->type<=J_OBJECT; }
static void value_release(Value *v) { if (has_node(v)) node_unref(v->v.node); memset(v,0,sizeof(*v)); }
static void value_assign(Value *dst,const Value *src) {
    if (dst==src) return;
    Value copy=*src;
    if (has_node(&copy)) node_ref(copy.v.node);
    value_release(dst);
    *dst=copy;
}

/* Answers for what is not there: a null value, an empty string and array. */
static Value null_value;
static JNode empty_string={.refs=1<<30,.kind=J_STRING,.self={&empty_string},.text=(char[1]){0}};
static JNode empty_array={.refs=1<<30,.kind=J_ARRAY,.self={&empty_array}};

/* ---- The parser (RFC 8259; a NUL ends the text early, as the game's buffers are padded). ---- */
typedef struct { const char *at, *end; int depth; } Reader;
static void skip_space(Reader *r) { while (r->at<r->end && (*r->at==' ' || *r->at=='\t' || *r->at=='\n' || *r->at=='\r')) ++r->at; }
static int append(JNode *n,const Value *v,char *key) {
    if (n->count==n->capacity) {
        size_t cap=n->capacity ? n->capacity*2 : 8;
        Value *values=realloc(n->values,cap*sizeof(*values));
        if (!values) return 0;
        n->values=values;
        if (n->kind==J_OBJECT) {
            char **keys=realloc(n->keys,cap*sizeof(*keys));
            if (!keys) return 0;
            n->keys=keys;
        }
        n->capacity=cap;
    }
    n->values[n->count]=*v;
    if (n->kind==J_OBJECT) n->keys[n->count]=key;
    ++n->count;
    return 1;
}
static void put_utf8(char **out,uint32_t c) {
    char *o=*out;
    if (c<0x80) *o++=(char)c;
    else if (c<0x800) { *o++=(char)(0xc0|c>>6); *o++=(char)(0x80|(c&63)); }
    else if (c<0x10000) { *o++=(char)(0xe0|c>>12); *o++=(char)(0x80|(c>>6&63)); *o++=(char)(0x80|(c&63)); }
    else { *o++=(char)(0xf0|c>>18); *o++=(char)(0x80|(c>>12&63)); *o++=(char)(0x80|(c>>6&63)); *o++=(char)(0x80|(c&63)); }
    *out=o;
}
static int hex4(const char *p,uint32_t *out) {
    uint32_t v=0;
    for (int i=0;i<4;++i) {
        char c=p[i]; v<<=4;
        if (c>='0' && c<='9') v|=(uint32_t)(c-'0');
        else if (c>='a' && c<='f') v|=(uint32_t)(c-'a'+10);
        else if (c>='A' && c<='F') v|=(uint32_t)(c-'A'+10);
        else return 0;
    }
    *out=v; return 1;
}
/* A string token into a new buffer; *length is its size. */
static char *read_string(Reader *r,size_t *length) {
    if (r->at>=r->end || *r->at!='"') return NULL;
    const char *p=++r->at;
    while (p<r->end && *p!='"') p+=(*p=='\\' && p+1<r->end) ? 2 : 1;
    if (p>=r->end) return NULL;
    char *text=malloc((size_t)(p-r->at)+1), *o=text;
    if (!text) return NULL;
    for (const char *s=r->at;s<p;) {
        if (*s!='\\') { *o++=*s++; continue; }
        ++s;
        switch (*s++) {
        case '"': *o++='"'; break; case '\\': *o++='\\'; break; case '/': *o++='/'; break;
        case 'b': *o++='\b'; break; case 'f': *o++='\f'; break; case 'n': *o++='\n'; break;
        case 'r': *o++='\r'; break; case 't': *o++='\t'; break;
        case 'u': {
            uint32_t c, low;
            if (s+4>p || !hex4(s,&c)) { free(text); return NULL; }
            s+=4;
            if (c>=0xd800 && c<0xdc00 && s+6<=p && s[0]=='\\' && s[1]=='u' && hex4(s+2,&low) && low>=0xdc00 && low<0xe000) {
                c=0x10000+((c-0xd800)<<10)+(low-0xdc00); s+=6;
            }
            put_utf8(&o,c);
            break;
        }
        default: free(text); return NULL;
        }
    }
    *o=0; *length=(size_t)(o-text);
    r->at=p+1;
    return text;
}
static int read_value(Reader *r,Value *out);
static int read_container(Reader *r,Value *out,int object) {
    if (++r->depth>256) return 0;
    JNode *n=node_new(object ? J_OBJECT : J_ARRAY);
    if (!n) return 0;
    out->type=object ? J_OBJECT : J_ARRAY; out->v.node=n;
    ++r->at;
    skip_space(r);
    if (r->at<r->end && *r->at==(object ? '}' : ']')) { ++r->at; --r->depth; return 1; }
    for (;;) {
        char *key=NULL; size_t key_length;
        skip_space(r);
        if (object) {
            if (!(key=read_string(r,&key_length))) return 0;
            skip_space(r);
            if (r->at>=r->end || *r->at!=':') { free(key); return 0; }
            ++r->at;
        }
        Value v={0};
        if (!read_value(r,&v) || !append(n,&v,key)) { free(key); value_release(&v); return 0; }
        skip_space(r);
        if (r->at<r->end && *r->at==',') { ++r->at; continue; }
        if (r->at<r->end && *r->at==(object ? '}' : ']')) { ++r->at; --r->depth; return 1; }
        return 0;
    }
}
static int read_value(Reader *r,Value *out) {
    skip_space(r);
    if (r->at>=r->end) return 0;
    const char c=*r->at;
    if (c=='{' || c=='[') return read_container(r,out,c=='{');
    if (c=='"') {
        size_t length;
        char *text=read_string(r,&length);
        JNode *n=text ? node_new(J_STRING) : NULL;
        if (!n) { free(text); return 0; }
        n->text=text; n->count=length;
        out->type=J_STRING; out->v.node=n;
        return 1;
    }
    static const struct { const char *word; uint32_t type; uint64_t value; } words[]={
        {"true",J_BOOLEAN,1},{"false",J_BOOLEAN,0},{"null",J_NULL,0}};
    for (size_t i=0;i<3;++i) {
        const size_t n=strlen(words[i].word);
        if ((size_t)(r->end-r->at)>=n && !memcmp(r->at,words[i].word,n)) {
            r->at+=n; out->type=words[i].type; out->v.u=words[i].value; return 1;
        }
    }
    /* Numbers: copied out (the text need not end with a NUL). */
    char number[64]; size_t n=0; int real=0, negative=c=='-';
    while (r->at+n<r->end && n<sizeof(number)-1 && strchr("+-0123456789.eE",r->at[n])) {
        if (strchr(".eE",r->at[n])) real=1;
        number[n]=r->at[n]; ++n;
    }
    if (!n) return 0;
    number[n]=0;
    char *end;
    if (real) { out->type=J_REAL; out->v.d=strtod(number,&end); }
    else if (negative) { out->type=J_INTEGER; out->v.i=strtoll(number,&end,10); }
    else { out->type=J_UINTEGER; out->v.u=strtoull(number,&end,10); }
    if (end!=number+n) return 0;
    r->at+=n;
    return 1;
}

/* ---- Text of a value (Value::toString): a string's own text, other values as JSON. ---- */
typedef struct { char *text; size_t length, capacity; } Buffer;
static void put(Buffer *b,const char *s,size_t n) {
    if (b->length+n+1>b->capacity) {
        size_t cap=b->capacity ? b->capacity : 64;
        while (cap<b->length+n+1) cap*=2;
        char *t=realloc(b->text,cap);
        if (!t) return;
        b->text=t; b->capacity=cap;
    }
    memcpy(b->text+b->length,s,n); b->length+=n; b->text[b->length]=0;
}
static void put_quoted(Buffer *b,const char *s,size_t n) {
    put(b,"\"",1);
    for (size_t i=0;i<n;++i) {
        char esc[8]; const unsigned char c=(unsigned char)s[i];
        if (c=='"' || c=='\\') { esc[0]='\\'; esc[1]=(char)c; put(b,esc,2); }
        else if (c<0x20) { snprintf(esc,sizeof(esc),"\\u%04x",c); put(b,esc,6); }
        else put(b,(const char *)&s[i],1);
    }
    put(b,"\"",1);
}
static void write_value(Buffer *b,const Value *v,int top) {
    char number[64];
    switch (v->type) {
    case J_BOOLEAN: put(b,v->v.u ? "true" : "false",v->v.u ? 4 : 5); break;
    case J_INTEGER: put(b,number,(size_t)snprintf(number,sizeof(number),"%lld",(long long)v->v.i)); break;
    case J_UINTEGER: put(b,number,(size_t)snprintf(number,sizeof(number),"%llu",(unsigned long long)v->v.u)); break;
    case J_REAL: put(b,number,(size_t)snprintf(number,sizeof(number),"%.17g",v->v.d)); break;
    case J_STRING:
        if (top) put(b,v->v.node->text,v->v.node->count);
        else put_quoted(b,v->v.node->text,v->v.node->count);
        break;
    case J_ARRAY: case J_OBJECT: {
        const JNode *n=v->v.node;
        put(b,v->type==J_ARRAY ? "[" : "{",1);
        for (size_t i=0;i<n->count;++i) {
            if (i) put(b,",",1);
            if (v->type==J_OBJECT) { put_quoted(b,n->keys[i],strlen(n->keys[i])); put(b,":",1); }
            write_value(b,&n->values[i],0);
        }
        put(b,v->type==J_ARRAY ? "]" : "}",1);
        break;
    }
    default: put(b,"null",4);
    }
}
static JNode *string_node(const char *text,size_t length) {
    JNode *n=node_new(J_STRING);
    if (!n) return NULL;
    if (!(n->text=malloc(length+1))) { free(n); return NULL; }
    memcpy(n->text,text,length); n->text[length]=0; n->count=length;
    return n;
}

/* ---- sce::Json, as the game calls it (C++ member functions: this first; by-value class results
 * through a hidden pointer first). ---- */
static ABI int32_t parse(Value *out,const char *text,uint64_t size) {
    if (!out || !text) return JSON_ERROR_PARSE;
    const char *end=memchr(text,0,size);
    Reader r={text,end ? end : text+size,0};
    Value v={0};
    int ok=read_value(&r,&v);
    skip_space(&r);
    if (!ok || r.at!=r.end) {
        value_release(&v);
        printf("Runtime: Json parse failed at byte %zu of %zu\n",(size_t)(r.at-text),(size_t)(r.end-text));
        return JSON_ERROR_PARSE;
    }
    value_release(out);
    *out=v;
    return 0;
}
static ABI Value *value_init(Value *self) { memset(self,0,sizeof(*self)); return self; }
static ABI Value *value_copy_init(Value *self,const Value *other) { memset(self,0,sizeof(*self)); value_assign(self,other); return self; }
static ABI Value *value_assign_op(Value *self,const Value *other) { value_assign(self,other); return self; }
static ABI void value_destroy(Value *self) { value_release(self); }
static ABI void value_clear(Value *self) { value_release(self); }
static ABI int32_t value_type(const Value *self) { return (int32_t)self->type; }
static const Value *member(const Value *self,const char *key) {
    if (!key || self->type!=J_OBJECT) return &null_value;
    const JNode *n=self->v.node;
    for (size_t i=0;i<n->count;++i) if (!strcmp(n->keys[i],key)) return &n->values[i];
    return &null_value;
}
static ABI const Value *value_member(const Value *self,const char *key) { return member(self,key); }
static ABI const Value *value_get_value(const Value *self,const Handle *key) {
    return member(self,key && key->node ? key->node->text : NULL);
}
/* Numbers by reference; any number type reads as the one asked for (the storage is shared). */
static uint64_t number_scratch;
static ABI const uint64_t *value_unsigned(const Value *self) {
    if (self->type==J_UINTEGER || self->type==J_INTEGER || self->type==J_BOOLEAN) return &self->v.u;
    number_scratch=self->type==J_REAL ? (uint64_t)self->v.d : 0;
    return &number_scratch;
}
static ABI const int64_t *value_integer(const Value *self) { return (const int64_t *)value_unsigned(self); }
static ABI const Handle *value_string(const Value *self) {
    return self->type==J_STRING ? &self->v.node->self : &empty_string.self;
}
static ABI const Handle *value_array(const Value *self) {
    return self->type==J_ARRAY ? &self->v.node->self : &empty_array.self;
}
static ABI int32_t value_to_string(const Value *self,Handle *out) {
    Buffer b={0};
    write_value(&b,self,1);
    JNode *n=string_node(b.text ? b.text : "",b.length);
    free(b.text);
    if (!n) return JSON_ERROR_MEMORY;
    if (out->node!=&empty_string) node_unref(out->node);
    out->node=n;
    return 0;
}
static void handle_release(Handle *h) { if (h->node && h->node!=&empty_string && h->node!=&empty_array) node_unref(h->node); h->node=NULL; }
static ABI Handle *string_init(Handle *self) { self->node=NULL; return self; }
static ABI Handle *string_from(Handle *self,const char *text) {
    self->node=string_node(text ? text : "",text ? strlen(text) : 0);
    return self;
}
static ABI void string_destroy(Handle *self) { handle_release(self); }
static ABI const char *string_c_str(const Handle *self) { return self->node ? self->node->text : ""; }
static ABI uint64_t string_length(const Handle *self) { return self->node ? self->node->count : 0; }
static ABI Handle *array_copy_init(Handle *self,const Handle *other) {
    self->node=other && other->node ? other->node : &empty_array;
    if (self->node!=&empty_array) node_ref(self->node);
    return self;
}
static ABI void array_destroy(Handle *self) { handle_release(self); }
static ABI uint64_t array_size(const Handle *self) { return self->node ? self->node->count : 0; }
static ABI Iterator *array_begin(Iterator *out,const Handle *self) { out->at=self->node ? self->node->values : NULL; return out; }
static ABI Iterator *array_end(Iterator *out,const Handle *self) {
    out->at=self->node && self->node->values ? self->node->values+self->node->count : NULL;
    return out;
}
static ABI int32_t iterator_differ(const Iterator *a,const Iterator *b) { return a->at!=b->at; }
static ABI const Value *iterator_value(const Iterator *self) { return self->at ? self->at : &null_value; }
static ABI Iterator *iterator_next(Iterator *self) { if (self->at) ++self->at; return self; }
static ABI void iterator_destroy(Iterator *self) { self->at=NULL; }

static const RuntimeExport exports[]={
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm",parse},
    {"_ZN3sce4Json5ValueC1Ev",value_init}, {"_ZN3sce4Json5ValueC1ERKS1_",value_copy_init},
    {"_ZN3sce4Json5ValueaSERKS1_",value_assign_op}, {"_ZN3sce4Json5ValueD1Ev",value_destroy},
    {"_ZN3sce4Json5Value5clearEv",value_clear}, {"_ZNK3sce4Json5Value7getTypeEv",value_type},
    {"_ZNK3sce4Json5ValueixEPKc",value_member},
    {"_ZNK3sce4Json5Value8getValueERKNS0_6StringE",value_get_value},
    {"_ZNK3sce4Json5Value11getUIntegerEv",value_unsigned}, {"_ZNK3sce4Json5Value10getIntegerEv",value_integer},
    {"_ZNK3sce4Json5Value9getStringEv",value_string}, {"_ZNK3sce4Json5Value8getArrayEv",value_array},
    {"_ZNK3sce4Json5Value8toStringERNS0_6StringE",value_to_string},
    {"_ZN3sce4Json6StringC1Ev",string_init}, {"_ZN3sce4Json6StringC1EPKc",string_from},
    {"_ZN3sce4Json6StringD1Ev",string_destroy}, {"_ZNK3sce4Json6String5c_strEv",string_c_str},
    {"_ZNK3sce4Json6String6lengthEv",string_length},
    {"_ZN3sce4Json5ArrayC1ERKS1_",array_copy_init}, {"_ZN3sce4Json5ArrayD1Ev",array_destroy},
    {"_ZNK3sce4Json5Array4sizeEv",array_size}, {"_ZNK3sce4Json5Array5beginEv",array_begin},
    {"_ZNK3sce4Json5Array3endEv",array_end}, {"_ZNK3sce4Json5Array8iteratorneERKS2_",iterator_differ},
    {"_ZNK3sce4Json5Array8iteratordeEv",iterator_value}, {"_ZN3sce4Json5Array8iteratorppEv",iterator_next},
    {"_ZN3sce4Json5Array8iteratorD1Ev",iterator_destroy},
};
uintptr_t runtime_json_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
