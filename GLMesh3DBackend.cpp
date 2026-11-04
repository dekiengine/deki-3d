// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

#include "GLMesh3DBackend.h"

#include "deki-rendering/GLApi.h"
#include "deki-rendering/GLPresenter.h"

#include "Math3D.h"
#include "deki-rendering/ShaderAsset.h"
#include "deki-rendering/PixelFormat.h"

#include <deki/LogSystem.h>
#include <deki/Time.h>

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

namespace Deki3D
{
using namespace DekiGL;

namespace
{

// ---------------------------------------------------------------------------
// Shaders. Written in HLSL (editor/MeshShaderUse.cpp) and compiled by the
// editor to GLSL ES 3.00 and GLSL 3.30. The built-in program draws every
// material without a shader of its own: the shading model, the texture kind
// and the alpha test are uniforms, so a batch of them never switches
// programs. It is that use with no hooks, generated into the .inl below; a
// material with a .shader draws with the asset's "mesh" program, the same
// stages with the file's hooks called.
// ---------------------------------------------------------------------------

#include "GLMesh3DBuiltinShader.inl"

enum Attribute : GLuint
{
    kPosition = 0,
    kNormal = 1,
    kUV = 2,
    kColor = 3,
};

/// The texels of `tex` as RGBA8, which every GL takes. Holes get alpha 0, as
/// the software rasteriser reads them: palette index 0, RGB565 magenta, and
/// alpha below half in the formats that carry it.
std::vector<uint8_t> ToRGBA(const Texture3D& tex)
{
    const size_t count = static_cast<size_t>(tex.width) * tex.height;
    std::vector<uint8_t> out(count * 4);
    const int texel = TexelBytes(tex.format);
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t* o = &out[i * 4];
        uint8_t r = 255, g = 255, b = 255, a = 255;
        if (tex.palette)
        {
            // Palette entries are 565 values, not memory bytes.
            const uint8_t index = tex.pixels[i];
            DekiPixel::UnpackRGB565(tex.palette[index], r, g, b);
            a = index == 0 ? 0 : 255;
        }
        else
        {
            const uint8_t* p = tex.pixels + i * texel;
            switch (tex.format)
            {
                case TexelFormat::RGB565:
                {
                    const uint16_t v = DekiPixel::Load565(p);
                    DekiPixel::UnpackRGB565(v, r, g, b);
                    a = v == 0xF81F ? 0 : 255;
                    break;
                }
                case TexelFormat::RGB565A8:
                    DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB565A8>(p, true, r, g, b, a);
                    break;
                case TexelFormat::RGB888:
                    DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGB888>(p, false, r, g, b, a);
                    break;
                case TexelFormat::RGBA8888:
                    DekiPixel::ReadSrcPixel<DekiPixel::SrcKind::RGBA8888>(p, true, r, g, b, a);
                    break;
                case TexelFormat::ALPHA8: a = p[0]; break;
            }
        }
        o[0] = r;
        o[1] = g;
        o[2] = b;
        o[3] = a;
    }
    return out;
}

void SetTint(const GL& gl, GLint location, uint32_t abgr)
{
    gl.Uniform4f(location, static_cast<float>(abgr & 0xFF) / 255.0f, static_cast<float>((abgr >> 8) & 0xFF) / 255.0f,
                 static_cast<float>((abgr >> 16) & 0xFF) / 255.0f, static_cast<float>((abgr >> 24) & 0xFF) / 255.0f);
}

}  // namespace

// ---------------------------------------------------------------------------
// The device: the context, the program and the uploads, shared by every
// backend this package makes. Meshes and textures are kept by revision and
// dropped once unused for a while, which is how an unloaded asset's upload
// goes away; an untracked one (revision 0) goes through scratch objects
// every draw.
// ---------------------------------------------------------------------------

class GLDevice
{
public:
    static std::unique_ptr<GLDevice> Create()
    {
        // The program's one context: the presenting window's when there is
        // one, so meshes can draw into its frames.
        std::shared_ptr<GLContext> context = SharedContext();
        if (!context)
        {
            return nullptr;
        }
        std::unique_ptr<GLDevice> device(new GLDevice(std::move(context)));
        if (!device->Build())
        {
            return nullptr;
        }
        return device;
    }

    ~GLDevice()
    {
        if (!m_Context->MakeCurrent())
        {
            return;  // the context goes with its objects
        }
        for (auto& entry : m_Meshes)
        {
            m_GL.DeleteBuffers(2, entry.second.buffers);
        }
        for (auto& entry : m_Textures)
        {
            m_GL.DeleteTextures(1, &entry.second.texture);
        }
        m_GL.DeleteBuffers(2, m_ScratchBuffers);
        m_GL.DeleteTextures(1, &m_ScratchTexture);
        m_GL.DeleteVertexArrays(1, &m_VertexArray);
        m_GL.DeleteProgram(m_Builtin.id);
        for (auto& entry : m_Custom)
        {
            m_GL.DeleteProgram(entry.second.id);
        }
    }

    const GL& gl() const { return m_GL; }
    bool MakeCurrent() { return m_Context->MakeCurrent(); }
    const GLContext* Context() const { return m_Context.get(); }

    struct Uniforms
    {
        GLint mvp, model, normalMatrix, ambient, lightCount, shading, hasNormals, affine, snap, viewport;
        GLint texture, texMode, alphaTest, tint, time, params, color;
        GLint shadowMatrix0, shadowMatrix1, shadowBias, shadowMap0, shadowMap1;
        GLint lightPosition[RasterConfig::kMaxLights], lightDirection[RasterConfig::kMaxLights];
        GLint lightColor[RasterConfig::kMaxLights], lightCone[RasterConfig::kMaxLights];
    };
    struct Program
    {
        GLuint id = 0;
        Uniforms u{};
        // Whether its text reads uTime. Not from the uniform's location: the
        // uniforms are one struct, and some drivers (Mesa) keep every member
        // of a struct that is used at all.
        bool readsTime = false;
    };

