/* This fixture includes the implementation directly, so it can inspect target
 * lifetimes and drive the demo's actual release geometry, shaders and
 * render_frame. */
#define main microcosm_unused_main
#include "../../demos/hello-world/main.c"
#undef main
#include "../../src/render.c"
#include "profile.h"
#include <assert.h>
#include <EGL/egl.h>
static PFNGLCHECKFRAMEBUFFERSTATUSPROC real_status;
static int fail_at, calls;
static GLenum APIENTRY fail_status(GLenum t) { return ++calls==fail_at ? GL_FRAMEBUFFER_UNSUPPORTED : real_status(t); }
static PFNGLGETSHADERIVPROC real_shader_status;
static void APIENTRY fail_shader(GLuint s, GLenum p, GLint *v) { real_shader_status(s,p,v); if(p==GL_COMPILE_STATUS)*v=0; }
static void dump_shader(const char *dir,const char *name,const char *source) {
    char path[1024]; snprintf(path,sizeof path,"%s/%s",dir,name); FILE *f=fopen(path,"w"); assert(f); fputs(source,f); fclose(f);
}
static void gl_checks(void) {
    typedef GLboolean (APIENTRY *IsTexture)(GLuint);
    typedef void (APIENTRY *TexLevel)(GLenum,GLint,GLenum,GLint*);
    IsTexture istexture=(IsTexture)eglGetProcAddress("glIsTexture");
    TexLevel texlevel=(TexLevel)eglGetProcAddress("glGetTexLevelParameteriv");
    assert(istexture && texlevel);
    for(int i=0;i<64;++i) {
        mc_target t=mc_target_make_format(16,16,0,1); assert(mc_target_ok(&t) && !t.color);
        mc_target_bind(&t); GLint b; glGetIntegerv(GL_DRAW_BUFFER,&b); assert(b==GL_NONE); glGetIntegerv(GL_READ_BUFFER,&b); assert(b==GL_NONE);
        GLuint depth=t.depth; assert(istexture(depth)); mc_target_free(&t); assert(!istexture(depth));
    }
    mc_renderer *r=mc_renderer_make(65,37); assert(r && r->ao.w==33 && r->ao.h==19);
    GLint format; glBindTexture(GL_TEXTURE_2D,r->ao.color); texlevel(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&format); assert(format==GL_RGBA8);
    real_status=mcgl.CheckFramebufferStatus;
    for(fail_at=1;fail_at<=7;++fail_at) {
        GLuint scene=r->scene.fbo,depth=r->scene.depth; calls=0; mcgl.CheckFramebufferStatus=fail_status;
        assert(!mc_renderer_resize(r,73,41)); mcgl.CheckFramebufferStatus=real_status;
        assert(r->scene.fbo==scene && r->scene.depth==depth && istexture(depth));
        assert(r->w==65 && r->h==37); mc_renderer_bind_scene(r); assert(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE);
    }
    assert(!mc_renderer_resize(r,0,50)); assert(!mc_renderer_resize(r,0x7fffffff,1));
    assert(mc_renderer_set_scale(r,75));
    assert(r->w==49 && r->h==28 && fabsf(mc_renderer_aspect(r)-65.0f/37)<1e-6f);
    for(fail_at=1;fail_at<=7;++fail_at) {
        GLuint previous=r->scene.fbo;calls=0;mcgl.CheckFramebufferStatus=fail_status;
        assert(!mc_renderer_set_scale(r,50));mcgl.CheckFramebufferStatus=real_status;
        assert(r->scene.fbo==previous && r->scale==75 && r->w==49 && r->h==28);
    }
    assert(!mc_renderer_set_scale(r,0) && !mc_renderer_set_scale(r,201));
    assert(mc_renderer_set_scale(r,100));
    assert(mc_renderer_resize(r,1,1) && r->ao.w==1 && r->ao.h==1);
    for(int i=0;i<8;++i) { GLuint depth=r->scene.depth; assert(mc_renderer_resize(r,48+i,32+i)); assert(!istexture(depth)); }
    mc_renderer_free(r);
    /* Real compile and link failures, plus a missing required demo shader. */
    mc_shader bad=mc_shader_make("#version 450 core\ninvalid",BRIGHT_FS); assert(!bad.prog);
    /* A stage-interface mismatch fails at link time. Both stages are written out
     * here because the engine's post shaders arrive minified, with their varying
     * renamed. */
    bad=mc_shader_make("#version 450 core\nout vec3 uv;void main(){uv=vec3(1);gl_Position=vec4(0);}",
                       "#version 450 core\nin vec2 uv;out vec4 o;void main(){o=vec4(uv,0,1);}"); assert(!bad.prog);
    real_shader_status=mcgl.GetShaderiv; mcgl.GetShaderiv=fail_shader;
    Demo *d=calloc(1,sizeof *d); assert(d); assert(!load_shaders(d)); mcgl.GetShaderiv=real_shader_status; demo_free(d);
    mc_shader bake=program_source(MC_PROG_BAKE); assert(bake.prog);
    calls=0;fail_at=1;mcgl.CheckFramebufferStatus=fail_status; assert(!bake_tex(bake,0)); mcgl.CheckFramebufferStatus=real_status;mc_shader_free(&bake);
    assert(glGetError()==GL_NO_ERROR);
    puts("GL: depth lifetime, depth-only FBO, odd/minimum R8 targets, resize rollback and required-shader failures passed");
}

