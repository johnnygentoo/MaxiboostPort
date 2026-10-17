/* libSceFont / libSceFontFt: glyphs drawn by FreeType into the game's own surfaces.
 *
 * The game opens its fonts from memory or from its files, or asks for the PS4's system font set,
 * which is not part of a game dump: the host's own fonts stand in for it (Noto Sans, then Noto
 * Sans CJK for Japanese, or BB_SYSTEM_FONT / BB_SYSTEM_FONT_CJK). Each glyph is rendered at the
 * font's pixel scale with its pen at (x, baseline y) into an 8-bit (coverage) or 32-bit surface,
 * inside the surface's scissor. Libraries, renderers and the memory descriptors the game passes
 * are bookkeeping only; FreeType allocates on the host.
 *
 * Layouts follow shadPS4 (src/core/libraries/font/font.h). BB_FONT_TRACE=1 logs the calls. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_BITMAP_H

#define ERR_INVALID_PARAMETER ((int32_t)0x80460002)
#define ERR_INVALID_LIBRARY ((int32_t)0x80460005)
#define ERR_INVALID_FONT_HANDLE ((int32_t)0x80460006)
#define ERR_INVALID_RENDERER ((int32_t)0x8046000A)
#define ERR_FONT_OPEN_FAILED ((int32_t)0x80460010)
#define ERR_NO_SUPPORT_CODE ((int32_t)0x80460015)
#define MAGIC_LIBRARY 0x464e544cu  /* FNTL */
#define MAGIC_RENDERER 0x464e5452u /* FNTR */
#define MAGIC_FONT 0x464e5446u     /* FNTF */
#define MAX_FACES 3

typedef struct {
    void *buffer;
    int32_t width_byte;
    int8_t pixel_size_byte;
    uint8_t pad0, style, pad2;
    int32_t width, height;
    uint32_t sc_x0, sc_y0, sc_x1, sc_y1;
    uint64_t reserved[11];
} RenderSurface;
typedef struct { float width, height, h_bearing_x, h_bearing_y, h_advance, v_bearing_x, v_bearing_y, v_advance; } GlyphMetrics;
typedef struct {
    const void *stage;
    struct { uint8_t *address; uint32_t width_byte; uint8_t pixel_size_byte, pixel_format; uint16_t pad; } image;
    struct { uint32_t x, y, w, h; } update;
    struct { float bearing_x, bearing_y, advance, stride; uint32_t width, height; } metrics;
} RenderOutput;
typedef struct { float baseline_offset, line_advance, decoration_extent; } HorizontalLayout;
typedef struct { uint16_t kind, attributes; uint32_t region_size; void *region, *mspace; const void *iface;
                 void *destroy, *destroy_ctx, *ctx1, *ctx2; } FontMemory;
typedef struct { uint32_t magic; } Library;
typedef struct { uint32_t magic; } Renderer;
typedef struct {
    uint32_t magic;
    FT_Face face[MAX_FACES];
    unsigned faces;
    void *owned; /* file contents read for sceFontOpenFontFile */
    float scale_w, scale_h, weight_x, weight_y;
} Font;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static FT_Library freetype;
static size_t opened, glyphs, missing;
static int trace=-1;
static int tracing(void) {
    if (trace<0) { const char *v=getenv("BB_FONT_TRACE"); trace=v && v[0]=='1'; }
    return trace;
}
#define TRACE(...) do { if (tracing()) { printf("Font: " __VA_ARGS__); fflush(stdout); } } while (0)
static int ft_ready(void) {
    if (!freetype && FT_Init_FreeType(&freetype)) { freetype=NULL; puts("Runtime: FreeType could not start; no text is drawn"); }
    return freetype!=NULL;
}
static Font *font_of(uintptr_t handle) {
    Font *f=(Font *)handle;
    return f && f->magic==MAGIC_FONT ? f : NULL;
}