    /// The program that draws `shader`: its own, compiled the first time,
    /// or the built-in one for none, or for one that does not compile.
    const Program& ProgramFor(const DekiRendering::ShaderAsset* shader)
    {
        if (!shader)
        {
            return m_Builtin;
        }
        auto [it, fresh] = m_Custom.try_emplace(shader->Revision());
        if (fresh)
        {
            it->second = Link(shader);  // id 0 when it failed: said once, then built-in
        }
        return it->second.id ? it->second : m_Builtin;
    }

    /// Everything a batch needs bound, once per batch.
    void Use()
    {
        m_GL.BindVertexArray(m_VertexArray);
        m_GL.ActiveTexture(gl::TEXTURE0);

        // Old uploads go every so often, not every frame.
        if (++m_Frame % 64 == 0)
        {
            Evict();
        }
    }

    /// Binds `mesh`'s buffers and points the attributes at them.
    void BindMesh(const Mesh3D& mesh)
    {
        const size_t vertexBytes = static_cast<size_t>(mesh.vertexCount) * mesh.layout.stride;
        const size_t indexBytes = static_cast<size_t>(mesh.indexCount) * mesh.indexBytes;
        const GLuint* buffers = m_ScratchBuffers;
        bool upload = true;
        if (mesh.revision != 0)
        {
            auto [it, fresh] = m_Meshes.try_emplace(mesh.revision);
            if (fresh)
            {
                m_GL.GenBuffers(2, it->second.buffers);
            }
            it->second.lastUsed = m_Frame;
            buffers = it->second.buffers;
            upload = fresh;
        }
        m_GL.BindBuffer(gl::ARRAY_BUFFER, buffers[0]);
        m_GL.BindBuffer(gl::ELEMENT_ARRAY_BUFFER, buffers[1]);
        if (upload)
        {
            const GLenum usage = mesh.revision != 0 ? gl::STATIC_DRAW : gl::STREAM_DRAW;
            m_GL.BufferData(gl::ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexBytes), mesh.vertices, usage);
            m_GL.BufferData(gl::ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBytes), mesh.indices, usage);
        }

        const VertexLayout& l = mesh.layout;
        Point(kPosition, 3, gl::FLOAT, false, l.stride, l.positionOffset, 0.0f, 0.0f, 0.0f, 1.0f);
        Point(kNormal, 3, gl::FLOAT, false, l.stride, l.normalOffset, 0.0f, 0.0f, 1.0f, 0.0f);
        Point(kUV, 2, gl::FLOAT, false, l.stride, l.uvOffset, 0.0f, 0.0f, 0.0f, 1.0f);
        // 0xAABBGGRR, which is R, G, B, A in memory.
        Point(kColor, 4, gl::UNSIGNED_BYTE, true, l.stride, l.colorOffset, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    /// Binds `tex`'s upload to unit 0.
    void BindTexture(const Texture3D& tex)
    {
        GLuint id = m_ScratchTexture;
        bool upload = true;
        if (tex.revision != 0)
        {
            auto [it, fresh] = m_Textures.try_emplace(tex.revision);
            if (fresh)
            {
                m_GL.GenTextures(1, &it->second.texture);
            }
            it->second.lastUsed = m_Frame;
            id = it->second.texture;
            upload = fresh;
        }
        m_GL.BindTexture(gl::TEXTURE_2D, id);
        if (upload)
        {
            const std::vector<uint8_t> rgba = ToRGBA(tex);
            m_GL.PixelStorei(gl::UNPACK_ALIGNMENT, 4);
            if (tex.width > MaxTextureSize() || tex.height > MaxTextureSize())
            {
                // Said at each upload, once for an asset's texture. OpenGL
                // refuses it, and the material samples black.
                DEKI_LOG_ERROR("deki-3d: a %ux%u texture is over this GPU's largest, %d. Set a Max Size for this "
                               "platform in the model's settings.",
                               static_cast<unsigned>(tex.width), static_cast<unsigned>(tex.height), MaxTextureSize());
            }
            m_GL.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::RGBA8), tex.width, tex.height, 0, gl::RGBA,
                            gl::UNSIGNED_BYTE, rgba.data());
            // The image's filter (TextureFilter.h). Trilinear reads mipmaps:
            // a big texture seen from afar would otherwise shimmer.
            GLenum minFilter = gl::NEAREST, magFilter = gl::NEAREST;  // Point, as the software sampler draws
            if (tex.filter == Deki::TextureFilter::Bilinear)
            {
                minFilter = magFilter = gl::LINEAR;
            }
            else if (tex.filter == Deki::TextureFilter::Trilinear)
            {
                m_GL.GenerateMipmap(gl::TEXTURE_2D);
                minFilter = gl::LINEAR_MIPMAP_LINEAR;
                magFilter = gl::LINEAR;
            }
            m_GL.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<GLint>(minFilter));
            m_GL.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<GLint>(magFilter));
            // Wrapping, as the software sampler masks.
            m_GL.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<GLint>(gl::REPEAT));
            m_GL.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<GLint>(gl::REPEAT));
        }
    }

    /// Logs the first GL error seen, once, so a broken driver is visible
    /// without flooding the log every frame.
    void CheckErrors(const char* where)
    {
        const GLenum error = m_GL.GetError();
        if (error != 0 && !m_ErrorReported)
        {
            m_ErrorReported = true;
            DEKI_LOG_WARNING("deki-3d: OpenGL error 0x%04X %s", error, where);
        }
    }

    /// The largest texture side this GPU takes.
    int MaxTextureSize()
    {
        if (m_MaxTextureSize == 0)
        {
            GLint size = 0;
            m_GL.GetIntegerv(gl::MAX_TEXTURE_SIZE, &size);
            m_MaxTextureSize = size > 0 ? size : 2048;  // what OpenGL ES 3.0 promises at least
        }
        return m_MaxTextureSize;
    }

