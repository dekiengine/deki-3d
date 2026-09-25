#include "ObjImporter.h"

#ifdef DEKI_EDITOR

#include "../MeshAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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

/// The largest power of two no bigger than `value`, capped so a stray 4K
/// texture does not land whole on a microcontroller.
uint16_t FitPowerOfTwo(int value, int cap)
{
    if (value > cap)
        value = cap;
    if (value < 1)
        value = 1;
    uint16_t size = 1;
    while (static_cast<int>(size) * 2 <= value)
        size = static_cast<uint16_t>(size * 2);
    return size;
}

/// Box-filter down to the target size, keeping 8-bit RGBA. Averaging rather
/// than point sampling, because these textures are shrunk a long way and a
/// point-sampled reduction aliases badly.
void ResampleRgba(const std::vector<uint8_t>& rgba, int srcW, int srcH,
                  uint16_t dstW, uint16_t dstH, std::vector<uint8_t>& out)
{
    out.resize(static_cast<size_t>(dstW) * dstH * 4);

    for (uint16_t y = 0; y < dstH; ++y)
    {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * srcH / dstH);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * srcH / dstH));
        for (uint16_t x = 0; x < dstW; ++x)
        {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * srcW / dstW);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * srcW / dstW));

            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = y0; sy < y1 && sy < srcH; ++sy)
            {
                for (int sx = x0; sx < x1 && sx < srcW; ++sx)
                {
                    const size_t i = (static_cast<size_t>(sy) * srcW + sx) * 4;
                    r += rgba[i];
                    g += rgba[i + 1];
                    b += rgba[i + 2];
                    a += rgba[i + 3];
                    ++n;
                }
            }
            if (n == 0)
                n = 1;
            uint8_t* d = out.data() + (static_cast<size_t>(y) * dstW + x) * 4;
            d[0] = static_cast<uint8_t>(r / n);
            d[1] = static_cast<uint8_t>(g / n);
            d[2] = static_cast<uint8_t>(b / n);
            d[3] = static_cast<uint8_t>(a / n);
        }
    }
}

/// 8-bit RGBA texels into `format`. RGB565 truncates as it always has, so a
/// model compiled for an RGB565 target stores exactly what it did before.
void EncodeTexels(const std::vector<uint8_t>& rgba, TexelFormat format, std::vector<uint8_t>& out)
{
    const size_t count = rgba.size() / 4;
    out.resize(count * TexelBytes(format));
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        const uint16_t v565 = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        uint8_t* d = out.data() + i * TexelBytes(format);
        switch (format)
        {
            case TexelFormat::RGB565: d[0] = v565 & 0xFF; d[1] = v565 >> 8; break;
            case TexelFormat::RGB565A8: d[0] = v565 & 0xFF; d[1] = v565 >> 8; d[2] = a; break;
            case TexelFormat::RGB888: d[0] = r; d[1] = g; d[2] = b; break;
            case TexelFormat::RGBA8888: d[0] = r; d[1] = g; d[2] = b; d[3] = a; break;
            case TexelFormat::ALPHA8: d[0] = a; break;
        }
    }
}

bool HasTransparentTexel(const std::vector<uint8_t>& rgba)
{
    for (size_t i = 3; i < rgba.size(); i += 4)
        if (rgba[i] < 128)
            return true;
    return false;
}

/// What one `newmtl` block says that this pipeline can use.
struct MtlEntry
{
    std::string diffuseMap;  // map_Kd, as written
    uint32_t tint = 0xFFFFFFFFu;
    bool alphaTest = false;
};

/// Every `newmtl` block in a material library, by name. Kd becomes the
/// material's tint, so a model that colours its parts without texturing them
/// still arrives coloured.
std::map<std::string, MtlEntry> ParseMaterialLibrary(const std::string& mtlText)
{
    std::map<std::string, MtlEntry> materials;
    std::string current;

    std::istringstream in(mtlText);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::istringstream ls(line);
        std::string keyword;
        ls >> keyword;

        if (keyword == "newmtl")
        {
            current.clear();
            ls >> current;
            if (!current.empty())
                materials.emplace(current, MtlEntry{});
        }
        else if (current.empty())
        {
            continue;  // anything before the first newmtl belongs to nobody
        }
        else if (keyword == "map_Kd")
        {
            // Options such as "-s 1 1 1" may precede the filename, so the
            // last token on the line is the one wanted.
            std::string token, last;
            while (ls >> token)
                last = token;
            materials[current].diffuseMap = last;
        }
        else if (keyword == "Kd")
        {
            float r = 1.0f, g = 1.0f, b = 1.0f;
            ls >> r >> g >> b;
            auto channel = [](float v) -> uint32_t {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<uint32_t>(v * 255.0f + 0.5f);
            };
            materials[current].tint =
                0xFF000000u | (channel(b) << 16) | (channel(g) << 8) | channel(r);
        }
        else if (keyword == "d" || keyword == "Tr")
        {
            // Anything not fully opaque gets the alpha test, which is the
            // only transparency the span loop has.
            float value = 1.0f;
            ls >> value;
            const float opacity = keyword == "d" ? value : 1.0f - value;
            materials[current].alphaTest = opacity < 0.999f;
        }
    }
    return materials;
}

