/* libSceNgs2: the PS4's software audio renderer, which GODZILLA plays all its sound through.
 *
 * A system holds racks; a rack holds voices. Sampler voices play waveforms (ATRAC9 from the game's
 * .at9 files, or PCM) from the game's memory; their ports patch them into the inputs of submixer,
 * reverb or mastering voices, through a matrix of channel levels and a volume. sceNgs2SystemRender
 * renders one grain (256 samples by default) through that graph and the mastering voices write it
 * into the game's buffers, which the game then hands to sceAudioOutOutput.
 *
 * Handles are pointers to host objects: the context buffers the game allocates for them are not
 * used. Envelopes, filters, distortion and reverb are not applied yet (the dry sound passes);
 * sample rates are converted by linear interpolation. Layouts follow the PS4 SDK as documented by
 * shadPS4 (src/core/libraries/ngs2), parameter IDs and semantics as KytyPS5/AnyPS5 implement them;
 * BB_NGS2_TRACE=1 logs every call with the structures the game passes. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define ERR_INVALID_OPTION_SIZE ((int32_t)0x804A0081)
#define ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A8010)
#define ERR_INVALID_OUT_SIZE ((int32_t)0x804A8011)
#define ERR_INVALID_SYSTEM_HANDLE ((int32_t)0x804A0230)
#define ERR_INVALID_RACK_ID ((int32_t)0x804A0260)
#define ERR_INVALID_RACK_HANDLE ((int32_t)0x804A0261)
#define ERR_INVALID_VOICE_HANDLE ((int32_t)0x804A0300)
#define ERR_INVALID_VOICE_INDEX ((int32_t)0x804A8305)
#define ERR_INVALID_EVENT_TYPE ((int32_t)0x804A0303)
#define ERR_INVALID_PORT_INDEX ((int32_t)0x804A0322)
#define ERR_INVALID_MATRIX_INDEX ((int32_t)0x804A0330)
#define ERR_INVALID_WAVEFORM_DATA ((int32_t)0x804A8430)
#define ERR_INVALID_WAVEFORM_FORMAT ((int32_t)0x804A8431)
#define ERR_UNKNOWN_WAVEFORM_FORMAT ((int32_t)0x804A8432)
#define ERR_INVALID_BUFFER ((int32_t)0x804A0050)

enum { RACK_SAMPLER=0x1000, RACK_SUBMIXER=0x2000, RACK_REVERB=0x2001, RACK_MASTERING=0x3000,
       RACK_CUSTOM_SAMPLER=0x4001, RACK_CUSTOM_SUBMIXER=0x4002, RACK_CUSTOM_MASTERING=0x4003 };
enum { WAVE_PCM_I16L=0x12, WAVE_PCM_F32L=0x18, WAVE_ATRAC9=0x40 };
/* PS4 voice events are an enumeration (the PS5's are bit flags): GODZILLA plays its mastering
 * voices with event 0. */
enum { EVENT_PLAY=0, EVENT_STOP=1, EVENT_STOP_IMM=2, EVENT_KILL=3, EVENT_PAUSE=4, EVENT_RESUME=5 };
enum { FLAG_INUSE=1, FLAG_PLAYING=2, FLAG_PAUSED=4, FLAG_STOPPED=8, FLAG_EMPTY=0x20 };
enum { STATE_EMPTY=0, STATE_PLAYING=FLAG_INUSE|FLAG_PLAYING, STATE_PAUSED=FLAG_INUSE|FLAG_PAUSED,
       STATE_STOPPED=FLAG_INUSE|FLAG_PLAYING|FLAG_STOPPED };
enum { PARAM_MATRIX_LEVELS=1, PARAM_PORT_VOLUME=2, PARAM_PORT_MATRIX=3, PARAM_PORT_DELAY=4,
       PARAM_PATCH=5, PARAM_EVENT=6, PARAM_CALLBACK=7 };
/* Rack-specific parameters: the rack id in the high half (0x1000 -> 0x10000000), then the index. */
enum { SAMPLER_SETUP=0, SAMPLER_WAVEFORM_BLOCKS=1, SAMPLER_WAVEFORM_ADDRESS=2,
       SAMPLER_WAVEFORM_FRAME_OFFSET=3, SAMPLER_EXIT_LOOP=4, SAMPLER_PITCH=5 };
enum { BLOCKS_CONTINUE=1, BLOCKS_APPEND=2, BLOCKS_RESET=4 };
enum { CALLBACK_BLOCK_END=1, CALLBACK_BLOCK_REPEAT=2 };
#define MAX_CH 8
#define MAX_PORTS 8
#define MAX_MATRICES 8
#define MAX_BLOCKS 64
#define MAX_GRAIN 1024
#define MAGIC_SYSTEM 0x4e475331u /* NGS1 */
#define MAGIC_RACK 0x4e475352u   /* NGSR */
#define MAGIC_VOICE 0x4e475356u  /* NGSV */

/* PS4 structures. */
typedef struct { void *buffer; uint64_t size; uintptr_t reserved[5]; uintptr_t user; } ContextBufferInfo;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain, num_grain, sample_rate, reserved[6]; } SystemOption;
typedef struct { uint64_t size; char name[16]; uint32_t flags, max_grain, max_voices, max_input_delay_blocks,
                 max_matrices, max_ports, reserved[20]; } RackOption;
typedef struct { void *buffer; uint64_t size; uint32_t waveform_type, channels; } RenderBufferInfo;
typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
typedef struct { uint32_t type, channels, sample_rate, config, frame_offset, frame_margin; } WaveformFormat;
typedef struct { uint32_t offset, size, repeats, skip, samples, reserved; uintptr_t user; } WaveformBlock;
typedef struct {
    WaveformFormat format;
    uint32_t data_offset, data_size, loop_begin, loop_end, samples, unit_size, unit_samples,
             units_per_frame, frame_size, frame_samples, delay_samples, blocks;
    WaveformBlock block[4];
} WaveformInfo;
typedef struct { uintptr_t callback_data, voice; uint32_t flag, reserved; uintptr_t user_data;
                 const void *block_data; uint32_t block_size, repeated, attributes, reserved2; } CallbackInfo;
typedef ABI void (*CallbackHandler)(const CallbackInfo *);
_Static_assert(sizeof(WaveformBlock)==32 && sizeof(WaveformInfo)==200, "PS4 Ngs2 waveform layout");

