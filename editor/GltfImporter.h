#pragma once

#ifdef DEKI_EDITOR

#include <string>
#include <vector>

namespace Deki3D
{

/// A glTF 2.0 file (.gltf or .glb), as the Wavefront text ObjImporter compiles.
///
/// Going through .obj means a glTF model gets everything an .obj does (Detail
/// reduction, texture settings per target, the check that it fits a board) from
/// one compile path. What .obj cannot hold is dropped: animation and skinning.
///
/// Node transforms are baked into the vertices, so a scene of many nodes is one
/// mesh. Each glTF material becomes a material with its base colour and base
/// colour texture. A texture embedded in the file is written to `tempDirectory`,
/// which the caller deletes after compiling.
struct GltfConversion
{
    std::string objText;                   // names "model.mtl", in tempDirectory
    std::vector<std::string> sourceFiles;  // the .bin and images it reads
};

/// Reads the file at `path`. False with `error` set when it is not a glTF the
/// importer can read.
bool ConvertGltfToObj(const std::string& path, const std::string& tempDirectory, GltfConversion& out, std::string& error);

/// The same from the file's bytes; `baseDirectory` is where its relative URIs
/// point.
bool ConvertGltfBytesToObj(const std::vector<unsigned char>& bytes, const std::string& baseDirectory,
                           const std::string& tempDirectory, GltfConversion& out, std::string& error);

}  // namespace Deki3D

#endif  // DEKI_EDITOR
