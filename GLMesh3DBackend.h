#pragma once

// Only where the platform has OpenGL: the build defines DEKI_CAPABILITY_OPENGL
// for one whose platform provides "opengl" (the desktop, a Linux handheld) and
// never for a microcontroller, where this file compiles to nothing.
#ifdef DEKI_CAPABILITY_OPENGL

// deki-3d's meshes on the GPU, through OpenGL ES 3.0 or desktop OpenGL 3.3.
//
// A Mesh3DBackend like the software rasteriser, drawing the same frame into
// the same CPU framebuffer: the batch draws into a framebuffer object of its
// own, cleared to transparent, and EndFrame reads back the part the meshes
// could have touched and writes the pixels they did over the 2D content
// below. The 2D path, the pass's sorting and the display stay as they are.
//
// Where the display presents through deki-rendering's GLPresenter, it does
// better: at a batch's first mesh, the 2D drawn so far goes to the presenter
// as a layer, the meshes draw straight into the frame it is building, and the
// CPU buffer is cleared to transparent for the 2D that sorts after them.
// Nothing is read back.
//
// What it draws is what the software rasteriser draws: the three shading
// models against the one light, textures in every Texture3D format with the
// same holes, affine texturing and vertex snapping when the scene asks for
// them. Where the GPU does better for nothing it does: lighting and vertex
// colours are interpolated across a triangle, where the software rasteriser
// takes its first vertex's, and the light is not cut to 32 levels.

#include "Mesh3DBackend.h"

#include <cstdint>
#include <vector>

namespace DekiGL
{
class GLPresenter;
}

namespace Deki3D
{

class GLDevice;

class GLMesh3DBackend : public Mesh3DBackend
{
public:
    explicit GLMesh3DBackend(GLDevice& device);
    ~GLMesh3DBackend() override;

    GLMesh3DBackend(const GLMesh3DBackend&) = delete;
    GLMesh3DBackend& operator=(const GLMesh3DBackend&) = delete;

    void BeginFrame(uint8_t* buffer, int32_t width, int32_t height, Deki::ColorFormat format,
                    const RasterConfig& config) override;
    void DrawMesh(const Mesh3D& mesh, const Material3D* materials, uint16_t materialCount,
                  const Deki::Mat4& mvp, const Deki::Mat4& normalMatrix,
                  const Deki::Mat4& model = Deki::Mat4::Identity()) override;
    void EndFrame() override;
    /// Only when it reads back: into the presenter's frame, nothing.
    bool WritesBuffer() const override { return m_Presenter == nullptr; }

private:
    /// Sets the GPU up for the batch, at its first mesh: pushes the 2D drawn
    /// so far to the presenter and binds its frame, or clears this backend's
    /// own framebuffer. False when there is nowhere to draw.
    bool Start();
    /// Makes the framebuffer object `width` x `height`, if it is not already.
    bool Resize(int32_t width, int32_t height);
    /// Widens the readback rectangle by where `mesh` can land on screen.
    void Cover(const Mesh3D& mesh, const Deki::Mat4& mvp);
    void ReleaseTargets();

    GLDevice& m_Device;

    unsigned int m_Framebuffer = 0;
    unsigned int m_ColorBuffer = 0;
    unsigned int m_DepthBuffer = 0;
    int32_t m_TargetWidth = 0;
    int32_t m_TargetHeight = 0;

    // The frame.
    uint8_t* m_Buffer = nullptr;
    int32_t m_Width = 0;
    int32_t m_Height = 0;
    Deki::ColorFormat m_Format = Deki::ColorFormat::RGB565;
    RasterConfig m_Config;
    bool m_Ready = false;    // the batch can draw
    bool m_Started = false;  // Start() has run for this batch
    bool m_Drawn = false;    // a mesh drew since
    // The presenter the batch draws into, or null to read back.
    DekiGL::GLPresenter* m_Presenter = nullptr;
    int m_LoggedMode = 0;  // the path last logged: 1 into the presenter, 2 read back
    // Screen rectangle the batch can have touched, top-down like the
    // framebuffer, inclusive.
    int32_t m_MinX = 0;
    int32_t m_MinY = 0;
    int32_t m_MaxX = -1;
    int32_t m_MaxY = -1;

    // Uniform values for the batch and for the mesh being drawn, set on each
    // program as the draw switches to it: a mesh with a shader of its own
    // draws with another program than one without.
    float m_Time = 0.0f;
    const void* m_Program = nullptr;         // the GLDevice::Program bound
    const void* m_MeshUniformsOn = nullptr;  // the program the mesh's are set on
    const Deki::Mat4* m_Mvp = nullptr;
    const Deki::Mat4* m_NormalMatrix = nullptr;
    const Deki::Mat4* m_Model = nullptr;
    bool m_HasNormals = false;

    std::vector<uint8_t> m_Readback;
};

}  // namespace Deki3D

#endif  // DEKI_CAPABILITY_OPENGL