typedef struct System System;
typedef struct Rack Rack;
typedef struct Voice Voice;
typedef struct { Voice *dest; float volume; int32_t matrix; } Port;
typedef struct { WaveformBlock b; uint32_t repeated; } Block;
struct Voice {
    uint32_t magic, index;
    Rack *rack;
    uint32_t state, channels, rate, type;
    unsigned char config[4];
    const unsigned char *data;
    Block blocks[MAX_BLOCKS];
    uint32_t nblocks, accepts;
    /* Decoding: the current block's position (samples played of it, bytes read of it). */
    uint32_t played, read_bytes, skip_left;
    void *at9;
    Atrac9CodecInfo at9_info;
    uint32_t at9_frame_in_superframe;
    float frame[MAX_CH*256*4];
    uint32_t frame_count, frame_pos;
    float current[MAX_CH], next[MAX_CH];
    int primed;
    double phase;
    float pitch;
    uint64_t decoded_samples, decoded_bytes;
    Port ports[MAX_PORTS];
    float matrix[MAX_MATRICES][MAX_CH*MAX_CH];
    uint32_t matrix_levels[MAX_MATRICES];
    uint32_t output_id;
    int failed;
    CallbackHandler callback;
    uintptr_t callback_data;
    uint32_t callback_flags;
    float samples[MAX_CH*MAX_GRAIN];
    int has_samples, rendered, rendering;
};
struct Rack {
    uint32_t magic, id, max_voices, uid;
    System *system;
    Voice *voices;
    Rack *next;
    char name[16];
};
struct System {
    uint32_t magic, sample_rate, grain, uid;
    uint64_t renders;
    Rack *racks;
    pthread_mutex_t lock;
    char name[16];
};
static int trace=-1;
static uint32_t next_uid=1;
static size_t renders, voices_played, waveforms_parsed;
static int tracing(void) {
    if (trace<0) { const char *v=getenv("BB_NGS2_TRACE"); trace=v && v[0]=='1'; }
    return trace;
}
#define TRACE(...) do { if (tracing()) { printf("Ngs2: " __VA_ARGS__); fflush(stdout); } } while (0)
static void dump(const char *what,const void *data,size_t size) {
    if (!tracing() || !data) return;
    printf("Ngs2:   %s (%zu bytes):",what,size);
    for (size_t i=0;i<size && i<256;++i) printf("%s%02x",i%16 ? " " : "\n    ",((const unsigned char *)data)[i]);
    printf("\n");
}

static System *system_of(uintptr_t handle) {
    System *s=(System *)handle;
    return s && s->magic==MAGIC_SYSTEM ? s : NULL;
}
static Rack *rack_of(uintptr_t handle) {
    Rack *r=(Rack *)handle;
    return r && r->magic==MAGIC_RACK ? r : NULL;
}
static Voice *voice_of(uintptr_t handle) {
    Voice *v=(Voice *)handle;
    return v && v->magic==MAGIC_VOICE ? v : NULL;
}
static int is_sampler(const Rack *r) { return r->id==RACK_SAMPLER || r->id==RACK_CUSTOM_SAMPLER; }
static int is_mastering(const Rack *r) { return r->id==RACK_MASTERING || r->id==RACK_CUSTOM_MASTERING; }

/* ---- System ---- */
static ABI int32_t system_query(const SystemOption *option,ContextBufferInfo *info) {
    TRACE("SystemQueryBufferSize(option=%p)\n",(const void *)option);
    if (option) dump("system option",option,option->size<256 ? option->size : 64);
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    info->size=64*1024; return 0;
}
static ABI int32_t system_create(const SystemOption *option,const ContextBufferInfo *buffer,uintptr_t *out) {
    TRACE("SystemCreate(option=%p, buffer=%p)\n",(const void *)option,(const void *)buffer);
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (option && option->size!=sizeof(SystemOption)) {
        printf("Ngs2: system option of %llu bytes (expected %zu)\n",(unsigned long long)option->size,sizeof(SystemOption));
        dump("system option",option,64);
    }
    System *s=calloc(1,sizeof(*s));
    if (!s) return ERR_INVALID_BUFFER;
    s->magic=MAGIC_SYSTEM; s->sample_rate=48000; s->grain=256; s->uid=next_uid++;
    if (option && option->size>=sizeof(SystemOption)) {
        if (option->sample_rate) s->sample_rate=option->sample_rate;
        if (option->num_grain && option->num_grain<=MAX_GRAIN) s->grain=option->num_grain;
        memcpy(s->name,option->name,sizeof(s->name)-1);
    }
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s->lock,&attr); pthread_mutexattr_destroy(&attr);
    *out=(uintptr_t)s;
    printf("Runtime: Ngs2 system '%s': %u Hz, grain %u samples\n",s->name,s->sample_rate,s->grain);
    return 0;
}
static ABI int32_t system_create_alloc(const SystemOption *option,const void *allocator,uintptr_t *out) {
    (void)allocator; return system_create(option,NULL,out);
}
static void voice_release_decoder(Voice *v) { if (v->at9) { Atrac9ReleaseHandle(v->at9); v->at9=NULL; } }
static void rack_free(Rack *r) {
    for (uint32_t i=0;i<r->max_voices;++i) { voice_release_decoder(&r->voices[i]); r->voices[i].magic=0; }
    free(r->voices); r->magic=0; free(r);
}
static ABI int32_t system_destroy(uintptr_t handle,ContextBufferInfo *info) {
    System *s=system_of(handle);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    pthread_mutex_lock(&s->lock);
    for (Rack *r=s->racks,*next;r;r=next) { next=r->next; rack_free(r); }
    s->racks=NULL; s->magic=0;
    pthread_mutex_unlock(&s->lock);
    if (info) memset(info,0,sizeof(*info));
    /* The system's memory stays: a render racing the destroy finds a dead magic, not freed memory. */
    return 0;
}

