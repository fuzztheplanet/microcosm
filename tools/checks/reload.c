/* The shader sources are a temporary copy that verify.py makes for this check. */
#define main unused_demo_main
#include "../../demos/hello-world/main.c"
#undef main
#include <assert.h>
#include <EGL/egl.h>

static void replace_source(const char *name,const char *source) {
    char path[1024];snprintf(path,sizeof path,"%s/%s",MC_SHADER_ROOT,name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(source,1,strlen(source),f)==strlen(source));assert(!fclose(f));
}
static char *copy_source(const char *name) {
    char path[1024];snprintf(path,sizeof path,"%s/%s",MC_SHADER_ROOT,name);return read_file(path);
}
static void change_comment(const char *name) {
    char *s=copy_source(name);assert(s);char *changed=malloc(strlen(s)+40);assert(changed);
    sprintf(changed,"%s\n// edited during reload check\n",s);replace_source(name,changed);free(s);free(changed);
}
static void reload_now(Demo *d) {d->shader_poll_at=0;maybe_reload_shaders(d);}
/* Every program that reads the Frame block must see FrameUniforms' std140
 * layout on binding 0. GL resets block bindings to 0 on every link, including
 * the relinks after a reload. */
#define M(name,field) {name,offsetof(FrameUniforms,field)}
static const struct {const char *name;size_t offset;} members[]={
    M("uViewProj",view_proj),M("uLightVP",light_vp),M("uCam",cam),M("uTime",time),M("uLightDir",light_dir),
    M("uReal",real),M("uFogColor",fog_color),M("uClarity",clarity),M("uSkyColor",sky_color),M("uQ",q),
    M("uGroundColor",ground_color),M("uFog",fog),M("uRim",rim),M("uWobble",wobble),M("uSun",sun),
    M("uBlobFog",blob_fog),M("uTop",top),M("uGape",gape),M("uBottom",bottom),M("uGlowAmt",glow_amt),
    M("uGlow",glow),M("uGridFade",grid_fade),M("uGridColor",grid_color),M("uAtmo",atmo),M("uSky",sea_sky),
    M("uBright",bright),M("uRes",res),M("uSunUV",sun_uv),M("uTexScale",tex_scale),M("uAlgaeH",algae_h)};
#undef M
static int frame_block_check(Demo *d) {
    PFNGLGETUNIFORMBLOCKINDEXPROC index=(void*)eglGetProcAddress("glGetUniformBlockIndex");
    PFNGLGETACTIVEUNIFORMBLOCKIVPROC block=(void*)eglGetProcAddress("glGetActiveUniformBlockiv");
    PFNGLGETUNIFORMINDICESPROC indices=(void*)eglGetProcAddress("glGetUniformIndices");
    PFNGLGETACTIVEUNIFORMSIVPROC active=(void*)eglGetProcAddress("glGetActiveUniformsiv");
    assert(index&&block&&indices&&active);
    int programs_with_block=0;
    for(int i=0;i<MC_PROGRAM_COUNT;++i) {
        if(programs[i].offset==SIZE_MAX)continue;
        GLuint p=program_at(d,programs[i].offset)->prog,b=index(p,"Frame");GLint v;
        if(b==GL_INVALID_INDEX)continue;
        ++programs_with_block;
        block(p,b,GL_UNIFORM_BLOCK_BINDING,&v);assert(v==0);
        block(p,b,GL_UNIFORM_BLOCK_DATA_SIZE,&v);assert(v==(GLint)sizeof(FrameUniforms));
        for(unsigned k=0;k<sizeof members/sizeof *members;++k) {
            GLuint u;indices(p,1,&members[k].name,&u);assert(u!=GL_INVALID_INDEX);   /* std140 keeps every member active */
            active(p,1,&u,GL_UNIFORM_OFFSET,&v);assert(v==(GLint)members[k].offset);
        }
    }
    assert(glGetError()==GL_NO_ERROR);
    return programs_with_block;
}
static int uniform_int(mc_shader s,const char *name) {
    PFNGLGETUNIFORMIVPROC get=(void*)eglGetProcAddress("glGetUniformiv");assert(get);
    GLint loc=mc_uniform(s,name);assert(loc>=0);GLint v;get(s.prog,loc,&v);return v;
}

int main(void) {
    mc_window_desc desc={64,36,"reload check",0,0};mc_window *w=mc_window_open(&desc);assert(w);
    Demo *d=demo_make(64,36);assert(d);
    int with_block=frame_block_check(d);assert(with_block>0);
    GLuint rock=d->uwrock.prog,shadow=d->shadowrock.prog,sky=d->sky.prog;
    change_comment("rock.vert");reload_now(d);
    assert(d->uwrock.prog!=rock&&d->shadowrock.prog!=shadow&&d->sky.prog==sky);
    /* relinked programs get their fixed sampler units back */
    assert(uniform_int(d->uwrock,"uShadow")==4&&uniform_int(d->uwrock,"uTexRock")==2&&uniform_int(d->uwrock,"uNoise")==1);
    GLuint fish=d->fishsh.prog;shadow=d->shadowfish.prog;
    change_comment("fish.vert");reload_now(d);
    assert(d->fishsh.prog!=fish&&d->shadowfish.prog!=shadow);
    assert(mc_uniform(d->fishsh,"uK")==1&&mc_uniform(d->shadowfish,"uK")==1);   /* the animals' flags, at their fixed location */
    assert(uniform_int(d->fishsh,"uSkin")==3&&frame_block_check(d)==with_block);
    GLuint blob;fish=d->fishsh.prog;shadow=d->shadowfish.prog;   /* a fragment edit leaves the shadow program (same vertex shader) alone */
    change_comment("fish.frag");reload_now(d);assert(d->fishsh.prog!=fish&&d->shadowfish.prog==shadow);
    GLuint sand=d->tex_sand;change_comment("uwbake.frag");reload_now(d);assert(d->tex_sand!=sand);
    /* Two edits, one of which fails: neither gets installed. Fixing only the
     * failed source must then install the other pending change too. */
    char *valid=copy_source("uwsurf.frag");assert(valid);
    rock=d->uwrock.prog;blob=d->blobsh.prog;sand=d->tex_sand;
    change_comment("blob.frag");replace_source("uwsurf.frag","#version 450 core\ninvalid");reload_now(d);
    assert(d->uwrock.prog==rock&&d->blobsh.prog==blob&&d->tex_sand==sand);
    /* The same bad contents again don't trigger another compile attempt. */
    reload_now(d);assert(d->uwrock.prog==rock);
    char *fixed=malloc(strlen(valid)+32);assert(fixed);sprintf(fixed,"%s\n// fixed\n",valid);
    replace_source("uwsurf.frag",fixed);reload_now(d);
    assert(d->uwrock.prog!=rock&&d->blobsh.prog!=blob);
    free(fixed);free(valid);
    assert(frame_block_check(d)==with_block);
    render_frame(d,w,196);assert(glGetError()==GL_NO_ERROR);
    demo_free(d);mc_window_close(w);
    printf("Frame block: std140 layout and binding 0 hold in %d programs across reloads\n",with_block);
    puts("Hot reload: material/shared-vertex dependencies, rapid edits, uniform refresh and transactional retry passed");
}