private:
    explicit GLDevice(std::shared_ptr<GLContext> context) : m_Context(std::move(context)), m_GL(m_Context->Functions())
    {
    }

    bool Build()
    {
        m_Builtin = Link(nullptr);
        if (!m_Builtin.id)
        {
            return false;
        }

        m_GL.GenVertexArrays(1, &m_VertexArray);
        m_GL.GenBuffers(2, m_ScratchBuffers);
        m_GL.GenTextures(1, &m_ScratchTexture);

        const char* renderer = reinterpret_cast<const char*>(m_GL.GetString(gl::RENDERER));
        const char* glVersion = reinterpret_cast<const char*>(m_GL.GetString(gl::VERSION));
        DEKI_LOG_INFO("deki-3d: %s, %s", renderer ? renderer : "?", glVersion ? glVersion : "?");
        CheckErrors("setting up");
        return true;
    }

    /// The built-in program for `shader` null, otherwise the shader's "mesh"
    /// program. Id 0, with the reason logged, when it does not build.
    Program Link(const DekiRendering::ShaderAsset* shader)
    {
        const bool es = m_Context->IsES();
        std::string vertex = es ? kBuiltinMeshVertexES : kBuiltinMeshVertexGL;
        std::string fragment = es ? kBuiltinMeshFragmentES : kBuiltinMeshFragmentGL;
        if (shader)
        {
            const DekiRendering::ShaderProgramSource* source = shader->Program("mesh", es);
            if (!source)
            {
                DEKI_LOG_ERROR("deki-3d: a mesh's shader has neither Shade() nor Displace(), so it is not a mesh "
                               "shader; drawing without it");
                return {};
            }
            vertex = source->vertex;
            fragment = source->fragment;
        }

        const char* what = shader ? "a mesh's shader" : "the built-in mesh shader";
        const GLuint vs = Compile(gl::VERTEX_SHADER, vertex, what);
        const GLuint fs = vs ? Compile(gl::FRAGMENT_SHADER, fragment, what) : 0;
        Program program;
        if (!vs || !fs)
        {
            m_GL.DeleteShader(vs);
            return program;
        }
        program.id = m_GL.CreateProgram();
        m_GL.AttachShader(program.id, vs);
        m_GL.AttachShader(program.id, fs);
        m_GL.LinkProgram(program.id);
        m_GL.DeleteShader(vs);
        m_GL.DeleteShader(fs);
        GLint linked = 0;
        m_GL.GetProgramiv(program.id, gl::LINK_STATUS, &linked);
        if (!linked)
        {
            ReportLog(program.id, false, what);
            m_GL.DeleteProgram(program.id);
            program.id = 0;
            return program;
        }
        // The compiler keeps the uniforms in one struct, `u`, and names a
        // texture as the shader does (deki-editor's ShaderCompiler.h).
        auto at = [&](const char* name) { return m_GL.GetUniformLocation(program.id, name); };
        program.u.mvp = at("u.uMvp");
        program.u.model = at("u.uModel");
        program.u.normalMatrix = at("u.uNormalMatrix");
        program.u.ambient = at("u.uAmbient");
        program.u.lightCount = at("u.uLightCount");
        program.u.shading = at("u.uShading");
        program.u.hasNormals = at("u.uHasNormals");
        program.u.affine = at("u.uAffine");
        program.u.snap = at("u.uSnap");
        program.u.viewport = at("u.uViewport");
        program.u.texture = at("uTexture");
        program.u.texMode = at("u.uTexMode");
        program.u.alphaTest = at("u.uAlphaTest");
        program.u.tint = at("u.uTint");
        program.u.time = at("u.uTime");
        program.u.params = at("u.uParams");
        program.u.color = at("u.uColor");
        program.u.shadowMatrix0 = at("u.uShadowMatrix0");
        program.u.shadowMatrix1 = at("u.uShadowMatrix1");
        program.u.shadowBias = at("u.uShadowBias");
        program.u.shadowMap0 = at("uShadowMap0");
        program.u.shadowMap1 = at("uShadowMap1");
        // Each sampler on its own unit, from the start: a colour and a shadow
        // sampler on one unit is an error at the first draw.
        m_GL.UseProgram(program.id);
        m_GL.Uniform1i(program.u.texture, 0);
        m_GL.Uniform1i(program.u.shadowMap0, 1);
        m_GL.Uniform1i(program.u.shadowMap1, 2);
        // An array is a struct of its own, its elements in `data`.
        char name[48];
        for (int l = 0; l < RasterConfig::kMaxLights; ++l)
        {
            std::snprintf(name, sizeof(name), "u.uLightPosition.data[%d]", l);
            program.u.lightPosition[l] = at(name);
            std::snprintf(name, sizeof(name), "u.uLightDirection.data[%d]", l);
            program.u.lightDirection[l] = at(name);
            std::snprintf(name, sizeof(name), "u.uLightColor.data[%d]", l);
            program.u.lightColor[l] = at(name);
            std::snprintf(name, sizeof(name), "u.uLightCone.data[%d]", l);
            program.u.lightCone[l] = at(name);
        }
        // The compiler writes a read of a member as "u.uTime"; the struct's
        // declaration names it without the "u.".
        program.readsTime =
            vertex.find("u.uTime") != std::string::npos || fragment.find("u.uTime") != std::string::npos;
        return program;
    }

    GLuint Compile(GLenum type, const std::string& source, const char* what)
    {
        const GLuint shader = m_GL.CreateShader(type);
        const GLchar* sources[] = { source.c_str() };
        m_GL.ShaderSource(shader, 1, sources, nullptr);
        m_GL.CompileShader(shader);
        GLint ok = 0;
        m_GL.GetShaderiv(shader, gl::COMPILE_STATUS, &ok);
        if (!ok)
        {
            ReportLog(shader, true, what);
            m_GL.DeleteShader(shader);
            return 0;
        }
        return shader;
    }

    void ReportLog(GLuint object, bool shader, const char* what)
    {
        GLint length = 0;
        (shader ? m_GL.GetShaderiv : m_GL.GetProgramiv)(object, gl::INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
        (shader ? m_GL.GetShaderInfoLog : m_GL.GetProgramInfoLog)(object, length, nullptr, log.data());
        DEKI_LOG_ERROR("deki-3d: %s does not build; drawing without it: %s", what, log.c_str());
    }

    /// Points `index` at a vertex attribute, or holds it at a constant when
    /// the layout has none.
    void Point(GLuint index, GLint size, GLenum type, bool normalized, uint16_t stride, int16_t offset, float x,
               float y, float z, float w)
    {
        if (offset < 0)
        {
            m_GL.DisableVertexAttribArray(index);
            m_GL.VertexAttrib4f(index, x, y, z, w);
            return;
        }
        m_GL.EnableVertexAttribArray(index);
        m_GL.VertexAttribPointer(index, size, type, normalized ? gl::kTrue : gl::kFalse, stride,
                                 reinterpret_cast<const void*>(static_cast<uintptr_t>(offset)));
    }

    void Evict()
    {
        constexpr uint32_t kKeepFrames = 600;  // ten seconds at 60 fps
        for (auto it = m_Meshes.begin(); it != m_Meshes.end();)
        {
            if (m_Frame - it->second.lastUsed > kKeepFrames)
            {
                m_GL.DeleteBuffers(2, it->second.buffers);
                it = m_Meshes.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = m_Textures.begin(); it != m_Textures.end();)
        {
            if (m_Frame - it->second.lastUsed > kKeepFrames)
            {
                m_GL.DeleteTextures(1, &it->second.texture);
                it = m_Textures.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    struct MeshEntry
    {
        GLuint buffers[2] = { 0, 0 };  // vertices, indices
        uint32_t lastUsed = 0;
    };
    struct TextureEntry
    {
        GLuint texture = 0;
        uint32_t lastUsed = 0;
    };

    std::shared_ptr<GLContext> m_Context;
    const GL& m_GL;
    Program m_Builtin;
    // By shader revision; a revision that did not build keeps id 0.
    std::unordered_map<uint32_t, Program> m_Custom;
    GLuint m_VertexArray = 0;
    GLuint m_ScratchBuffers[2] = { 0, 0 };
    GLuint m_ScratchTexture = 0;
    std::unordered_map<uint32_t, MeshEntry> m_Meshes;
    std::unordered_map<uint32_t, TextureEntry> m_Textures;
    uint32_t m_Frame = 0;
    int m_MaxTextureSize = 0;
    bool m_ErrorReported = false;
};

// ---------------------------------------------------------------------------
// The backend
// ---------------------------------------------------------------------------

GLMesh3DBackend::GLMesh3DBackend(GLDevice& device) : m_Device(device)
{
}

GLMesh3DBackend::~GLMesh3DBackend()
{
    if (m_Device.MakeCurrent())
    {
        ReleaseTargets();
        ReleaseShadowMaps();
    }
}

void GLMesh3DBackend::ReleaseShadowMaps()
{
    const GL& gl = m_Device.gl();
    for (int i = 0; i < RasterConfig::kMaxShadows; ++i)
    {
        if (m_ShadowFramebuffer[i])
        {
            gl.DeleteFramebuffers(1, &m_ShadowFramebuffer[i]);
        }
        if (m_ShadowTexture[i])
        {
            gl.DeleteTextures(1, &m_ShadowTexture[i]);
        }
        m_ShadowFramebuffer[i] = m_ShadowTexture[i] = 0;
        m_ShadowSize[i] = 0;
    }
}

bool GLMesh3DBackend::EnsureShadowMap(int slot, int size)
{
    if (m_ShadowTexture[slot] && m_ShadowSize[slot] == size)
    {
        return true;
    }
    const GL& gl = m_Device.gl();
    if (!m_ShadowTexture[slot])
    {
        gl.GenTextures(1, &m_ShadowTexture[slot]);
        gl.GenFramebuffers(1, &m_ShadowFramebuffer[slot]);
    }
    gl.ActiveTexture(slot == 0 ? gl::TEXTURE1 : gl::TEXTURE2);
    gl.BindTexture(gl::TEXTURE_2D, m_ShadowTexture[slot]);
    gl.TexImage2D(gl::TEXTURE_2D, 0, static_cast<GLint>(gl::DEPTH_COMPONENT24), size, size, 0, gl::DEPTH_COMPONENT,
                  gl::UNSIGNED_INT, nullptr);
    // Compared, not read: a lookup says how much of the light gets past,
    // filtered between the four nearest texels.
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<GLint>(gl::LINEAR));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<GLint>(gl::LINEAR));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<GLint>(gl::CLAMP_TO_EDGE));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_MODE, static_cast<GLint>(gl::COMPARE_REF_TO_TEXTURE));
    gl.TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_COMPARE_FUNC, static_cast<GLint>(gl::LEQUAL));
    gl.ActiveTexture(gl::TEXTURE0);
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_ShadowFramebuffer[slot]);
    gl.FramebufferTexture2D(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::TEXTURE_2D, m_ShadowTexture[slot], 0);
    // Depth only: no colour to draw into or read from.
    const GLenum none = gl::NONE;
    gl.DrawBuffers(1, &none);
    gl.ReadBuffer(gl::NONE);
    if (gl.CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE)
    {
        DEKI_LOG_WARNING("deki-3d: no %dx%d shadow map; its light casts no shadows", size, size);
        gl.DeleteFramebuffers(1, &m_ShadowFramebuffer[slot]);
        gl.DeleteTextures(1, &m_ShadowTexture[slot]);
        m_ShadowFramebuffer[slot] = m_ShadowTexture[slot] = 0;
        m_ShadowSize[slot] = 0;
        return false;
    }
    m_ShadowSize[slot] = size;
    return true;
}