/* ---- Racks ---- */
static ABI int32_t rack_query(uint32_t id,const RackOption *option,ContextBufferInfo *info) {
    TRACE("RackQueryBufferSize(id=%#x, option=%p)\n",id,(const void *)option);
    if (option) dump("rack option",option,option->size<512 ? option->size : 128);
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    info->size=64*1024; return 0;
}
static void voice_reset(Voice *v) {
    voice_release_decoder(v);
    v->state=STATE_EMPTY; v->channels=0; v->rate=0; v->type=0; v->data=NULL;
    v->nblocks=0; v->accepts=1; v->played=v->read_bytes=v->skip_left=0;
    v->frame_count=v->frame_pos=0; v->primed=0; v->phase=0; v->pitch=1.0f;
    v->decoded_samples=v->decoded_bytes=0;
    for (int p=0;p<MAX_PORTS;++p) v->ports[p]=(Port){NULL,1.0f,-1};
    memset(v->matrix_levels,0,sizeof(v->matrix_levels));
    v->output_id=0; v->callback=NULL;
}
static ABI int32_t rack_create(uintptr_t system,uint32_t id,const RackOption *option,const ContextBufferInfo *buffer,uintptr_t *out) {
    (void)buffer;
    TRACE("RackCreate(system=%#lx, id=%#x, option=%p)\n",(unsigned long)system,id,(const void *)option);
    System *s=system_of(system);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (id!=RACK_SAMPLER && id!=RACK_SUBMIXER && id!=RACK_REVERB && id!=RACK_MASTERING &&
        id!=RACK_CUSTOM_SAMPLER && id!=RACK_CUSTOM_SUBMIXER && id!=RACK_CUSTOM_MASTERING) {
        printf("Ngs2: rack id %#x is not implemented\n",id);
        return ERR_INVALID_RACK_ID;
    }
    uint32_t voices=id==RACK_SAMPLER ? 256 : 1;
    if (option) {
        if (option->size<sizeof(RackOption)) {
            printf("Ngs2: rack option of %llu bytes (expected at least %zu)\n",(unsigned long long)option->size,sizeof(RackOption));
            dump("rack option",option,option->size<512 ? option->size : 128);
        }
        if (option->max_voices && option->max_voices<=4096) voices=option->max_voices;
    }
    Rack *r=calloc(1,sizeof(*r));
    if (!r) return ERR_INVALID_BUFFER;
    r->voices=calloc(voices,sizeof(Voice));
    if (!r->voices) { free(r); return ERR_INVALID_BUFFER; }
    r->magic=MAGIC_RACK; r->id=id; r->max_voices=voices; r->system=s; r->uid=next_uid++;
    if (option) memcpy(r->name,option->name,sizeof(r->name)-1);
    for (uint32_t i=0;i<voices;++i) {
        Voice *v=&r->voices[i];
        v->magic=MAGIC_VOICE; v->index=i; v->rack=r;
        voice_reset(v);
        /* Submixer and mastering voices keep their channel count from their setup; until then
           they pass stereo. */
        if (!is_sampler(r)) v->channels=2;
    }
    pthread_mutex_lock(&s->lock);
    Rack **tail=&s->racks;
    while (*tail) tail=&(*tail)->next;
    *tail=r;
    pthread_mutex_unlock(&s->lock);
    *out=(uintptr_t)r;
    printf("Runtime: Ngs2 rack %#x '%s': %u voices\n",id,r->name,voices);
    return 0;
}
static ABI int32_t rack_create_alloc(uintptr_t system,uint32_t id,const RackOption *option,const void *allocator,uintptr_t *out) {
    (void)allocator; return rack_create(system,id,option,NULL,out);
}
static ABI int32_t rack_destroy(uintptr_t handle,ContextBufferInfo *info) {
    Rack *r=rack_of(handle);
    if (!r) return ERR_INVALID_RACK_HANDLE;
    System *s=r->system;
    pthread_mutex_lock(&s->lock);
    for (Rack **p=&s->racks;*p;p=&(*p)->next) if (*p==r) { *p=r->next; break; }
    /* Ports of other voices patched into this rack's voices lose their destination. */
    for (Rack *o=s->racks;o;o=o->next)
        for (uint32_t i=0;i<o->max_voices;++i)
            for (int p=0;p<MAX_PORTS;++p)
                if (o->voices[i].ports[p].dest && o->voices[i].ports[p].dest->rack==r) o->voices[i].ports[p].dest=NULL;
    rack_free(r);
    pthread_mutex_unlock(&s->lock);
    if (info) memset(info,0,sizeof(*info));
    return 0;
}
static ABI int32_t rack_voice(uintptr_t handle,uint32_t index,uintptr_t *out) {
    Rack *r=rack_of(handle);
    if (!r) return ERR_INVALID_RACK_HANDLE;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (index>=r->max_voices) return ERR_INVALID_VOICE_INDEX;
    *out=(uintptr_t)&r->voices[index]; return 0;
}
/* OrbisNgs2RackInfo: name[16], rack handle, buffer info, owner system, type, rack id, uid, ... */
static ABI int32_t rack_info(uintptr_t handle,unsigned char *info,uint64_t size) {
    Rack *r=rack_of(handle);
    if (!r) return ERR_INVALID_RACK_HANDLE;
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    if (size<168) return ERR_INVALID_OUT_SIZE;
    memset(info,0,size);
    memcpy(info,r->name,16);
    uintptr_t self=(uintptr_t)r, owner=(uintptr_t)r->system;
    memcpy(info+16,&self,8); memcpy(info+88,&owner,8);
    uint32_t values[]={0,r->id,r->uid,64,MAX_GRAIN,r->max_voices,r->max_voices,1,MAX_MATRICES,MAX_PORTS};
    memcpy(info+96,values,sizeof(values));
    uint32_t active=0;
    for (uint32_t i=0;i<r->max_voices;++i) active+=r->voices[i].state!=STATE_EMPTY;
    memcpy(info+160,&active,4);
    return 0;
}

/* ---- Waveforms ---- */
static uint32_t le32(const unsigned char *p) { return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0]|p[1]<<8); }
static const unsigned char ATRAC9_GUID[16]={0xd2,0x42,0xe1,0x47,0xba,0x36,0x8d,0x4d,0x88,0xfc,0x61,0x65,0x4f,0x8c,0x83,0x6c};
/* Adds the block of samples [begin, begin+samples) of an ATRAC9 waveform: whole superframes,
 * starting one frame early when it can (the decoder's overlap), the extra samples skipped. */