static void context_checks(void) {
    for(int i=0;i<3;++i) {
        mc_window_desc desc={8,8,"context reopen",0,0};
        mc_window *w=mc_window_open(&desc);assert(w);
        mc_shader sh=mc_shader_make(POST_VS,"#version 450 core\nout vec4 o;void main(){o=vec4(1,0,1,1);}");
        assert(sh.prog);mc_target_bind_fbo(mc_window_output_fbo(w),8,8);
        mc_shader_use(sh);mc_fullscreen_triangle();
        unsigned char pixels[8*8*4];assert(mc_window_read_pixels(w,pixels));
        assert(pixels[0]==255 && pixels[1]==0 && pixels[2]==255 && glGetError()==GL_NO_ERROR);
        mc_shader_free(&sh);mc_window_close(w);
    }
    puts("GL: open/draw/close/reopen contexts passed");
}

static void index_checks(void) {
    PFNGLGETBUFFERPARAMETERIVPROC size=(void*)eglGetProcAddress("glGetBufferParameteriv");
    PFNGLGETBUFFERSUBDATAPROC read=(void*)eglGetProcAddress("glGetBufferSubData");
    assert(size&&read);
    mc_mesh m=mc_mesh_new();m.vert_count=65537;m.verts=calloc(m.vert_count,sizeof *m.verts);assert(m.verts);
    mc_u32 indices[]={0,1,65535};m.indices=indices;m.index_count=3;
    for(int mode=0;mode<2;++mode) {
        indices[2]=65535+mode;mc_gpu_mesh g=mc_mesh_upload(&m);
        assert(g.index_type==(mode?GL_UNSIGNED_INT:GL_UNSIGNED_SHORT));
        glBindVertexArray(g.vao);GLint bytes=0;size(GL_ELEMENT_ARRAY_BUFFER,GL_BUFFER_SIZE,&bytes);
        assert(bytes==(mode?12:6));
        if(mode) {mc_u32 data[3];read(GL_ELEMENT_ARRAY_BUFFER,0,12,data);assert(!memcmp(data,indices,12));}
        else {uint16_t data[3];read(GL_ELEMENT_ARRAY_BUFFER,0,6,data);assert(data[0]==0&&data[1]==1&&data[2]==65535);}
        mc_mesh_gpu_free(&g);
    }
    free(m.verts);assert(glGetError()==GL_NO_ERROR);
    puts("GL: uint16 maximum index and uint32 fallback preserve GPU index data");
}

static MC_PFN_DRAWARRAYS post_draw;
static int post_draws;
static void APIENTRY count_post(GLenum mode,GLint first,GLsizei n) {++post_draws;post_draw(mode,first,n);}
static void post_checks(void) {
    mc_renderer *r=mc_renderer_make(32,32);assert(r);
    mc_target output=mc_target_make(32,32,0,0);assert(mc_target_ok(&output));
    mc_post p=mc_post_defaults();p.bloom=0;p.ssao=0;p.grain=0;
    post_draw=mcgl1.DrawArrays;mcgl1.DrawArrays=count_post;
    unsigned char a[32*32*4],b[32*32*4];
    for(int blend=0;blend<2;++blend) {
        mc_renderer_begin(r,mc_vec3(.2f,.3f,.4f));
        if(blend) {glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE);}
        post_draws=0;mc_renderer_end(r,output.fbo,32,32,p);assert(post_draws==3);
        glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,blend?b:a);
    }
    assert(!memcmp(a,b,sizeof a));
    p.bloom=.2f;post_draws=0;mc_renderer_begin(r,mc_vec3(.2f,.3f,.4f));
    mc_renderer_end(r,output.fbo,32,32,p);assert(post_draws==14);
    mcgl1.DrawArrays=post_draw;mc_target_free(&output);mc_renderer_free(r);assert(glGetError()==GL_NO_ERROR);
    puts("GL: custom blending does not leak into post; absent effects skip their dependent passes");
}

