/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Offline native worker: actual ALG configuration/init and verified type29
 * pen-prefix preparation. Only use the linkage copy from prepare_alg_gnu.py.
 * No device/FIFO access; the complete context/process chain remains separate
 * until actual main-touch/SC state and synchronized pen input are available.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "piano-pen-frame.h"

/* ALG's only __sF use adds0x130 and calls fwrite on allocation failure.
 * These are address tokens, not fabricated FILE structs. pfwr maps tokens
 * onto the actual GNU standard streams, preserving real output behavior.
 */
unsigned char pSF[3 * 152];
size_t pfwr(const void *ptr, size_t size, size_t count, void *stream)
{
    uintptr_t p=(uintptr_t)stream, b=(uintptr_t)pSF;
    if (p>=b && p<b+sizeof(pSF)) {
        uintptr_t offset=p-b;
        if (offset%152) { errno=EINVAL; return 0; }
        FILE *files[3]={stdin,stdout,stderr};
        return fwrite(ptr,size,count,files[offset/152]);
    }
    return fwrite(ptr,size,count,(FILE *)stream);
}
size_t pstrlen_chk(const char *text, size_t bound)
{
    const char *end=memchr(text,0,bound);
    if (!end) abort();
    return (size_t)(end-text);
}

/* Bionic mutex storage cannot be handed to a48-byte GNU pthread_mutex_t.
 * Map each opaque original address to a real native mutex; never overwrite
 * the vendor object's storage. There are no thread/condvar imports in ALG.
 */
struct mutex_entry { void *key; pthread_mutex_t mutex; };
static struct mutex_entry mutexes[16];
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static int adapter_error;
static struct mutex_entry *mutex_find(void *key, int create)
{
    if (!key) { adapter_error=EINVAL;return NULL; }
    struct mutex_entry *entry=NULL;
    pthread_mutex_lock(&registry);
    for (size_t i=0;i<16;i++) if (mutexes[i].key==key) { entry=&mutexes[i]; break; }
    if (!entry && create) for (size_t i=0;i<16;i++) if (!mutexes[i].key) {
        int r=pthread_mutex_init(&mutexes[i].mutex,NULL);
        if (r) { adapter_error=r; break; }
        mutexes[i].key=key;entry=&mutexes[i];break;
    }
    pthread_mutex_unlock(&registry);
    if (!entry) adapter_error=ENOMEM;
    return entry;
}
int piano_mutex_init(void *key, const void *attributes)
{
    if (!key || attributes) { adapter_error=EINVAL; return EINVAL; }
    /* Actual stylus init passes NULL attributes; other ABI not inferred. */
    return mutex_find(key,1)?0:adapter_error;
}
int piano_mutex_lock(void *key)
{
    struct mutex_entry *entry=mutex_find(key,1);
    return entry?pthread_mutex_lock(&entry->mutex):adapter_error;
}
int piano_mutex_unlock(void *key)
{
    struct mutex_entry *entry=mutex_find(key,0);
    return entry?pthread_mutex_unlock(&entry->mutex):adapter_error;
}
int piano_mutex_destroy(void *key)
{
    struct mutex_entry *entry=mutex_find(key,0);
    if (!entry) return adapter_error;
    int r=pthread_mutex_destroy(&entry->mutex);
    if (!r) entry->key=NULL;
    return r;
}