static int at9_range(WaveformInfo *w,uint64_t begin,uint32_t samples,uint32_t repeats) {
    if (!samples) return 1;
    if (w->blocks>=4) return 0;
    uint64_t preroll=begin<w->unit_samples ? begin : w->unit_samples;
    uint64_t first=(begin-preroll)/w->frame_samples, skip=begin-first*w->frame_samples;
    uint64_t frames=(skip+samples+w->frame_samples-1)/w->frame_samples;
    uint64_t offset=first*w->frame_size, bytes=frames*w->frame_size;
    if (offset>w->data_size) return 0;
    if (bytes>w->data_size-offset) bytes=w->data_size-offset;
    w->block[w->blocks++]=(WaveformBlock){(uint32_t)(w->data_offset+offset),(uint32_t)bytes,repeats,(uint32_t)skip,samples,0,0};
    return 1;
}
static int32_t parse_riff(const unsigned char *d,uint64_t size,WaveformInfo *w) {
    memset(w,0,sizeof(*w));
    if (size<12 || memcmp(d,"RIFF",4) || memcmp(d+8,"WAVE",4)) return ERR_INVALID_WAVEFORM_DATA;
    const unsigned char *fmt=NULL, *fact=NULL, *smpl=NULL;
    uint32_t fmt_size=0, fact_size=0, smpl_size=0, data_offset=0, data_size=0;
    for (uint64_t p=12;p+8<=size;) {
        uint32_t length=le32(d+p+4);
        const unsigned char *body=d+p+8;
        if (!memcmp(d+p,"fmt ",4)) { fmt=body; fmt_size=length; }
        else if (!memcmp(d+p,"fact",4)) { fact=body; fact_size=length; }
        else if (!memcmp(d+p,"smpl",4)) { smpl=body; smpl_size=length; }
        else if (!memcmp(d+p,"data",4)) { data_offset=(uint32_t)(p+8); data_size=length; break; }
        p+=8+(uint64_t)length+(length&1);
    }
    if (!fmt || fmt_size<16 || !data_offset) return ERR_INVALID_WAVEFORM_DATA;
    if (data_offset+(uint64_t)data_size>size) data_size=(uint32_t)(size-data_offset);
    const uint16_t tag=le16(fmt), channels=le16(fmt+2);
    const uint32_t rate=le32(fmt+4);
    w->data_offset=data_offset; w->data_size=data_size;
    if (tag==0xfffe && fmt_size>=52 && !memcmp(fmt+24,ATRAC9_GUID,16)) {
        unsigned char config[4]; memcpy(config,fmt+44,4);
        void *h=Atrac9GetHandle();
        Atrac9CodecInfo c;
        int ok=h && !Atrac9InitDecoder(h,config) && !Atrac9GetCodecInfo(h,&c);
        if (h) Atrac9ReleaseHandle(h);
        if (!ok || !fact || fact_size<8) return ERR_INVALID_WAVEFORM_FORMAT;
        w->format=(WaveformFormat){WAVE_ATRAC9,(uint32_t)c.channels,(uint32_t)c.samplingRate,
                                   (uint32_t)config[0]<<24|(uint32_t)config[1]<<16|(uint32_t)config[2]<<8|config[3],0,0};
        w->samples=le32(fact);
        w->unit_size=(uint32_t)(c.superframeSize/c.framesInSuperframe);
        w->unit_samples=(uint32_t)c.frameSamples;
        w->units_per_frame=(uint32_t)c.framesInSuperframe;
        w->frame_size=(uint32_t)c.superframeSize;
        w->frame_samples=(uint32_t)(c.frameSamples*c.framesInSuperframe);
        w->delay_samples=le32(fact+4);
        const uint32_t skip=fact_size>=12 ? le32(fact+8) : w->delay_samples;
        w->blocks=1;
        w->block[0]=(WaveformBlock){data_offset,data_size,0,skip,w->samples,0,0};
        if (smpl && smpl_size>=36 && le32(smpl+28)>=1 && smpl_size>=60) {
            const unsigned char *loop=smpl+36;
            uint64_t begin=le32(loop+8), end=(uint64_t)le32(loop+12)+1, waveform_end=(uint64_t)skip+w->samples;
            uint32_t plays=le32(loop+20);
            if (begin>=skip && begin<end && end<=waveform_end) {
                w->loop_begin=(uint32_t)(begin-skip); w->loop_end=(uint32_t)(end-skip);
                w->blocks=0;
                if (!at9_range(w,skip,w->loop_begin,0) ||
                    !at9_range(w,begin,(uint32_t)(end-begin),plays ? plays-1 : UINT32_MAX) ||
                    !at9_range(w,end,(uint32_t)(waveform_end-end),0)) return ERR_INVALID_WAVEFORM_DATA;
            }
        }
        return 0;
    }
    if (tag==1 && le16(fmt+14)==16 && channels && channels<=MAX_CH) {
        w->format=(WaveformFormat){WAVE_PCM_I16L,channels,rate,0,0,0};
        w->samples=data_size/(2u*channels);
        w->unit_size=w->frame_size=2u*channels; w->unit_samples=w->frame_samples=w->units_per_frame=1;
        w->blocks=1;
        w->block[0]=(WaveformBlock){data_offset,w->samples*2u*channels,0,0,w->samples,0,0};
        if (smpl && smpl_size>=60 && le32(smpl+28)>=1) {
            const unsigned char *loop=smpl+36;
            uint32_t begin=le32(loop+8), end=le32(loop+12)+1, plays=le32(loop+20);
            if (begin<end && end<=w->samples) {
                w->loop_begin=begin; w->loop_end=end; w->blocks=0;
                const uint32_t frame=2u*channels;
                if (begin) w->block[w->blocks++]=(WaveformBlock){data_offset,begin*frame,0,0,begin,0,0};
                w->block[w->blocks++]=(WaveformBlock){data_offset+begin*frame,(end-begin)*frame,plays ? plays-1 : UINT32_MAX,0,end-begin,0,0};
                if (end<w->samples) w->block[w->blocks++]=(WaveformBlock){data_offset+end*frame,(w->samples-end)*frame,0,0,w->samples-end,0,0};
            }
        }
        return 0;
    }
    printf("Ngs2: waveform format %#x (%u channels) is not implemented\n",tag,channels);
    return ERR_UNKNOWN_WAVEFORM_FORMAT;
}
static ABI int32_t parse_data(const void *data,uint64_t size,WaveformInfo *info) {
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    if (!data) return ERR_INVALID_WAVEFORM_DATA;
    int32_t r=parse_riff(data,size,info);
    ++waveforms_parsed;
    TRACE("ParseWaveformData(%p, %llu) = %#x: type %#x, %u ch, %u Hz, %u samples, %u blocks\n",data,(unsigned long long)size,
          (uint32_t)r,info->format.type,info->format.channels,info->format.sample_rate,info->samples,info->blocks);
    return r;
}
static ABI int32_t parse_file(const char *path,uint64_t offset,WaveformInfo *info) {
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    memset(info,0,sizeof(*info));
    if (!path) return ERR_INVALID_WAVEFORM_DATA;
    int fd=(int)runtime_file_open(path,0,0);
    if (fd<0) { printf("Ngs2: ParseWaveformFile: %s not found\n",path); return ERR_INVALID_WAVEFORM_DATA; }
    /* The headers are at the start; 64 KiB covers them (the data chunk is not read). */
    unsigned char *head=malloc(65536);
    int64_t n=head && runtime_file_lseek(fd,(int64_t)offset,0)>=0 ? runtime_file_read(fd,head,65536) : -1;
    runtime_file_close(fd);
    int32_t r=n>0 ? parse_riff(head,(uint64_t)n,info) : ERR_INVALID_WAVEFORM_DATA;
    free(head);
    if (!r) {
        info->data_offset+=(uint32_t)offset;
        for (uint32_t i=0;i<info->blocks;++i) info->block[i].offset+=(uint32_t)offset;
    }
    ++waveforms_parsed;
    TRACE("ParseWaveformFile(%s, %llu) = %#x\n",path,(unsigned long long)offset,(uint32_t)r);
    return r;
}