void GLMesh3DBackend::BindShadowMaps()
{
    const GL& gl = m_Device.gl();
    for (int i = 0; i < RasterConfig::kMaxShadows; ++i)
    {
        if (i >= m_Config.shadowCount && !m_ShadowTexture[i])
        {
            EnsureShadowMap(i, 1);
        }
        gl.ActiveTexture(i == 0 ? gl::TEXTURE1 : gl::TEXTURE2);
        gl.BindTexture(gl::TEXTURE_2D, m_ShadowTexture[i]);
    }
    gl.ActiveTexture(gl::TEXTURE0);
}

void GLMesh3DBackend::DrawShadowCaster(int slot, const Mesh3D& mesh, const Material3D* materials,
                                       uint16_t materialCount, const Deki::Mat4& model)
{
    if (!m_Ready || slot < 0 || slot >= m_Config.shadowCount || !mesh.vertices || !mesh.indices ||
        mesh.layout.positionOffset < 0 || mesh.vertexCount == 0 || !m_Device.MakeCurrent())
    {
        return;
    }
    const ShadowMap& map = m_Config.shadows[slot];
    const int size = std::min(std::max(map.resolution, 16), 8192);
    if (!EnsureShadowMap(slot, size))
    {
        return;
    }
    const GL& gl = m_Device.gl();
    // Nothing samples a map while it is drawn into.
    for (GLenum unit : { gl::TEXTURE1, gl::TEXTURE2 })
    {
        gl.ActiveTexture(unit);
        gl.BindTexture(gl::TEXTURE_2D, 0);
    }
    gl.ActiveTexture(gl::TEXTURE0);
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_ShadowFramebuffer[slot]);
    gl.Viewport(0, 0, size, size);
    gl.Disable(gl::BLEND);
    gl.Disable(gl::SCISSOR_TEST);
    gl.Enable(gl::DEPTH_TEST);
    gl.DepthFunc(gl::LESS);
    gl.DepthMask(gl::kTrue);
    if (!m_ShadowCleared[slot])
    {
        m_ShadowCleared[slot] = true;
        gl.SetClearDepth(1.0f);
        gl.Clear(gl::DEPTH_BUFFER_BIT);
    }
    // Both faces cast; the slope offset keeps a lit surface off its own depth.
    gl.Disable(gl::CULL_FACE);
    gl.ColorMask(gl::kFalse, gl::kFalse, gl::kFalse, gl::kFalse);
    gl.Enable(gl::POLYGON_OFFSET_FILL);
    gl.PolygonOffset(1.5f, 4.0f);

    m_Device.Use();
    m_Device.BindMesh(mesh);
    const Deki::Mat4 mvp = Mul(map.viewProjection, model);
    for (uint16_t s = 0; s < mesh.submeshCount; ++s)
    {
        const Submesh3D& sub = mesh.submeshes[s];
        if (static_cast<uint64_t>(sub.firstIndex) + sub.indexCount > mesh.indexCount)
        {
            continue;
        }
        const Material3D material =
            (materials && sub.materialIndex < materialCount) ? materials[sub.materialIndex] : Material3D{};
        // The mesh's own program, so its shader moves the vertices as when
        // it is drawn; unlit, which samples no shadow map.
        const GLDevice::Program& program = m_Device.ProgramFor(material.shader);
        const GLDevice::Uniforms& u = program.u;
        gl.UseProgram(program.id);
        gl.UniformMatrix4fv(u.mvp, 1, gl::kTrue, &mvp.m[0][0]);
        gl.UniformMatrix4fv(u.model, 1, gl::kTrue, &model.m[0][0]);
        gl.UniformMatrix4fv(u.normalMatrix, 1, gl::kTrue, &model.m[0][0]);
        gl.Uniform1i(u.hasNormals, mesh.layout.HasNormals() ? 1 : 0);
        gl.Uniform1i(u.shading, static_cast<GLint>(ShadingModel::Unlit));
        gl.Uniform1i(u.lightCount, 0);
        gl.Uniform1i(u.affine, 0);
        gl.Uniform1i(u.snap, 0);
        gl.Uniform2f(u.viewport, static_cast<float>(size), static_cast<float>(size));
        gl.Uniform1f(u.time, m_Time);
        gl.Uniform1i(u.texture, 0);
        gl.Uniform3f(u.params, material.shaderParams.x, material.shaderParams.y, material.shaderParams.z);
        gl.Uniform1i(u.alphaTest, material.alphaTest ? 1 : 0);
        const Texture3D* tex = material.texture;
        if (tex && tex->Valid() && material.alphaTest)
        {
            m_Device.BindTexture(*tex);
            const bool coverageOnly = !tex->palette && tex->format == TexelFormat::ALPHA8;
            gl.Uniform1i(u.texMode, coverageOnly ? 2 : 1);
        }
        else
        {
            gl.Uniform1i(u.texMode, 0);
        }
        const GLsizei count = static_cast<GLsizei>(sub.indexCount - sub.indexCount % 3);
        gl.DrawElements(gl::TRIANGLES, count, mesh.indexBytes == 4 ? gl::UNSIGNED_INT : gl::UNSIGNED_SHORT,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(sub.firstIndex) * mesh.indexBytes));
    }
    gl.Disable(gl::POLYGON_OFFSET_FILL);
    gl.ColorMask(gl::kTrue, gl::kTrue, gl::kTrue, gl::kTrue);
    m_Program = nullptr;  // the next mesh sets its program's uniforms again
    m_Device.CheckErrors("drawing a shadow map");
}

