#ifdef DEKI_EDITOR

#include "GltfImporter.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace Deki3D
{

namespace
{

constexpr uint32_t kGlbMagic = 0x46546C67;  // "glTF"
constexpr uint32_t kChunkJson = 0x4E4F534A;
constexpr uint32_t kChunkBin = 0x004E4942;

using Bytes = std::vector<unsigned char>;
using Mat4 = std::array<float, 16>;  // column-major, as glTF stores it

uint32_t ReadU32(const Bytes& data, size_t at)
{
    uint32_t value;
    std::memcpy(&value, data.data() + at, 4);
    return value;
}

Mat4 Identity()
{
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int k = 0; k < 4; ++k)
            {
                r[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            }
        }
    }
    return r;
}

Mat4 NodeMatrix(const json& node)
{
    if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16)
    {
        Mat4 m;
        for (int i = 0; i < 16; ++i)
        {
            m[i] = node["matrix"][i].get<float>();
        }
        return m;
    }

    std::array<float, 3> t{ 0, 0, 0 };
    std::array<float, 4> q{ 0, 0, 0, 1 };
    std::array<float, 3> s{ 1, 1, 1 };
    if (node.contains("translation"))
    {
        t = node["translation"].get<std::array<float, 3>>();
    }
    if (node.contains("rotation"))
    {
        q = node["rotation"].get<std::array<float, 4>>();
    }
    if (node.contains("scale"))
    {
        s = node["scale"].get<std::array<float, 3>>();
    }

    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return { (1 - 2 * (y * y + z * z)) * s[0],
             (2 * (x * y + z * w)) * s[0],
             (2 * (x * z - y * w)) * s[0],
             0,
             (2 * (x * y - z * w)) * s[1],
             (1 - 2 * (x * x + z * z)) * s[1],
             (2 * (y * z + x * w)) * s[1],
             0,
             (2 * (x * z + y * w)) * s[2],
             (2 * (y * z - x * w)) * s[2],
             (1 - 2 * (x * x + y * y)) * s[2],
             0,
             t[0],
             t[1],
             t[2],
             1 };
}