/* ---- Voices ---- */
/* A voice that cannot play its waveform stops, with the reason logged once per voice. */
static void voice_fail(Voice *v,const char *why) {
    if (!v->failed) {
        v->failed=1;
        printf("Ngs2: voice %u of rack %#x stopped: %s (type %#x, %u ch, %u Hz, %u blocks)\n",v->index,v->rack->id,why,
               v->type,v->channels,v->rate,v->nblocks);
    }
    v->state=STATE_EMPTY; v->nblocks=0;
}
static void block_restart(Voice *v) {
    v->played=0; v->read_bytes=0; v->frame_count=v->frame_pos=0; v->at9_frame_in_superframe=0;
    v->skip_left=v->nblocks ? v->blocks[0].b.skip : 0;
    /* One decoder per waveform (setup): LibAtrac9 builds its tables in InitDecoder, too slow for
       every block and repeat. A block starts a frame early (at9_range), which settles the
       overlap. */
    if (v->type==WAVE_ATRAC9 && !v->at9) {
        v->at9=Atrac9GetHandle();
        if (v->at9 && !Atrac9InitDecoder(v->at9,v->config)) Atrac9GetCodecInfo(v->at9,&v->at9_info);
        else { voice_release_decoder(v); voice_fail(v,"ATRAC9 configuration not accepted"); }
    }
}
static void set_event(Voice *v,uint32_t event) {
    switch (event) {
    case EVENT_PLAY:
        if (v->state==STATE_EMPTY || v->state==STATE_STOPPED) {
            v->state=STATE_PLAYING; v->primed=0; v->phase=0;
            ++voices_played;
        }
        break;
    case EVENT_STOP: if (v->state==STATE_PLAYING || v->state==STATE_PAUSED) v->state=STATE_STOPPED; break;
    case EVENT_STOP_IMM: case EVENT_KILL: v->state=STATE_EMPTY; break;
    case EVENT_PAUSE: if (v->state==STATE_PLAYING) v->state=STATE_PAUSED; break;
    case EVENT_RESUME: if (v->state==STATE_PAUSED) v->state=STATE_PLAYING; break;
    }
}
static int32_t sampler_setup(Voice *v,const unsigned char *p,uint32_t size) {
    if (size<8+sizeof(WaveformFormat)) return ERR_INVALID_OUT_SIZE;
    WaveformFormat f; memcpy(&f,p+8,sizeof(f));
    const int was_playing=v->state!=STATE_EMPTY;
    voice_release_decoder(v);
    v->state=STATE_EMPTY; v->nblocks=0; v->accepts=1; v->data=NULL; v->frame_count=v->frame_pos=0;
    v->decoded_samples=v->decoded_bytes=0; v->pitch=1.0f; v->primed=0; v->phase=0; v->failed=0;
    (void)was_playing;
    if (!f.channels || f.channels>MAX_CH) return ERR_INVALID_WAVEFORM_FORMAT;
    v->type=f.type; v->channels=f.channels; v->rate=f.sample_rate ? f.sample_rate : 48000;
    v->config[0]=(unsigned char)(f.config>>24); v->config[1]=(unsigned char)(f.config>>16);
    v->config[2]=(unsigned char)(f.config>>8); v->config[3]=(unsigned char)f.config;
    if (v->type!=WAVE_ATRAC9 && v->type!=WAVE_PCM_I16L && v->type!=WAVE_PCM_F32L) {
        printf("Ngs2: sampler waveform type %#x is not implemented\n",v->type);
        return ERR_UNKNOWN_WAVEFORM_FORMAT;
    }
    return 0;
}
static int32_t sampler_blocks(Voice *v,const unsigned char *p,uint32_t size) {
    if (size<32) return ERR_INVALID_OUT_SIZE;
    const void *data; uint32_t flags, count; const WaveformBlock *blocks;
    memcpy(&data,p+8,8); memcpy(&flags,p+16,4); memcpy(&count,p+20,4); memcpy(&blocks,p+24,8);
    TRACE("  waveform blocks: data=%p flags=%#x count=%u\n",data,flags,count);
    if (count && (!data || !blocks)) return ERR_INVALID_WAVEFORM_DATA;
    const int append=(flags&BLOCKS_APPEND) && v->nblocks;
    if (!append) {
        v->nblocks=0; v->data=data;
    } else if (data!=v->data) {
        /* Appended blocks of another buffer: offsets are rebased onto the first buffer. */
        const intptr_t delta=(const unsigned char *)data-v->data;
        for (uint32_t i=0;i<count && v->nblocks<MAX_BLOCKS;++i) {
            Block *b=&v->blocks[v->nblocks++];
            b->b=blocks[i]; b->b.offset=(uint32_t)((intptr_t)b->b.offset+delta); b->repeated=0;
        }
        v->accepts=(flags&BLOCKS_CONTINUE)!=0;
        return 0;
    }
    for (uint32_t i=0;i<count && v->nblocks<MAX_BLOCKS;++i) {
        dump("block",&blocks[i],sizeof(*blocks));
        v->blocks[v->nblocks].b=blocks[i]; v->blocks[v->nblocks].repeated=0; ++v->nblocks;
    }
    v->accepts=(flags&BLOCKS_CONTINUE)!=0;
    if (!append) {
        /* A new waveform after one stopped inside a superframe: a fresh decoder (rare). */
        if (v->at9 && v->at9_frame_in_superframe) voice_release_decoder(v);
        block_restart(v);
    }
    return 0;
}
static int32_t matrix_levels(Voice *v,const unsigned char *p,uint32_t size) {
    if (size<24) return ERR_INVALID_OUT_SIZE;
    uint32_t id, count; const float *levels;
    memcpy(&id,p+8,4); memcpy(&count,p+12,4); memcpy(&levels,p+16,8);
    if (id>=MAX_MATRICES) return ERR_INVALID_MATRIX_INDEX;
    if (count>MAX_CH*MAX_CH || (count && !levels)) return ERR_INVALID_MATRIX_INDEX;
    memcpy(v->matrix[id],levels,count*sizeof(float)); v->matrix_levels[id]=count;
    return 0;
}
static int32_t voice_param(Voice *v,const ParamHeader *h) {
    const unsigned char *p=(const unsigned char *)h;
    const uint32_t id=h->id;
    if (tracing()) { printf("Ngs2: voice %p (rack %#x #%u) param %#x size %u\n",(void *)v,v->rack->id,v->index,id,h->size); dump("param",h,h->size); }
    switch (id) {
    case PARAM_MATRIX_LEVELS: return matrix_levels(v,p,h->size);
    case PARAM_PORT_VOLUME: {
        uint32_t port; float level; memcpy(&port,p+8,4); memcpy(&level,p+12,4);
        if (port>=MAX_PORTS) return ERR_INVALID_PORT_INDEX;
        v->ports[port].volume=level; return 0;
    }
    case PARAM_PORT_MATRIX: {
        uint32_t port; int32_t matrix; memcpy(&port,p+8,4); memcpy(&matrix,p+12,4);
        if (port>=MAX_PORTS) return ERR_INVALID_PORT_INDEX;
        if (matrix<-1 || matrix>=MAX_MATRICES) return ERR_INVALID_MATRIX_INDEX;
        v->ports[port].matrix=matrix; return 0;
    }
    case PARAM_PORT_DELAY: return 0; /* not applied */
    case PARAM_PATCH: {
        uint32_t port, input; uintptr_t dest; memcpy(&port,p+8,4); memcpy(&input,p+12,4); memcpy(&dest,p+16,8);
        if (port>=MAX_PORTS) return ERR_INVALID_PORT_INDEX;
        Voice *d=dest ? voice_of(dest) : NULL;
        if (dest && !d) return ERR_INVALID_VOICE_HANDLE;
        v->ports[port].dest=d; return 0;
    }
    case PARAM_EVENT: {
        uint32_t event; memcpy(&event,p+8,4);
        if (event>EVENT_RESUME) return ERR_INVALID_EVENT_TYPE;
        set_event(v,event);
        return 0;
    }
    case PARAM_CALLBACK: {
        memcpy(&v->callback,p+8,8); memcpy(&v->callback_data,p+16,8); memcpy(&v->callback_flags,p+24,4);
        return 0;
    }
    }
    const uint32_t rack=id>>16, index=id&0xffff;
    if ((rack==RACK_SAMPLER || rack==RACK_CUSTOM_SAMPLER) && is_sampler(v->rack)) {
        switch (index) {
        case SAMPLER_SETUP: return sampler_setup(v,p,h->size);
        case SAMPLER_WAVEFORM_BLOCKS: return sampler_blocks(v,p,h->size);
        case SAMPLER_EXIT_LOOP:
            for (uint32_t i=0;i<v->nblocks;++i) if (v->blocks[i].b.repeats) v->blocks[i].b.repeats=0;
            return 0;
        case SAMPLER_PITCH: { float ratio; memcpy(&ratio,p+8,4); if (ratio>0 && ratio<64) v->pitch=ratio; return 0; }
        default: return 0; /* envelope, distortion, filters, peak meter: not applied */
        }
    }
    if ((rack==RACK_SUBMIXER || rack==RACK_REVERB || rack==RACK_MASTERING || rack==RACK_CUSTOM_SUBMIXER ||
         rack==RACK_CUSTOM_MASTERING) && index==0 && h->size>=16) {
        uint32_t channels; memcpy(&channels,p+8,4);
        if (rack==RACK_REVERB && h->size>=20) memcpy(&channels,p+12,4); /* input, output channels */
        if (channels && channels<=MAX_CH) v->channels=channels;
        return 0;
    }
    if ((rack==RACK_MASTERING || rack==RACK_CUSTOM_MASTERING) && index==5 && h->size>=12) {
        memcpy(&v->output_id,p+8,4); return 0;
    }
    return 0; /* effect parameters (reverb, EQ, limiter, user effects): not applied */
}
static ABI int32_t voice_control(uintptr_t handle,const ParamHeader *list) {
    Voice *v=voice_of(handle);
    if (!v) return ERR_INVALID_VOICE_HANDLE;
    if (!list) return 0;
    System *s=v->rack->system;
    pthread_mutex_lock(&s->lock);
    int32_t r=0;
    for (const ParamHeader *h=list;;) {
        int32_t e=voice_param(v,h);
        if (e && !r) r=e;
        if (!h->next) break;
        h=(const ParamHeader *)((const unsigned char *)h+h->next);
    }
    pthread_mutex_unlock(&s->lock);
    if (r) TRACE("VoiceControl -> %#x\n",(uint32_t)r);
    return r;
}
static uint32_t state_flags(const Voice *v) {
    uint32_t flags=v->state;
    if (v->state==STATE_PLAYING && is_sampler(v->rack) && !v->nblocks && v->accepts) flags|=FLAG_EMPTY;
    return flags;
}
static ABI int32_t voice_state(uintptr_t handle,unsigned char *state,uint64_t size) {
    Voice *v=voice_of(handle);
    if (!v) return ERR_INVALID_VOICE_HANDLE;
    if (!state) return ERR_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&v->rack->system->lock);
    memset(state,0,size);
    uint32_t flags=state_flags(v);
    if (size>=4) memcpy(state,&flags,4);
    /* OrbisNgs2SamplerVoiceState (48 bytes on the PS4, 56 on the PS5): flags, envelope height,
       peak, then decoded samples and bytes (how a streaming game knows what it may refill),
       the block's user data and the waveform. */
    static int size_noted;
    if (!size_noted && is_sampler(v->rack)) { size_noted=1; TRACE("VoiceGetState: sampler state of %llu bytes\n",(unsigned long long)size); }
    if (is_sampler(v->rack) && size>=48) {
        float envelope=1.0f; uintptr_t user=v->nblocks ? v->blocks[0].b.user : 0; const void *data=v->data;
        memcpy(state+4,&envelope,4);
        memcpy(state+16,&v->decoded_samples,8); memcpy(state+24,&v->decoded_bytes,8);
        memcpy(state+32,&user,8); memcpy(state+40,&data,8);
    }
    pthread_mutex_unlock(&v->rack->system->lock);
    return 0;
}
static ABI int32_t voice_state_flags(uintptr_t handle,uint32_t *flags) {
    Voice *v=voice_of(handle);
    if (!v) return ERR_INVALID_VOICE_HANDLE;
    if (!flags) return ERR_INVALID_OUT_ADDRESS;
    *flags=state_flags(v); return 0;
}