void GLMesh3DBackend::ReleaseTargets()
{
    const GL& gl = m_Device.gl();
    if (m_Framebuffer)
    {
        gl.DeleteFramebuffers(1, &m_Framebuffer);
    }
    const GLuint renderbuffers[2] = { m_ColorBuffer, m_DepthBuffer };
    gl.DeleteRenderbuffers(2, renderbuffers);
    m_Framebuffer = m_ColorBuffer = m_DepthBuffer = 0;
    m_TargetWidth = m_TargetHeight = 0;
}

bool GLMesh3DBackend::Resize(int32_t width, int32_t height)
{
    if (m_Framebuffer && width == m_TargetWidth && height == m_TargetHeight)
    {
        return true;
    }
    ReleaseTargets();
    const GL& gl = m_Device.gl();
    gl.GenFramebuffers(1, &m_Framebuffer);
    gl.GenRenderbuffers(1, &m_ColorBuffer);
    gl.GenRenderbuffers(1, &m_DepthBuffer);
    gl.BindRenderbuffer(gl::RENDERBUFFER, m_ColorBuffer);
    gl.RenderbufferStorage(gl::RENDERBUFFER, gl::RGBA8, width, height);
    gl.BindRenderbuffer(gl::RENDERBUFFER, m_DepthBuffer);
    gl.RenderbufferStorage(gl::RENDERBUFFER, gl::DEPTH_COMPONENT24, width, height);
    gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
    gl.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::RENDERBUFFER, m_ColorBuffer);
    gl.FramebufferRenderbuffer(gl::FRAMEBUFFER, gl::DEPTH_ATTACHMENT, gl::RENDERBUFFER, m_DepthBuffer);
    if (gl.CheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE)
    {
        DEKI_LOG_WARNING("deki-3d: no %dx%d framebuffer; this frame's meshes are not drawn", width, height);
        ReleaseTargets();
        return false;
    }
    m_TargetWidth = width;
    m_TargetHeight = height;
    return true;
}