static void bake_checks(void) {
    /* GLSL 4.x matches varyings strictly by name, so use the minified shader's own input name. */
    const char *fs=mc_demo_shader("uwbake.frag"),*in=strstr(fs,"in vec2 ");char vs[400],uv[16];
    assert(in&&sscanf(in+8,"%15[A-Za-z0-9_]",uv)==1);
    snprintf(vs,sizeof vs,"#version 450 core\nout vec2 %s;uniform float edge;uniform int axis;"
        "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "%s=axis==0?vec2(edge,p.x):vec2(p.x,edge);gl_Position=vec4(p*2-1,0,1);}",uv,uv);
    mc_shader sh=mc_shader_make(vs,fs);assert(sh.prog);
    mc_target t=mc_target_make(64,1,0,0);assert(mc_target_ok(&t));
    unsigned char a[256],b[256];glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);
    for(int material=0;material<4;++material)for(int axis=0;axis<2;++axis) {
        mc_target_bind(&t);mc_shader_use(sh);mc_set_i(0,material);   /* uType: location 0 */
        mc_set_i(mc_uniform(sh,"axis"),axis);
        for(int edge=0;edge<2;++edge) {mc_set_f(mc_uniform(sh,"edge"),edge);mc_fullscreen_triangle();glReadPixels(0,0,64,1,GL_RGBA,GL_UNSIGNED_BYTE,edge?b:a);}
        for(int i=0;i<256;++i)assert(abs((int)a[i]-(int)b[i])<=1);
    }
    mc_target_free(&t);mc_shader_free(&sh);assert(glGetError()==GL_NO_ERROR);
    puts("Materials: all four bake types have matching opposing edges in every channel");
}

static void shadow_fit_checks(Demo *d) {
    for(double t=S2_BREACH;t<SCENE2_DUR;t+=.73) {
        mc_camera cam=camera_at2(t);mc_m4 m=uw_light_vp(d,&cam);mc_frustum f=mc_frustum_make(m);
        mc_v3 forward=mc_v3_normalize(mc_v3_sub(cam.target,cam.pos));
        mc_v3 right=mc_v3_normalize(mc_v3_cross(forward,cam.up)),up=mc_v3_cross(right,forward);
        for(int depth=0;depth<2;++depth)for(int x=-1;x<=1;x+=2)for(int y=-1;y<=1;y+=2) {
            float z=depth?40:.1f,h=z*tanf(cam.fovy*.5f),w=h*mc_renderer_aspect(d->R);
            mc_v3 p=mc_v3_add(cam.pos,mc_v3_add(mc_v3_scale(forward,z),mc_v3_add(mc_v3_scale(right,x*w),mc_v3_scale(up,y*h))));
            assert(mc_frustum_sphere(&f,p,0));
        }
        for(int axis=12;axis<=13;++axis) {float pixel=m.m[axis]*UW_SHADOW_RES*.5f;assert(fabsf(pixel-roundf(pixel))<.005f);}
    }
    puts("Shadows: authored receiver frustum fits and projection translations stay on texels");
}
/* uwplace.comp's fy() in C: the rendered seabed's height at (x, z), worked out
 * on the grid's own triangles (each cell is split along its a-c diagonal). */
static float uw_floor_y(float x, float z) {
    float gj=mc_clampf((x+UW_TERR_SIZE*.5f)/(UW_TERR_SIZE/UW_TERR_SUB),0,UW_TERR_SUB-1e-4f);
    float gi=mc_clampf((z+UW_TERR_SIZE*.5f)/(UW_TERR_SIZE/UW_TERR_SUB),0,UW_TERR_SUB-1e-4f);
    int j=(int)gj,i=(int)gi,a=j*(UW_TERR_SUB+1)+i,n=UW_TERR_SUB;float fj=gj-j,fi=gi-i,ya=uw_in.h[a],yc=uw_in.h[a+n+2];
    if(fi>=fj)return ya+(uw_in.h[a+1]-ya)*fi+(yc-uw_in.h[a+1])*fj;
    return ya+(uw_in.h[a+n+1]-ya)*fj+(yc-uw_in.h[a+n+1])*fi;
}
/* Checks the seabed and the boulder that uwplace.comp's first pass leaves in its
 * buffer. The heights must match the engine's own terrain (mc_mesh_terrain,
 * whose mesh the seabed used to be), also when sampled through fy()'s
 * triangles. The boulder's vertices must be within a thousandth of its
 * placement radius (UW_ROCK_R), with unit normals that point outward. */