/* ---- Memory, libraries, renderers: bookkeeping ---- */
static ABI int32_t memory_init(FontMemory *mem,void *region,uint32_t size,const void *iface,void *mspace,void *destroy,void *ctx) {
    if (!mem) return ERR_INVALID_PARAMETER;
    memset(mem,0,sizeof(*mem));
    mem->kind=0x0f00; mem->region_size=size; mem->region=region; mem->mspace=mspace; mem->iface=iface;
    mem->destroy=destroy; mem->destroy_ctx=ctx;
    return 0;
}
static ABI int32_t memory_term(FontMemory *mem) { if (!mem) return ERR_INVALID_PARAMETER; return 0; }
static const uint32_t library_selection[4]={0x46540001u,0,0,0}, renderer_selection[4]={0x46540002u,0,0,0};
static ABI const void *select_library(int32_t value) { (void)value; return library_selection; }
static ABI const void *select_renderer(int32_t value) { (void)value; return renderer_selection; }
static ABI int32_t create_library(const FontMemory *mem,const void *selection,uint64_t edition,uintptr_t *out) {
    (void)mem; (void)selection; (void)edition;
    if (!out) return ERR_INVALID_PARAMETER;
    Library *l=calloc(1,sizeof(*l));
    if (!l) return ERR_INVALID_PARAMETER;
    l->magic=MAGIC_LIBRARY; *out=(uintptr_t)l;
    pthread_mutex_lock(&lock); ft_ready(); pthread_mutex_unlock(&lock);
    TRACE("CreateLibrary -> %p\n",(void *)l);
    return 0;
}
static ABI int32_t destroy_library(uintptr_t *library) {
    if (!library || !*library || ((Library *)*library)->magic!=MAGIC_LIBRARY) return ERR_INVALID_LIBRARY;
    ((Library *)*library)->magic=0; free((void *)*library); *library=0; return 0;
}
static int library_ok(uintptr_t library) { return library && ((Library *)library)->magic==MAGIC_LIBRARY; }
static ABI int32_t library_ok_call(uintptr_t library) { return library_ok(library) ? 0 : ERR_INVALID_LIBRARY; }
static ABI int32_t support_external(uintptr_t library,uint32_t max,uint32_t formats) { (void)max; (void)formats; return library_ok_call(library); }
static ABI int32_t attach_cache(uintptr_t library,void *buffer,uint32_t size) { (void)buffer; (void)size; return library_ok_call(library); }
static ABI int32_t create_renderer(const FontMemory *mem,const void *selection,uint64_t edition,uintptr_t *out) {
    (void)mem; (void)selection; (void)edition;
    if (!out) return ERR_INVALID_PARAMETER;
    Renderer *r=calloc(1,sizeof(*r));
    if (!r) return ERR_INVALID_PARAMETER;
    r->magic=MAGIC_RENDERER; *out=(uintptr_t)r; return 0;
}
static ABI int32_t destroy_renderer(uintptr_t *renderer) {
    if (!renderer || !*renderer || ((Renderer *)*renderer)->magic!=MAGIC_RENDERER) return ERR_INVALID_RENDERER;
    ((Renderer *)*renderer)->magic=0; free((void *)*renderer); *renderer=0; return 0;
}
static ABI int32_t bind_renderer(uintptr_t font,uintptr_t renderer) {
    if (!font_of(font)) return ERR_INVALID_FONT_HANDLE;
    if (!renderer || ((Renderer *)renderer)->magic!=MAGIC_RENDERER) return ERR_INVALID_RENDERER;
    return 0;
}