void GLMesh3DBackend::BeginFrame(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                                 const RasterConfig& config)
{
    m_Buffer = buffer;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_Config = config;
    m_Drawn = false;
    m_Started = false;
    for (bool& cleared : m_ShadowCleared)
    {
        cleared = false;
    }
    m_MinX = m_MinY = 0;
    m_MaxX = m_MaxY = -1;
    m_Ready = buffer && width > 0 && height > 0;

    // Into the presenter's frame when it is in this context and can take the
    // buffer as layers; read back otherwise (the editor, a display that does
    // not present through GL, a scene that keeps its frame).
    GLPresenter* presenter = config.composeOnGpu ? GLPresenter::Active() : nullptr;
    m_Presenter = presenter && &presenter->Context() == m_Device.Context() &&
                          presenter->TakesLayers(width, height, format)
                      ? presenter
                      : nullptr;
    // Which way the meshes reach the screen, said once and again whenever it
    // changes, since the two differ so much in cost.
    const int mode = m_Presenter ? 1 : 2;
    if (mode != m_LoggedMode)
    {
        m_LoggedMode = mode;
        DEKI_LOG_INFO(m_Presenter ? "deki-3d: drawing meshes straight into the presented frame"
                                  : "deki-3d: drawing meshes off-screen and reading them back");
    }
}

bool GLMesh3DBackend::Start()
{
    if (!m_Device.MakeCurrent())
    {
        return false;
    }
    const GL& gl = m_Device.gl();
    const int32_t width = m_Width;
    const int32_t height = m_Height;
    const RasterConfig& config = m_Config;
    // First: making a map binds its framebuffer.
    BindShadowMaps();
    if (m_Presenter)
    {
        // What sorts below these meshes goes up as a layer; what sorts above
        // starts from transparent.
        m_Presenter->PushLayer(m_Buffer, width, height, m_Format);
        std::memset(m_Buffer, 0, Deki::FrameBufferBytes(m_Format, width, height));
        m_Presenter->BindFrame(width, height);
        gl.SetClearDepth(1.0f);
        gl.DepthMask(gl::kTrue);
        gl.Clear(gl::DEPTH_BUFFER_BIT);
    }
    else
    {
        if (!Resize(width, height))
        {
            return false;
        }
        gl.BindFramebuffer(gl::FRAMEBUFFER, m_Framebuffer);
        gl.Viewport(0, 0, width, height);
        // Transparent, so EndFrame can tell what the meshes drew.
        gl.ClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        gl.SetClearDepth(1.0f);
        gl.DepthMask(gl::kTrue);
        gl.Clear(gl::COLOR_BUFFER_BIT | gl::DEPTH_BUFFER_BIT);
    }
    gl.Disable(gl::BLEND);
    gl.Disable(gl::SCISSOR_TEST);
    gl.Disable(gl::DITHER);
    gl.FrontFace(gl::CCW);
    gl.CullFace(gl::BACK);
    if (config.depthTest)
    {
        gl.Enable(gl::DEPTH_TEST);
        gl.DepthFunc(gl::LESS);
    }
    else
    {
        gl.Disable(gl::DEPTH_TEST);
    }
    gl.DepthMask(config.depthWrite ? gl::kTrue : gl::kFalse);

    m_Device.Use();
    m_Config.lightCount = std::min(std::max(m_Config.lightCount, 0), RasterConfig::kMaxLights);
    for (int i = 0; i < m_Config.lightCount; ++i)
    {
        Deki::Vector3& d = m_Config.lights[i].direction;
        if (d.LengthSquared() > 0.0f)
        {
            d.Normalize();
        }
    }
    // Seconds since the program started: a shader's clock.
    static const auto s_Start = std::chrono::steady_clock::now();
    m_Time = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_Start).count();
    m_Program = nullptr;
    return true;
}

