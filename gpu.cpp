#include "gpu.h"

#include <epoxy/gl.h>
#include <epoxy/glx.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {
constexpr int atlas_size = 4096;
constexpr int image_atlas_size = 1024;
constexpr int raster_scale = 4;
constexpr float distance_range = 8;

struct Vertex {
    float x, y, u, v;
    float r, g, b, a;
    float mode;
};

void quad(std::vector<Vertex>& vertices, CanvasRect rect, GpuImage uv, std::array<float, 4> color,
          float mode)
{
    const float x = static_cast<float>(rect.origin.x), y = static_cast<float>(rect.origin.y);
    const float right = x + static_cast<float>(rect.size.x),
                bottom = y + static_cast<float>(rect.size.y);
    auto vertex = [&](float px, float py, float u, float v) {
        return Vertex{px, py, u, v, color[0], color[1], color[2], color[3], mode};
    };
    const auto a = vertex(x, y, uv.u0, uv.v0), b = vertex(right, y, uv.u1, uv.v0);
    const auto c = vertex(right, bottom, uv.u1, uv.v1), d = vertex(x, bottom, uv.u0, uv.v1);
    vertices.insert(vertices.end(), {a, b, c, a, c, d});
}

// Exact squared Euclidean distance transform, linear in the number of pixels.
void distance_row(const std::vector<float>& input, std::vector<float>& output, int count)
{
    std::vector<float> cuts(count + 1);
    std::vector<int> sites(count);

    int k = 0;
    sites[0] = 0;
    cuts[0] = -1e20f;
    cuts[1] = 1e20f;
    for (int q = 1; q < count; ++q) {
        float crossing;
        do {
            const int p = sites[k];
            crossing = ((input[q] + q * q) - (input[p] + p * p)) / (2.0f * (q - p));
            if (crossing > cuts[k])
                break;
            --k;
        } while (k >= 0);
        ++k;
        sites[k] = q;
        cuts[k] = crossing;
        cuts[k + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < count; ++q) {
        while (cuts[k + 1] < q)
            ++k;
        const int distance = q - sites[k];
        output[q] = distance * distance + input[sites[k]];
    }
}

std::vector<float> distances(const unsigned char* pixels, int stride, int width, int height,
                             bool inside)
{
    const int longest = std::max(width, height);
    std::vector<float> data(width * height), input(longest), output(longest);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            data[y * width + x] = ((pixels[y * stride + x] >= 128) == inside) ? 0.0f : 1e10f;

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y)
            input[y] = data[y * width + x];
        distance_row(input, output, height);
        for (int y = 0; y < height; ++y)
            data[y * width + x] = output[y];
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x)
            input[x] = data[y * width + x];
        distance_row(input, output, width);
        for (int x = 0; x < width; ++x)
            data[y * width + x] = output[x];
    }
    return data;
}

GLuint shader(GLenum type, const char* source)
{
    const GLuint result = glCreateShader(type);
    glShaderSource(result, 1, &source, nullptr);
    glCompileShader(result);
    GLint compiled = 0;
    glGetShaderiv(result, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[4096]{};
        glGetShaderInfoLog(result, sizeof(log), nullptr, log);
        glDeleteShader(result);
        throw std::runtime_error(std::string("GPU shader failed: ") + log);
    }
    return result;
}
} // namespace

struct GpuRenderer::Impl {
    bool dark_theme = false;
    Display* display = nullptr;
    Window window = 0;
    Colormap colormap = 0;
    GLXContext context = nullptr;
    bool owns_window = true;
    GLuint program = 0, vao = 0, atlas = 0, images = 0, scratch = 0;
    GLint screen_uniform = 0, origin_uniform = 0, zoom_uniform = 0;
    std::set<GLuint> image_textures;
    int atlas_x = 0, atlas_y = 0, atlas_row = 0, image_x = 0, image_y = 0, image_row = 0;
    std::set<GLuint> buffers;
    std::set<cairo_scaled_font_t*> fonts;
    struct Glyph {
        CanvasRect bounds;
        GpuImage uv;
    };
    std::map<std::pair<cairo_scaled_font_t*, unsigned long>, Glyph> glyphs;

