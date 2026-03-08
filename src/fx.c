#include "fx.h"
#include "gl_loader.h"
#include <stdio.h>
#include <stdlib.h>

/* --- Shaders --- */

static const char *fx_vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

/*
 * u_progress: 0.0 = blink just started, 1.0 = blink complete.
 * All effects should peak early and fade to clean by progress=1.0.
 */
static const char *fx_frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_scene;\n"
    "uniform vec2 u_resolution;\n"
    "uniform int u_mode;\n"
    "uniform float u_progress;\n"
    "\n"
    "void main() {\n"
    "    vec4 col = texture(u_scene, v_uv);\n"
    "    vec2 center = v_uv - 0.5;\n"
    "    float r = length(center);\n"
    "\n"
    "    if (u_mode == 0) {\n"
    "        /* VIGNETTE — snap shut, smooth open */\n"
    "        /* Close hard in first 15%, hold dark 15-40%, open 40-100% */\n"
    "        float shut;\n"
    "        if (u_progress < 0.15)\n"
    "            shut = u_progress / 0.15;\n"              /* 0→1 fast close */
    "        else if (u_progress < 0.4)\n"
    "            shut = 1.0;\n"                             /* hold black */
    "        else\n"
    "            shut = 1.0 - (u_progress - 0.4) / 0.6;\n" /* 1→0 smooth open */
    "        shut = shut * shut;\n"                         /* sharpen the curve */
    "\n"
    "        /* Eyelid bands from top and bottom */\n"
    "        float lid = abs(v_uv.y - 0.5) * 2.0;\n"
    "        float close_edge = mix(1.2, 0.0, shut);\n"    /* 1.2=fully open, 0=slammed */
    "        float band = smoothstep(close_edge - 0.1, close_edge, lid);\n"
    "\n"
    "        /* Radial iris on top */\n"
    "        float iris = smoothstep(0.0, mix(0.8, 0.05, shut), r);\n"
    "\n"
    "        float dark = max(band, iris);\n"
    "        col.rgb *= 1.0 - dark;\n"
    "\n"
    "    } else if (u_mode == 1) {\n"
    "        /* TUNNEL — radial speed lines, peak at start, fade out */\n"
    "        float fade = 1.0 - u_progress;\n"
    "        fade = fade * fade;\n"                         /* front-loaded intensity */
    "\n"
    "        float angle = atan(center.y, center.x);\n"
    "        float streak = abs(sin(angle * 32.0));\n"     /* 32 radial lines */
    "        streak = pow(streak, 12.0);\n"                 /* razor thin */
    "        float radial = smoothstep(0.02, 0.4, r);\n"   /* edges only */
    "        float intensity = streak * radial * fade;\n"
    "\n"
    "        /* Radial zoom blur */\n"
    "        vec2 blur = center * 0.04 * fade;\n"
    "        vec3 blurred = texture(u_scene, v_uv - blur).rgb;\n"
    "        col.rgb = mix(col.rgb, blurred, fade * 0.5);\n"
    "\n"
    "        /* Additive speed lines — cold blue-white */\n"
    "        col.rgb += vec3(0.5, 0.6, 1.0) * intensity;\n"
    "\n"
    "    } else if (u_mode == 2) {\n"
    "        /* CHROMATIC ABERRATION — hard snap, fast decay */\n"
    "        float fade = 1.0 - u_progress;\n"
    "        fade = fade * fade * fade;\n"                  /* cubic decay — punchy */
    "\n"
    "        float aberr = r * r * fade * 0.04;\n"          /* stronger split */
    "        vec2 dir = normalize(center + 0.0001);\n"
    "        float cr = texture(u_scene, v_uv + dir * aberr).r;\n"
    "        float cg = texture(u_scene, v_uv).g;\n"
    "        float cb = texture(u_scene, v_uv - dir * aberr).b;\n"
    "        col.rgb = vec3(cr, cg, cb);\n"
    "\n"
    "        /* White flash at impact */\n"
    "        col.rgb += vec3(0.15, 0.12, 0.2) * fade;\n"
    "\n"
    "    } else if (u_mode == 3) {\n"
    "        /* SCANLINE WIPE — CRT resync, sweep top to bottom */\n"
    "        float scan_y = u_progress;\n"                  /* sweeps 0→1 */
    "        float band = 1.0 - smoothstep(0.0, 0.12, abs(v_uv.y - scan_y));\n"
    "\n"
    "        /* High-freq scanlines in band */\n"
    "        float scanline = sin(v_uv.y * u_resolution.y * 3.14159);\n"
    "        scanline = scanline * scanline;\n"
    "\n"
    "        /* Horizontal tear/jitter */\n"
    "        float jitter = band * sin(v_uv.y * 300.0 + u_progress * 80.0) * 0.006;\n"
    "        vec3 torn = texture(u_scene, vec2(v_uv.x + jitter, v_uv.y)).rgb;\n"
    "\n"
    "        /* Darken + scanline modulation */\n"
    "        float darken = band * (0.4 + 0.6 * scanline);\n"
    "        col.rgb = torn * (1.0 - darken * 0.7);\n"
    "\n"
    "        /* Green phosphor tint */\n"
    "        col.rgb += vec3(-0.03, 0.06, -0.03) * band;\n"
    "\n"
    "        /* Brightness spike at the sweep front */\n"
    "        float front = 1.0 - smoothstep(0.0, 0.02, abs(v_uv.y - scan_y));\n"
    "        col.rgb += vec3(0.2, 0.25, 0.2) * front;\n"
    "    }\n"
    "\n"
    "    frag_color = col;\n"
    "}\n";