void GLMesh3DBackend::Cover(const Mesh3D& mesh, const Deki::Mat4& mvp)
{
    const Deki::Vector3& lo = mesh.boundsMin;
    const Deki::Vector3& hi = mesh.boundsMax;
    const bool noBounds =
        lo.x > hi.x || lo.y > hi.y || lo.z > hi.z || (lo.x == hi.x && lo.y == hi.y && lo.z == hi.z);
    bool whole = noBounds;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    for (int i = 0; i < 8 && !whole; ++i)
    {
        const Deki::Vector3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        float w = 1.0f;
        const Deki::Vector3 clip = TransformPoint(mvp, corner, w);
        if (w <= 1e-6f)
        {
            whole = true;  // behind the eye: the projection of the box is unbounded
            break;
        }
        const float x = (clip.x / w + 1.0f) * 0.5f * static_cast<float>(m_Width);
        const float y = (1.0f - clip.y / w) * 0.5f * static_cast<float>(m_Height);
        minX = i == 0 ? x : std::min(minX, x);
        maxX = i == 0 ? x : std::max(maxX, x);
        minY = i == 0 ? y : std::min(minY, y);
        maxY = i == 0 ? y : std::max(maxY, y);
    }
    int32_t x0 = 0, y0 = 0, x1 = m_Width - 1, y1 = m_Height - 1;
    if (!whole)
    {
        // A pixel of slack for rasterisation and vertex snapping.
        x0 = std::max<int32_t>(0, static_cast<int32_t>(std::floor(minX)) - 1);
        y0 = std::max<int32_t>(0, static_cast<int32_t>(std::floor(minY)) - 1);
        x1 = std::min<int32_t>(m_Width - 1, static_cast<int32_t>(std::ceil(maxX)) + 1);
        y1 = std::min<int32_t>(m_Height - 1, static_cast<int32_t>(std::ceil(maxY)) + 1);
        if (x0 > x1 || y0 > y1)
        {
            return;  // entirely off-screen
        }
    }
    if (m_MaxX < m_MinX)
    {
        m_MinX = x0;
        m_MinY = y0;
        m_MaxX = x1;
        m_MaxY = y1;
    }
    else
    {
        m_MinX = std::min(m_MinX, x0);
        m_MinY = std::min(m_MinY, y0);
        m_MaxX = std::max(m_MaxX, x1);
        m_MaxY = std::max(m_MaxY, y1);
    }
}

void GLMesh3DBackend::DrawMesh(const Mesh3D& mesh, const Material3D* materials,
                               uint16_t materialCount, const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix,
                               const Deki::Mat4& model)
{
    if (!m_Ready || !mesh.vertices || !mesh.indices || mesh.layout.positionOffset < 0 || mesh.vertexCount == 0)
    {
        return;
    }
    if (!m_Presenter)
    {
        Cover(mesh, mvp);
        if (m_MaxX < m_MinX)
        {
            return;  // nothing of this batch is on screen yet
        }
    }
    if (!m_Started)
    {
        m_Started = true;
        m_Ready = Start();
        if (!m_Ready)
        {
            return;
        }
    }

    const GL& gl = m_Device.gl();
    m_Device.BindMesh(mesh);
    m_Mvp = &mvp;
    m_NormalMatrix = &normalMatrix;
    m_Model = &model;
    m_HasNormals = mesh.layout.HasNormals();
    m_MeshUniformsOn = nullptr;

    for (uint16_t s = 0; s < mesh.submeshCount; ++s)
    {
        const Submesh3D& sub = mesh.submeshes[s];
        if (static_cast<uint64_t>(sub.firstIndex) + sub.indexCount > mesh.indexCount)
        {
            continue;
        }
        const Material3D material =
            (materials && sub.materialIndex < materialCount) ? materials[sub.materialIndex] : Material3D{};

        // The material's program: its shader's, or the built-in one. A switch
        // sets the batch's uniforms on it, and the mesh's once per mesh.
        const GLDevice::Program& program = m_Device.ProgramFor(material.shader);
        const GLDevice::Uniforms& u = program.u;
        if (m_Program != &program)
        {
            gl.UseProgram(program.id);
            gl.Uniform1i(u.texture, 0);
            gl.Uniform3f(u.ambient, m_Config.ambient.x, m_Config.ambient.y, m_Config.ambient.z);
            gl.Uniform1i(u.lightCount, m_Config.lightCount);
            for (int l = 0; l < m_Config.lightCount; ++l)
            {
                const Light3D& light = m_Config.lights[l];
                gl.Uniform4f(u.lightPosition[l], light.position.x, light.position.y, light.position.z,
                             static_cast<float>(light.kind));
                gl.Uniform4f(u.lightDirection[l], light.direction.x, light.direction.y, light.direction.z,
                             light.range);
                gl.Uniform4f(u.lightColor[l], light.color.x, light.color.y, light.color.z, 0.0f);
                // Its shadow map, 1 or 2, or 0 for none.
                const float map = light.shadow >= 0 && light.shadow < m_Config.shadowCount
                                      ? static_cast<float>(light.shadow + 1)
                                      : 0.0f;
                gl.Uniform4f(u.lightCone[l], light.cosOuter, light.cosInner, map, 0.0f);
            }
            gl.Uniform1i(u.shadowMap0, 1);
            gl.Uniform1i(u.shadowMap1, 2);
            gl.UniformMatrix4fv(u.shadowMatrix0, 1, gl::kTrue, &m_Config.shadows[0].viewProjection.m[0][0]);
            gl.UniformMatrix4fv(u.shadowMatrix1, 1, gl::kTrue, &m_Config.shadows[1].viewProjection.m[0][0]);
            gl.Uniform2f(u.shadowBias, m_Config.shadows[0].bias, m_Config.shadows[1].bias);
            gl.Uniform1i(u.affine, m_Config.perspectiveCorrect ? 0 : 1);
            gl.Uniform1i(u.snap, m_Config.vertexSnap ? 1 : 0);
            gl.Uniform2f(u.viewport, static_cast<float>(m_Width), static_cast<float>(m_Height));
            gl.Uniform1f(u.time, m_Time);
            if (program.readsTime)
            {
                // A shader that reads the time moves on its own.
                Deki::Time::NoteTimeDrivenFrame();
            }
            m_Program = &program;
            m_MeshUniformsOn = nullptr;
        }
        if (m_MeshUniformsOn != &program)
        {
            // Transposed: the shaders read matrices row-major (ShaderCompiler.h).
            gl.UniformMatrix4fv(u.mvp, 1, gl::kTrue, &m_Mvp->m[0][0]);
            gl.UniformMatrix4fv(u.normalMatrix, 1, gl::kTrue, &m_NormalMatrix->m[0][0]);
            gl.UniformMatrix4fv(u.model, 1, gl::kTrue, &m_Model->m[0][0]);
            gl.Uniform1i(u.hasNormals, m_HasNormals ? 1 : 0);
            m_MeshUniformsOn = &program;
        }

        SetTint(gl, u.tint, material.tint);
        SetTint(gl, u.color, material.shaderColor);
        gl.Uniform3f(u.params, material.shaderParams.x, material.shaderParams.y, material.shaderParams.z);
        gl.Uniform1i(u.shading, static_cast<GLint>(material.shading));
        gl.Uniform1i(u.alphaTest, material.alphaTest ? 1 : 0);
        const Texture3D* tex = material.texture;
        if (tex && tex->Valid())
        {
            m_Device.BindTexture(*tex);
            const bool coverageOnly = !tex->palette && tex->format == TexelFormat::ALPHA8;
            gl.Uniform1i(u.texMode, coverageOnly ? 2 : 1);
        }
        else
        {
            gl.Uniform1i(u.texMode, 0);
        }
        if (m_Config.backfaceCull && !material.doubleSided)
        {
            gl.Enable(gl::CULL_FACE);
        }
        else
        {
            gl.Disable(gl::CULL_FACE);
        }

        const GLsizei count = static_cast<GLsizei>(sub.indexCount - sub.indexCount % 3);
        gl.DrawElements(gl::TRIANGLES, count, mesh.indexBytes == 4 ? gl::UNSIGNED_INT : gl::UNSIGNED_SHORT,
                        reinterpret_cast<const void*>(static_cast<uintptr_t>(sub.firstIndex) * mesh.indexBytes));
    }
    m_Drawn = true;
}