    void initialize(Display* connection, int screen, bool benchmark)
    {
        display = connection;
        int attributes[] = {
            GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
        auto* visual = glXChooseVisual(display, screen, attributes);
        if (!visual)
            throw std::runtime_error("No OpenGL window visual available");
        colormap = XCreateColormap(display, RootWindow(display, screen), visual->visual, AllocNone);
        XSetWindowAttributes settings{};
        settings.colormap = colormap;
        settings.background_pixel = WhitePixel(display, screen);
        window =
            XCreateWindow(display, RootWindow(display, screen), 0, 0, 800, 600, 0, visual->depth,
                          InputOutput, visual->visual, CWColormap | CWBackPixel, &settings);
        context = glXCreateContext(display, visual, nullptr, True);
        XFree(visual);
        if (!context || !glXMakeCurrent(display, window, context))
            throw std::runtime_error("Could not create an OpenGL context");
        if (epoxy_gl_version() < 33)
            throw std::runtime_error("OpenGL 3.3 or newer is required");
        if (epoxy_has_glx_extension(display, screen, "GLX_EXT_swap_control"))
            glXSwapIntervalEXT(display, window, benchmark ? 0 : 1);

        const char* vertex_source = R"GLSL(#version 330 core
layout(location=0) in vec2 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;
layout(location=3) in float mode;
uniform vec2 screen_size;
uniform vec2 origin;
uniform float zoom;
out vec2 uv;
out vec4 tint;
flat out int kind;
void main() {
    vec2 pixel = origin + position * zoom;
    if (int(mode) == 3) pixel += texcoord;
    gl_Position = vec4(pixel.x / screen_size.x * 2.0 - 1.0,
                       1.0 - pixel.y / screen_size.y * 2.0, 0, 1);
    uv = texcoord;
    tint = color;
    kind = int(mode);
})GLSL";
        const char* fragment_source = R"GLSL(#version 330 core
uniform bool dark_theme;
uniform sampler2D glyph_atlas;
uniform sampler2D icon_atlas;
in vec2 uv;
in vec4 tint;
flat in int kind;
out vec4 output_color;
void main() {
    if (kind == 2 || kind == 4) {
        output_color = texture(icon_atlas, uv);
        if (kind == 4 && dark_theme)
            output_color.rgb = vec3(0.84, 0.87, 0.91) * output_color.a;
        return;
    }
    float coverage = 1.0;
    if (kind == 1) {
        float distance = texture(glyph_atlas, uv).r;
        float smoothing = max(0.5 * fwidth(distance), 1.0 / 255.0);
        coverage = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
    }
    float alpha = tint.a * coverage;
    vec3 color = tint.rgb;
    if (dark_theme) {
        float lightness = (max(max(color.r, color.g), color.b) + min(min(color.r, color.g), color.b)) * 0.5;
        float target = 0.08 + (1.0 - lightness) * 0.82;
        color = clamp(color + vec3(target - lightness) + vec3(0.0, 0.008, 0.022), 0.0, 1.0);
        if (kind == 0 && tint.a < 0.15) color = vec3(0.0);
    }
    output_color = vec4(color * alpha, alpha);
})GLSL";
        const auto vertex = shader(GL_VERTEX_SHADER, vertex_source);
        const auto fragment = shader(GL_FRAGMENT_SHADER, fragment_source);
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
            throw std::runtime_error("Could not link GPU text shaders");
        glUseProgram(program);
        screen_uniform = glGetUniformLocation(program, "screen_size");
        origin_uniform = glGetUniformLocation(program, "origin");
        zoom_uniform = glGetUniformLocation(program, "zoom");
        glUniform1i(glGetUniformLocation(program, "glyph_atlas"), 0);
        glUniform1i(glGetUniformLocation(program, "icon_atlas"), 1);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &scratch);
        glBindBuffer(GL_ARRAY_BUFFER, scratch);
        glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 6, nullptr, GL_STREAM_DRAW);
        glGenTextures(1, &atlas);
        glGenTextures(1, &images);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const auto texture : {atlas, images}) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, atlas);
        const std::vector<unsigned char> empty_atlas(atlas_size * atlas_size, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_size, atlas_size, 0, GL_RED, GL_UNSIGNED_BYTE,
                     empty_atlas.data());
        glBindTexture(GL_TEXTURE_2D, images);
        const std::vector<unsigned char> empty_images(image_atlas_size * image_atlas_size * 4, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image_atlas_size, image_atlas_size, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, empty_images.data());
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        std::cerr << "OpenGL renderer: " << glGetString(GL_RENDERER) << '\n';
    }

    Glyph glyph(cairo_scaled_font_t* font, unsigned long index)
    {
        const auto key = std::make_pair(font, index);
        if (const auto found = glyphs.find(key); found != glyphs.end())
            return found->second;
        if (fonts.insert(font).second)
            cairo_scaled_font_reference(font);
        cairo_glyph_t glyph{index, 0, 0};
        cairo_text_extents_t extents{};
        cairo_scaled_font_glyph_extents(font, &glyph, 1, &extents);
        if (extents.width == 0 || extents.height == 0)
            return glyphs[key] = {};
        constexpr double padding = 3;
        const double left = std::floor(extents.x_bearing) - padding;
        const double top = std::floor(extents.y_bearing) - padding;
        const int width = static_cast<int>(
            (std::ceil(extents.x_bearing + extents.width) + padding - left) * raster_scale);
        const int height = static_cast<int>(
            (std::ceil(extents.y_bearing + extents.height) + padding - top) * raster_scale);
        if (atlas_x + width + 2 > atlas_size) {
            atlas_x = 0;
            atlas_y += atlas_row + 2;
            atlas_row = 0;
        }
        if (atlas_y + height > atlas_size)
            throw std::runtime_error("GPU glyph atlas is full");
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
        auto* cr = cairo_create(surface);
        cairo_scale(cr, raster_scale, raster_scale);
        cairo_set_font_face(cr, cairo_scaled_font_get_font_face(font));
        cairo_matrix_t matrix;
        cairo_scaled_font_get_font_matrix(font, &matrix);
        cairo_set_font_matrix(cr, &matrix);
        auto* options = cairo_font_options_create();
        cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_NONE);
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
        cairo_set_font_options(cr, options);
        cairo_font_options_destroy(options);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        glyph.x = -left;
        glyph.y = -top;
        cairo_show_glyphs(cr, &glyph, 1);
        cairo_surface_flush(surface);
        const auto* pixels = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        const auto inside = distances(pixels, stride, width, height, true);
        const auto outside = distances(pixels, stride, width, height, false);
        std::vector<unsigned char> sdf(width * height);
        for (std::size_t i = 0; i < sdf.size(); ++i) {
            float distance = std::sqrt(outside[i]) - std::sqrt(inside[i]);
            distance += distance > 0 ? -0.5f : 0.5f;
            sdf[i] = static_cast<unsigned char>(
                std::clamp(0.5f + distance / (2 * distance_range), 0.0f, 1.0f) * 255);
        }
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlas);
        glTexSubImage2D(GL_TEXTURE_2D, 0, atlas_x, atlas_y, width, height, GL_RED, GL_UNSIGNED_BYTE,
                        sdf.data());
        Glyph result{{{left, top},
                      {static_cast<double>(width) / raster_scale,
                       static_cast<double>(height) / raster_scale}},
                     {static_cast<float>(atlas_x) / atlas_size,
                      static_cast<float>(atlas_y) / atlas_size,
                      static_cast<float>(atlas_x + width) / atlas_size,
                      static_cast<float>(atlas_y + height) / atlas_size}};
        atlas_x += width + 2;
        atlas_row = std::max(atlas_row, height);
        return glyphs[key] = result;
    }

    static bool is_shape_run(const PangoLayoutRun* run)
    {
        for (auto* item = run->item->analysis.extra_attrs; item; item = item->next)
            if (static_cast<PangoAttribute*>(item->data)->klass->type == PANGO_ATTR_SHAPE)
                return true;
        return false;
    }

    void text_run(std::vector<Vertex>& vertices, const PlacedText& placed,
                  PangoLayoutIter* iterator)
    {
        auto* run = pango_layout_iter_get_run_readonly(iterator);
        if (!run || !PANGO_IS_CAIRO_FONT(run->item->analysis.font))
            return;
        if (is_shape_run(run))
            return;
        auto* font = pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(run->item->analysis.font));
        if (!font)
            return;
        PangoRectangle logical{};
        pango_layout_iter_get_run_extents(iterator, nullptr, &logical);
        double x = placed.origin.x + static_cast<double>(logical.x) / PANGO_SCALE;
        const double first_x = x;
        const double baseline =
            placed.origin.y +
            static_cast<double>(pango_layout_iter_get_baseline(iterator)) / PANGO_SCALE;
        std::array<float, 4> color{0.14f, 0.16f, 0.19f, 1};
        std::array<float, 4> underline_color{0.8f, 0.13f, 0.13f, 1};
        bool underline = false, strike = false;
        for (auto* item = run->item->analysis.extra_attrs; item; item = item->next) {
            const auto* attribute = static_cast<PangoAttribute*>(item->data);
            if (attribute->klass->type == PANGO_ATTR_FOREGROUND ||
                attribute->klass->type == PANGO_ATTR_UNDERLINE_COLOR) {
                const auto& c = reinterpret_cast<const PangoAttrColor*>(attribute)->color;
                const std::array<float, 4> value{c.red / 65535.0f, c.green / 65535.0f,
                                                 c.blue / 65535.0f, 1};
                if (attribute->klass->type == PANGO_ATTR_FOREGROUND)
                    color = value;
                else
                    underline_color = value;
            } else if (attribute->klass->type == PANGO_ATTR_UNDERLINE)
                underline =
                    reinterpret_cast<const PangoAttrInt*>(attribute)->value != PANGO_UNDERLINE_NONE;
            else if (attribute->klass->type == PANGO_ATTR_STRIKETHROUGH)
                strike = reinterpret_cast<const PangoAttrInt*>(attribute)->value;
        }
        for (int i = 0; i < run->glyphs->num_glyphs; ++i) {
            const auto& info = run->glyphs->glyphs[i];
            if (info.glyph != PANGO_GLYPH_EMPTY) {
                const auto shape =
                    glyph(font, info.glyph & PANGO_GLYPH_UNKNOWN_FLAG ? 0 : info.glyph);
                auto bounds = shape.bounds;
                bounds.origin.x += x + static_cast<double>(info.geometry.x_offset) / PANGO_SCALE;
                bounds.origin.y +=
                    baseline + static_cast<double>(info.geometry.y_offset) / PANGO_SCALE;
                if (bounds.size.x > 0)
                    quad(vertices, bounds, shape.uv, color, 1);
            }
            x += static_cast<double>(info.geometry.width) / PANGO_SCALE;
        }
        if (strike)
            quad(vertices, {{first_x, baseline - 5}, {x - first_x, 1}}, {}, color, 0);
        if (underline)
            for (double position = first_x; position < x; position += 1) {
                const double y = baseline + 2 + std::sin((position - first_x) * 1.57);
                quad(vertices, {{position, y}, {std::min(1.0, x - position), 1}}, {},
                     underline_color, 0);
            }
    }
    void text(std::vector<Vertex>& vertices, const PlacedText& placed)
    {
        auto* iterator = pango_layout_get_iter(placed.layout);
        do {
            text_run(vertices, placed, iterator);
        } while (pango_layout_iter_next_run(iterator));
        pango_layout_iter_free(iterator);
    }

    void bind(GLuint buffer)
    {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        const GLint sizes[] = {2, 2, 4, 1};
        const std::size_t offsets[] = {offsetof(Vertex, x), offsetof(Vertex, u),
                                       offsetof(Vertex, r), offsetof(Vertex, mode)};
        for (GLuint i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  reinterpret_cast<const void*>(offsets[i]));
        }
    }

    ~Impl()
    {
        if (context) {
            for (const auto buffer : buffers)
                glDeleteBuffers(1, &buffer);
            glDeleteBuffers(1, &scratch);
            glDeleteVertexArrays(1, &vao);
            glDeleteTextures(1, &atlas);
            glDeleteTextures(1, &images);
            for (const auto texture : image_textures)
                glDeleteTextures(1, &texture);
            glDeleteProgram(program);
            glXMakeCurrent(display, None, nullptr);
            glXDestroyContext(display, context);
        }
        for (auto* font : fonts)
            cairo_scaled_font_destroy(font);
        if (window && owns_window)
            XDestroyWindow(display, window);
        if (colormap)
            XFreeColormap(display, colormap);
    }
};

