#ifdef ENABLE_OPENGL

#include <stdint.h>
#include <stdbool.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengles2.h>
#endif

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

struct ShaderProgram {
    uint32_t shader_id;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[7];
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;
    bool used_noise;
    GLint noise_frame_location;
    GLint noise_scale_location;
    GLint texture_width_location;
    GLint texture_height_location;
    GLint texture_linear_filtering_location;
};

static struct ShaderProgram shader_program_pool[64];
static struct ShaderProgram *current_shader_program;
static uint8_t shader_program_pool_size;
static GLuint opengl_vbo;

static uint32_t noise_frame;
static float noise_scale[2];
#ifdef THREE_POINT_FILTERING
struct TextureInfo {
    uint16_t width;
    uint16_t height;
    bool linear_filtering;

} textures[1024];
static GLuint current_texture_ids[2];
static uint8_t current_tile;
#endif

static bool gfx_opengl_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        glEnableVertexAttribArray(prg->attrib_locations[i]);
        glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE, num_floats * sizeof(float), (void *) (pos * sizeof(float)));
        pos += prg->attrib_sizes[i];
    }
}

static void gfx_opengl_set_per_program_uniforms() {
    if (current_shader_program->used_noise) {
        glUniform1i(current_shader_program->noise_frame_location, noise_frame);
        glUniform2f(current_shader_program->noise_scale_location, noise_scale[0], noise_scale[1]);
    }
}

static void gfx_opengl_set_per_draw_uniforms() {
#ifdef THREE_POINT_FILTERING
    if (current_shader_program->used_textures[0] || current_shader_program->used_textures[1]) {
        GLint filtering[2] = { textures[current_texture_ids[0]].linear_filtering, textures[current_texture_ids[1]].linear_filtering };
        glUniform1iv(current_shader_program->texture_linear_filtering_location, 2, filtering);

        GLint width[2] = { textures[current_texture_ids[0]].width, textures[current_texture_ids[1]].width };
        glUniform1iv(current_shader_program->texture_width_location, 2, width);

        GLint height[2] = { textures[current_texture_ids[0]].height, textures[current_texture_ids[1]].height };
        glUniform1iv(current_shader_program->texture_height_location, 2, height);
    }
#endif
}

static void gfx_opengl_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            glDisableVertexAttribArray(old_prg->attrib_locations[i]);
        }
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram *new_prg) {
    current_shader_program = new_prg;
    glUseProgram(new_prg->opengl_program_id);
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_per_program_uniforms();
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)" : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
        }
    }
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram *gfx_opengl_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id, &cc_features);

    char vs_buf[1024];
    char fs_buf[2048];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader
    append_line(vs_buf, &vs_len, "#version 110");
    append_line(vs_buf, &vs_len, "attribute vec4 aVtxPos;");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "attribute vec2 aTexCoord;");
        append_line(vs_buf, &vs_len, "varying vec2 vTexCoord;");
        num_floats += 2;
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "attribute vec4 aFog;");
        append_line(vs_buf, &vs_len, "varying vec4 vFog;");
        num_floats += 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "attribute vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(vs_buf, &vs_len, "varying vec4 screenPos;");
    }
    append_line(vs_buf, &vs_len, "void main() {");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "vTexCoord = aTexCoord;");
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(vs_buf, &vs_len, "screenPos = aVtxPos;");
    }
    append_line(vs_buf, &vs_len, "gl_Position = aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
    append_line(fs_buf, &fs_len, "#version 110");
    //append_line(fs_buf, &fs_len, "precision mediump float;");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "varying vec2 vTexCoord;");
    }
    if (cc_features.opt_fog) {
        append_line(fs_buf, &fs_len, "varying vec4 vFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "varying vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
    }
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "varying vec4 screenPos;");
    }
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "uniform int noise_frame;");
        append_line(fs_buf, &fs_len, "uniform vec2 noise_scale;");

        append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
        append_line(fs_buf, &fs_len, "    float random = dot(value, vec3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }

#ifdef THREE_POINT_FILTERING
    // 3 point texture filtering
    // Original author: ArthurCarvalho
    // Based on GLSL implementation by twinaphex, mupen64plus-libretro project.

    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform int texture_width[2];");
        append_line(fs_buf, &fs_len, "uniform int texture_height[2];");
        append_line(fs_buf, &fs_len, "uniform bool texture_linear_filtering[2];");
        append_line(fs_buf, &fs_len, "#define TEX_OFFSET(tex, texCoord, off, texSize) texture2D(tex, texCoord - off / texSize)");
        append_line(fs_buf, &fs_len, "vec4 tex2D3PointFilter(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord * texSize - vec2(0.5, 0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(tex, texCoord, offset, texSize);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(tex, texCoord, vec2(offset.x - sign(offset.x), offset.y), texSize);");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(tex, texCoord, vec2(offset.x, offset.y - sign(offset.y)), texSize);");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
    }
