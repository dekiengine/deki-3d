#pragma once

#include "Deki3DAPI.h"
#include "Mesh3D.h"

#include "deki-rendering/RendererComponent.h"
#include <deki/Color.h>
#include <deki/reflection/Property.h>

/// Which built-in shape to draw. Primitives exist so a scene can have 3D in
/// it before the mesh asset pipeline does, and they stay afterwards because
/// a box and a plane are what most blocking-out needs. Mesh assets arrive as
/// a separate property, not as another entry here.
enum class MeshPrimitive : uint8_t
{
    Cube = 0,
    Quad = 1,
    Pyramid = 2,
};

DEKI_CATEGORY("3D")
DEKI_DESCRIPTION("Draws a 3D mesh. The object's transform places it, and the scene needs a Camera3DComponent to see it.")
class DEKI_3D_API MeshComponent : public RendererComponent
{
public:
    MeshComponent();
    ~MeshComponent() override;

    DEKI_TOOLTIP("Which built-in shape to draw until a mesh asset is assigned.")
    DEKI_EXPORT
    MeshPrimitive primitive = MeshPrimitive::Cube;

    DEKI_TOOLTIP("Multiplies the mesh's own colours. White leaves them alone.")
    DEKI_EXPORT
    Deki::Color tintColor;

    DEKI_TOOLTIP("Unlit is flat colour. Vertex lit shades each vertex against the scene light. Flat shades whole triangles, which is the faceted look.")
    DEKI_EXPORT
    Deki3D::ShadingModel shading = Deki3D::ShadingModel::VertexLit;

    DEKI_TOOLTIP("Draw the inside of the mesh as well as the outside. Costs roughly double the fill.")
    DEKI_EXPORT
    bool doubleSided = false;

    /// The geometry to draw this frame, built lazily from `primitive`. Null
    /// when there is nothing to draw.
    const Deki3D::Mesh3D* Resolve() const;

    /// RendererComponent's 2D path draws nothing: a mesh is not a quad of
    /// pixels that can be blitted, so Mesh3DPass rasterises it instead. The
    /// component still derives from RendererComponent because that is what
    /// puts it in the renderer's sort, next to the sprites.
    bool RenderContent(const Deki::Object* owner, QuadBlit::Source& outSource,
                       float& outPivotX, float& outPivotY,
                       uint8_t& outTintR, uint8_t& outTintG, uint8_t& outTintB,
                       uint8_t& outTintA) override;

private:
    mutable MeshPrimitive m_BuiltPrimitive = static_cast<MeshPrimitive>(0xFF);
    mutable Deki3D::Mesh3D m_Mesh;
    mutable void* m_Storage = nullptr;  // owns the vertex, index and submesh arrays
};