GpuRenderer::GpuRenderer(Display* display, int screen, bool benchmark)
    : impl(std::make_unique<Impl>())
{
    impl->initialize(display, screen, benchmark);
}
GpuRenderer::~GpuRenderer() = default;
Window GpuRenderer::window() const
{
    return impl->window;
}
std::string GpuRenderer::device() const
{
    return reinterpret_cast<const char*>(glGetString(GL_RENDERER));
}
std::size_t GpuRenderer::glyph_count() const
{
    return impl->glyphs.size();
}
void GpuRenderer::window_destroyed()
{
    impl->owns_window = false;
}

GpuImage GpuRenderer::image(cairo_surface_t* surface)
{
    const int width = cairo_image_surface_get_width(surface),
              height = cairo_image_surface_get_height(surface);
    if (impl->image_x + width + 2 > image_atlas_size) {
        impl->image_x = 0;
        impl->image_y += impl->image_row + 2;
        impl->image_row = 0;
    }
    if (impl->image_y + height > image_atlas_size)
        throw std::runtime_error("GPU icon atlas is full");
    cairo_surface_flush(surface);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, impl->images);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride(surface) / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, impl->image_x, impl->image_y, width, height, GL_BGRA,
                    GL_UNSIGNED_BYTE, cairo_image_surface_get_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    const GpuImage result{static_cast<float>(impl->image_x) / image_atlas_size,
                          static_cast<float>(impl->image_y) / image_atlas_size,
                          static_cast<float>(impl->image_x + width) / image_atlas_size,
                          static_cast<float>(impl->image_y + height) / image_atlas_size};
    impl->image_x += width + 2;
    impl->image_row = std::max(impl->image_row, height);
    return result;
}

