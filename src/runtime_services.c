/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>

#define USER_ID 1
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* ---- UserService ---- */
/* User 1 is logged in from the start; users 2..4 log in as further gamepads connect (couch co-op,
 * runtime_pad.c), each with a login event. Names: BB_USER_NAME for user 1 (default "Pilot"),
 * BB_USER2_NAME.. for the others (default "Pilot 2".."Pilot 4"). */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static int logged_in(int32_t id) {
    int32_t ids[4];
    runtime_users_logged_in(ids);
    for (int i=0;i<4;++i) if (ids[i]==id) return 1;
    return 0;
}
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    runtime_users_logged_in(ids); return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (!name || id<1 || id>4 || !logged_in(id)) return USER_INVALID_ARGUMENT;
    char key[16], fallback[16];
    if (id==1) snprintf(key,sizeof(key),"BB_USER_NAME"); else snprintf(key,sizeof(key),"BB_USER%d_NAME",id);
    if (id==1) snprintf(fallback,sizeof(fallback),"Pilot"); else snprintf(fallback,sizeof(fallback),"Pilot %d",id);
    const char *value=getenv(key) && *getenv(key) ? getenv(key) : fallback;
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    const int user=pending ? USER_ID : runtime_users_next_login();
    if (!user) return USER_NO_EVENT;
    if (user!=USER_ID) printf("Runtime: UserService: user %d logged in\n",user);
    event[0]=0; event[1]=user; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("BB_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }
static ABI int32_t http_fail(void) { return HTTP_NETWORK; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_object(void) { return new_id(); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
/* Status: 0 none, 1 initialized, 2 running, 3 finished. An opened dialog runs until the game's
 * next status poll and finishes as if its first button (OK / Yes) was pressed; a progress bar
 * (persistent) runs until the game closes it, as the game writes meanwhile. */
typedef struct { const char *name; int initialized, status, persistent; uint32_t mode; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0,0},{"MsgDialog",0,0,0,0},{"SaveDataDialog",0,0,0,0},
                         {"NpProfileDialog",0,0,0,0},{"NpCommerceDialog",0,0,0,0},{"ImeDialog",0,0,0,0},
                         {"ErrorDialog",0,0,0,0},{"NpFriendListDialog",0,0,0,0},{"SigninDialog",0,0,0,0},
                         {"InvitationDialog",0,0,0,0},{"WebBrowserDialog",0,0,0,0}};
static int common_initialized;
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
/* BB_DIALOG_TRACE=1: every dialog call and its result (a status poll only when it changes). */
static int dialog_tracing(void) {
    static int trace=-1;
    if (trace<0) { const char *v=getenv("BB_DIALOG_TRACE"); trace=v && v[0]=='1'; }
    return trace;
}
#define DTRACE(...) do { if (dialog_tracing()) { printf("Dialog: " __VA_ARGS__); fflush(stdout); } } while (0)
static int32_t dialog_init(int i) {
    int32_t r=dialogs[i].initialized ? (int32_t)0x80B80004 : 0;
    if (!r) { dialogs[i].initialized=1; dialogs[i].status=1; }
    DTRACE("%s Initialize -> %#x\n",dialogs[i].name,(uint32_t)r);
    return r;
}
static int32_t dialog_open_mode(int i,uint32_t mode,int persistent) {
    if (!dialogs[i].initialized) { DTRACE("%s Open: not initialized\n",dialogs[i].name); return DIALOG_NOT_INITIALIZED; }
    DTRACE("%s Open mode %u%s\n",dialogs[i].name,mode,persistent ? " (progress bar: runs until closed)" : "");
    dialogs[i].status=2; dialogs[i].mode=mode; dialogs[i].persistent=persistent;
    return 0;
}
static int32_t dialog_open(int i) { return dialog_open_mode(i,0,0); }
static int32_t dialog_status(int i) {
    static int last[sizeof(dialogs)/sizeof(*dialogs)]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    if (dialogs[i].status==2 && !dialogs[i].persistent) dialogs[i].status=3; /* answered: OK / Yes */
    if (last[i]!=dialogs[i].status) { last[i]=dialogs[i].status; DTRACE("%s status %d\n",dialogs[i].name,dialogs[i].status); }
    return dialogs[i].status;
}
static int32_t dialog_term(int i) {
    DTRACE("%s Terminate (initialized %d)\n",dialogs[i].name,dialogs[i].initialized);
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { \
        if (p && dialog_tracing()) { \
            const unsigned char *b=p; printf("Dialog: %s Open param:",dialogs[i].name); \
            for (int k=0;k<96;++k) { printf("%s%02x",k%16 ? " " : "\n    ",b[k]); } \
            printf("\n"); \
        } \
        return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4) DIALOG(friends,7) DIALOG(signin,8) DIALOG(invitation,9) DIALOG(browser,10)
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_status(void) { return dialog_status(1); }
static ABI int32_t msg_term(void) { return dialog_term(1); }
/* OrbisMsgDialogParam: base (0x30), size (8), mode (1 user message, 2 progress bar, 3 system). */
static ABI int32_t msg_open(const unsigned char *param) {
    uint32_t mode=0;
    if (param) memcpy(&mode,param+0x38,4);
    return dialog_open_mode(1,mode,mode==2);
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_status(void) { return dialog_status(2); }
static ABI int32_t save_term(void) { return dialog_term(2); }
/* OrbisSaveDataDialogParam: base (0x30), size, mode (1 list, 2 user message, 3 system message,
 * 4 error code, 5 progress bar), type (save, load, delete), animation, items (0x48), ...,
 * user data (0x70). Items: user, title id, dir names (0x10) and their count (0x18), new item,
 * focus, focus dir name (0x30). A list answers with the first listed save (the one in focus). */
static struct { char dir_name[32]; uintptr_t user_data; } save_dialog;
static ABI int32_t save_open(const unsigned char *param) {
    uint32_t mode=0;
    memset(&save_dialog,0,sizeof(save_dialog));
    if (param) {
        const unsigned char *items;
        memcpy(&mode,param+0x34,4); memcpy(&items,param+0x48,8); memcpy(&save_dialog.user_data,param+0x70,8);
        if (mode==1 && items) {
            const char *focus, *dirs; uint32_t count;
            memcpy(&dirs,items+0x10,8); memcpy(&count,items+0x18,4); memcpy(&focus,items+0x30,8);
            const char *chosen=focus && focus[0] ? focus : dirs && count ? dirs : NULL;
            if (chosen) snprintf(save_dialog.dir_name,sizeof(save_dialog.dir_name),"%.31s",chosen);
            DTRACE("SaveDataDialog list of %u saves: answers '%s'\n",count,save_dialog.dir_name);
        }
    }
    return dialog_open_mode(2,mode,mode==5);
}
/* Closing a dialog finishes it; its result is the first button (OK / Yes), a list dialog's is
 * "cancelled" (nothing chosen). OrbisCommonDialog results: 0 OK, 1 user cancelled. */
static int32_t dialog_close(int i) {
    DTRACE("%s Close\n",dialogs[i].name);
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].status=3; return 0;
}
static ABI int32_t msg_close(void) { return dialog_close(1); }
static ABI int32_t save_close(const void *param) { (void)param; return dialog_close(2); }
static ABI int32_t commerce_close(void) { return dialog_close(4); }
static ABI int32_t friends_close(void) { return dialog_close(7); }
/* OrbisMsgDialogResult / OrbisSaveDataDialogResult: mode, result, button id, ... */
static ABI int32_t msg_result(int32_t *result) {
    DTRACE("MsgDialog GetResult\n");
    if (!result) return (int32_t)0x80B80006; /* ARG_NULL */
    result[0]=(int32_t)dialogs[1].mode; result[1]=0; result[2]=1; /* OK, the first button (Yes) */
    return 0;
}
/* OrbisSaveDataDialogResult: mode, result, button, dir name* (0x10), param* (0x18), user data (0x20). */
static ABI int32_t save_result(unsigned char *result) {
    DTRACE("SaveDataDialog GetResult: mode %u, '%s'\n",dialogs[2].mode,save_dialog.dir_name);
    if (!result) return (int32_t)0x80B80006;
    const int32_t values[3]={(int32_t)dialogs[2].mode,0,1};
    memcpy(result,values,sizeof(values));
    char *dir; memcpy(&dir,result+0x10,8);
    if (dir && save_dialog.dir_name[0]) memcpy(dir,save_dialog.dir_name,strlen(save_dialog.dir_name)+1);
    memcpy(result+0x20,&save_dialog.user_data,8);
    return 0;
}
static ABI int32_t save_ready(void) { DTRACE("SaveDataDialog IsReadyToDisplay\n"); return 1; }
static ABI int32_t save_progress(int32_t target,uint32_t rate) { DTRACE("SaveDataDialog progress %d: %u\n",target,rate); return 0; }
static ABI int32_t cancelled_result(int32_t *result) { if (result) result[0]=1; return 0; }
/* ErrorDialog: the system's error message for a code; the code is logged, the dialog finishes. */
typedef struct { int32_t size, error_code, user, reserved; } ErrorDialogParam;
static ABI int32_t error_dialog_init(void) { return dialog_init(6); }
static ABI int32_t error_dialog_open(const ErrorDialogParam *param) {
    if (param) printf("Runtime: ErrorDialog for error 0x%08x\n",(uint32_t)param->error_code);
    return dialog_open(6);
}
static ABI int32_t error_dialog_status(void) { return dialog_status(6); }
static ABI int32_t error_dialog_close(void) { return dialog_close(6); }
static ABI int32_t error_dialog_term(void) { return dialog_term(6); }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("BB_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    if (!bbgpu_text_input_begin(initial,prompt[0] ? prompt : "Text")) {
        const char *name=getenv("BB_USER_NAME");
        ime_complete(0,name ? name : initial[0] ? initial : "Hunter");
    } else printf("Runtime: ImeDialog opened: type in the game window, Enter to confirm, Esc to cancel\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=bbgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies: recorded in BB_TROPHY_DIR (scripts/trophies.py), shown in the overlay ----
 * trophies.tsv lists the game's trophies (id, grade, hidden, name, detail, platinum), from the
 * dump; earned.tsv gets "id<TAB>Unix time" for each one the first time the game unlocks it. The
 * platinum, which the PS4 itself awards, follows once every trophy that counts toward it is
 * earned: the base game's, not an add-on's (Bloodborne's 34-39, The Old Hunters). */
#define TROPHY_MAX 128
typedef struct { char grade; char name[160]; } TrophyEntry;
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static TrophyEntry trophy_list[TROPHY_MAX];
static unsigned char trophy_known[TROPHY_MAX], trophy_earned[TROPHY_MAX], trophy_counts[TROPHY_MAX];
static int trophy_loaded, trophy_counts_known;
static void trophy_load(void) {
    if (trophy_loaded) return;
    trophy_loaded=1;
    const char *dir=getenv("BB_TROPHY_DIR");
    if (!dir || !dir[0]) return;
    char path[1024], line[1024];
    snprintf(path,sizeof(path),"%s/trophies.tsv",dir);
    FILE *f=fopen(path,"r");
    while (f && fgets(line,sizeof(line),f)) {
        char *fields[6]={line}; int n=1;
        for (char *c=line;*c && n<6;++c) if (*c=='\t') { *c=0; fields[n++]=c+1; }
        int id=atoi(fields[0]);
        if (n<4 || id<0 || id>=TROPHY_MAX) continue;
        fields[3][strcspn(fields[3],"\r\n")]=0;
        trophy_known[id]=1;
        trophy_list[id].grade=fields[1][0];
        if (n==6) { trophy_counts_known=1; trophy_counts[id]=fields[5][0]=='1'; }
        snprintf(trophy_list[id].name,sizeof(trophy_list[id].name),"%s",fields[3]);
    }
    if (f) fclose(f);
    snprintf(path,sizeof(path),"%s/earned.tsv",dir);
    f=fopen(path,"r");
    while (f && fgets(line,sizeof(line),f)) {
        int id=atoi(line);
        if (id>=0 && id<TROPHY_MAX) trophy_earned[id]=1;
    }
    if (f) fclose(f);
}
/* Records a trophy the first time; true when it was new. Called with trophy_lock held. */
static int trophy_record(int id) {
    if (id<0 || id>=TROPHY_MAX || trophy_earned[id]) return 0;
    trophy_earned[id]=1;
    const char *dir=getenv("BB_TROPHY_DIR");
    if (dir && dir[0]) {
        char path[1024];
        snprintf(path,sizeof(path),"%s/earned.tsv",dir);
        FILE *f=fopen(path,"a");
        if (f) { fprintf(f,"%d\t%lld\n",id,(long long)time(NULL)); fclose(f); }
    }
    const char *name=trophy_known[id] && trophy_list[id].name[0] ? trophy_list[id].name : NULL;
    char fallback[32], icon[1100];
    if (!name) { snprintf(fallback,sizeof(fallback),"#%d",id); name=fallback; }
    printf("Runtime: trophy %d unlocked: %s\n",id,name);
    if (dir && dir[0]) snprintf(icon,sizeof(icon),"%s/icons/TROP%03d.rgba",dir,id);
    bbgpu_trophy_notice(id,trophy_known[id] ? trophy_list[id].grade : '-',name,dir && dir[0] ? icon : NULL);
    return 1;
}
/* The platinum's id once every trophy that counts toward it is earned, else -1. A list without
 * that column (written by an older scripts/trophies.py) counts every other trophy. */
static int trophy_platinum_due(void) {
    int platinum=-1;
    for (int i=0;i<TROPHY_MAX;++i) {
        if (!trophy_known[i]) continue;
        if (trophy_list[i].grade=='P') platinum=i;
        else if ((trophy_counts[i] || !trophy_counts_known) && !trophy_earned[i]) return -1;
    }
    return platinum>=0 && !trophy_earned[platinum] ? platinum : -1;
}
/* The game's first trophy call (which one is logged). BB_TROPHY_PREVIEW=<id> then shows that
 * trophy's notice without recording it, to check how it looks. */
static void trophy_first_call(const char *function) {
    static int called;
    pthread_mutex_lock(&trophy_lock);
    if (!called) {
        called=1;
        trophy_load();
        printf("Runtime: trophies: first call %s\n",function);
        const char *preview=getenv("BB_TROPHY_PREVIEW"), *dir=getenv("BB_TROPHY_DIR");
        const int id=preview && preview[0] ? atoi(preview) : -1;
        if (id>=0 && id<TROPHY_MAX) {
            char icon[1100];
            if (dir && dir[0]) snprintf(icon,sizeof(icon),"%s/icons/TROP%03d.rgba",dir,id);
            bbgpu_trophy_notice(id,trophy_known[id] ? trophy_list[id].grade : '-',
                                trophy_known[id] && trophy_list[id].name[0] ? trophy_list[id].name : "Trophy preview",
                                dir && dir[0] ? icon : NULL);
        }
    }
    pthread_mutex_unlock(&trophy_lock);
}
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    trophy_first_call("sceNpTrophyCreateContext");
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) {
    trophy_first_call("sceNpTrophyCreateHandle");
    if (!handle) return TROPHY_INVALID;
    *handle=new_id(); return 0;
}
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) {
    (void)ctx; (void)handle; (void)options;
    trophy_first_call("sceNpTrophyRegisterContext");
    return 0;
}
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    trophy_first_call("sceNpTrophyUnlockTrophy");
    int awarded=-1;
    pthread_mutex_lock(&trophy_lock);
    trophy_load();
    if (!trophy_record(id)) printf("Runtime: trophy %d unlocked (already earned)\n",id);
    const int due=trophy_platinum_due();
    if (due>=0 && trophy_record(due)) awarded=due;
    pthread_mutex_unlock(&trophy_lock);
    if (platinum) *platinum=awarded;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    trophy_first_call("sceNpTrophyGetGameInfo");
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}

/* The state of every trophy, as the flag array the game reads (128 bits) and their count. */
static ABI int32_t trophy_destroy(int32_t id) { (void)id; return 0; }
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    (void)ctx; (void)handle;
    trophy_first_call("sceNpTrophyGetTrophyUnlockState");
    if (!flags || !count) return TROPHY_INVALID;
    memset(flags,0,TROPHY_MAX/8);
    pthread_mutex_lock(&trophy_lock);
    trophy_load();
    uint32_t n=0;
    for (int i=0;i<TROPHY_MAX;++i) {
        if (trophy_known[i]) n=(uint32_t)i+1;
        if (trophy_earned[i]) flags[i/32]|=UINT32_C(1)<<(i%32);
    }
    pthread_mutex_unlock(&trophy_lock);
    *count=n; return 0;
}

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }

