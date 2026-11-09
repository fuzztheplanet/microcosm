/* Count the actual opaque tyrannosaurus samples that survive the forest's
 * depth buffer. The visibility gates must stay outside the visible entrance
 * and exit; the first glimpse must occupy less of the picture than the pause.
 * Included by render.c, after the demo and engine implementations. */
static PFNGLDRAWELEMENTSINSTANCEDPROC rex_real_elements;
static PFNGLBEGINQUERYPROC rex_begin_query;
static PFNGLENDQUERYPROC rex_end_query;
static PFNGLGETQUERYOBJECTUIVPROC rex_query_result;
static GLuint rex_query, rex_vao, rex_scene, rex_samples;

static void APIENTRY rex_count_samples(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances) {
    GLint vao, fbo;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    int watch = (GLuint)vao == rex_vao && (GLuint)fbo == rex_scene;
    if (watch) rex_begin_query(GL_SAMPLES_PASSED, rex_query);
    rex_real_elements(mode, count, type, indices, instances);
    if (watch) {
        GLuint samples;
        rex_end_query(GL_SAMPLES_PASSED);
        rex_query_result(rex_query, GL_QUERY_RESULT, &samples);
        rex_samples += samples;
    }
}

static void rex_checks(Demo *d, mc_window *w) {
    PFNGLGENQUERIESPROC gen = (void *)eglGetProcAddress("glGenQueries");
    PFNGLDELETEQUERIESPROC del = (void *)eglGetProcAddress("glDeleteQueries");
    rex_begin_query = (void *)eglGetProcAddress("glBeginQuery");
    rex_end_query = (void *)eglGetProcAddress("glEndQuery");
    rex_query_result = (void *)eglGetProcAddress("glGetQueryObjectuiv");
    assert(gen && del && rex_begin_query && rex_end_query && rex_query_result);
    gen(1, &rex_query);
    rex_vao = d->jr_body.vao;
    rex_real_elements = mcgl.DrawElementsInstanced;
    mcgl.DrawElementsInstanced = rex_count_samples;
    const float moments[] = { S3_REX - 4.0f + .02f, 50.0f, 52.0f, 55.0f,
                              57.93f, 58.54f, 62.0f, 66.0f, S3_BLOB - .02f };
    GLuint samples[MC_ARRAY_COUNT(moments)];
    for (int i = 0; i < MC_ARRAY_COUNT(moments); ++i) {
        double t = SCENE3_START + S3_J0 + BAR_S * unstretch(moments[i] / (float)BAR_S, S3_SEG);
        rex_scene = d->R->scene.fbo;
        rex_samples = 0;
        render_frame(d, w, t);
        samples[i] = rex_samples;
        printf("Rex: jungle %.3f s, visible samples %u\n", moments[i], samples[i]);
    }
    mcgl.DrawElementsInstanced = rex_real_elements;
    del(1, &rex_query);
    assert(samples[0] == 0 && samples[1] < samples[2]);
    assert(samples[2] > 0 && samples[3] > samples[2]);
    assert(samples[4] > samples[2] && samples[5] > samples[2]);
    assert(samples[7] == 0 && samples[8] == 0);
    assert(glGetError() == GL_NO_ERROR);
    puts("Rex: opaque tree-occluded entrance, threatening pause and off-screen visibility gates passed");
}
