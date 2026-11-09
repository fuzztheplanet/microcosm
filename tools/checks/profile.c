/* GL 3.3 timer queries, read back at least eight frames later. A query result
 * is never read before it's available, except in the drain after the caller's
 * final glFinish. */
#include "profile.h"
#include "microcosm/mc_profile.h"
#include "microcosm/mc_gl.h"
#include "microcosm/mc_platform.h"
#include <EGL/egl.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define RING 16
#define SEGMENTS 24
typedef struct {
    GLuint query[SEGMENTS]; int pass[SEGMENTS], count, pending;
    unsigned long long frame, bytes, triangles;
    unsigned draws, uploads, lookups;
    double time, start, cpu_ms, wall_ms, upload_ms, cpu[2];
} Frame;
static Frame frames[RING], *current;
static unsigned long long serial;
static int active;
static FILE *output;
static PFNGLGENQUERIESPROC gen;
static PFNGLDELETEQUERIESPROC del;
static PFNGLBEGINQUERYPROC begin;
static PFNGLENDQUERYPROC end;
static PFNGLGETQUERYOBJECTIVPROC available;
static PFNGLGETQUERYOBJECTUI64VPROC result;
static PFNGLGETUNIFORMLOCATIONPROC real_loc;
static PFNGLBUFFERSUBDATAPROC real_upload;
static PFNGLDRAWELEMENTSINSTANCEDPROC real_inst;
static MC_PFN_DRAWELEMENTS real_draw;
static MC_PFN_DRAWARRAYS real_arrays;
static GLint APIENTRY lookup(GLuint p, const GLchar *s) { if(current) ++current->lookups; return real_loc(p,s); }
static void APIENTRY upload(GLenum t, GLintptr o, GLsizeiptr n, const void *p) {
    double start = current ? mc_time_seconds() : 0;
    if(current) { ++current->uploads; current->bytes += n; } real_upload(t,o,n,p);
    if(current) current->upload_ms += (mc_time_seconds()-start)*1000;
}
static void count(GLenum mode, GLsizei n, GLsizei instances) {
    if(current) { ++current->draws; if(mode == GL_TRIANGLES) current->triangles += (unsigned long long)(n/3)*instances; }
}
static void APIENTRY inst(GLenum m, GLsizei n, GLenum t, const void *p, GLsizei c) { count(m,n,c); real_inst(m,n,t,p,c); }
static void APIENTRY draw(GLenum m, GLsizei n, GLenum t, const void *p) { count(m,n,1); real_draw(m,n,t,p); }
static void APIENTRY arrays(GLenum m, GLint first, GLsizei n) { count(m,n,1); real_arrays(m,first,n); }
static int retire(Frame *f) {
    if (!f->pending) return 1;
    for(int i=0; i<f->count; ++i) { GLint ready=0; available(f->query[i],GL_QUERY_RESULT_AVAILABLE,&ready); if(!ready) return 0; }
    double gpu[MC_P_COUNT]={0};
    for(int i=0; i<f->count; ++i) { GLuint64 ns=0; result(f->query[i],GL_QUERY_RESULT,&ns); gpu[f->pass[i]] += ns*1e-6; }
    fprintf(output,"%llu,%.6f,%.6f,%.6f,%.6f,%u,%u,%llu,%u,%llu",f->frame,f->time,f->cpu_ms,f->cpu[0],f->cpu[1],f->lookups,f->uploads,f->bytes,f->draws,f->triangles);
    for(int i=0; i<MC_P_COUNT; ++i) fprintf(output,",%.6f",gpu[i]);
    fprintf(output,",%.6f,%.6f\n",f->wall_ms,f->upload_ms); f->pending=0; return 1;
}
int profile_init(FILE *csv) {
#define LOAD(v,n) v=(void*)eglGetProcAddress(n); if(!v) return 0
    LOAD(gen,"glGenQueries"); LOAD(del,"glDeleteQueries"); LOAD(begin,"glBeginQuery"); LOAD(end,"glEndQuery");
    LOAD(available,"glGetQueryObjectiv"); LOAD(result,"glGetQueryObjectui64v");
#undef LOAD
    output=csv; serial=0; current=NULL; active=0; memset(frames,0,sizeof frames);
    for(int i=0;i<RING;++i) gen(SEGMENTS,frames[i].query);
    real_loc=mcgl.GetUniformLocation; mcgl.GetUniformLocation=lookup;
    real_upload=mcgl.BufferSubData; mcgl.BufferSubData=upload;
    real_inst=mcgl.DrawElementsInstanced; mcgl.DrawElementsInstanced=inst;
    real_draw=mcgl1.DrawElements; mcgl1.DrawElements=draw;
    real_arrays=mcgl1.DrawArrays; mcgl1.DrawArrays=arrays;
    fputs("frame,time,cpu_submit_ms,simulation_ms,prepare_ms,lookups,uploads,upload_bytes,draws,triangles,scene_gpu_ms,shadow_gpu_ms,ao_gpu_ms,ao_filter_gpu_ms,bright_gpu_ms,rays_gpu_ms,bloom_gpu_ms,composite_gpu_ms,taa_gpu_ms,dof_gpu_ms,wall_ms,upload_cpu_ms\n",output);
    return 1;
}
void profile_frame(double time) {
    assert(!current);
    if(serial >= 8) retire(&frames[(serial-8)%RING]);
    Frame *f=&frames[serial%RING];
    /* Never overwrite unread queries. If the GPU is saturated, drop the sample
     * instead of making the render thread wait, and say so. */
    if(!retire(f)) { fprintf(stderr,"profile: dropped sample %llu (query ring busy)\n",serial++); return; }
    GLuint q[SEGMENTS]; memcpy(q,f->query,sizeof q); memset(f,0,sizeof *f); memcpy(f->query,q,sizeof q);
    f->frame=serial++; f->time=time; f->start=mc_time_seconds(); current=f;
}
void mc_profile_pass_end(void) { if(active) { end(GL_TIME_ELAPSED); active=0; } }
void mc_profile_pass(int pass) {
    mc_profile_pass_end(); if(!current) return;
    assert(current->count<SEGMENTS);
    int n=current->count++; current->pass[n]=pass; begin(GL_TIME_ELAPSED,current->query[n]); active=1;
}
void mc_profile_cpu(int slot,double ms) { if(current) current->cpu[slot]+=ms; }
void profile_end(void) {
    mc_profile_pass_end(); if(!current) return;
    current->cpu_ms=(mc_time_seconds()-current->start)*1000; current->pending=1; current=NULL;
}
void profile_drain(void) { glFinish(); for(int i=0;i<RING;++i) assert(retire(&frames[i])); fflush(output); }
void profile_close(void) {
    profile_drain(); for(int i=0;i<RING;++i) del(SEGMENTS,frames[i].query);
    mcgl.GetUniformLocation=real_loc; mcgl.BufferSubData=real_upload;
    mcgl.DrawElementsInstanced=real_inst; mcgl1.DrawElements=real_draw; mcgl1.DrawArrays=real_arrays;
}

void profile_wall(double ms) { Frame *f=&frames[(serial-1)%RING]; if(f->pending && f->frame==serial-1) f->wall_ms=ms; }
void mc_profile_startup(const char *label,double ms) { fprintf(stderr,"startup %s: %.6f ms\n",label,ms); }
