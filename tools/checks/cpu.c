#include "microcosm/mc_mesh.h"
#include "microcosm/mc_scene.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
static int near(float a,float b) { return fabsf(a-b)<1e-5f; }
int main(void) {
    mc_key_f f[257]; mc_key_v3 v[257];
    for(int i=0;i<257;++i) { f[i]=(mc_key_f){i,i*2,MC_EASE_LINEAR}; v[i]=(mc_key_v3){i,mc_vec3(i,i*2,i*3),MC_EASE_LINEAR}; }
    assert(mc_track_f(NULL,0,2)==0); assert(mc_track_f(f,1,2)==0);
    assert(mc_track_f(f,257,-1)==0 && mc_track_f(f,257,999)==512);
    assert(near(mc_track_f(f,257,200.25f),400.5f));
    assert(near(mc_track_v3(v,257,200.25f).z,600.75f));
    mc_key_f dup[]={{0,0,0},{1,1,0},{1,3,0},{2,5,0}};
    assert(mc_track_f(dup,4,1)==3); assert(mc_track_f(dup,4,1.5f)==4);
    mc_mesh m=mc_mesh_new(), a=mc_mesh_new();
    mc_mesh_push_vertex(&m,mc_vec3(0,0,0),mc_vec3(0,0,1));
    mc_mesh_push_vertex(&m,mc_vec3(1,0,0),mc_vec3(0,0,1));
    mc_mesh_push_vertex(&m,mc_vec3(0,1,0),mc_vec3(0,0,1));
    m.verts[0].uv=mc_vec2(0,0); m.verts[1].uv=mc_vec2(1,0); m.verts[2].uv=mc_vec2(0,1);
    mc_mesh_push_tri(&m,0,1,2); mc_mesh_append(&a,&m,mc_m4_translate(mc_vec3(4,5,6)));
    assert(a.verts[1].uv.x==1 && a.verts[2].uv.y==1); assert(a.verts[1].pos.x==5);
    mc_mesh_subdivide(&m,2);
    for(int i=0;i<m.vert_count;++i) { assert(near(m.verts[i].uv.x,m.verts[i].pos.x)); assert(near(m.verts[i].uv.y,m.verts[i].pos.y)); }
    mc_mesh_free(&m); mc_mesh_free(&a);
    mc_frustum box=mc_frustum_make(mc_m4_identity());
    assert(mc_frustum_sphere(&box,mc_vec3(0,0,0),0));
    assert(mc_frustum_sphere(&box,mc_vec3(2,0,0),1));
    for(int axis=0;axis<3;++axis) for(int sign=-1;sign<=1;sign+=2) {
        mc_v3 p=mc_vec3(0,0,0); if(axis==0)p.x=sign*2.1f; if(axis==1)p.y=sign*2.1f; if(axis==2)p.z=sign*2.1f;
        assert(!mc_frustum_sphere(&box,p,1));
    }
    mc_frustum perspective=mc_frustum_make(mc_m4_perspective(MC_PI/2,1,1,10));
    assert(mc_frustum_sphere(&perspective,mc_vec3(0,0,-5),.1f));
    assert(!mc_frustum_sphere(&perspective,mc_vec3(0,0,5),.1f));
    assert(!mc_frustum_sphere(&perspective,mc_vec3(0,0,-11),.5f));
    puts("CPU: long/duplicate tracks, UV append/subdivision and frustum boundaries passed");
}