static void bake_shape_checks(Demo *d) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,d->uw_buf);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,(GLsizeiptr)offsetof(struct UwIn,path),&uw_in);
    mc_mesh m=mc_mesh_terrain(UW_TERR_SIZE,UW_TERR_SUB,UW_TERR_AMP,UW_TERR_SEED);
    assert(m.vert_count==(UW_TERR_SUB+1)*(UW_TERR_SUB+1));
    float worst=0,far=0,low=9;
    for(int i=0;i<m.vert_count;++i)worst=fmaxf(worst,fabsf(uw_in.h[i]-(m.verts[i].pos.y+UW_FLOOR_BASE)));
    assert(worst<1e-4f);
    for(int i=0;i<m.index_count;i+=93) { /* sample both triangulations across the grid */
        mc_v3 a=m.verts[m.indices[i]].pos,b=m.verts[m.indices[i+1]].pos,c=m.verts[m.indices[i+2]].pos;
        mc_v3 p=mc_v3_add(mc_v3_scale(a,.2f),mc_v3_add(mc_v3_scale(b,.3f),mc_v3_scale(c,.5f)));
        assert(fabsf(uw_floor_y(p.x,p.z)-(p.y+UW_FLOOR_BASE))<.0002f);
    }
    mc_mesh_free(&m);
    for(int i=0;i<UW_ROCK_VERTS;++i) {   /* each LOD has 6 (n + 1)^2 vertices; LODs start UW_ROCK_VERTS / 3 apart */
        int m=(UW_ROCK_N>>i/(UW_ROCK_VERTS/3))+1; if(i%(UW_ROCK_VERTS/3)>=6*m*m) continue;
        mc_v3 p=mc_vec3(uw_in.rock[2*i].x,uw_in.rock[2*i].y,uw_in.rock[2*i].z),n=mc_vec3(uw_in.rock[2*i+1].x,uw_in.rock[2*i+1].y,uw_in.rock[2*i+1].z);
        far=fmaxf(far,mc_v3_len(p));low=fminf(low,mc_v3_len(p));
        assert(fabsf(mc_v3_len(n)-1)<1e-4f && mc_v3_dot(n,mc_v3_normalize(p))>.2f);
    }
    assert(low>.5f && far<UW_ROCK_R+1e-3f);
    printf("Seabed: GPU heights within %.2g m of mc_mesh_terrain; samples agree with rendered triangles; "
           "boulder radius %.4f..%.4f (placement's %.4f)\n",worst,low,far,UW_ROCK_R);
}
#include "uwplace.h"
static void ppm(const char *dir,const char *label,double t,int w,int h,const unsigned char *pixels) {
    char path[1024];snprintf(path,sizeof path,"%s/%s-%.2f.ppm",dir,label,t); FILE *f=fopen(path,"wb");assert(f);
    fprintf(f,"P6\n%d %d\n255\n",w,h);for(int i=0;i<w*h;++i)fwrite(pixels+i*4,1,3,f);fclose(f);
}
static void deformation_check(Demo *d) {
    mc_target target=mc_target_make_format(128,128,0,1); assert(mc_target_ok(&target));
    mc_gpu_mesh *meshes[]={&d->uw_rock[0],&d->uw_pred,&d->uw_whale};   /* the blob is a lens and casts no shadow */
    mc_shader color[]={d->uwrock,d->fishsh,d->whalesh};
    mc_shader shadow[]={d->shadowrock,d->shadowfish,d->shadowfish};
    float *a=malloc(128*128*sizeof(float)),*b=malloc(128*128*sizeof(float));assert(a&&b);
    glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);
    for(int i=0;i<3;++i)for(int moment=0;moment<2;++moment) {
        mc_instance item={mc_m4_rotate(mc_vec3(0,1,0),.7f),mc_vec4(.1f,.2f,.3f,7.3f)};
        mc_mesh_set_instances(meshes[i],&item,1);
        float r=meshes[i]->radius+.4f;
        mc_m4 vp=mc_m4_mul(mc_m4_ortho(-r,r,-r,r,.1f,10*r),mc_m4_look_at(mc_vec3(0,0,3*r),mc_vec3(0,0,0),mc_vec3(0,1,0)));
        FrameUniforms frame={0};frame.view_proj=vp;frame.real=(float)moment;frame.time=moment*91.3f;
        frame.wobble=moment?.15f:1;frame.gape=moment?1:.1f;mc_gl_frame_block(&frame,sizeof frame);   /* predator jaws */
        for(int pass=0;pass<2;++pass) {
            mc_shader sh=pass?shadow[i]:color[i];mc_target_bind(&target);glClear(GL_DEPTH_BUFFER_BIT);mc_shader_use(sh);
            if(i>0)glUniform1i(1,i>1?4:8);   /* fish.vert's uK: 4 for the whale, 8 for the predator */
            mc_mesh_draw_instanced(*meshes[i],1);glReadPixels(0,0,128,128,GL_DEPTH_COMPONENT,GL_FLOAT,pass?b:a);
        }
        int covered=0;for(int p=0;p<128*128;++p){if(a[p]<1)++covered;assert(fabsf(a[p]-b[p])<1e-6f);}assert(covered>100);
    }
    free(a);free(b);mc_target_free(&target);assert(glGetError()==GL_NO_ERROR);
    puts("Shadows: rock/predator/whale depth silhouettes match color deformation at two animation endpoints");
}