namespace
{

/// Writes the pixels the meshes drew (alpha set) from a bottom-up RGBA
/// readback of the rectangle into the framebuffer, in its format.
template <Deki::ColorFormat F>
void Composite(const uint8_t* rgba, uint8_t* buffer, int32_t width, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    const int32_t w = x1 - x0 + 1;
    const size_t bpp = Deki::FrameBufferBytes(F, 1, 1);
    for (int32_t y = y0; y <= y1; ++y)
    {
        const uint8_t* src = rgba + static_cast<size_t>(y1 - y) * w * 4;
        uint8_t* row = buffer + static_cast<size_t>(y) * width * bpp;
        for (int32_t x = 0; x < w; ++x, src += 4)
        {
            if (src[3] != 0)
            {
                DekiPixel::WriteDstPixel<F>(row, static_cast<size_t>(x0 + x), src[0], src[1], src[2], 255);
            }
        }
    }
}

}  // namespace

void GLMesh3DBackend::EndFrame()
{
    // Drawn into the presenter's frame: nothing to bring back.
    if (!m_Ready || !m_Drawn || m_Presenter || m_MaxX < m_MinX)
    {
        m_Ready = false;
        return;
    }
    m_Ready = false;

    const GL& gl = m_Device.gl();
    const int32_t w = m_MaxX - m_MinX + 1;
    const int32_t h = m_MaxY - m_MinY + 1;
    m_Readback.resize(static_cast<size_t>(w) * h * 4);
    gl.PixelStorei(gl::PACK_ALIGNMENT, 4);
    // GL counts rows from the bottom.
    gl.ReadPixels(m_MinX, m_Height - 1 - m_MaxY, w, h, gl::RGBA, gl::UNSIGNED_BYTE, m_Readback.data());
    m_Device.CheckErrors("drawing meshes");

    switch (m_Format)
    {
        case Deki::ColorFormat::RGB565:
            Composite<Deki::ColorFormat::RGB565>(m_Readback.data(), m_Buffer, m_Width, m_MinX, m_MinY, m_MaxX,
                                                 m_MaxY);
            break;
        case Deki::ColorFormat::RGB888:
            Composite<Deki::ColorFormat::RGB888>(m_Readback.data(), m_Buffer, m_Width, m_MinX, m_MinY, m_MaxX,
                                                 m_MaxY);
            break;
        case Deki::ColorFormat::ARGB8888:
            Composite<Deki::ColorFormat::ARGB8888>(m_Readback.data(), m_Buffer, m_Width, m_MinX, m_MinY, m_MaxX,
                                                   m_MaxY);
            break;
        case Deki::ColorFormat::RGB565A8:
            Composite<Deki::ColorFormat::RGB565A8>(m_Readback.data(), m_Buffer, m_Width, m_MinX, m_MinY, m_MaxX,
                                                   m_MaxY);
            break;
    }
}

// ---------------------------------------------------------------------------
// The backends passes draw through (Mesh3DBackend.h). The device comes up
// with the first. When the program ends, or the editor unloads the package,
// whatever backends are left go before the device, and a pass destroying its
// own after that finds nothing to do.
// ---------------------------------------------------------------------------

namespace
{

std::unique_ptr<GLDevice> s_Device;
bool s_DeviceFailed = false;
std::vector<GLMesh3DBackend*> s_Backends;

struct Teardown
{
    ~Teardown()
    {
        for (GLMesh3DBackend* backend : s_Backends)
        {
            delete backend;
        }
        s_Backends.clear();
        s_Device.reset();
    }
};
Teardown s_Teardown;

}  // namespace

Mesh3DBackend* CreateGpuBackend()
{
    if (!s_Device && !s_DeviceFailed)
    {
        s_Device = GLDevice::Create();
        s_DeviceFailed = !s_Device;
    }
    if (!s_Device)
    {
        return nullptr;
    }
    GLMesh3DBackend* backend = new GLMesh3DBackend(*s_Device);
    s_Backends.push_back(backend);
    return backend;
}

void DestroyGpuBackend(Mesh3DBackend* backend)
{
    auto it = std::find(s_Backends.begin(), s_Backends.end(), backend);
    if (it != s_Backends.end())
    {
        s_Backends.erase(it);
        delete backend;
    }
}

const char* GpuBackendName()
{
#if defined(_WIN32)
    return "OpenGL 3.3";
#else
    return "OpenGL ES 3.0";
#endif
}

}  // namespace Deki3D

#endif  // DEKI_CAPABILITY_OPENGL