/// Join a base directory and a path written inside a model file. Those paths
/// are relative to the model, and may use either separator.
std::string ResolveRelative(const std::string& baseDirectory, const std::string& relative)
{
    if (baseDirectory.empty())
        return relative;
    std::string joined = baseDirectory;
    if (joined.back() != '/' && joined.back() != '\\')
        joined += '/';
    joined += relative;
    return joined;
}

}  // namespace

bool CompileObjToMesh(const std::string& objText, const std::string& baseDirectory,
                      const ImageDecoder& decodeImage, std::vector<uint8_t>& outBlob,
                      std::string& error, const TextureFormatChooser& chooseFormat, int maxTextureSize)
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
    std::string materialLibrary;

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
        else if (keyword == "mtllib")
        {
            if (materialLibrary.empty())
                ls >> materialLibrary;
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

    // --- materials and their textures --------------------------------------
    //
    // One entry per `usemtl` name the faces actually used, in the order the
    // submeshes index them. A texture is decoded once per distinct image, so
    // several materials sharing a map share the texture too.
    std::map<std::string, MtlEntry> library;
    if (!materialLibrary.empty())
    {
        std::ifstream mtlFile(ResolveRelative(baseDirectory, materialLibrary), std::ios::binary);
        if (mtlFile)
        {
            std::ostringstream mtlText;
            mtlText << mtlFile.rdbuf();
            library = ParseMaterialLibrary(mtlText.str());
        }
    }

    std::vector<MeshFileMaterial> outMaterials(materialIds.size());
    std::vector<MeshFileTexture> outTextures;
    std::vector<uint8_t> texturePixels;
    std::map<std::string, int16_t> textureByPath;
    std::vector<bool> textureCutsOut;  // per texture: turns its materials' alpha test on

    for (const auto& entry : materialIds)
    {
        MeshFileMaterial material;
        material.textureIndex = -1;
        material.flags = 0;
        material.tint = 0xFFFFFFFFu;

        auto found = library.find(entry.first);
        if (found != library.end())
        {
            material.tint = found->second.tint;
            if (found->second.alphaTest)
                material.flags |= MeshMaterial_AlphaTest;

            // No texture coordinates means nothing to sample with, so the
            // image is not worth carrying.
            const std::string& map = found->second.diffuseMap;
            if (hasUVs && decodeImage && !map.empty())
            {
                auto cached = textureByPath.find(map);
                if (cached != textureByPath.end())
                {
                    material.textureIndex = cached->second;
                    if (textureCutsOut[static_cast<size_t>(cached->second)])
                        material.flags |= MeshMaterial_AlphaTest;
                }
                else
                {
                    int srcW = 0, srcH = 0;
                    std::vector<uint8_t> rgba;
                    if (decodeImage(ResolveRelative(baseDirectory, map), srcW, srcH, rgba) &&
                        srcW > 0 && srcH > 0 &&
                        rgba.size() >= static_cast<size_t>(srcW) * srcH * 4)
                    {
                        MeshFileTexture texture{};
                        const int cap = maxTextureSize > 0 ? maxTextureSize : kDefaultMeshTextureSize;
                        texture.width = FitPowerOfTwo(srcW, cap);
                        texture.height = FitPowerOfTwo(srcH, cap);
                        texture.byteOffset = static_cast<uint32_t>(texturePixels.size());

                        std::vector<uint8_t> scaled;
                        ResampleRgba(rgba, srcW, srcH, texture.width, texture.height, scaled);
                        const bool transparent = HasTransparentTexel(scaled);
                        const TexelFormat format = chooseFormat ? chooseFormat(transparent) : TexelFormat::RGB565;
                        texture.format = static_cast<uint8_t>(format);

                        // Transparent texels cut out, where the format keeps them.
                        const bool keepsAlpha = format == TexelFormat::RGB565A8 || format == TexelFormat::RGBA8888 ||
                                                format == TexelFormat::ALPHA8;
                        const bool cutsOut = transparent && keepsAlpha;
                        if (cutsOut)
                            material.flags |= MeshMaterial_AlphaTest;

                        std::vector<uint8_t> pixels;
                        EncodeTexels(scaled, format, pixels);
                        texturePixels.insert(texturePixels.end(), pixels.begin(), pixels.end());

                        material.textureIndex = static_cast<int16_t>(outTextures.size());
                        outTextures.push_back(texture);
                        textureCutsOut.push_back(cutsOut);
                        textureByPath.emplace(map, material.textureIndex);
                    }
                }
            }
        }
        outMaterials[entry.second] = material;
    }

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
    header.materialCount = static_cast<uint16_t>(outMaterials.size());
    header.textureCount = static_cast<uint16_t>(outTextures.size());
    header.texturePixelBytes = static_cast<uint32_t>(texturePixels.size());

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
        MeshFileSubmesh sub;
        sub.firstIndex = r.firstIndex;
        sub.indexCount = r.indexCount;
        sub.materialIndex = r.materialIndex;
        sub.padding = 0;
        append(&sub, sizeof(sub));
    }
    for (const MeshFileMaterial& material : outMaterials)
        append(&material, sizeof(material));
    for (const MeshFileTexture& texture : outTextures)
        append(&texture, sizeof(texture));
    if (!texturePixels.empty())
        append(texturePixels.data(), texturePixels.size());

    return true;
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