/* --- State --- */

static FxMode mode = FX_VIGNETTE;

/* GPU resources */
static GLuint fbo, fbo_tex;
static GLuint quad_vao, quad_vbo;
static GLuint fx_program;
static GLint  loc_scene, loc_resolution, loc_mode, loc_progress;
static int    fbo_w, fbo_h;

/* --- Helpers --- */

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "fx: shader error: %s\n", log);
        exit(1);
    }
    return s;
}

static void create_fbo_texture(int w, int h) {
    if (fbo_tex) glDeleteTextures(1, &fbo_tex);
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "fx: framebuffer incomplete: 0x%x\n", status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    fbo_w = w;
    fbo_h = h;
}

/* --- Public API --- */

void fx_init(int w, int h) {
    glGenFramebuffers(1, &fbo);
    fbo_tex = 0;
    create_fbo_texture(w, h);

    static const float quad_verts[] = {
        -1, -1,  0, 0,
        +1, -1,  1, 0,
        +1, +1,  1, 1,
        -1, -1,  0, 0,
        +1, +1,  1, 1,
        -1, +1,  0, 1,
    };
    glGenVertexArrays(1, &quad_vao);
    glBindVertexArray(quad_vao);
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    GLuint vs = compile_shader(GL_VERTEX_SHADER, fx_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fx_frag_src);
    fx_program = glCreateProgram();
    glAttachShader(fx_program, vs);
    glAttachShader(fx_program, fs);
    glLinkProgram(fx_program);
    GLint ok;
    glGetProgramiv(fx_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(fx_program, sizeof(log), NULL, log);
        fprintf(stderr, "fx: link error: %s\n", log);
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    loc_scene      = glGetUniformLocation(fx_program, "u_scene");
    loc_resolution = glGetUniformLocation(fx_program, "u_resolution");
    loc_mode       = glGetUniformLocation(fx_program, "u_mode");
    loc_progress   = glGetUniformLocation(fx_program, "u_progress");
}

void fx_resize(int w, int h) {
    if (w == fbo_w && h == fbo_h) return;
    create_fbo_texture(w, h);
}

void fx_cycle(void) {
    mode = (FxMode)((mode + 1) % FX_COUNT);
    static const char *names[] = {"vignette", "tunnel", "chromatic", "scanline", "off"};
    fprintf(stderr, "utterance: blink fx → %s\n", names[mode]);
}

FxMode fx_mode(void) {
    return mode;
}

void fx_begin(float progress) {
    if (mode == FX_OFF || progress <= 0.0f || progress >= 1.0f) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void fx_end(int w, int h, float progress) {
    if (mode == FX_OFF || progress <= 0.0f || progress >= 1.0f) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glUseProgram(fx_program);
    glUniform1i(loc_scene, 0);
    glUniform2f(loc_resolution, (float)w, (float)h);
    glUniform1i(loc_mode, (int)mode);
    glUniform1f(loc_progress, progress);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);

    glBindVertexArray(quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void fx_destroy(void) {
    if (quad_vbo) glDeleteBuffers(1, &quad_vbo);
    if (quad_vao) glDeleteVertexArrays(1, &quad_vao);
    if (fbo_tex)  glDeleteTextures(1, &fbo_tex);
    if (fbo)      glDeleteFramebuffers(1, &fbo);
    if (fx_program) glDeleteProgram(fx_program);
}