#endif

    append_line(fs_buf, &fs_len, "void main() {");

    if (cc_features.used_textures[0]) {
#ifdef THREE_POINT_FILTERING
        append_line(fs_buf, &fs_len, "    vec4 texVal0;");
        append_line(fs_buf, &fs_len, "    if (texture_linear_filtering[0])");
        append_line(fs_buf, &fs_len, "        texVal0 = tex2D3PointFilter(uTex0, vTexCoord, vec2(texture_width[0], texture_height[0]));");
        append_line(fs_buf, &fs_len, "    else");
        append_line(fs_buf, &fs_len, "        texVal0 = texture2D(uTex0, vTexCoord);");
#else
        append_line(fs_buf, &fs_len, "vec4 texVal0 = texture2D(uTex0, vTexCoord);");
#endif
    }
    if (cc_features.used_textures[1]) {
#ifdef THREE_POINT_FILTERING
        append_line(fs_buf, &fs_len, "    vec4 texVal1;");
        append_line(fs_buf, &fs_len, "    if (texture_linear_filtering[1])");
        append_line(fs_buf, &fs_len, "        texVal1 = tex2D3PointFilter(uTex1, vTexCoord, vec2(texture_width[1], texture_height[1]));");
        append_line(fs_buf, &fs_len, "    else");
        append_line(fs_buf, &fs_len, "        texVal1 = texture2D(uTex1, vTexCoord);");
#else
        append_line(fs_buf, &fs_len, "vec4 texVal1 = texture2D(uTex1, vTexCoord);");
#endif
    }

    append_str(fs_buf, &fs_len, cc_features.opt_alpha ? "vec4 texel = " : "vec3 texel = ");
    if (!cc_features.color_alpha_same && cc_features.opt_alpha) {
        append_str(fs_buf, &fs_len, "vec4(");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0], cc_features.do_multiply[0], cc_features.do_mix[0], false, false, true);
        append_str(fs_buf, &fs_len, ", ");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[1], cc_features.do_multiply[1], cc_features.do_mix[1], true, true, true);
        append_str(fs_buf, &fs_len, ")");
    } else {
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0], cc_features.do_multiply[0], cc_features.do_mix[0], cc_features.opt_alpha, false, cc_features.opt_alpha);
    }
    append_line(fs_buf, &fs_len, ";");

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "vec2 coords = (screenPos.xy / screenPos.w) * noise_scale;");
        append_line(fs_buf, &fs_len, "texel.a *= floor(clamp(random(vec3(floor(coords), float(noise_frame))) + texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "gl_FragColor = texel;");
    } else {
        append_line(fs_buf, &fs_len, "gl_FragColor = vec4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /*puts("Vertex shader:");
    puts(vs_buf);
    puts("Fragment shader:");
    puts(fs_buf);
    puts("End");*/

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { vs_len, fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        fprintf(stderr, "Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        fprintf(stderr, "%s\n", &error_log[0]);
        abort();
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    size_t cnt = 0;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aTexCoord");
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->shader_id = shader_id;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;

    gfx_opengl_load_shader(prg);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        prg->noise_frame_location = glGetUniformLocation(shader_program, "noise_frame");
        prg->noise_scale_location = glGetUniformLocation(shader_program, "noise_scale");
        prg->used_noise = true;
    } else {
        prg->used_noise = false;
    }

#ifdef THREE_POINT_FILTERING
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        prg->texture_width_location = glGetUniformLocation(shader_program, "texture_width");
        prg->texture_height_location = glGetUniformLocation(shader_program, "texture_height");
        prg->texture_linear_filtering_location = glGetUniformLocation(shader_program, "texture_linear_filtering");
    }
#endif

    return prg;
}

static struct ShaderProgram *gfx_opengl_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
#ifdef THREE_POINT_FILTERING
    current_texture_ids[tile] = texture_id;
    current_tile = tile;
#endif
}

static void gfx_opengl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
#ifdef THREE_POINT_FILTERING
    textures[current_texture_ids[current_tile]].width = width;
    textures[current_texture_ids[current_tile]].height = height;
#endif
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + tile);

#ifdef THREE_POINT_FILTERING
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    textures[current_texture_ids[tile]].linear_filtering = linear_filter;
#else
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

static void gfx_opengl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_opengl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);

    float aspect_ratio = (float) width / (float) height;
    noise_scale[0] = 120 * aspect_ratio; // 120 = N64 height resolution (240) / 2
    noise_scale[1] = 120;
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    //printf("flushing %d tris\n", buf_vbo_num_tris);
    gfx_opengl_set_per_draw_uniforms();
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

static void gfx_opengl_init(void) {
#if FOR_WINDOWS
    glewInit();
#endif
    
    glGenBuffers(1, &opengl_vbo);
    
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    noise_frame++;
    if (noise_frame > 150) {
        // No high values, as noise starts to look ugly
        noise_frame = 0;
    }

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_opengl_end_frame(void) {
}

static void gfx_opengl_finish_render(void) {
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_z_is_from_0_to_1,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_test,
    gfx_opengl_set_depth_mask,
    gfx_opengl_set_zmode_decal,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render
};

#endif