/* ---- Rendering ---- */
/* The current block is done: repeat it, or move to the next one; tells the game through its
 * callback. False when the voice has nothing more to play now. */
static int block_finished(Voice *v) {
    Block *b=&v->blocks[0];
    const int repeat=b->b.repeats!=0;
    CallbackInfo info={v->callback_data,(uintptr_t)v,repeat ? CALLBACK_BLOCK_REPEAT : CALLBACK_BLOCK_END,0,
                       b->b.user,v->data ? v->data+b->b.offset : NULL,b->b.size,b->repeated,0,0};
    if (repeat) {
        if (b->b.repeats!=UINT32_MAX) --b->b.repeats;
        ++b->repeated;
        info.repeated=b->repeated;
    } else {
        memmove(&v->blocks[0],&v->blocks[1],(v->nblocks-1)*sizeof(Block));
        --v->nblocks;
    }
    block_restart(v);
    if (v->callback && (v->callback_flags&info.flag)) v->callback(&info);
    return v->nblocks>0 && v->state==STATE_PLAYING;
}
/* Decodes the next frames of the current block into v->frame; false at its end. */
static int decode_more(Voice *v) {
    if (!v->nblocks || !v->data) return 0;
    Block *b=&v->blocks[0];
    const unsigned char *src=v->data+b->b.offset;
    v->frame_count=v->frame_pos=0;
    const uint32_t channels=v->channels;
    while (!v->frame_count) {
        if (v->played>=b->b.samples) {
            /* The block ends inside a superframe: its other frames are decoded and dropped, so
               the decoder starts the next block at a superframe (LibAtrac9 counts them). */
            int16_t drop[256*MAX_CH];
            while (v->type==WAVE_ATRAC9 && v->at9 && v->at9_frame_in_superframe && v->read_bytes<b->b.size) {
                int used=0;
                if (Atrac9Decode(v->at9,src+v->read_bytes,drop,&used)) break;
                v->read_bytes+=(uint32_t)used;
                if (++v->at9_frame_in_superframe==(uint32_t)v->at9_info.framesInSuperframe) v->at9_frame_in_superframe=0;
            }
            return 0;
        }
        uint32_t produced=0;
        if (v->type==WAVE_ATRAC9) {
            if (!v->at9 || v->read_bytes>=b->b.size) return 0;
            const uint32_t frame=(uint32_t)v->at9_info.frameSamples, ch=(uint32_t)v->at9_info.channels;
            if (ch!=channels || frame*ch>256*MAX_CH) { voice_fail(v,"ATRAC9 channel count differs from the setup"); return 0; }
            int16_t pcm[256*MAX_CH];
            int used=0;
            const int e=Atrac9Decode(v->at9,src+v->read_bytes,pcm,&used);
            if (e) {
                char why[96]; snprintf(why,sizeof(why),"ATRAC9 frame at block byte %u does not decode (%d)",v->read_bytes,e);
                voice_fail(v,why); return 0;
            }
            v->read_bytes+=(uint32_t)used; v->decoded_bytes+=(uint32_t)used;
            if (++v->at9_frame_in_superframe==(uint32_t)v->at9_info.framesInSuperframe) {
                /* Superframes are padded to their size. */
                const uint32_t superframe=(uint32_t)v->at9_info.superframeSize;
                v->read_bytes=(v->read_bytes+superframe-1)/superframe*superframe;
                v->at9_frame_in_superframe=0;
            }
            uint32_t start=v->skip_left<frame ? v->skip_left : frame;
            v->skip_left-=start;
            for (uint32_t s=start;s<frame;++s) for (uint32_t c=0;c<ch;++c)
                v->frame[(s-start)*ch+c]=pcm[s*ch+c]/32768.0f;
            produced=frame-start;
        } else {
            const uint32_t bytes=v->type==WAVE_PCM_I16L ? 2 : 4, stride=bytes*channels;
            /* PCM blocks: the skipped samples come first, then up to 1024 samples at a time. */
            if (v->skip_left) { v->read_bytes+=v->skip_left*stride; v->skip_left=0; }
            uint32_t left=(b->b.size>v->read_bytes ? b->b.size-v->read_bytes : 0)/stride;
            uint32_t n=left<1024 ? left : 1024;
            if (!n) return 0;
            for (uint32_t s=0;s<n;++s) for (uint32_t c=0;c<channels;++c) {
                const unsigned char *q=src+v->read_bytes+(uint64_t)s*stride+c*bytes;
                if (bytes==2) v->frame[s*channels+c]=(int16_t)le16(q)/32768.0f;
                else memcpy(&v->frame[s*channels+c],q,4);
            }
            v->read_bytes+=n*stride; v->decoded_bytes+=n*stride;
            produced=n;
        }
        const uint32_t room=b->b.samples-v->played;
        if (produced>room) produced=room;
        v->frame_count=produced;
    }
    return 1;
}
/* The next source sample frame of the voice (all channels); false when it ran out. */
static int next_source(Voice *v,float *out) {
    /* Blocks that end without a sample (empty, or a repeat of one) would loop for ever. */
    unsigned empty_blocks=0;
    for (;;) {
        if (v->frame_pos<v->frame_count) {
            memcpy(out,&v->frame[v->frame_pos*v->channels],v->channels*sizeof(float));
            ++v->frame_pos; ++v->played; ++v->decoded_samples;
            return 1;
        }
        if (decode_more(v)) continue;
        if (!v->nblocks || v->state!=STATE_PLAYING) return 0;
        if (!v->played && ++empty_blocks>v->nblocks+1) { voice_fail(v,"its blocks hold no samples"); return 0; }
        if (!block_finished(v)) return 0;
    }
}
static void render_sampler(Voice *v,uint32_t grain,uint32_t system_rate) {
    const uint32_t ch=v->channels;
    const double step=(double)v->rate*v->pitch/system_rate;
    if (!v->primed) {
        if (!next_source(v,v->current)) memset(v->current,0,sizeof(v->current));
        if (!next_source(v,v->next)) memcpy(v->next,v->current,sizeof(v->next));
        v->primed=1;
    }
    int ended=0;
    for (uint32_t i=0;i<grain;++i) {
        const float t=(float)v->phase;
        for (uint32_t c=0;c<ch;++c) v->samples[c*grain+i]=v->current[c]+(v->next[c]-v->current[c])*t;
        v->phase+=step;
        while (v->phase>=1.0) {
            v->phase-=1.0;
            memcpy(v->current,v->next,sizeof(v->current));
            if (!ended && !next_source(v,v->next)) { ended=1; memset(v->next,0,sizeof(v->next)); }
            else if (ended) memset(v->next,0,sizeof(v->next));
        }
    }
    v->has_samples=1;
    if (ended && !v->nblocks && !v->accepts) v->state=STATE_EMPTY;
}
static float default_level(uint32_t from_channels,uint32_t src,uint32_t to_channels,uint32_t dst) {
    if (from_channels==to_channels) return src==dst ? 1.0f : 0.0f;
    if (from_channels==1) return to_channels==2 ? 0.70710678f : (dst==2 ? 1.0f : 0.0f); /* mono: centre */
    if (to_channels==1) return 0.70710678f;
    if (from_channels==2) return src==dst ? 1.0f : 0.0f;                                 /* stereo into surround: front */
    if (to_channels==2) {                                                                 /* surround into stereo */
        if (src<2) return src==dst ? 1.0f : 0.0f;
        if (src==2) return 0.70710678f;                                                   /* centre */
        if (src==3) return 0.0f;                                                          /* LFE */
        return (src%2==0)==(dst==0) ? 0.70710678f : 0.0f;                                 /* surround and back */
    }
    return src==dst ? 1.0f : 0.0f;
}
static void render_voice(System *s,Voice *v);
static void mix_inputs(System *s,Voice *dest) {
    const uint32_t grain=s->grain;
    for (Rack *r=s->racks;r;r=r->next)
        for (uint32_t i=0;i<r->max_voices;++i) {
            Voice *src=&r->voices[i];
            for (int p=0;p<MAX_PORTS;++p) {
                const Port *port=&src->ports[p];
                if (port->dest!=dest || port->volume==0.0f) continue;
                render_voice(s,src);
                if (!src->has_samples) continue;
                const float *m=port->matrix>=0 && src->matrix_levels[port->matrix] ? src->matrix[port->matrix] : NULL;
                const uint32_t rows=m ? src->matrix_levels[port->matrix]/src->channels : dest->channels;
                for (uint32_t d=0;d<dest->channels && d<rows;++d)
                    for (uint32_t c=0;c<src->channels;++c) {
                        const float level=port->volume*(m ? m[d*src->channels+c] : default_level(src->channels,c,dest->channels,d));
                        if (level==0.0f) continue;
                        for (uint32_t k=0;k<grain;++k) dest->samples[d*grain+k]+=src->samples[c*grain+k]*level;
                    }
                dest->has_samples=1;
            }
        }
}
static void render_voice(System *s,Voice *v) {
    if (v->rendered || v->rendering) return; /* a patch cycle passes nothing back */
    v->rendering=1;
    v->has_samples=0;
    if (v->state==STATE_PLAYING && v->channels) {
        memset(v->samples,0,(size_t)v->channels*s->grain*sizeof(float));
        if (is_sampler(v->rack)) render_sampler(v,s->grain,s->sample_rate);
        else mix_inputs(s,v);
    } else if (v->state==STATE_STOPPED) v->state=STATE_EMPTY;
    v->rendering=0; v->rendered=1;
}
static ABI int32_t system_render(uintptr_t handle,const RenderBufferInfo *buffers,uint32_t count) {
    System *s=system_of(handle);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    if (count && !buffers) return ERR_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&s->lock);
    const uint32_t grain=s->grain;
    static float mix[4][MAX_CH*MAX_GRAIN];
    for (uint32_t i=0;i<count && i<4;++i) memset(mix[i],0,sizeof(mix[i]));
    for (Rack *r=s->racks;r;r=r->next)
        for (uint32_t i=0;i<r->max_voices;++i) r->voices[i].rendered=0;
    for (Rack *r=s->racks;r;r=r->next) {
        if (!is_mastering(r)) continue;
        for (uint32_t i=0;i<r->max_voices;++i) {
            Voice *v=&r->voices[i];
            render_voice(s,v);
            if (!v->has_samples || v->output_id>=count || v->output_id>=4) continue;
            const RenderBufferInfo *out=&buffers[v->output_id];
            const uint32_t outc=out->channels<=MAX_CH ? out->channels : MAX_CH;
            for (uint32_t d=0;d<outc;++d) for (uint32_t c=0;c<v->channels;++c) {
                const float level=default_level(v->channels,c,outc,d);
                if (level==0.0f) continue;
                for (uint32_t k=0;k<grain;++k) mix[v->output_id][k*outc+d]+=v->samples[c*grain+k]*level;
            }
        }
    }
    /* Voices nobody listens to still advance, as on the PS4 (their callbacks and states). */
    for (Rack *r=s->racks;r;r=r->next)
        for (uint32_t i=0;i<r->max_voices;++i) render_voice(s,&r->voices[i]);
    for (uint32_t i=0;i<count;++i) {
        const RenderBufferInfo *out=&buffers[i];
        if (!out->buffer || !out->channels) continue;
        const uint32_t outc=out->channels<=MAX_CH ? out->channels : MAX_CH;
        const uint64_t samples=(uint64_t)grain*outc;
        if (out->waveform_type==WAVE_PCM_F32L && out->size>=samples*4) {
            if (i<4) memcpy(out->buffer,mix[i],samples*4); else memset(out->buffer,0,samples*4);
        } else if (out->waveform_type==WAVE_PCM_I16L && out->size>=samples*2) {
            int16_t *pcm=out->buffer;
            for (uint64_t k=0;k<samples;++k) {
                float f=i<4 ? mix[i][k] : 0.0f;
                f=f>1.0f ? 1.0f : f<-1.0f ? -1.0f : f;
                pcm[k]=(int16_t)lrintf(f*32767.0f);
            }
        } else if (out->buffer && out->size) memset(out->buffer,0,out->size);
    }
    /* BB_NGS2_WAV=<file>: the first output as rendered (32-bit float, the output's channels),
       to check the sound without listening; the header's sizes are left open. */
    static FILE *wav; static int wav_checked;
    if (!wav_checked) {
        wav_checked=1;
        const char *path=getenv("BB_NGS2_WAV");
        if (path && path[0] && count && (wav=fopen(path,"wb"))) {
            const uint32_t ch=buffers[0].channels<=MAX_CH ? buffers[0].channels : MAX_CH, rate=s->sample_rate;
            const uint32_t header[]={0x46464952u,0xffffffffu,0x45564157u,0x20746d66u,16,(ch<<16)|3u,rate,rate*ch*4,(32u<<16)|(ch*4),0x61746164u,0xffffffffu};
            fwrite(header,sizeof(header),1,wav);
        }
    }
    if (wav && count && buffers[0].channels) fwrite(mix[0],sizeof(float),(size_t)grain*(buffers[0].channels<=MAX_CH ? buffers[0].channels : MAX_CH),wav);
    ++s->renders; ++renders;
    pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t system_info(uintptr_t handle,unsigned char *info,uint64_t size) {
    System *s=system_of(handle);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    if (!info) return ERR_INVALID_OUT_ADDRESS;
    if (size<136) return ERR_INVALID_OUT_SIZE;
    memset(info,0,size);
    memcpy(info,s->name,16);
    uintptr_t self=(uintptr_t)s; memcpy(info+16,&self,8);
    uint32_t values[]={s->uid,64,MAX_GRAIN,0};
    memcpy(info+88,values,sizeof(values));
    memcpy(info+120,&s->renders,8); memcpy(info+128,&s->sample_rate,4); memcpy(info+132,&s->grain,4);
    return 0;
}
static ABI int32_t system_set_grain(uintptr_t handle,uint32_t grain) {
    System *s=system_of(handle);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    if (!grain || grain>MAX_GRAIN) return ERR_INVALID_OUT_SIZE;
    pthread_mutex_lock(&s->lock); s->grain=grain; pthread_mutex_unlock(&s->lock);
    return 0;
}
static ABI int32_t system_set_rate(uintptr_t handle,uint32_t rate) {
    System *s=system_of(handle);
    if (!s) return ERR_INVALID_SYSTEM_HANDLE;
    if (rate) s->sample_rate=rate;
    return 0;
}
static ABI int32_t system_lock(uintptr_t handle) { System *s=system_of(handle); if (!s) return ERR_INVALID_SYSTEM_HANDLE; pthread_mutex_lock(&s->lock); return 0; }
static ABI int32_t system_unlock(uintptr_t handle) { System *s=system_of(handle); if (!s) return ERR_INVALID_SYSTEM_HANDLE; pthread_mutex_unlock(&s->lock); return 0; }