static void images(Demo *d,mc_window *w,int width,int height,const char *dir) {
    size_t n=(size_t)width*height*4;unsigned char *a=malloc(n),*b=malloc(n);assert(a&&b);
    /* Fixed story moments: scene 1, underwater story times (uw_t), the jungle and
     * the end card. S3J turns the jungle's story seconds into show time,
     * stretched the way the show plays them. */
#define S3J(j) (SCENE3_START+S3_J0+BAR_S*unstretch((float)(j)/(float)BAR_S,S3_SEG))
    const double times[]={30,SCENE2_START+uw_t(.01f),SCENE2_START+uw_t(4.3f),SCENE2_START+uw_t(40.3f),SCENE2_START+uw_t(46.3f),
                          SCENE2_START+uw_t(53.3f),SCENE2_START+uw_t(68.3f),SCENE2_START+uw_t(84.3f),SCENE2_START+uw_t(97.3f),SCENE2_START+uw_t(101.3f),
                          S3J(3),S3J(11),S3J(S3_UP+4),S3J(S3_MACRO+4),S3J(S3_FLY+2),
                          S3J(S3_GAP+0.5),S3J(S3_REX+3.5),S3J(S3_BLOB+6),
                          S3J(S3_BLOB+21.7),SCENE4_START+4.5};
    for(unsigned k=0;k<sizeof times/sizeof *times;++k) {
        double t=times[k]; if(t<SCENE1_DUR)sim_advance(d,t);
        d->disable_lod=1;d->disable_culling=0;render_frame(d,w,t);mc_window_read_pixels(w,a);
        d->disable_culling=1;render_frame(d,w,t);mc_window_read_pixels(w,b);
        unsigned changed=0,maxerr=0;unsigned long long sum=0;
        for(size_t i=0;i<n;++i){unsigned e=abs((int)a[i]-(int)b[i]);if(e){++changed;sum+=e;if(e>maxerr)maxerr=e;}}
        printf("culling t=%.2f changed_channels=%u max=%u mean=%.8f\n",t,changed,maxerr,(double)sum/n);
        /* Allow raster ties on shared boundaries, but fail on large or missing silhouettes. */
        assert((double)sum/n<.02 && changed<n/1000+4);
        d->disable_lod=0;d->disable_culling=0;render_frame(d,w,t);mc_window_read_pixels(w,b);
        ppm(dir,"frame",t,width,height,b);
        if(t>=SCENE2_START+S2_BREACH)ppm(dir,"full-lod",t,width,height,a);
        d->disable_lod=1;d->legacy_order=1;render_frame(d,w,t);mc_window_read_pixels(w,b);
        unsigned long long order_error=0;for(size_t i=0;i<n;++i)order_error+=abs((int)a[i]-(int)b[i]);
        printf("draw-order t=%.2f mean=%.8f\n",t,(double)order_error/n);
        assert((double)order_error/n<.05);d->legacy_order=0;d->disable_lod=0;
        assert(glGetError()==GL_NO_ERROR);
    }
    free(a);free(b);
}
#include "rex.h"

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    assert(argc>=6);const char *mode=argv[1],*dir=argv[2];int width=atoi(argv[3]),height=atoi(argv[4]),repeats=atoi(argv[5]);
    if(!strcmp(mode,"check"))context_checks();
    mc_window_desc desc={width,height,"private checks",0,0};mc_window *w=mc_window_open(&desc);assert(w);
    fprintf(stderr,"GL renderer: %s\nGL version: %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    dump_shader(dir,"post.vert",POST_VS);
    const char *names[]={"bright.frag","blur.frag","ao_filter.frag","composite.frag","gtao.frag","taa.frag","present.frag"};
    const char *sources[]={BRIGHT_FS,BLUR_FS,AO_BLUR_FS,COMPOSITE_FS,GTAO_FS,TAA_FS,PRESENT_FS};
    for(int i=0;i<7;++i)dump_shader(dir,names[i],sources[i]);
    if(!strcmp(mode,"check")){ gl_checks(); index_checks(); post_checks(); bake_checks(); }
    double start=mc_time_seconds();Demo *d=demo_make(width,height);assert(d);glFinish();
    if(argc>8) assert(mc_renderer_set_scale(d->R,atoi(argv[8])));
    printf("init_seconds=%.6f props=%d fish=%d\n",mc_time_seconds()-start,d->uw_np,d->uw_nf);
    assert(d->uw_np>0);
    if(!strcmp(mode,"check")){ bake_shape_checks(d); placement_checks(d); shadow_fit_checks(d); deformation_check(d); images(d,w,width,height,dir); rex_checks(d,w); }
    else {
        char path[1024];snprintf(path,sizeof path,"%s/profile.csv",dir);FILE *csv=fopen(path,"w");assert(csv);assert(profile_init(csv));
        if(argc>6) d->disable_culling=atoi(argv[6]);
        if(argc>7) d->disable_lod=atoi(argv[7]);
        if(argc>9) d->legacy_order=atoi(argv[9]);
        if(!strcmp(mode,"sequence")) {
            double from=atof(argv[10]),duration=atof(argv[11]),fps=atof(argv[12]);
            if(from<SCENE1_DUR)sim_advance(d,from);
            int frames=(int)ceil(duration*fps);
            for(int frame=0;frame<frames;++frame) {
                double t=from+(double)frame/fps;
                double wall=mc_time_seconds();profile_frame(t);
                double sim=mc_time_seconds();if(t<SCENE1_DUR)sim_advance(d,t);
                mc_profile_cpu(0,(mc_time_seconds()-sim)*1000);
                render_frame(d,w,t);profile_end();glFinish();
                profile_wall((mc_time_seconds()-wall)*1000);
                assert(glGetError()==GL_NO_ERROR);
            }
            printf("sequence start=%.3f duration=%.3f fps=%.3f frames=%d; advancing simulation included\n",from,duration,fps,frames);
        } else {
        const double times[]={30,SCENE2_START+uw_t(4.3f),SCENE2_START+uw_t(40.3f),SCENE2_START+uw_t(68.3f),SCENE2_START+uw_t(97.3f),
                              S3J(6),S3J(S3_UP+4),S3J(S3_MACRO+4),
                              S3J(S3_REX+3.5),S3J(S3_BLOB+6),S3J(S3_BLOB+21.7)};
        for(unsigned k=0;k<sizeof times/sizeof *times;++k) {
            double t=times[k];
            if(t<SCENE1_DUR)sim_advance(d,t);
            double first=mc_time_seconds();render_frame(d,w,t);glFinish();
            printf("frame_t=%.1f first_ms=%.6f\n",t,(mc_time_seconds()-first)*1000);
            for(int j=0;j<7;++j) render_frame(d,w,t);
            glFinish();
            start=mc_time_seconds();
            for(int j=0;j<repeats;++j) {
                double wall_start=mc_time_seconds();profile_frame(t);double sim=mc_time_seconds();if(t<SCENE1_DUR)sim_advance(d,t);
                mc_profile_cpu(0,(mc_time_seconds()-sim)*1000);render_frame(d,w,t);profile_end();glFinish();profile_wall((mc_time_seconds()-wall_start)*1000);
                assert(glGetError()==GL_NO_ERROR);
            }
            printf("frame_t=%.1f repeat_mean_ms=%.6f\n",t,(mc_time_seconds()-start)*1000/repeats);
            unsigned char *pixels=malloc((size_t)width*height*4);assert(pixels);
            assert(mc_window_read_pixels(w,pixels));ppm(dir,"frame",t,width,height,pixels);free(pixels);
        }
        }
        profile_close();fclose(csv);
    }
    demo_free(d);mc_window_close(w);puts("render checks completed");return 0;
}