void GpuRenderer::upload(GpuMesh& mesh, const std::vector<PlacedText>& text,
                         const std::vector<PlacedImage>& images)
{
    std::vector<Vertex> vertices;
    for (const auto& line : text)
        impl->text(vertices, line);
    for (const auto& image : images)
        quad(vertices, image.bounds, image.image, {1, 1, 1, 1}, image.image.monochrome ? 4 : 2);
    if (!mesh.buffer) {
        glGenBuffers(1, &mesh.buffer);
        impl->buffers.insert(mesh.buffer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.buffer);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(),
                 GL_STATIC_DRAW);
    mesh.vertices = static_cast<int>(vertices.size());
}

void GpuRenderer::upload_image(GpuMesh& mesh, cairo_surface_t* surface, CanvasRect bounds)
{
    release(mesh);
    upload(mesh, {}, {{{0, 0, 1, 1}, bounds}});
    glGenTextures(1, &mesh.texture);
    impl->image_textures.insert(mesh.texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mesh.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cairo_surface_flush(surface);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, cairo_image_surface_get_stride(surface) / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cairo_image_surface_get_width(surface),
                 cairo_image_surface_get_height(surface), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                 cairo_image_surface_get_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void GpuRenderer::release(GpuMesh& mesh)
{
    if (mesh.texture) {
        glDeleteTextures(1, &mesh.texture);
        impl->image_textures.erase(mesh.texture);
    }
    if (mesh.buffer) {
        glDeleteBuffers(1, &mesh.buffer);
        impl->buffers.erase(mesh.buffer);
        mesh = {};
    }
}

void GpuRenderer::paths(GpuMesh& mesh, const std::vector<GpuPath>& paths)
{
    std::vector<Vertex> vertices;
    for (const auto& path : paths) {
        auto vertex = [&](Vec2 p, float x, float y) {
            return Vertex{static_cast<float>(p.x),
                          static_cast<float>(p.y),
                          x,
                          y,
                          path.color[0],
                          path.color[1],
                          path.color[2],
                          path.color[3],
                          3};
        };
        for (std::size_t i = 1; i < path.points.size(); ++i) {
            const auto a = path.points[i - 1], b = path.points[i];
            const double length = std::hypot(b.x - a.x, b.y - a.y);
            if (length < 0.001)
                continue;
            const float nx = static_cast<float>(-(b.y - a.y) / length) * path.width / 2;
            const float ny = static_cast<float>((b.x - a.x) / length) * path.width / 2;
            const auto p = vertex(a, nx, ny), q = vertex(a, -nx, -ny);
            const auto r = vertex(b, -nx, -ny), s = vertex(b, nx, ny);
            vertices.insert(vertices.end(), {p, q, r, p, r, s});
        }
        if (path.arrow && path.points.size() > 1) {
            const auto tip = path.points.back(), previous = path.points[path.points.size() - 2];
            const double length = std::hypot(tip.x - previous.x, tip.y - previous.y);
            if (length < 0.001)
                continue;
            const float dx = static_cast<float>((tip.x - previous.x) / length);
            const float dy = static_cast<float>((tip.y - previous.y) / length);
            vertices.insert(vertices.end(),
                            {vertex(tip, 0, 0), vertex(tip, -dx * 7 - dy * 3, -dy * 7 + dx * 3),
                             vertex(tip, -dx * 7 + dy * 3, -dy * 7 - dx * 3)});
        }
    }
    if (!mesh.buffer) {
        glGenBuffers(1, &mesh.buffer);
        impl->buffers.insert(mesh.buffer);
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh.buffer);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(),
                 GL_DYNAMIC_DRAW);
    mesh.vertices = static_cast<int>(vertices.size());
}

void GpuRenderer::set_dark_theme(bool dark)
{
    impl->dark_theme = dark;
}

void GpuRenderer::begin(int width, int height)
{
    glViewport(0, 0, width, height);
    if (impl->dark_theme)
        glClearColor(0.08f, 0.088f, 0.102f, 1);
    else
        glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(impl->program);
    glUniform1i(glGetUniformLocation(impl->program, "dark_theme"), impl->dark_theme);
    glBindVertexArray(impl->vao);
    glUniform2f(impl->screen_uniform, width, height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl->atlas);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, impl->images);
}