/* ---- Opening fonts ---- */
static Font *font_new(void) {
    Font *f=calloc(1,sizeof(*f));
    if (f) { f->magic=MAGIC_FONT; f->scale_w=f->scale_h=16.0f; }
    return f;
}
static int add_face_memory(Font *f,const void *data,uint64_t size,int index) {
    if (f->faces>=MAX_FACES) return 0;
    FT_Face face;
    if (FT_New_Memory_Face(freetype,data,(FT_Long)size,index,&face)) return 0;
    if (!face->charmap) FT_Select_Charmap(face,FT_ENCODING_UNICODE);
    f->face[f->faces++]=face; return 1;
}
static int add_face_file(Font *f,const char *path) {
    if (!path || !path[0] || f->faces>=MAX_FACES) return 0;
    FT_Face face;
    if (FT_New_Face(freetype,path,0,&face)) return 0;
    if (!face->charmap) FT_Select_Charmap(face,FT_ENCODING_UNICODE);
    f->face[f->faces++]=face; return 1;
}
static ABI int32_t open_memory(uintptr_t library,const void *data,uint32_t size,const void *params,uintptr_t *out) {
    (void)params;
    if (!library_ok(library)) return ERR_INVALID_LIBRARY;
    if (!data || !size || !out) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Font *f=ft_ready() ? font_new() : NULL;
    int ok=f && add_face_memory(f,data,size,0);
    pthread_mutex_unlock(&lock);
    if (!ok) { free(f); printf("Runtime: font in memory (%u bytes) could not be opened\n",size); return ERR_FONT_OPEN_FAILED; }
    ++opened; *out=(uintptr_t)f;
    printf("Runtime: font opened from memory (%u bytes): %s %s\n",size,f->face[0]->family_name,f->face[0]->style_name);
    return 0;
}
static ABI int32_t open_file(uintptr_t library,const char *path,uint32_t mode,const void *params,uintptr_t *out) {
    (void)mode; (void)params;
    if (!library_ok(library)) return ERR_INVALID_LIBRARY;
    if (!path || !out) return ERR_INVALID_PARAMETER;
    int fd=(int)runtime_file_open(path,0,0);
    if (fd<0) { printf("Runtime: font file %s not found\n",path); return ERR_FONT_OPEN_FAILED; }
    int64_t size=runtime_file_lseek(fd,0,2);
    void *data=size>0 ? malloc((size_t)size) : NULL;
    if (data && (runtime_file_lseek(fd,0,0)<0 || runtime_file_read(fd,data,(uint64_t)size)!=size)) { free(data); data=NULL; }
    runtime_file_close(fd);
    if (!data) return ERR_FONT_OPEN_FAILED;
    pthread_mutex_lock(&lock);
    Font *f=ft_ready() ? font_new() : NULL;
    int ok=f && add_face_memory(f,data,(uint64_t)size,0);
    pthread_mutex_unlock(&lock);
    if (!ok) { free(f); free(data); printf("Runtime: font file %s could not be opened\n",path); return ERR_FONT_OPEN_FAILED; }
    f->owned=data; ++opened; *out=(uintptr_t)f;
    printf("Runtime: font opened from %s: %s %s\n",path,f->face[0]->family_name,f->face[0]->style_name);
    return 0;
}
/* The PS4's system font set (SST and its Japanese/CJK companions): host fonts in its place. */
static const char *first_existing(const char *const *paths) {
    for (; *paths; ++paths) { FILE *f=fopen(*paths,"rb"); if (f) { fclose(f); return *paths; } }
    return NULL;
}
static ABI int32_t open_font_set(uintptr_t library,uint32_t type,uint32_t mode,const void *params,uintptr_t *out) {
    (void)mode; (void)params;
    if (!library_ok(library)) return ERR_INVALID_LIBRARY;
    if (!out) return ERR_INVALID_PARAMETER;
    static const char *const latin[]={"/usr/share/fonts/noto/NotoSans-Regular.ttf","/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf","/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf","/usr/share/fonts/TTF/DejaVuSans.ttf",NULL};
    static const char *const cjk[]={"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc","/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc","/usr/share/fonts/noto-cjk/NotoSansCJKjp-Regular.otf",NULL};
    const char *primary=getenv("BB_SYSTEM_FONT"), *secondary=getenv("BB_SYSTEM_FONT_CJK");
    if (!primary || !primary[0]) primary=first_existing(latin);
    if (!secondary || !secondary[0]) secondary=first_existing(cjk);
    pthread_mutex_lock(&lock);
    Font *f=ft_ready() ? font_new() : NULL;
    if (f) { add_face_file(f,primary); add_face_file(f,secondary); }
    pthread_mutex_unlock(&lock);
    if (!f || !f->faces) {
        free(f);
        printf("Runtime: system font set %#x: no host font found (install Noto Sans or set BB_SYSTEM_FONT)\n",type);
        return ERR_FONT_OPEN_FAILED;
    }
    ++opened; *out=(uintptr_t)f;
    printf("Runtime: system font set %#x: %s%s%s\n",type,primary ? primary : "",secondary ? " + " : "",secondary ? secondary : "");
    return 0;
}
static ABI int32_t close_font(uintptr_t handle) {
    Font *f=font_of(handle);
    if (!f) return ERR_INVALID_FONT_HANDLE;
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<f->faces;++i) FT_Done_Face(f->face[i]);
    f->magic=0;
    pthread_mutex_unlock(&lock);
    free(f->owned); free(f);
    return 0;
}