struct item { char *key,*value; struct item *next; };
struct section { char *name; struct item *items; struct section *next,*root; };
static char *trim(char *s)
{
    while (*s==' ' || *s=='\t' || *s=='\r' || *s=='\n') s++;
    size_t n=strlen(s);
    while (n && (s[n-1]==' ' || s[n-1]=='\t' || s[n-1]=='\r' || s[n-1]=='\n')) s[--n]=0;
    return s;
}
static struct section *find_section(struct section *root,const char *name)
{
    for (struct section *s=root;s;s=s->next) if (!strcmp(s->name,name)) return s;
    return NULL;
}
static struct item *find_item(struct section *node,const char *key)
{
    if (!node || !key) return NULL;
    const char *dot=strchr(key,'.');
    if (dot) {
        size_t n=(size_t)(dot-key);
        for (struct section *s=node->root;s;s=s->next)
            if (strlen(s->name)==n && !memcmp(s->name,key,n)) return find_item(s,dot+1);
        return NULL;
    }
    for (struct item *i=node->items;i;i=i->next) if (!strcmp(i->key,key)) return i;
    return NULL;
}
static struct section *read_ini(const char *path)
{
    FILE *file=fopen(path,"r");if (!file) return NULL;
    struct section *root=NULL,*current=NULL;char *line=NULL;size_t capacity=0;
    while (getline(&line,&capacity,file)>=0) {
        char *comment=strchr(line,'#');if (comment) *comment=0;
        char *text=trim(line);if (!*text) continue;
        char *equals=strchr(text,'=');
        if (!equals) {
            if (current) current->next=calloc(1,sizeof(*current));
            else root=calloc(1,sizeof(*root));
            current=current?current->next:root;if (!current) goto fail;
            current->root=root;current->name=strdup(text);if (!current->name) goto fail;
        } else {
            if (!current) { errno=EINVAL;goto fail; }
            *equals=0;struct item *item=calloc(1,sizeof(*item));if (!item) goto fail;
            item->key=strdup(trim(text));item->value=strdup(trim(equals+1));
            item->next=current->items;current->items=item;
            if (!item->key || !item->value) goto fail;
        }
    }
    if (ferror(file)) goto fail;
    free(line);fclose(file);return root;
fail:
    free(line);fclose(file);return NULL;
}
/* Native equivalents of the observed HAL reader table at0x58bd0:
 * +0 section/node, +8 int/default, +16 exact int-array, +24 string/default.
 * These read real external ini values; there are no stub configuration values.
 */