void GpuRenderer::draw(const GpuMesh& mesh, Vec2 origin, double zoom)
{
    if (!mesh.vertices)
        return;
    glUniform2f(impl->origin_uniform, static_cast<float>(origin.x), static_cast<float>(origin.y));
    glUniform1f(impl->zoom_uniform, static_cast<float>(zoom));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mesh.texture ? mesh.texture : impl->images);
    impl->bind(mesh.buffer);
    glDrawArrays(GL_TRIANGLES, 0, mesh.vertices);
}

void GpuRenderer::rectangle(CanvasRect bounds, std::array<float, 4> color)
{
    std::vector<Vertex> vertices;
    quad(vertices, bounds, {}, color, 0);
    glBindBuffer(GL_ARRAY_BUFFER, impl->scratch);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(Vertex), vertices.data());
    draw({impl->scratch, 6}, {0, 0}, 1);
}
void GpuRenderer::clip(CanvasRect bounds, int viewport_height)
{
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<int>(bounds.origin.x),
              viewport_height - static_cast<int>(std::ceil(bounds.origin.y + bounds.size.y)),
              std::max(0, static_cast<int>(std::ceil(bounds.size.x))),
              std::max(0, static_cast<int>(std::ceil(bounds.size.y))));
}

void GpuRenderer::clear_clip()
{
    glDisable(GL_SCISSOR_TEST);
}

void GpuRenderer::present()
{
    glXSwapBuffers(impl->display, impl->window);
}
void GpuRenderer::finish()
{
    glFinish();
}