/* ---- Scale, weight, layout ---- */
static ABI int32_t scale_pixel(uintptr_t handle,float w,float h) {
    Font *f=font_of(handle);
    if (!f) return ERR_INVALID_FONT_HANDLE;
    if (!(w>0) || !(h>0)) return ERR_INVALID_PARAMETER;
    f->scale_w=w; f->scale_h=h; return 0;
}
static ABI int32_t effect_weight(uintptr_t handle,float x,float y,uint32_t mode) {
    (void)mode;
    Font *f=font_of(handle);
    if (!f) return ERR_INVALID_FONT_HANDLE;
    f->weight_x=x; f->weight_y=y; return 0;
}
static void set_size(Font *f,FT_Face face) {
    FT_Set_Char_Size(face,(FT_F26Dot6)lrintf(f->scale_w*64.0f),(FT_F26Dot6)lrintf(f->scale_h*64.0f),72,72);
}
static ABI int32_t horizontal_layout(uintptr_t handle,HorizontalLayout *layout) {
    Font *f=font_of(handle);
    if (!f) return ERR_INVALID_FONT_HANDLE;
    if (!layout) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    FT_Face face=f->face[0];
    set_size(f,face);
    layout->baseline_offset=face->size->metrics.ascender/64.0f;
    layout->line_advance=face->size->metrics.height/64.0f;
    layout->decoration_extent=FT_IS_SCALABLE(face) ? -FT_MulFix(face->underline_position,face->size->metrics.y_scale)/64.0f : 0.0f;
    pthread_mutex_unlock(&lock);
    TRACE("horizontal layout at %.1fx%.1f: baseline %.2f, line %.2f, decoration %.2f\n",f->scale_w,f->scale_h,
          layout->baseline_offset,layout->line_advance,layout->decoration_extent);
    return 0;
}

