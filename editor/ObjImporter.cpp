#include "ObjImporter.h"

#ifdef DEKI_EDITOR

#include "../MeshAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace Deki3D
{
namespace
{

struct Vec3
{
    float x = 0, y = 0, z = 0;
};

struct Vec2
{
    float u = 0, v = 0;
};

/// One corner of a face as .obj spells it: indices into the three source
/// arrays, already resolved to zero-based. -1 means the file did not give it.
struct Corner
{
    int position = -1;
    int uv = -1;
    int normal = -1;

    bool operator<(const Corner& o) const
    {
        if (position != o.position) return position < o.position;
        if (uv != o.uv) return uv < o.uv;
        return normal < o.normal;
    }
};

/// .obj indices are one-based and may be negative, meaning "counting back
/// from the end of what has been declared so far".
bool ResolveIndex(int raw, size_t declared, int& out)
{
    if (raw > 0)
        out = raw - 1;
    else if (raw < 0)
        out = static_cast<int>(declared) + raw;
    else
        return false;  // 0 is not a valid .obj index
    return out >= 0 && static_cast<size_t>(out) < declared;
}

/// "12", "12/4", "12//7" or "12/4/7".
bool ParseCorner(const std::string& token, size_t positions, size_t uvs, size_t normals,
                 Corner& out)
{
    int parts[3] = { 0, 0, 0 };
    int partCount = 0;
    size_t start = 0;
    while (partCount < 3)
    {
        const size_t slash = token.find('/', start);
        const std::string piece = token.substr(start, slash == std::string::npos
                                                          ? std::string::npos
                                                          : slash - start);
        parts[partCount++] = piece.empty() ? 0 : std::atoi(piece.c_str());
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }

    if (!ResolveIndex(parts[0], positions, out.position))
        return false;
    if (parts[1] != 0 && !ResolveIndex(parts[1], uvs, out.uv))
        out.uv = -1;
    if (parts[2] != 0 && !ResolveIndex(parts[2], normals, out.normal))
        out.normal = -1;
    return true;
}

}  // namespace

bool CompileObjToMesh(const std::string& objText, std::vector<uint8_t>& outBlob,
                      std::string& error)
{
    error.clear();
    outBlob.clear();

    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;

    // The deduplicated vertex buffer, built as faces are read.
    std::map<Corner, uint16_t> seen;
    std::vector<Vec3> outPositions;
    std::vector<Vec3> outNormals;
    std::vector<Vec2> outUVs;

    struct Range
    {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        uint16_t materialIndex = 0;
    };
    std::vector<Range> submeshes;
    std::map<std::string, uint16_t> materialIds;
    std::vector<uint16_t> indices;

    auto beginSubmesh = [&](const std::string& materialName) {
        if (!submeshes.empty())
        {
            submeshes.back().indexCount =
                static_cast<uint32_t>(indices.size()) - submeshes.back().firstIndex;
            if (submeshes.back().indexCount == 0)
                submeshes.pop_back();  // usemtl with no faces after it
        }
        Range r;
        r.firstIndex = static_cast<uint32_t>(indices.size());
        auto it = materialIds.find(materialName);
        if (it == materialIds.end())
        {
            const uint16_t id = static_cast<uint16_t>(materialIds.size());
            materialIds.emplace(materialName, id);
            r.materialIndex = id;
        }
        else
        {
            r.materialIndex = it->second;
        }
        submeshes.push_back(r);
    };

    std::istringstream in(objText);
    std::string line;
    size_t lineNumber = 0;
    bool overflowed = false;

    while (std::getline(in, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;

        if (keyword == "v")
        {
            Vec3 p;
            ls >> p.x >> p.y >> p.z;
            positions.push_back(p);
        }
        else if (keyword == "vt")
        {
            Vec2 t;
            ls >> t.u >> t.v;
            t.v = 1.0f - t.v;  // .obj measures V from the bottom, the sampler from the top
            uvs.push_back(t);
        }
        else if (keyword == "vn")
        {
            Vec3 n;
            ls >> n.x >> n.y >> n.z;
            normals.push_back(n);
        }
        else if (keyword == "usemtl")
        {
            std::string name;
            ls >> name;
            beginSubmesh(name);
        }
        else if (keyword == "f")
        {
            if (submeshes.empty())
                beginSubmesh("");  // a file with no usemtl is still one submesh

            std::vector<uint16_t> face;
            std::string token;
            while (ls >> token)
            {
                Corner corner;
                if (!ParseCorner(token, positions.size(), uvs.size(), normals.size(), corner))
                {
                    error = "line " + std::to_string(lineNumber) + ": face index out of range";
                    return false;
                }

                auto it = seen.find(corner);
                uint16_t index;
                if (it != seen.end())
                {
                    index = it->second;
                }
                else
                {
                    if (outPositions.size() >= 0xFFFFu)
                    {
                        overflowed = true;
                        break;
                    }
                    index = static_cast<uint16_t>(outPositions.size());
                    seen.emplace(corner, index);
                    outPositions.push_back(positions[corner.position]);
                    outNormals.push_back(corner.normal >= 0 ? normals[corner.normal] : Vec3{});
                    outUVs.push_back(corner.uv >= 0 ? uvs[corner.uv] : Vec2{});
                }
                face.push_back(index);
            }
            if (overflowed)
                break;

            // Fan triangulation: correct for the convex polygons .obj files
            // carry in practice, and it preserves winding.
            for (size_t i = 1; i + 1 < face.size(); ++i)
            {
                indices.push_back(face[0]);
                indices.push_back(face[i]);
                indices.push_back(face[i + 1]);
            }
        }
    }

    if (overflowed)
    {
        error = "more than 65535 vertices; the index buffer is 16-bit. Split the model.";
        return false;
    }
    if (outPositions.empty() || indices.empty())
    {
        error = "no faces found";
        return false;
    }
    if (!submeshes.empty())
    {
        submeshes.back().indexCount =
            static_cast<uint32_t>(indices.size()) - submeshes.back().firstIndex;
        if (submeshes.back().indexCount == 0)
            submeshes.pop_back();
    }

    const bool hadNormals = !normals.empty();
    if (!hadNormals)
    {
        // Smooth normals: accumulate each face's normal onto its corners, then
        // normalise. Vertices shared between faces end up averaged, which is
        // what a model without normals almost always wants.
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const Vec3& a = outPositions[indices[i]];
            const Vec3& b = outPositions[indices[i + 1]];
            const Vec3& c = outPositions[indices[i + 2]];
            const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            const Vec3 n{ uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
            for (int k = 0; k < 3; ++k)
            {
                outNormals[indices[i + k]].x += n.x;
                outNormals[indices[i + k]].y += n.y;
                outNormals[indices[i + k]].z += n.z;
            }
        }
        for (Vec3& n : outNormals)
        {
            const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (len > 0.0f)
            {
                n.x /= len;
                n.y /= len;
                n.z /= len;
            }
            else
            {
                n.y = 1.0f;  // a degenerate face leaves nothing to point at
            }
        }
    }

    const bool hasUVs = !uvs.empty();

    MeshFileHeader header{};
    std::memcpy(header.magic, "DMSH", 4);
    header.version = kMeshFileVersion;
    header.attributes = MeshAttribute_Position | MeshAttribute_Normal;
    if (hasUVs)
        header.attributes |= MeshAttribute_UV;
    header.vertexCount = static_cast<uint32_t>(outPositions.size());
    header.indexCount = static_cast<uint32_t>(indices.size());
    header.submeshCount = static_cast<uint16_t>(submeshes.size());
    header.vertexStride =
        static_cast<uint16_t>(sizeof(float) * 3 + sizeof(float) * 3 + (hasUVs ? sizeof(float) * 2 : 0));

    float lo[3] = { outPositions[0].x, outPositions[0].y, outPositions[0].z };
    float hi[3] = { lo[0], lo[1], lo[2] };
    for (const Vec3& p : outPositions)
    {
        lo[0] = std::min(lo[0], p.x); hi[0] = std::max(hi[0], p.x);
        lo[1] = std::min(lo[1], p.y); hi[1] = std::max(hi[1], p.y);
        lo[2] = std::min(lo[2], p.z); hi[2] = std::max(hi[2], p.z);
    }
    std::memcpy(header.boundsMin, lo, sizeof(lo));
    std::memcpy(header.boundsMax, hi, sizeof(hi));

    const size_t vertexBytes = static_cast<size_t>(header.vertexCount) * header.vertexStride;
    outBlob.reserve(sizeof(header) + vertexBytes + indices.size() * sizeof(uint16_t) +
                    submeshes.size() * sizeof(Submesh3D));

    auto append = [&outBlob](const void* data, size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        outBlob.insert(outBlob.end(), bytes, bytes + size);
    };

    append(&header, sizeof(header));
    for (size_t i = 0; i < outPositions.size(); ++i)
    {
        append(&outPositions[i], sizeof(float) * 3);
        append(&outNormals[i], sizeof(float) * 3);
        if (hasUVs)
            append(&outUVs[i], sizeof(float) * 2);
    }
    append(indices.data(), indices.size() * sizeof(uint16_t));
    for (const Range& r : submeshes)
    {
        Submesh3D sub;
        sub.firstIndex = r.firstIndex;
        sub.indexCount = r.indexCount;
        sub.materialIndex = r.materialIndex;
        append(&sub, sizeof(sub));
    }

    return true;
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