static const RuntimeExport exports[]={
    {"sceNgs2SystemQueryBufferSize",system_query}, {"sceNgs2SystemCreate",system_create},
    {"sceNgs2SystemCreateWithAllocator",system_create_alloc}, {"sceNgs2SystemDestroy",system_destroy},
    {"sceNgs2SystemRender",system_render}, {"sceNgs2SystemGetInfo",system_info},
    {"sceNgs2SystemSetGrainSamples",system_set_grain}, {"sceNgs2SystemSetSampleRate",system_set_rate},
    {"sceNgs2SystemLock",system_lock}, {"sceNgs2SystemUnlock",system_unlock},
    {"sceNgs2RackQueryBufferSize",rack_query}, {"sceNgs2RackCreate",rack_create},
    {"sceNgs2RackCreateWithAllocator",rack_create_alloc}, {"sceNgs2RackDestroy",rack_destroy},
    {"sceNgs2RackGetVoiceHandle",rack_voice}, {"sceNgs2RackGetInfo",rack_info},
    {"sceNgs2VoiceControl",voice_control}, {"sceNgs2VoiceGetState",voice_state},
    {"sceNgs2VoiceGetStateFlags",voice_state_flags},
    {"sceNgs2ParseWaveformData",parse_data}, {"sceNgs2ParseWaveformFile",parse_file},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ngs2_report(void) {
    printf("Runtime: Ngs2 renders=%zu, voices played=%zu, waveforms parsed=%zu\n",renders,voices_played,waveforms_parsed);
}
#else
uintptr_t runtime_ngs2_resolve(const char *name) { (void)name; return 0; }
void runtime_ngs2_report(void) {}
#endif