/* ---- Rendering ---- */
static void render_surface_init(RenderSurface *s,void *buffer,int32_t width_byte,int32_t pixel_size,int32_t width,int32_t height) {
    if (!s) return;
    memset(s,0,sizeof(*s));
    s->buffer=buffer; s->width_byte=width_byte; s->pixel_size_byte=(int8_t)pixel_size;
    s->width=width; s->height=height;
    s->sc_x0=0; s->sc_y0=0; s->sc_x1=(uint32_t)(width>0 ? width : 0); s->sc_y1=(uint32_t)(height>0 ? height : 0);
}
static ABI void surface_init(RenderSurface *s,void *buffer,int32_t width_byte,int32_t pixel_size,int32_t width,int32_t height) {
    render_surface_init(s,buffer,width_byte,pixel_size,width,height);
}
static ABI void surface_scissor(RenderSurface *s,int32_t x0,int32_t y0,int32_t w,int32_t h) {
    if (!s) return;
    int32_t x1=x0+w, y1=y0+h;
    s->sc_x0=(uint32_t)(x0<0 ? 0 : x0); s->sc_y0=(uint32_t)(y0<0 ? 0 : y0);
    s->sc_x1=(uint32_t)(x1>s->width ? s->width : x1<0 ? 0 : x1); s->sc_y1=(uint32_t)(y1>s->height ? s->height : y1<0 ? 0 : y1);
}
static ABI int32_t render_glyph(uintptr_t handle,uint32_t code,RenderSurface *surf,float x,float y,GlyphMetrics *metrics,RenderOutput *result) {
    Font *f=font_of(handle);
    if (!f) return ERR_INVALID_FONT_HANDLE;
    pthread_mutex_lock(&lock);
    FT_Face face=NULL;
    FT_UInt index=0;
    for (unsigned i=0;i<f->faces && !index;++i) { face=f->face[i]; index=FT_Get_Char_Index(face,code); }
    if (!index) { face=f->face[0]; ++missing; }
    set_size(f,face);
    int32_t r=0;
    if (FT_Load_Glyph(face,index,FT_LOAD_DEFAULT|FT_LOAD_NO_BITMAP)) { pthread_mutex_unlock(&lock); return ERR_NO_SUPPORT_CODE; }
    FT_GlyphSlot g=face->glyph;
    /* Weight: scales where 1 is the font's own (GODZILLA draws its subtitles at 1.04); the
       outline grows by the excess, as a fraction of the size. */
    const float grow_x=f->weight_x>1.0f ? f->weight_x-1.0f : 0.0f, grow_y=f->weight_y>1.0f ? f->weight_y-1.0f : 0.0f;
    if ((grow_x>0 || grow_y>0) && g->format==FT_GLYPH_FORMAT_OUTLINE)
        FT_Outline_EmboldenXY(&g->outline,(FT_Pos)lrintf(grow_x*f->scale_w*64.0f),(FT_Pos)lrintf(grow_y*f->scale_h*64.0f));
    FT_Render_Glyph(g,FT_RENDER_MODE_NORMAL);
    const FT_Bitmap *b=&g->bitmap;
    const float advance=g->advance.x/64.0f;
    if (metrics) {
        metrics->width=g->metrics.width/64.0f; metrics->height=g->metrics.height/64.0f;
        metrics->h_bearing_x=g->metrics.horiBearingX/64.0f; metrics->h_bearing_y=g->metrics.horiBearingY/64.0f;
        metrics->h_advance=advance;
        metrics->v_bearing_x=g->metrics.vertBearingX/64.0f; metrics->v_bearing_y=g->metrics.vertBearingY/64.0f;
        metrics->v_advance=g->metrics.vertAdvance/64.0f;
    }
    /* (x, y) is the top left of the line: the baseline is the ascender below it. GODZILLA draws
       every glyph at (0, 0) of a scratch surface and copies the rectangle reported back. */
    const int32_t ascender=(int32_t)((face->size->metrics.ascender+63)/64);
    int32_t left=(int32_t)lrintf(x)+g->bitmap_left, top=(int32_t)lrintf(y)+ascender-g->bitmap_top;
    uint32_t ux=0, uy=0, uw=0, uh=0;
    if (surf && surf->buffer && b->buffer && b->width && b->rows) {
        const int32_t px=surf->pixel_size_byte>0 ? surf->pixel_size_byte : 1;
        const int32_t x0=left<(int32_t)surf->sc_x0 ? (int32_t)surf->sc_x0 : left;
        const int32_t y0=top<(int32_t)surf->sc_y0 ? (int32_t)surf->sc_y0 : top;
        int32_t x1=left+(int32_t)b->width, y1=top+(int32_t)b->rows;
        if (x1>(int32_t)surf->sc_x1) x1=(int32_t)surf->sc_x1;
        if (y1>(int32_t)surf->sc_y1) y1=(int32_t)surf->sc_y1;
        for (int32_t py=y0;py<y1;++py) {
            unsigned char *row=(unsigned char *)surf->buffer+(size_t)py*(size_t)surf->width_byte;
            const unsigned char *src=b->buffer+(size_t)(py-top)*(size_t)b->pitch;
            for (int32_t pxl=x0;pxl<x1;++pxl) {
                const unsigned char a=src[pxl-left];
                unsigned char *dst=row+(size_t)pxl*(size_t)px;
                for (int32_t k=0;k<px;++k) dst[k]=a; /* coverage; 32-bit: white with coverage */
            }
        }
        if (x1>x0 && y1>y0) { ux=(uint32_t)x0; uy=(uint32_t)y0; uw=(uint32_t)(x1-x0); uh=(uint32_t)(y1-y0); }
    }
    if (result) {
        memset(result,0,sizeof(*result));
        if (surf) {
            /* The image starts at the update rectangle. */
            const uint32_t px=surf->pixel_size_byte>0 ? (uint32_t)surf->pixel_size_byte : 1;
            result->image.address=(uint8_t *)surf->buffer+(size_t)uy*(size_t)surf->width_byte+(size_t)ux*px;
            result->image.width_byte=(uint32_t)surf->width_byte;
            result->image.pixel_size_byte=(uint8_t)surf->pixel_size_byte;
        }
        result->update.x=ux; result->update.y=uy; result->update.w=uw; result->update.h=uh;
        result->metrics.bearing_x=(float)g->bitmap_left; result->metrics.bearing_y=(float)g->bitmap_top;
        result->metrics.advance=advance; result->metrics.stride=surf ? (float)surf->width_byte : 0.0f;
        result->metrics.width=b->width; result->metrics.height=b->rows;
    }
    ++glyphs;
    pthread_mutex_unlock(&lock);
    TRACE("glyph U+%04X buffer %p weight %.3f,%.3f at (%.1f, %.1f) size %.1fx%.1f -> %ux%u left %d top %d, advance %.1f; surface %dx%d stride %d px %d scissor %u,%u-%u,%u\n",
          code,surf ? surf->buffer : NULL,f->weight_x,f->weight_y,x,y,f->scale_w,f->scale_h,b->width,b->rows,g->bitmap_left,g->bitmap_top,advance,
          surf ? surf->width : 0,surf ? surf->height : 0,surf ? surf->width_byte : 0,surf ? surf->pixel_size_byte : 0,
          surf ? surf->sc_x0 : 0,surf ? surf->sc_y0 : 0,surf ? surf->sc_x1 : 0,surf ? surf->sc_y1 : 0);
    return r;
}