static ABI int32_t playgo_language_mask(int32_t handle,uint64_t mask) { (void)mask; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }

/* ---- Sharing and streaming: screenshots, live streaming, Share Play and video recording are
 * the PS4's own features; the game's switches for them are accepted and change nothing. ---- */
static ABI int32_t accept_any(void) { return 0; }
/* ---- Rudp (reliable UDP over the PSN connection): the library starts, no peer is reached ---- */
#define RUDP_NOT_ACCEPTABLE ((int32_t)0x80770012)
static ABI int32_t rudp_unreachable(void) { return RUDP_NOT_ACCEPTABLE; }
static ABI int32_t rudp_nothing(void) { return 0; }
static ABI int32_t netctl_result(int32_t event,int32_t *error) { (void)event; if (error) *error=NET_CTL_NOT_CONNECTED; return 0; }
static ABI int32_t user_color(int32_t user,int32_t *color) {
    if (!color || user<1 || user>4 || !logged_in(user)) return USER_INVALID_ARGUMENT;
    *color=user-1; /* blue, red, green, pink: the users' colours in login order */
    return 0;
}

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


/* The "A" (account id) generation of the NP calls GUNDAM VERSUS makes; signed out as above. */
static ABI int32_t np_user_signed_out(int32_t user,void *out) { (void)user; (void)out; return NP_SIGNED_OUT; }
static ABI int32_t np_title_ctx(const void *label,int32_t user) { (void)label; (void)user; return new_id(); }
static ABI int32_t np_ctx_request(int32_t ctx) { (void)ctx; return new_id(); }
/* Async requests (Score, TUS, TSS, WordFilter): every one ends signed out when polled. */
static ABI int32_t np_poll_async(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_reachability_register(void *cb,void *arg) { (void)cb; (void)arg; return 0; }
static ABI int32_t np_reachability_unregister(void) { return 0; }

/* ---- NpToolkit2 (C++ API of the game's own libSceNpToolkit2.prx, served here): the toolkit
 * starts, every PSN request fails signed out. Constructors and destructors of request and
 * response objects leave the game's objects as they are; a response's data is empty. ---- */
static ABI void *tk_this(void *self) { return self; }
static ABI void tk_destroy(void *self) { (void)self; }
static ABI int32_t tk_ok(const void *request) { (void)request; return 0; }
static ABI int32_t tk_request(const void *request,void *response) { (void)request; (void)response; return NP_SIGNED_OUT; }
static ABI int32_t tk_return_code(const void *response) { (void)response; return NP_SIGNED_OUT; }
static _Alignas(16) unsigned char tk_empty[65536];
static ABI void *tk_response_get(const void *response) { (void)response; return tk_empty; }
static ABI int32_t tk_init(const void *params) {
    (void)params; note("NpToolkit2 initialized (offline: every PSN request fails signed out)");
    return 0;
}

/* ---- libSceJson2: the initializer runs; parsing fails (nothing comes from the network), the
 * values the game builds stay empty (null, "", []). ---- */
static _Alignas(16) unsigned char json_null[64];
static ABI int32_t json_ok(void *self) { (void)self; return 0; }
static ABI int32_t json_parse(void *value,const char *text,uint64_t size) { (void)value; (void)text; (void)size; return (int32_t)0x80920001; }
static ABI void *json_value_init(void *self) { memset(self,0,16); return self; }
static ABI void *json_value_copy(void *self,const void *other) { memmove(self,other,16); return self; }
static ABI void json_value_clear(void *self) { memset(self,0,16); }
static ABI int32_t json_value_type(const void *self) { int32_t t; memcpy(&t,self,4); return t; }
static ABI void *json_static_null(const void *self) { (void)self; return json_null; }
static ABI uint64_t json_zero(const void *self) { (void)self; return 0; }
static ABI const char *json_c_str(const void *self) { (void)self; return ""; }
static ABI void *json_ptr_init(void *self) { memset(self,0,8); return self; }
static ABI void *json_ptr_copy(void *self,const void *other) { (void)other; memset(self,0,8); return self; }
static ABI void *json_iterator(void *out,const void *self) { (void)self; memset(out,0,8); return out; }
static ABI int32_t json_iterator_differ(const void *a,const void *b) { (void)a; (void)b; return 0; }
static ABI int32_t json_to_string(const void *self,void *string) { (void)self; (void)string; return 0; }
static ABI void *json_init_rtti(void *self,void *alloc,void *user,uint64_t size) { (void)alloc; (void)user; (void)size; return self; }

/* ---- Other dialogs' results: the user cancelled (did not sign in, invite or browse). ---- */
static ABI int32_t signin_result(int32_t *result) { if (result) result[0]=1; return 0; }
static ABI int32_t browser_close(void) { return dialog_close(10); }
static ABI int32_t profile_close(void) { return dialog_close(3); }

/* ---- Remote Play, crash dumps, the system keyboard: none on this console. ---- */
static ABI int32_t remoteplay_status(int32_t user,int32_t *status) { (void)user; if (status) *status=0; return 0; } /* disconnected */
static ABI int32_t ime_keyboard_none(void) { return (int32_t)0x80bc0103; } /* no keyboard connected */
static ABI int32_t ime_panel_size(const void *param,const void *extended,uint32_t *width,uint32_t *height) {
    (void)param; (void)extended;
    if (width) *width=0;
    if (height) *height=0;
    return 0;
}
static ABI int32_t shareplay_connection(void *info) { if (info) memset(info,0,32); return 0; } /* not connected */
static ABI int32_t addcont_download(int32_t user,const void *label) { (void)user; (void)label; return (int32_t)0x80d90007; } /* no network */
/* PlayGo: everything is installed. */
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *progress) {
    (void)ids; (void)count;
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!progress) return PLAYGO_BAD_POINTER;
    progress[0]=progress[1]=1; return 0; /* progress size = total size */
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    (void)ids; (void)count;
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!eta) return PLAYGO_BAD_POINTER;
    *eta=0; return 0;
}
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=2; return 0; /* trickle..full: full */
}
static ABI int32_t playgo_close(int32_t handle) { if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE; playgo_handle=0; return 0; }
static ABI int32_t playgo_terminate(void) { return 0; }
/* libc.prx's heap tracing (libSceLibcInternalExt): where its trace tables are. */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *atomic_id_mask, *mstate_table; } HeapTraceInfo;
static ABI void heap_trace_info(HeapTraceInfo *info) {
    static uint64_t id_mask, mstate_table[64];
    if (!info) return;
    info->atomic_id_mask=&id_mask; info->mstate_table=mstate_table; info->get_segment_info=0;
}
static ABI int32_t discmap_7c98(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size;
    if (flags) *flags=0;
    if (r1) *r1=0;
    if (r2) *r2=0;
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNpCheckNpAvailabilityA",np_user_signed_out}, {"sceNpGetAccountCountryA",np_user_signed_out},
    {"sceNpGetAccountIdA",np_user_signed_out}, {"sceNpGetParentalControlInfoA",np_signed_out},
    {"sceNpRegisterNpReachabilityStateCallback",np_reachability_register},
    {"sceNpUnregisterNpReachabilityStateCallback",np_reachability_unregister},
    {"sceNpScoreCreateNpTitleCtxA",np_title_ctx}, {"sceNpScorePollAsync",np_poll_async},
    {"sceNpScoreGetBoardInfoAsync",np_signed_out}, {"sceNpScoreGetFriendsRankingAAsync",np_signed_out},
    {"sceNpScoreGetGameDataByAccountIdAsync",np_signed_out}, {"sceNpScoreGetRankingByAccountIdPcIdAsync",np_signed_out},
    {"sceNpScoreGetRankingByRangeAAsync",np_signed_out}, {"sceNpScoreRecordGameDataAsync",np_signed_out},
    {"sceNpScoreRecordScoreAsync",np_signed_out},
    {"sceNpTusCreateNpTitleCtxA",np_title_ctx}, {"sceNpTssCreateNpTitleCtxA",np_title_ctx},
    {"sceNpTusDeleteNpTitleCtx",ok_void}, {"sceNpTusCreateRequest",np_ctx_request}, {"sceNpTusDeleteRequest",ok_void},
    {"sceNpTusPollAsync",np_poll_async}, {"sceNpTusGetDataAAsync",np_signed_out}, {"sceNpTusSetDataAAsync",np_signed_out},
    {"sceNpTusGetMultiSlotVariableAAsync",np_signed_out}, {"sceNpTusSetMultiSlotVariableAAsync",np_signed_out},
    {"sceNpTssGetData",np_signed_out}, {"sceNpTssGetDataAsync",np_signed_out},
    {"sceNpMatching2CreateContextA",np_signed_out}, {"sceNpMatching2CreateJoinRoomA",np_signed_out},
    {"sceNpMatching2JoinRoomA",np_signed_out}, {"sceNpMatching2GetRoomDataInternal",np_signed_out},
    {"sceNpMatching2GetUserInfoListA",np_signed_out}, {"sceNpMatching2SignalingGetConnectionInfoA",np_signed_out},
    {"sceNpSignalingCreateContextA",np_signed_out}, {"sceNpSignalingGetLocalNetInfo",np_signed_out},
    {"sceNpWordFilterCreateTitleCtxA",np_title_ctx}, {"sceNpWordFilterDeleteTitleCtx",ok_void},
    {"sceNpWordFilterCreateAsyncRequest",np_ctx_request}, {"sceNpWordFilterDeleteRequest",ok_void},
    {"sceNpWordFilterCensorComment",np_signed_out}, {"sceNpWordFilterPollAsync",np_poll_async},
    {"sceNpProfileDialogOpenA",profile_open}, {"sceNpProfileDialogClose",profile_close},
    {"sceSigninDialogInitialize",signin_init}, {"sceSigninDialogOpen",signin_open},
    {"sceSigninDialogUpdateStatus",signin_status}, {"sceSigninDialogGetStatus",signin_status},
    {"sceSigninDialogGetResult",signin_result}, {"sceSigninDialogTerminate",signin_term},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogGetResultA",cancelled_result},
    {"sceInvitationDialogTerminate",invitation_term},
    {"sceWebBrowserDialogInitialize",browser_init}, {"sceWebBrowserDialogOpen",browser_open},
    {"sceWebBrowserDialogUpdateStatus",browser_status}, {"sceWebBrowserDialogClose",browser_close},
    {"sceWebBrowserDialogTerminate",browser_term},
    {"sceRemoteplayInitialize",ok_void}, {"sceRemoteplayTerminate",ok_void},
    {"sceRemoteplayGetConnectionStatus",remoteplay_status}, {"sceRemoteplayProhibitStreaming",ok_void},
    {"sceCoredumpRegisterCoredumpHandler",ok_void}, {"sceCoredumpAttachUserFile",ok_void},
    {"sceCoredumpWriteUserData",ok_void}, {"sceCoredumpAttachMemoryRegionAsUserFile",ok_void},
    {"sceImeKeyboardOpen",ime_keyboard_none}, {"sceImeKeyboardGetInfo",ime_keyboard_none},
    {"sceImeKeyboardClose",ime_keyboard_none}, {"sceImeDialogGetPanelSizeExtended",ime_panel_size},
    {"sceSharePlayGetCurrentConnectionInfoA",shareplay_connection},
    {"sceScreenShotSetOverlayImageWithOrigin",accept_any},
    {"sceAppContentAddcontEnqueueDownload",addcont_download},
    {"scePlayGoGetProgress",playgo_progress}, {"scePlayGoGetEta",playgo_eta},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoClose",playgo_close},
    {"scePlayGoTerminate",playgo_terminate},
    {"sceLibcHeapGetTraceInfo",heap_trace_info}, {"sceDiscMap_7C980FFB0AA27E7A",discmap_7c98},
    {"_ZN3sce4Json5ArrayD1Ev",tk_destroy}, /* sce::Json::Array::~Array() */
    {"_ZN3sce4Json6StringC1Ev",json_ptr_init}, /* sce::Json::String::String() */
    {"_ZN3sce4Json5ValueD1Ev",tk_destroy}, /* sce::Json::Value::~Value() */
    {"_ZN3sce4Json5Value5clearEv",json_value_clear}, /* sce::Json::Value::clear() */
    {"_ZNK3sce4Json5Array4sizeEv",json_zero}, /* sce::Json::Array::size() const */
    {"_ZNK3sce4Json6String5c_strEv",json_c_str}, /* sce::Json::String::c_str() const */
    {"_ZNK3sce4Json5Value9getStringEv",json_static_null}, /* sce::Json::Value::getString() const */
    {"_ZN3sce4Json5Array8iteratorppEv",tk_this}, /* sce::Json::Array::iterator::operator++() */
    {"_ZN3sce4Json6StringD1Ev",tk_destroy}, /* sce::Json::String::~String() */
    {"_ZNK3sce4Json6String6lengthEv",json_zero}, /* sce::Json::String::length() const */
    {"_ZNK3sce4Json5Value11getUIntegerEv",json_zero}, /* sce::Json::Value::getUInteger() const */
    {"_ZNK3sce4Json5Array3endEv",json_iterator}, /* sce::Json::Array::end() const */
    {"_ZN3sce4Json5ArrayC1ERKS1_",json_ptr_copy}, /* sce::Json::Array::Array(sce::Json::Array const&) */
    {"_ZNK3sce4Json5Value8getArrayEv",json_static_null}, /* sce::Json::Value::getArray() const */
    {"_ZNK3sce4Json5Array5beginEv",json_iterator}, /* sce::Json::Array::begin() const */
    {"_ZNK3sce4Json5Array8iteratorneERKS2_",json_iterator_differ}, /* sce::Json::Array::iterator::operator!=(sce::Json::Array::iterator const&) const */
    {"_ZN3sce4Json5ValueC1Ev",json_value_init}, /* sce::Json::Value::Value() */
    {"_ZNK3sce4Json5Array8iteratordeEv",json_static_null}, /* sce::Json::Array::iterator::operator*() const */
    {"_ZN3sce4Json5Array8iteratorD1Ev",tk_destroy}, /* sce::Json::Array::iterator::~iterator() */
    {"_ZN3sce4Json5ValueC1ERKS1_",json_value_copy}, /* sce::Json::Value::Value(sce::Json::Value const&) */
    {"_ZN3sce4Json5ValueaSERKS1_",json_value_copy}, /* sce::Json::Value::operator=(sce::Json::Value const&) */
    {"_ZNK3sce4Json5Value7getTypeEv",json_value_type}, /* sce::Json::Value::getType() const */
    {"_ZNK3sce4Json5ValueixEPKc",json_static_null}, /* sce::Json::Value::operator[](char const*) const */
    {"_ZNK3sce4Json5Value8toStringERNS0_6StringE",json_to_string}, /* sce::Json::Value::toString(sce::Json::String&) const */
    {"_ZN3sce7Toolkit2NP2V24Core10OnlineUserD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::OnlineUser::~OnlineUser() */
    {"_ZN3sce7Toolkit2NP2V24Core10OnlineUserC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::OnlineUser::OnlineUser() */
    {"_ZN3sce7Toolkit2NP2V24Core11addCallbackERKNS3_7Request11AddCallbackE",tk_ok}, /* sce::Toolkit::NP::V2::Core::addCallback(sce::Toolkit::NP::V2::Core::Request::AddCallback c */
    {"_ZN3sce7Toolkit2NP2V24Core12ResponseBase13getReturnCodeEv",tk_return_code}, /* sce::Toolkit::NP::V2::Core::ResponseBase::getReturnCode() */
    {"_ZN3sce7Toolkit2NP2V24Core14removeCallbackERKNS3_7Request14RemoveCallbackE",tk_ok}, /* sce::Toolkit::NP::V2::Core::removeCallback(sce::Toolkit::NP::V2::Core::Request::RemoveCall */
    {"_ZN3sce7Toolkit2NP2V24Core4initERKNS3_7Request10InitParamsE",tk_init}, /* sce::Toolkit::NP::V2::Core::init(sce::Toolkit::NP::V2::Core::Request::InitParams const&) */
    {"_ZN3sce7Toolkit2NP2V24Core7Request10InitParamsC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Request::InitParams::InitParams() */
    {"_ZN3sce7Toolkit2NP2V24Core7Request10InitParamsD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Request::InitParams::~InitParams() */
    {"_ZN3sce7Toolkit2NP2V24Core7Request11AddCallbackC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Request::AddCallback::AddCallback() */
    {"_ZN3sce7Toolkit2NP2V24Core7Request11AddCallbackD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Request::AddCallback::~AddCallback() */
    {"_ZN3sce7Toolkit2NP2V24Core7Request14RemoveCallbackC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Request::RemoveCallback::RemoveCallback() */
    {"_ZN3sce7Toolkit2NP2V24Core7Request14RemoveCallbackD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Request::RemoveCallback::~RemoveCallback() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend7FriendsEE3getEv",tk_response_get}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Friend::Friends>::get() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend7FriendsEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Friend::Friends>::Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_6Friend7FriendsEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Friend::Friends>::~Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11InvitationsEE3getEv",tk_response_get}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Invitations>::get() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11InvitationsEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Invitations>::Response */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11InvitationsEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Invitations>::~Respons */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11SessionDataEE3getEv",tk_response_get}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionData>::get() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11SessionDataEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionData>::Response */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session11SessionDataEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionData>::~Respons */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session8SessionsEE3getEv",tk_response_get}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Sessions>::get() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session8SessionsEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Sessions>::Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session8SessionsEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::Sessions>::~Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session9SessionIdEE3getEv",tk_response_get}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionId>::get() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session9SessionIdEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionId>::Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS2_7Session9SessionIdEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Session::SessionId>::~Response( */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS3_5EmptyEEC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Core::Empty>::Response() */
    {"_ZN3sce7Toolkit2NP2V24Core8ResponseINS3_5EmptyEED1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Core::Response<sce::Toolkit::NP::V2::Core::Empty>::~Response() */
    {"_ZN3sce7Toolkit2NP2V26Friend7Request10GetFriendsC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Friend::Request::GetFriends::GetFriends() */
    {"_ZN3sce7Toolkit2NP2V26Friend7Request10GetFriendsD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Friend::Request::GetFriends::~GetFriends() */
    {"_ZN3sce7Toolkit2NP2V27Session20LocalizedSessionInfoC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::LocalizedSessionInfo::LocalizedSessionInfo() */
    {"_ZN3sce7Toolkit2NP2V27Session20LocalizedSessionInfoD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::LocalizedSessionInfo::~LocalizedSessionInfo() */
    {"_ZN3sce7Toolkit2NP2V27Session4joinERKNS3_7Request4JoinEPNS2_4Core8ResponseINS8_5EmptyEEE",tk_request}, /* sce::Toolkit::NP::V2::Session::join(sce::Toolkit::NP::V2::Session::Request::Join const&, s */
    {"_ZN3sce7Toolkit2NP2V27Session5leaveERKNS3_7Request5LeaveEPNS2_4Core8ResponseINS8_5EmptyEEE",tk_request}, /* sce::Toolkit::NP::V2::Session::leave(sce::Toolkit::NP::V2::Session::Request::Leave const&, */
    {"_ZN3sce7Toolkit2NP2V27Session7Request14SendInvitationC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::SendInvitation::SendInvitation() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request14SendInvitationD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::SendInvitation::~SendInvitation() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request22GetReceivedInvitationsC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::GetReceivedInvitations::GetReceivedInvitations() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request22GetReceivedInvitationsD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::GetReceivedInvitations::~GetReceivedInvitations() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request4JoinC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::Join::Join() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request4JoinD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::Join::~Join() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request5LeaveC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::Leave::Leave() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request5LeaveD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::Leave::~Leave() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request6CreateC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::Create::Create() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request6CreateD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::Create::~Create() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request6SearchC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::Search::Search() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request6SearchD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::Search::~Search() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request7GetDataC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::Request::GetData::GetData() */
    {"_ZN3sce7Toolkit2NP2V27Session7Request7GetDataD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::Request::GetData::~GetData() */
    {"_ZN3sce7Toolkit2NP2V27Session9SessionId5resetEv",tk_destroy}, /* sce::Toolkit::NP::V2::Session::SessionId::reset() */
    {"_ZN3sce7Toolkit2NP2V27Session9SessionIdC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Session::SessionId::SessionId() */
    {"_ZN3sce7Toolkit2NP2V27Session9SessionIdD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Session::SessionId::~SessionId() */
    {"_ZN3sce7Toolkit2NP2V28Presence7Request11SetPresenceC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Presence::Request::SetPresence::SetPresence() */
    {"_ZN3sce7Toolkit2NP2V28Presence7Request11SetPresenceD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Presence::Request::SetPresence::~SetPresence() */
    {"_ZN3sce7Toolkit2NP2V28Presence7Request19LocalizedGameStatusC1Ev",tk_this}, /* sce::Toolkit::NP::V2::Presence::Request::LocalizedGameStatus::LocalizedGameStatus() */
    {"_ZN3sce7Toolkit2NP2V28Presence7Request19LocalizedGameStatusD1Ev",tk_destroy}, /* sce::Toolkit::NP::V2::Presence::Request::LocalizedGameStatus::~LocalizedGameStatus() */
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE",json_ok}, /* sce::Json::Initializer::initialize(sce::Json::InitParameter const*) */
    {"_ZN3sce4Json11Initializer9terminateEv",json_ok}, /* sce::Json::Initializer::terminate() */
    {"_ZN3sce4Json11InitializerC1Ev",tk_this}, /* sce::Json::Initializer::Initializer() */
    {"_ZN3sce4Json11InitializerD1Ev",tk_destroy}, /* sce::Json::Initializer::~Initializer() */
    {"_ZN3sce4Json17InitParameterRttiC1EPNS0_14AllocParamRttiEPvm",json_init_rtti}, /* sce::Json::InitParameterRtti::InitParameterRtti(sce::Json::AllocParamRtti*, void*, unsigne */
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm",json_parse}, /* sce::Json::Parser::parse(sce::Json::Value&, char const*, unsigned long) */
    {"sceNpToolkit2_7zsee5IHLis",tk_request},
    {"sceNpToolkit2_cmvr0mOCX2Q",tk_request},
    {"sceNpToolkit2_o8e-sH1eU5s",tk_request},
    {"sceNpToolkit2_SX9IOde7tLo",tk_request},
    {"sceNpToolkit2_p3j95bppH44",tk_request},
    {"sceNpToolkit2_X8uNKyVpR1I",tk_request},
    {"sceNpToolkit2_xCg+igDBo3Q",tk_request},
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",lib_init_id}, {"sceHttpTerm",ok_void},
    {"sceHttpCreateTemplate",http_object}, {"sceHttpDeleteTemplate",ok_void},
    {"sceHttpCreateConnectionWithURL",http_object}, {"sceHttpCreateRequestWithURL",http_object},
    {"sceHttpSendRequest",http_fail}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",ok_void}, {"sceHttpSetRequestContentLength",ok_void},
    {"sceHttpDeleteConnection",ok_void}, {"sceHttpDeleteRequest",ok_void},
    {"sceHttpAbortWaitRequest",ok_void}, {"sceHttpDestroyEpoll",ok_void},
    {"sceHttpSetEpoll",ok_void}, {"sceHttpUnsetEpoll",ok_void}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_fail}, {"sceHttpGetResponseContentLength",http_fail},
    {"sceHttpReadData",http_fail},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",ok_void},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",ok_void},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",np_request}, {"sceNpAuthDeleteRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_signed_out},
    {"sceNpWebApiCreateRequest",np_signed_out}, {"sceNpWebApiSendRequest",np_signed_out},
    {"sceNpWebApiDeleteRequest",ok_void}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_signed_out},
    {"sceNpWebApiGetHttpStatusCode",np_signed_out}, {"sceNpWebApiGetHttpResponseHeaderValue",np_signed_out},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_signed_out},
    {"sceNpWebApiCreatePushEventFilter",np_signed_out}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",np_signed_out}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
    /* What GODZILLA calls besides. */
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogGetResult",msg_result},
    {"sceSaveDataDialogClose",save_close}, {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready}, {"sceSaveDataDialogProgressBarSetValue",save_progress},
    {"sceNpCommerceDialogClose",commerce_close}, {"sceNpCommerceDialogGetResult",cancelled_result},
    {"sceErrorDialogInitialize",error_dialog_init}, {"sceErrorDialogOpen",error_dialog_open},
    {"sceErrorDialogUpdateStatus",error_dialog_status}, {"sceErrorDialogClose",error_dialog_close},
    {"sceErrorDialogTerminate",error_dialog_term},
    {"sceNpFriendListDialogInitialize",friends_init}, {"sceNpFriendListDialogOpen",friends_open},
    {"sceNpFriendListDialogUpdateStatus",friends_status}, {"sceNpFriendListDialogGetStatus",friends_status},
    {"sceNpFriendListDialogGetResult",cancelled_result}, {"sceNpFriendListDialogClose",friends_close},
    {"sceNpFriendListDialogTerminate",friends_term},
    {"sceNpTrophyDestroyContext",trophy_destroy}, {"sceNpTrophyDestroyHandle",trophy_destroy},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
    {"scePlayGoSetLanguageMask",playgo_language_mask},
    {"sceScreenShotEnable",accept_any}, {"sceScreenShotDisable",accept_any},
    {"sceScreenShotSetParam",accept_any}, {"sceScreenShotSetOverlayImage",accept_any},
    {"sceGameLiveStreamingInitialize",accept_any}, {"sceGameLiveStreamingTerminate",accept_any},
    {"sceGameLiveStreamingEnableLiveStreaming",accept_any}, {"sceGameLiveStreamingPermitLiveStreaming",accept_any},
    {"sceGameLiveStreamingPermitServerSideRecording",accept_any},
    {"sceSharePlayInitialize",accept_any}, {"sceSharePlayTerminate",accept_any},
    {"sceSharePlaySetProhibition",accept_any}, {"sceVideoRecordingSetInfo",accept_any},
    {"sceRudpInit",rudp_nothing}, {"sceRudpEnd",rudp_nothing}, {"sceRudpEnableInternalIOThread",rudp_nothing},
    {"sceRudpSetEventHandler",rudp_nothing}, {"sceRudpSetOption",rudp_nothing},
    {"sceRudpCreateContext",rudp_unreachable}, {"sceRudpBind",rudp_unreachable},
    {"sceRudpInitiate",rudp_unreachable}, {"sceRudpTerminate",rudp_nothing},
    {"sceRudpRead",rudp_unreachable}, {"sceRudpWrite",rudp_unreachable},
    {"sceRudpFlush",rudp_nothing}, {"sceRudpNetFlush",rudp_nothing},
    {"sceRudpGetNumberOfPacketsToRead",rudp_nothing}, {"sceRudpGetSizeReadable",rudp_nothing},
    {"sceNetCtlGetResult",netctl_result},
    {"sceNetGetSockInfo",net_unreachable}, {"sceNetGetStatisticsInfo",net_unreachable},
    {"sceNpCreateRequest",np_request}, {"sceNpWebApiGetErrorCode",np_signed_out},
    {"sceNpMatching2AbortContextStart",ok_void}, {"sceNpMatching2SignalingCancelPeerNetInfo",ok_void},
    {"sceNpMatching2GetMemoryInfo",np_signed_out}, {"sceNpMatching2GetRoomDataExternalList",np_signed_out},
    {"sceNpMatching2GetUserInfoList",np_signed_out}, {"sceNpMatching2SignalingGetConnectionInfo",np_signed_out},
    {"sceNpMatching2SignalingGetLocalNetInfo",np_signed_out},
    {"sceNpScoreGetFriendsRanking",np_signed_out}, {"sceNpScoreGetRankingByNpId",np_signed_out},
    {"sceUserServiceGetUserColor",user_color},
};
uintptr_t runtime_services_resolve(const char *name) {
    const uintptr_t online=runtime_net_resolve(name); /* BB_ONLINE=1: the online module first */
    return online ? online : RUNTIME_LOOKUP(exports,name);
}
#else
uintptr_t runtime_services_resolve(const char *name) { (void)name; return 0; }
#endif
