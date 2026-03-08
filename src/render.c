#include "render.h"
#include "gl_loader.h"
#include <stdio.h>
#include <stdlib.h>

static const char *vert_src =
    "#version 330 core\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
    "}\n";

static const char *frag_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform vec3 u_color;\n"
    "void main() {\n"
    "    float dist = texture(u_atlas, v_uv).r;\n"
    "    float edge = 0.5;\n"
    "    float w = fwidth(dist);\n"
    "    float alpha = smoothstep(edge - w, edge + w, dist);\n"
    "    if (alpha < 0.01) discard;\n"
    "    frag_color = vec4(u_color, alpha);\n"
    "}\n";

static GLuint program;
static GLint loc_mvp, loc_atlas, loc_color;

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

int render_init(void) {
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

    return 0;
}

void render_text(const TextMesh *mesh, const Font *font, const float mvp[16], const float color[3]) {
    if (mesh->count == 0) return;

    glUseProgram(program);
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, mvp);
    glUniform1i(loc_atlas, 0);
    glUniform3f(loc_color, color[0], color[1], color[2]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font->texture);

    glBindVertexArray(mesh->vao);
    glDrawArrays(GL_TRIANGLES, 0, mesh->count * 6);
    glBindVertexArray(0);
}

void render_overlay(Font *font, const char *str, float x, float y, float scale,
                    int win_w, int win_h, const float color[3]) {
    /* Build ortho projection: top-left origin, pixel coords */
    float ortho[16] = {0};
    ortho[0]  =  2.0f / win_w;
    ortho[5]  = -2.0f / win_h;  /* flip Y: top = 0 */
    ortho[10] = -1.0f;
    ortho[12] = -1.0f;
    ortho[13] =  1.0f;
    ortho[15] =  1.0f;

    /* Layout glyphs into a small temp buffer */
    float verts[6 * 5 * 128]; /* max 128 chars */
    int n = 0;
    float cx = x, cy = y;

    for (const char *p = str; *p && n < 128; p++) {
        Glyph *g = font_glyph(font, (uint32_t)(unsigned char)*p);
        if (!g) continue;

        if (g->width > 0 && g->height > 0) {
            float x0 = cx + g->x_off * scale;
            float y0 = cy + (font->ascent + g->y_off) * scale;
            float x1 = x0 + g->width * scale;
            float y1 = y0 + g->height * scale;

            float *v = verts + n * 30;
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

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, n * 30 * sizeof(float), verts, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glUseProgram(program);
    glUniformMatrix4fv(loc_mvp, 1, GL_FALSE, ortho);
    glUniform1i(loc_atlas, 0);
    glUniform3f(loc_color, color[0], color[1], color[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, font->texture);

    glDrawArrays(GL_TRIANGLES, 0, n * 6);

    glBindVertexArray(0);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
}

void render_destroy(void) {
    if (program) glDeleteProgram(program);
}