static void *reader_section(void *opaque,const char *key)
{
    struct section *node=opaque;
    return node?find_section(node->root,key):NULL;
}
static int reader_int(void *opaque,const char *key,int fallback)
{
    struct item *item=find_item(opaque,key);if (!item) return fallback;
    char *end;errno=0;long value=strtol(item->value,&end,10);
    if (errno || *trim(end) || value<INT_MIN || value>INT_MAX) return fallback;
    return (int)value;
}
static void *reader_array(void *opaque,const char *key,int *count,int *output)
{
    struct item *item=find_item(opaque,key);
    if (!item || !count || *count<=0 || !output) { if (count) *count=0;return NULL; }
    char *text=item->value;
    if (*text!='{') { *count=0;return NULL; }
    int wanted=*count,seen=0;text++;
    while (*text && *text!='}') {
        while (*text==' ' || *text=='\t') text++;
        char *end;errno=0;long value=strtol(text,&end,10);
        if (errno || end==text || value<INT_MIN || value>INT_MAX || seen>=wanted) goto bad;
        output[seen++]=(int)value;text=end;
        while (*text==' ' || *text=='\t') text++;
        if (*text==',') text++;else if (*text!='}') goto bad;
    }
    if (*text!='}' || seen!=wanted) goto bad;
    return output;
bad:
    *count=0;return NULL;
}
static int reader_string(void *opaque,const char *key,char *output,int capacity,const char *fallback)
{
    struct item *item=find_item(opaque,key);const char *text=item?item->value:fallback;
    if (!output || capacity<=0 || !text) return 255;
    size_t length=strlen(text);if (length>=(size_t)capacity) length=(size_t)capacity-1;
    memset(output,0,(size_t)capacity);memcpy(output,text,length);
    return item?0:255;
}
struct readers {
    void *(*section)(void *,const char *);
    int (*integer)(void *,const char *,int);
    void *(*array)(void *,const char *,int *,int *);
    int (*string)(void *,const char *,char *,int,const char *);
};
static uint16_t u16(const unsigned char *p) { return (uint16_t)p[0]|(uint16_t)p[1]<<8; }
static uint32_t u32(const unsigned char *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void *required(void *library,const char *name)
{
    dlerror();void *symbol=dlsym(library,name);
    const char *error=dlerror();
    if (error || !symbol) { fprintf(stderr,"%s: %s\n",name,error?error:"absent");exit(1); }
    return symbol;
}
static int read_file(const char *path,unsigned char *output,size_t capacity,
                     size_t *bytes)
{
    FILE *file=fopen(path,"rb");if (!file) { perror(path);return 1; }
    struct stat status;
    if (fstat(fileno(file),&status) || !S_ISREG(status.st_mode) ||
        status.st_size<0 || (uint64_t)status.st_size>capacity) {
        fprintf(stderr,"%s: expected a bounded regular captured file\n",path);
        fclose(file);return 1;
    }
    *bytes=fread(output,1,capacity,file);
    int failed=ferror(file) || *bytes!=(size_t)status.st_size;
    fclose(file);return failed;
}
static int decode_frames(struct section *tree,const unsigned char *hardware,
                         void *library,void **stylus,const char *common_path,
                         int count,char **paths)
{
    const char *keys[4]={"stylus.stylus_num_col1","stylus.stylus_num_row1",
                         "stylus.stylus_num_col2","stylus.stylus_num_row2"};
    unsigned char sizes[4];
    for (unsigned i=0;i<4;i++) {
        int value=reader_int(tree,keys[i],0);
        if (value<1 || value>255) {
            fputs("Actual ini pen matrix geometry missing\n",stderr);return 1;
        }
        sizes[i]=(unsigned char)value;
    }
    uint32_t nodes=(uint32_t)u16(hardware+4)*u16(hardware+6);
    if (nodes>UINT16_MAX) return 1;
    struct piano_pen_geometry geometry={sizes[0],sizes[1],sizes[2],sizes[3],
                                        (uint16_t)nodes};
    struct piano_pen_frame decoded;
    int rc=piano_pen_frame_init(&decoded,&geometry);
    if (rc) { fprintf(stderr,"Pen frame allocation: %s\n",strerror(rc));return 1; }
    int (*receive_common)(void *,int *)=stylus[2];
    int (*prepare)(void *)=stylus[8];
    unsigned char *(*get_total)(void)=required(library,"get_stylus_data");
    if (common_path) {
        /* Actual HAL160e8..160f8 reads1032B into the common ring unchanged;
         * ALG86cf0's pressure branch checks byte1=7/u16+2=0x440, then reads
         * the two actual u32 body words at0xc/0x10. Never derive a record from
         * raw29 metadata pressure or reinterpret +8 as a packet length.
         */
        unsigned char record[1032];size_t bytes;
        if (read_file(common_path,record,sizeof(record),&bytes) || bytes!=sizeof(record) ||
            record[1]!=7 || u16(record+2)!=0x440 ||
            u16(record+4)<3 || u16(record+4)>256 || !receive_common || !prepare) {
            fputs("Expected actual exact1032-byte common7/0x440 record with at least three s32 body words\n",stderr);
            rc=1;goto done;
        }
        int result;
        receive_common(record,&result);
        if (adapter_error) { rc=1;goto done; }
    }
    for (int i=0;i<count;i++) {
        unsigned char packet[PIANO_PEN_MAX_PACKET];size_t bytes;
        if (read_file(paths[i],packet,sizeof(packet),&bytes)) { rc=1;break; }
        const char *error;
        if (piano_pen_frame_decode(&decoded,packet,bytes,&error)) {
            fprintf(stderr,"%s: %s\n",paths[i],error);rc=1;break;
        }
        if (common_path && prepare(decoded.frame)!=1) {
            fputs("Actual ALG rejected prepared type29 frame\n",stderr);rc=1;break;
        }
        unsigned char *total=common_path?get_total():NULL;
        printf("{\"status\":\"%s\",\"frame_type\":29,\"pen_frame_no\":%u,"
               "\"matrix_elements\":[%zu,%zu,%zu,%zu],\"outer_checksum_end\":%zu,"
               "\"pen_checksum_end\":%zu,\"hand_complete\":%s,"
               "\"frequency_request_byte\":%d,\"algorithm_pressure\":",
               common_path?"ALG_PEN_PREPARED_NOT_SOLVED":"CPU_PEN_ADAPTED_NOT_SOLVED",
               u16(packet+66),decoded.n1,decoded.n2,decoded.n1,decoded.n2,
               decoded.outer_end,decoded.pen_end,decoded.frame[0x165]?"true":"false",
               decoded.frequency_request);
        if (total) printf("%u",u16(total+4));else fputs("null",stdout);
        puts(",\"coordinates_available\":false,\"device_tested\":false}");
    }
done:
    piano_pen_frame_destroy(&decoded);return rc;
}
static int configure(const char *library_path,const char *ini_path,const char *header_path,
                     const char *common_path,int frame_count,char **frames)
{
    unsigned char hardware[214]={0};FILE *header=fopen(header_path,"rb");
    if (!header) { perror(header_path);return 1; }
    size_t n=fread(hardware,1,9,header);int extra=fgetc(header);fclose(header);
    if (n!=9 || extra!=EOF || !u16(hardware) || !u16(hardware+2) || !hardware[8]) {
        fputs("Expected real exact9-byte hardware header\n",stderr);return 1;
    }
    struct section *tree=read_ini(ini_path);
    if (!tree) { perror(ini_path);return 1; }
    void *library=dlopen(library_path,RTLD_NOW|RTLD_LOCAL);
    if (!library) { fprintf(stderr,"dlopen: %s\n",dlerror());return 1; }
    /* Exported verbosity setting avoids Android application log callbacks.
     * Never register or call the HAL module startup/device callbacks.
     */
    /* Some real ALG messages are level0; signed negative disables these too. */
    int *log_level=required(library,"alg_log_level");*log_level=-1;
    void *(*register_module)(void *)=required(library,"register_alg_module");
    void **(*get_stylus)(void)=required(library,"get_stylus_function_interface");
    int (*read_config)(void *,void *,const struct readers *)=required(library,"alg_read_config_param_core");
    void *(*get_hw)(void)=required(library,"get_hw_info");
    void *(*get_param)(void)=required(library,"get_tp_param");
    void (*pass_hw)(void *)=required(library,"alg_pass_hwinfo_core");
    void (*pass_param)(void *)=required(library,"alg_pass_param_core");
    void **(*get_context)(void)=required(library,"alg_get_touch_data");
    const struct readers readers={reader_section,reader_int,reader_array,reader_string};
    register_module(NULL); /* observed function only registers interface tables */
    void **stylus=get_stylus();
    int (*stylus_config)(void *,void *,const struct readers *)=stylus[3];
    void (*stylus_init)(void)=stylus[0];void (*stylus_exit)(void)=stylus[1];
    if (!stylus_config || !stylus_init || !stylus_exit) return 1;
    if ((read_config(hardware,tree,&readers)&255)==255 ||
        (stylus_config(hardware,tree,&readers)&255)==255) {
        fputs("Factory configuration reader failed\n",stderr);return 1;
    }
    unsigned char *hw=get_hw();pass_hw(hw);pass_param(get_param());
    unsigned char *context=*get_context();
    if (!context || u32(context+0x2c)!=u16(hw+0x24)) {
        fputs("Actual algorithm context was not constructed\n",stderr);return 1;
    }
    stylus_init();
    if (adapter_error) { fprintf(stderr,"native mutex adapter: %d\n",adapter_error);return 1; }
    int result=0;
    if (frame_count) {
        result=decode_frames(tree,hardware,library,stylus,common_path,frame_count,frames);
    } else printf("{\"status\":\"ALG_CONFIG_AND_INIT_NO_FRAMES\",\"resolution\":%u,"
           "\"scaled_x\":%u,\"scaled_y\":%u,\"cols\":%u,\"rows\":%u,"
           "\"stylus_enabled\":%u}\n",u32(context+0x2c),u32(context+0x20),
           u32(context+0x24),u32(context+0x10),u32(context+0x14),
           u32((unsigned char *)get_param()+0x619));
    stylus_exit();dlclose(library);return result;
}
int main(int argc,char **argv)
{
    if (argc==2 && !strcmp(argv[1],"--help")) {
        puts("piano-pen-worker --config ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin\n"
             "piano-pen-worker --decode29 ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin RAW29.bin [...]\n"
             "piano-pen-worker --prepare29 ALG.gnu.so FACTORY.ini REAL-HARDWARE-9B.bin COMMON-1032B.bin RAW29.bin [...]\n"
             "Offline captured regular files only. Type29 CPU decoding/preparation; no XY solver, FIFO, device or input events.\n"
             "RAW29 begins at original SPI frame_data_packet. COMMON must be an actual common7/id0x440 record.");
        return 0;
    }
    if (argc==5 && !strcmp(argv[1],"--config"))
        return configure(argv[2],argv[3],argv[4],NULL,0,NULL);
    if (argc>=6 && !strcmp(argv[1],"--decode29"))
        return configure(argv[2],argv[3],argv[4],NULL,argc-5,argv+5);
    if (argc>=7 && !strcmp(argv[1],"--prepare29"))
        return configure(argv[2],argv[3],argv[4],argv[5],argc-6,argv+6);
    fputs("Use --help with the derived ALG and actual captured input files\n",stderr);
    return 2;
}
