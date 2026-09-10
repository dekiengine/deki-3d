#include "MeshComponent.h"

#include <vector>

namespace
{

using Deki3D::Submesh3D;
using Deki3D::VertexPNTC;

/// The arrays a built primitive owns. Held behind a void* in the component so
/// the header does not have to name it.
struct PrimitiveStorage
{
    std::vector<VertexPNTC> vertices;
    std::vector<uint16_t> indices;
    std::vector<Submesh3D> submeshes;
};

void AddQuad(PrimitiveStorage& s, const Deki::Vector3& origin,
             const Deki::Vector3& du, const Deki::Vector3& dv,
             const Deki::Vector3& normal)
{
    const uint16_t base = static_cast<uint16_t>(s.vertices.size());
    const Deki::Vector3 corners[4] = { origin, origin + du, origin + du + dv, origin + dv };
    const float uvs[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; ++i)
    {
        VertexPNTC v;
        v.position = corners[i];
        v.normal = normal;
        v.u = uvs[i][0];
        v.v = uvs[i][1];
        v.color = 0xFFFFFFFFu;
        s.vertices.push_back(v);
    }
    // Counter-clockwise seen from the side the normal points at, which is
    // what the rasteriser treats as front-facing.
    const uint16_t quad[6] = { base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2),
                               base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3) };
    s.indices.insert(s.indices.end(), quad, quad + 6);
}

void BuildCube(PrimitiveStorage& s)
{
    using V = Deki::Vector3;
    // Each face starts at its lower-left corner and spans with du, dv.
    AddQuad(s, V(-1, -1, 1), V(2, 0, 0), V(0, 2, 0), V(0, 0, 1));    // front
    AddQuad(s, V(1, -1, -1), V(-2, 0, 0), V(0, 2, 0), V(0, 0, -1));  // back
    AddQuad(s, V(1, -1, 1), V(0, 0, -2), V(0, 2, 0), V(1, 0, 0));    // right
    AddQuad(s, V(-1, -1, -1), V(0, 0, 2), V(0, 2, 0), V(-1, 0, 0));  // left
    AddQuad(s, V(-1, 1, 1), V(2, 0, 0), V(0, 0, -2), V(0, 1, 0));    // top
    AddQuad(s, V(-1, -1, -1), V(2, 0, 0), V(0, 0, 2), V(0, -1, 0));  // bottom
}

void BuildQuad(PrimitiveStorage& s)
{
    AddQuad(s, Deki::Vector3(-1, -1, 0), Deki::Vector3(2, 0, 0), Deki::Vector3(0, 2, 0),
            Deki::Vector3(0, 0, 1));
}

void BuildPyramid(PrimitiveStorage& s)
{
    using V = Deki::Vector3;
    AddQuad(s, V(-1, -1, -1), V(2, 0, 0), V(0, 0, 2), V(0, -1, 0));  // base

    const V apex(0, 1, 0);
    const V corners[4] = { V(-1, -1, 1), V(1, -1, 1), V(1, -1, -1), V(-1, -1, -1) };
    for (int i = 0; i < 4; ++i)
    {
        const V& a = corners[i];
        const V& b = corners[(i + 1) % 4];
        V n = (b - a).Cross(apex - a);
        if (n.LengthSquared() > 0.0f)
            n.Normalize();

        const uint16_t base = static_cast<uint16_t>(s.vertices.size());
        const V tri[3] = { a, b, apex };
        const float uvs[3][2] = { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } };
        for (int k = 0; k < 3; ++k)
        {
            VertexPNTC v;
            v.position = tri[k];
            v.normal = n;
            v.u = uvs[k][0];
            v.v = uvs[k][1];
            v.color = 0xFFFFFFFFu;
            s.vertices.push_back(v);
        }
        s.indices.insert(s.indices.end(), { base, static_cast<uint16_t>(base + 1),
                                            static_cast<uint16_t>(base + 2) });
    }
}

}  // namespace

MeshComponent::MeshComponent() = default;

MeshComponent::~MeshComponent()
{
    delete static_cast<PrimitiveStorage*>(m_Storage);
}

const Deki3D::Mesh3D* MeshComponent::Resolve() const
{
    if (m_Storage && m_BuiltPrimitive == primitive)
        return &m_Mesh;

    PrimitiveStorage* storage = static_cast<PrimitiveStorage*>(m_Storage);
    if (!storage)
    {
        storage = new PrimitiveStorage();
        m_Storage = storage;
    }
    storage->vertices.clear();
    storage->indices.clear();
    storage->submeshes.clear();

    switch (primitive)
    {
        case MeshPrimitive::Quad: BuildQuad(*storage); break;
        case MeshPrimitive::Pyramid: BuildPyramid(*storage); break;
        case MeshPrimitive::Cube:
        default: BuildCube(*storage); break;
    }

    Submesh3D sub;
    sub.firstIndex = 0;
    sub.indexCount = static_cast<uint32_t>(storage->indices.size());
    sub.materialIndex = 0;
    storage->submeshes.push_back(sub);

    m_Mesh.vertices = storage->vertices.data();
    m_Mesh.vertexCount = static_cast<uint32_t>(storage->vertices.size());
    m_Mesh.layout = Deki3D::LayoutPNTC();
    m_Mesh.indices = storage->indices.data();
    m_Mesh.indexCount = static_cast<uint32_t>(storage->indices.size());
    m_Mesh.submeshes = storage->submeshes.data();
    m_Mesh.submeshCount = static_cast<uint16_t>(storage->submeshes.size());
    m_Mesh.boundsMin = Deki::Vector3(-1, -1, -1);
    m_Mesh.boundsMax = Deki::Vector3(1, 1, 1);

    m_BuiltPrimitive = primitive;
    return &m_Mesh;
}

bool MeshComponent::RenderContent(const Deki::Object*, QuadBlit::Source&, float&, float&,
                                  uint8_t&, uint8_t&, uint8_t&, uint8_t&)
{
    return false;  // Mesh3DPass draws this; see the note in the header.
}
