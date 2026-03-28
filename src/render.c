#include "render.h"
#include "gl_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "layout(location = 2) in vec3 a_color;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "out vec3 v_color;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "in vec3 v_color;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform vec3 u_color;\n"
    "uniform int u_use_vcolor;\n"
    "void main() {\n"
    "    float dist = texture(u_atlas, v_uv).r;\n"
    "    float edge = 0.5;\n"
    "    float w = fwidth(dist);\n"
    "    float alpha = smoothstep(edge - w, edge + w, dist);\n"
    "    if (alpha < 0.01) discard;\n"
    "    vec3 col = u_use_vcolor != 0 ? v_color : u_color;\n"
    "    frag_color = vec4(col, alpha);\n"
    "}\n";

#define OVERLAY_MAX_CHARS 128
#define OVERLAY_FLOATS_PER_GLYPH 30 /* 6 verts * 5 floats */

static GLuint program;
static GLint loc_mvp, loc_atlas, loc_color, loc_use_vcolor;

/* Persistent overlay GPU objects */
static GLuint overlay_vao, overlay_vbo;

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "utterance: shader error: %s\n", log);
        exit(1);
    }
    return s;
}

void render_init(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);

    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "utterance: link error: %s\n", log);
        exit(1);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    loc_mvp = glGetUniformLocation(program, "u_mvp");
    loc_atlas = glGetUniformLocation(program, "u_atlas");
    loc_color = glGetUniformLocation(program, "u_color");
    loc_use_vcolor = glGetUniformLocation(program, "u_use_vcolor");

    /* Allocate persistent overlay VAO/VBO (5 floats/vert — no color attrib) */
    glGenVertexArrays(1, &overlay_vao);
    glBindVertexArray(overlay_vao);
    glGenBuffers(1, &overlay_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, overlay_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 OVERLAY_MAX_CHARS * OVERLAY_FLOATS_PER_GLYPH * sizeof(float),
                 NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    /* attrib 2 (color) not enabled — reads generic default (0,0,0) */
    glBindVertexArray(0);
}

void render_text(const TextMesh *mesh, const Font *font, const float mvp[16]) {
    if (mesh->count == 0) return;

    glUseProgram(program);
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(loc_atlas, 0);
    glUniform1i(loc_use_vcolor, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font->texture);

    glBindVertexArray(mesh->vao);
    glDrawArrays(GL_TRIANGLES, 0, mesh->count * 6);
    glBindVertexArray(0);
}

void render_overlay(Font *font, const char *str, float x, float y, float scale,
                    int win_w, int win_h, const float color[3]) {
    float ortho[16] = {0};
    ortho[0]  =  2.0f / win_w;
    ortho[5]  = -2.0f / win_h;
    ortho[10] = -1.0f;
    ortho[12] = -1.0f;
    ortho[13] =  1.0f;
    ortho[15] =  1.0f;

    static float verts[OVERLAY_MAX_CHARS * OVERLAY_FLOATS_PER_GLYPH];
    int n = 0;
    float cx = x, cy = y;

    for (const char *p = str; *p && n < OVERLAY_MAX_CHARS; p++) {
        Glyph *g = font_glyph(font, (uint32_t)(unsigned char)*p);
        if (!g) continue;

        if (g->width > 0 && g->height > 0) {
            float x0 = cx + g->x_off * scale;
            float y0 = cy + (font->ascent + g->y_off) * scale;
            float x1 = x0 + g->width * scale;
            float y1 = y0 + g->height * scale;

            float *v = verts + n * OVERLAY_FLOATS_PER_GLYPH;
            v[0]=x0; v[1]=y0; v[2]=0; v[3]=g->s0; v[4]=g->t0;
            v[5]=x0; v[6]=y1; v[7]=0; v[8]=g->s0; v[9]=g->t1;
            v[10]=x1; v[11]=y1; v[12]=0; v[13]=g->s1; v[14]=g->t1;
            v[15]=x0; v[16]=y0; v[17]=0; v[18]=g->s0; v[19]=g->t0;
            v[20]=x1; v[21]=y1; v[22]=0; v[23]=g->s1; v[24]=g->t1;
            v[25]=x1; v[26]=y0; v[27]=0; v[28]=g->s1; v[29]=g->t0;
            n++;
        }
        cx += g->advance * scale;
    }
    if (n == 0) return;

    glBindBuffer(GL_ARRAY_BUFFER, overlay_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * OVERLAY_FLOATS_PER_GLYPH * sizeof(float), verts);

    glUseProgram(program);
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, ortho);
    glUniform1i(loc_atlas, 0);
    glUniform3f(loc_color, color[0], color[1], color[2]);
    glUniform1i(loc_use_vcolor, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font->texture);

    glBindVertexArray(overlay_vao);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    glBindVertexArray(0);
}

/* --- Highlight quad (world-space colored rectangle) --- */

static const char *hl_vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "uniform mat4 u_mvp;\n"
    "void main() {\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *hl_frag_src =
    "#version 330 core\n"
    "out vec4 frag_color;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    frag_color = u_color;\n"
    "}\n";

static GLuint hl_program = 0;
static GLint  hl_loc_mvp, hl_loc_color;
static GLuint hl_vao, hl_vbo;

static void hl_ensure_init(void) {
    if (hl_program) return;
    GLuint vs = compile_shader(GL_VERTEX_SHADER, hl_vert_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, hl_frag_src);
    hl_program = glCreateProgram();
    glAttachShader(hl_program, vs);
    glAttachShader(hl_program, fs);
    glLinkProgram(hl_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    hl_loc_mvp = glGetUniformLocation(hl_program, "u_mvp");
    hl_loc_color = glGetUniformLocation(hl_program, "u_color");

    glGenVertexArrays(1, &hl_vao);
    glBindVertexArray(hl_vao);
    glGenBuffers(1, &hl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, hl_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void render_highlight(const float mvp[16], const float bounds[4], const float color[3], float alpha) {
    hl_ensure_init();
    float x0 = bounds[0], y0 = bounds[1], x1 = bounds[2], y1 = bounds[3];
    float verts[18] = {
        x0, y0, 0,  x1, y0, 0,  x1, y1, 0,
        x0, y0, 0,  x1, y1, 0,  x0, y1, 0,
    };
    glBindBuffer(GL_ARRAY_BUFFER, hl_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glUseProgram(hl_program);
    glUniformMatrix4fv(hl_loc_mvp, 1, GL_FALSE, mvp);
    glUniform4f(hl_loc_color, color[0], color[1], color[2], alpha);

    glBindVertexArray(hl_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void render_destroy(void) {
    if (overlay_vbo) glDeleteBuffers(1, &overlay_vbo);
    if (overlay_vao) glDeleteVertexArrays(1, &overlay_vao);
    if (program) glDeleteProgram(program);
    if (hl_vbo) glDeleteBuffers(1, &hl_vbo);
    if (hl_vao) glDeleteVertexArrays(1, &hl_vao);
    if (hl_program) glDeleteProgram(hl_program);
}