bool DecodeBase64(const std::string& text, Bytes& out)
{
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t accumulator = 0;
    int bits = 0;
    out.clear();
    for (char c : text)
    {
        if (c == '=' || c == '\r' || c == '\n')
        {
            continue;
        }
        const size_t value = kAlphabet.find(c);
        if (value == std::string::npos)
        {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

bool ReadFile(const std::string& path, Bytes& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// A glTF URI is percent-encoded; spaces in file names are the usual case.
std::string DecodeUri(const std::string& uri)
{
    std::string out;
    for (size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size())
        {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
        {
            out += uri[i];
        }
    }
    return out;
}

class Reader
{
public:
    Reader(const json& root, const std::string& base, const Bytes& glbBin, GltfConversion& out, std::string& error)
        : m_Root(root),
          m_Base(base),
          m_GlbBin(glbBin),
          m_Out(out),
          m_Error(error)
    {
    }

    bool LoadBuffers()
    {
        if (!m_Root.contains("buffers"))
        {
            return Fail("the file has no buffers");
        }
        for (const json& buffer : m_Root["buffers"])
        {
            Bytes data;
            const std::string uri = buffer.value("uri", std::string());
            if (uri.empty())
            {
                data = m_GlbBin;  // a .glb's own binary chunk
            }
            else if (uri.rfind("data:", 0) == 0)
            {
                const size_t comma = uri.find(',');
                if (comma == std::string::npos || !DecodeBase64(uri.substr(comma + 1), data))
                {
                    return Fail("a buffer's data URI cannot be read");
                }
            }
            else
            {
                const std::string path = (fs::path(m_Base) / DecodeUri(uri)).string();
                if (!ReadFile(path, data))
                {
                    return Fail("cannot read its buffer " + uri);
                }
                m_Out.sourceFiles.push_back(path);
            }
            if (data.size() < buffer.value("byteLength", size_t(0)))
            {
                return Fail("a buffer is shorter than the file says");
            }
            m_Buffers.push_back(std::move(data));
        }
        return true;
    }

    // Reads `count` floats per element from an accessor of float components.
    bool Floats(int accessorIndex, int components, std::vector<float>& out)
    {
        size_t count = 0, stride = 0;
        const unsigned char* src = nullptr;
        int type = 0;
        if (!Locate(accessorIndex, components, count, stride, src, type))
        {
            return false;
        }
        if (type != 5126)
        {
            return Fail("only float positions, normals and texture coordinates are supported");
        }
        out.resize(count * components);
        for (size_t i = 0; i < count; ++i)
        {
            std::memcpy(&out[i * components], src + i * stride, sizeof(float) * components);
        }
        return true;
    }

    bool Indices(int accessorIndex, std::vector<uint32_t>& out)
    {
        size_t count = 0, stride = 0;
        const unsigned char* src = nullptr;
        int type = 0;
        if (!Locate(accessorIndex, 1, count, stride, src, type))
        {
            return false;
        }
        out.resize(count);
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char* p = src + i * stride;
            if (type == 5121)
            {
                out[i] = *p;
            }
            else if (type == 5123)
            {
                uint16_t v;
                std::memcpy(&v, p, 2);
                out[i] = v;
            }
            else if (type == 5125)
            {
                std::memcpy(&out[i], p, 4);
            }
            else
            {
                return Fail("unknown index type");
            }
        }
        return true;
    }

    bool Fail(const std::string& message)
    {
        m_Error = message;
        return false;
    }

    const Bytes* BufferViewBytes(int view, size_t& offset, size_t& length) const
    {
        const json& v = m_Root.at("bufferViews").at(view);
        const size_t buffer = v.at("buffer").get<size_t>();
        offset = v.value("byteOffset", size_t(0));
        length = v.at("byteLength").get<size_t>();
        // Written to survive sizes that overflow: the file says what they are.
        if (buffer >= m_Buffers.size() || length > m_Buffers[buffer].size() ||
            offset > m_Buffers[buffer].size() - length)
        {
            return nullptr;
        }
        return &m_Buffers[buffer];
    }

private:
    bool Locate(int accessorIndex, int components, size_t& count, size_t& stride, const unsigned char*& src, int& type)
    {
        if (!m_Root.contains("accessors") || accessorIndex < 0 ||
            static_cast<size_t>(accessorIndex) >= m_Root["accessors"].size())
        {
            return Fail("an accessor index is out of range");
        }
        const json& a = m_Root["accessors"][accessorIndex];
        if (!a.contains("bufferView"))
        {
            return Fail("a sparse or empty accessor is not supported");
        }
        count = a.at("count").get<size_t>();
        type = a.at("componentType").get<int>();
        const int componentBytes = type == 5121 ? 1 : (type == 5123 ? 2 : 4);

        size_t viewOffset = 0, viewLength = 0;
        const int viewIndex = a["bufferView"].get<int>();
        const Bytes* buffer = BufferViewBytes(viewIndex, viewOffset, viewLength);
        if (!buffer)
        {
            return Fail("a buffer view runs past its buffer");
        }
        const size_t accessorOffset = a.value("byteOffset", size_t(0));
        const size_t elementBytes = static_cast<size_t>(components) * componentBytes;
        stride = m_Root.at("bufferViews").at(viewIndex).value("byteStride", size_t(0));
        if (stride == 0)
        {
            stride = elementBytes;
        }
        if (stride < elementBytes || accessorOffset > viewLength || count > viewLength / stride + 1 ||
            (count > 0 && accessorOffset + (count - 1) * stride + elementBytes > viewLength))
        {
            return Fail("an accessor runs past its buffer view");
        }
        src = buffer->data() + viewOffset + accessorOffset;
        return true;
    }

    const json& m_Root;
    std::string m_Base;
    const Bytes& m_GlbBin;
    GltfConversion& m_Out;
    std::string& m_Error;
    std::vector<Bytes> m_Buffers;
};

const char* ImageExtension(const std::string& mime)
{
    return mime == "image/jpeg" ? ".jpg" : ".png";
}

}  // namespace

bool ConvertGltfBytesToObj(const Bytes& bytes, const std::string& baseDirectory, const std::string& tempDirectory,
                           GltfConversion& out, std::string& error)
try
{
    out = {};
    json root;
    Bytes glbBin;

    if (bytes.size() >= 12 && ReadU32(bytes, 0) == kGlbMagic)
    {
        size_t at = 12;
        while (at + 8 <= bytes.size())
        {
            const uint32_t length = ReadU32(bytes, at);
            const uint32_t type = ReadU32(bytes, at + 4);
            if (at + 8 + static_cast<uint64_t>(length) > bytes.size())
            {
                error = "a chunk runs past the end of the file";
                return false;
            }
            if (type == kChunkJson)
            {
                root = json::parse(bytes.begin() + at + 8, bytes.begin() + at + 8 + length);
            }
            else if (type == kChunkBin)
            {
                glbBin.assign(bytes.begin() + at + 8, bytes.begin() + at + 8 + length);
            }
            at += 8 + static_cast<size_t>(length);
        }
    }
    else
    {
        root = json::parse(bytes.begin(), bytes.end());
    }

    if (!root.is_object() || !root.contains("meshes") || !root.contains("nodes"))
    {
        error = "it has no meshes";
        return false;
    }

    Reader reader(root, baseDirectory, glbBin, out, error);
    if (!reader.LoadBuffers())
    {
        return false;
    }

    std::error_code ec;
    fs::create_directories(tempDirectory, ec);

    // --- materials ----------------------------------------------------------
    // A texture points at an image, which is in a buffer or a file. The .mtl
    // names a file either way.
    auto imagePath = [&](int imageIndex) -> std::string
    {
        const json& image = root.at("images").at(imageIndex);
        const std::string uri = image.value("uri", std::string());
        if (!uri.empty() && uri.rfind("data:", 0) != 0)
        {
            const std::string path = (fs::path(baseDirectory) / DecodeUri(uri)).string();
            out.sourceFiles.push_back(path);
            return path;
        }

        Bytes data;
        std::string mime = image.value("mimeType", std::string("image/png"));
        if (!uri.empty())
        {
            const size_t comma = uri.find(',');
            if (comma == std::string::npos || !DecodeBase64(uri.substr(comma + 1), data))
            {
                return "";
            }
            if (uri.find("image/jpeg") != std::string::npos)
            {
                mime = "image/jpeg";
            }
        }
        else
        {
            size_t offset = 0, length = 0;
            const Bytes* buffer = reader.BufferViewBytes(image.at("bufferView").get<int>(), offset, length);
            if (!buffer)
            {
                return "";
            }
            data.assign(buffer->begin() + offset, buffer->begin() + offset + length);
        }
        const fs::path written =
            fs::path(tempDirectory) / ("image" + std::to_string(imageIndex) + ImageExtension(mime));
        std::ofstream file(written, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return written.generic_string();
    };

    std::ostringstream mtl;
    const size_t materialCount = root.contains("materials") ? root["materials"].size() : 0;
    for (size_t i = 0; i < materialCount; ++i)
    {
        const json& material = root["materials"][i];
        const json pbr = material.value("pbrMetallicRoughness", json::object());
        std::array<float, 4> colour{ 1, 1, 1, 1 };
        if (pbr.contains("baseColorFactor"))
        {
            colour = pbr["baseColorFactor"].get<std::array<float, 4>>();
        }
        mtl << "newmtl m" << i << "\nKd " << colour[0] << ' ' << colour[1] << ' ' << colour[2] << '\n';
        if (pbr.contains("baseColorTexture") && root.contains("textures"))
        {
            const int texture = pbr["baseColorTexture"].value("index", -1);
            if (texture >= 0 && static_cast<size_t>(texture) < root["textures"].size())
            {
                const int source = root["textures"][texture].value("source", -1);
                if (source >= 0 && root.contains("images") && static_cast<size_t>(source) < root["images"].size())
                {
                    const std::string path = imagePath(source);
                    if (!path.empty())
                    {
                        mtl << "map_Kd " << path << '\n';
                    }
                }
            }
        }
    }
    {
        std::ofstream file(fs::path(tempDirectory) / "model.mtl", std::ios::binary);
        file << mtl.str();
    }

    // --- geometry, with every node's transform baked in -------------------
    std::ostringstream obj;
    obj << "mtllib model.mtl\n";
    size_t positionBase = 1, uvBase = 1, normalBase = 1;
    bool any = false;

    auto emitPrimitive = [&](const json& primitive, const Mat4& world) -> bool
    {
        if (primitive.value("mode", 4) != 4)
        {
            return true;  // lines and points draw nothing here
        }
        const json& attributes = primitive.at("attributes");
        if (!attributes.contains("POSITION"))
        {
            return true;
        }

        std::vector<float> positions, normals, uvs;
        if (!reader.Floats(attributes["POSITION"].get<int>(), 3, positions))
        {
            return false;
        }
        if (attributes.contains("NORMAL") && !reader.Floats(attributes["NORMAL"].get<int>(), 3, normals))
        {
            return false;
        }
        if (attributes.contains("TEXCOORD_0") && !reader.Floats(attributes["TEXCOORD_0"].get<int>(), 2, uvs))
        {
            return false;
        }
        const size_t vertexCount = positions.size() / 3;

        std::vector<uint32_t> indices;
        if (primitive.contains("indices"))
        {
            if (!reader.Indices(primitive["indices"].get<int>(), indices))
            {
                return false;
            }
        }
        else
        {
            indices.resize(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i)
            {
                indices[i] = static_cast<uint32_t>(i);
            }
        }
        for (uint32_t index : indices)
        {
            if (index >= vertexCount)
            {
                return reader.Fail("an index points past its vertices");
            }
        }

        for (size_t i = 0; i < vertexCount; ++i)
        {
            const float x = positions[i * 3], y = positions[i * 3 + 1], z = positions[i * 3 + 2];
            obj << "v " << world[0] * x + world[4] * y + world[8] * z + world[12] << ' '
                << world[1] * x + world[5] * y + world[9] * z + world[13] << ' '
                << world[2] * x + world[6] * y + world[10] * z + world[14] << '\n';
        }
        for (size_t i = 0; i < normals.size() / 3; ++i)
        {
            const float x = normals[i * 3], y = normals[i * 3 + 1], z = normals[i * 3 + 2];
            float nx = world[0] * x + world[4] * y + world[8] * z;
            float ny = world[1] * x + world[5] * y + world[9] * z;
            float nz = world[2] * x + world[6] * y + world[10] * z;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length > 0.0f)
            {
                nx /= length;
                ny /= length;
                nz /= length;
            }
            obj << "vn " << nx << ' ' << ny << ' ' << nz << '\n';
        }
        for (size_t i = 0; i < uvs.size() / 2; ++i)
        {
            // glTF measures V from the top, .obj from the bottom.
            obj << "vt " << uvs[i * 2] << ' ' << 1.0f - uvs[i * 2 + 1] << '\n';
        }

        const int material = primitive.value("material", -1);
        obj << "usemtl " << (material >= 0 ? "m" + std::to_string(material) : std::string("default")) << '\n';
        const bool hasNormals = !normals.empty();
        const bool hasUvs = !uvs.empty();
        auto corner = [&](uint32_t index)
        {
            obj << ' ' << positionBase + index;
            if (hasUvs || hasNormals)
            {
                obj << '/';
                if (hasUvs)
                {
                    obj << uvBase + index;
                }
                if (hasNormals)
                {
                    obj << '/' << normalBase + index;
                }
            }
        };
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            obj << 'f';
            corner(indices[i]);
            corner(indices[i + 1]);
            corner(indices[i + 2]);
            obj << '\n';
        }

        positionBase += vertexCount;
        uvBase += uvs.size() / 2;
        normalBase += normals.size() / 3;
        any = any || !indices.empty();
        return true;
    };

    std::function<bool(int, const Mat4&, int)> visit = [&](int nodeIndex, const Mat4& parent, int depth) -> bool
    {
        if (depth > 256 || nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= root.at("nodes").size())
        {
            return reader.Fail("the node tree is invalid");
        }
        const json& node = root.at("nodes").at(nodeIndex);
        const Mat4 world = Multiply(parent, NodeMatrix(node));
        if (node.contains("mesh"))
        {
            const json& mesh = root.at("meshes").at(node["mesh"].get<int>());
            for (const json& primitive : mesh.at("primitives"))
            {
                if (!emitPrimitive(primitive, world))
                {
                    return false;
                }
            }
        }
        if (node.contains("children"))
        {
            for (const json& child : node["children"])
            {
                if (!visit(child.get<int>(), world, depth + 1))
                {
                    return false;
                }
            }
        }
        return true;
    };

    std::vector<int> roots;
    const int sceneIndex = root.value("scene", 0);
    if (root.contains("scenes") && static_cast<size_t>(sceneIndex) < root["scenes"].size())
    {
        roots = root["scenes"][sceneIndex].value("nodes", std::vector<int>());
    }
    else
    {
        // No scene: every node nobody names as a child.
        std::vector<bool> isChild(root["nodes"].size(), false);
        for (const json& node : root["nodes"])
        {
            if (node.contains("children"))
            {
                for (const json& child : node["children"])
                {
                    const size_t index = child.get<size_t>();
                    if (index < isChild.size())
                    {
                        isChild[index] = true;
                    }
                }
            }
        }
        for (size_t i = 0; i < isChild.size(); ++i)
        {
            if (!isChild[i])
            {
                roots.push_back(static_cast<int>(i));
            }
        }
    }

    for (int nodeIndex : roots)
    {
        if (!visit(nodeIndex, Identity(), 0))
        {
            return false;
        }
    }
    if (!any)
    {
        error = "no triangles found";
        return false;
    }

    out.objText = obj.str();
    return true;
}
catch (const std::exception& e)
{
    error = std::string("cannot read the glTF: ") + e.what();
    return false;
}

bool ConvertGltfToObj(const std::string& path, const std::string& tempDirectory, GltfConversion& out, std::string& error)
{
    Bytes bytes;
    if (!ReadFile(path, bytes))
    {
        error = "cannot be read";
        return false;
    }
    return ConvertGltfBytesToObj(bytes, fs::path(path).parent_path().string(), tempDirectory, out, error);
}

}  // namespace Deki3D

#endif  // DEKI_EDITOR