static const RuntimeExport exports[]={
    {"sceFontMemoryInit",memory_init}, {"sceFontMemoryTerm",memory_term},
    {"sceFontSelectLibraryFt",select_library}, {"sceFontSelectRendererFt",select_renderer},
    {"sceFontCreateLibraryWithEdition",create_library}, {"sceFontDestroyLibrary",destroy_library},
    {"sceFontSupportSystemFonts",library_ok_call}, {"sceFontSupportExternalFonts",support_external},
    {"sceFontAttachDeviceCacheBuffer",attach_cache},
    {"sceFontCreateRendererWithEdition",create_renderer}, {"sceFontDestroyRenderer",destroy_renderer},
    {"sceFontBindRenderer",bind_renderer},
    {"sceFontOpenFontMemory",open_memory}, {"sceFontOpenFontFile",open_file}, {"sceFontOpenFontSet",open_font_set},
    {"sceFontCloseFont",close_font},
    {"sceFontSetupRenderScalePixel",scale_pixel}, {"sceFontSetupRenderEffectWeight",effect_weight},
    {"sceFontGetHorizontalLayout",horizontal_layout},
    {"sceFontRenderSurfaceInit",surface_init}, {"sceFontRenderSurfaceSetScissor",surface_scissor},
    {"sceFontRenderCharGlyphImage",render_glyph},
};
uintptr_t runtime_font_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_font_report(void) {
    printf("Runtime: fonts opened=%zu, glyphs drawn=%zu, glyphs missing from the font=%zu\n",opened,glyphs,missing);
}
#else
uintptr_t runtime_font_resolve(const char *name) { (void)name; return 0; }
void runtime_font_report(void) {}
#endif
