#include "zlong/engine/scene_file.h"

#include "zlong/engine/file_io.h"
#include "zlong/engine/image_file.h"
#include "zlong/engine/model_file.h"

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace zlong::engine {

namespace {

/// Split one line into tokens. Whitespace-separated; a token starting with '#'
/// begins a comment and ends the line.
std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        if (token[0] == '#') {
            break;
        }
        tokens.push_back(token);
    }
    return tokens;
}

bool ToFloat(const std::string& text, float& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtof(begin, &end);
    return end != begin && *end == '\0';
}

bool ToInt(const std::string& text, long& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtol(begin, &end, 10);
    return end != begin && *end == '\0';
}

/// Read `count` floats starting at `first`.
bool Floats(const std::vector<std::string>& tokens, std::size_t first, std::size_t count,
            float* out) {
    if (tokens.size() < first + count) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!ToFloat(tokens[first + index], out[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool ParseScene(const std::string& text, Scene& out, std::string& error,
                const std::string& base_dir) {
    std::istringstream stream(text);
    std::string line;
    int number = 0;
    while (std::getline(stream, line)) {
        ++number;
        const std::vector<std::string> tokens = Tokens(line);
        if (tokens.empty()) {
            continue;
        }
        const std::string head = tokens[0];
        const auto fail = [&](const std::string& what) {
            error = "line " + std::to_string(number) + ": " + what;
            return false;
        };

        if (head == "camera") {
            if (tokens.size() < 3) {
                return fail("camera needs a field and a value");
            }
            const std::string field = tokens[1];
            if (field == "eye" || field == "target" || field == "up") {
                float v[3];
                if (!Floats(tokens, 2, 3, v)) {
                    return fail("camera " + field + " needs three numbers");
                }
                const Vec3 value{v[0], v[1], v[2]};
                if (field == "eye") {
                    out.camera.eye = value;
                } else if (field == "target") {
                    out.camera.target = value;
                } else {
                    out.camera.up = value;
                }
            } else if (field == "fov" || field == "near" || field == "far") {
                float value = 0.0f;
                if (!ToFloat(tokens[2], value)) {
                    return fail("camera " + field + " needs a number");
                }
                if (field == "fov") {
                    out.camera.fov_y_radians = Radians(value);
                } else if (field == "near") {
                    out.camera.near_z = value;
                } else {
                    out.camera.far_z = value;
                }
            } else {
                return fail("unknown camera field '" + field + "'");
            }
        } else if (head == "background") {
            const std::size_t count = tokens.size() - 1;
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if ((count != 3 && count != 4) || !Floats(tokens, 1, count, v)) {
                return fail("background needs three or four numbers");
            }
            out.background = {v[0], v[1], v[2], v[3]};
        } else if (head == "light") {
            // light DIRECTION_X DIRECTION_Y DIRECTION_Z  R G B
            float v[6];
            if (!Floats(tokens, 1, 6, v)) {
                return fail("light needs a direction and a colour, six numbers");
            }
            Light light;
            light.direction = Vec3{v[0], v[1], v[2]};
            light.colour = Vec3{v[3], v[4], v[5]};
            out.lights.push_back(light);
        } else if (head == "ambient") {
            float v[3];
            if (!Floats(tokens, 1, 3, v)) {
                return fail("ambient needs three numbers");
            }
            out.ambient = Vec3{v[0], v[1], v[2]};
        } else if (head == "shadow") {
            long size = 0;
            if (tokens.size() < 2 || !ToInt(tokens[1], size) || size < 0) {
                return fail("shadow needs a map size, or 0 for none");
            }
            out.shadow_map_size = static_cast<std::uint32_t>(size);
        } else if (head == "mesh") {
            if (tokens.size() < 2) {
                return fail("mesh needs a kind");
            }
            if (tokens[1] == "box") {
                out.meshes.push_back(MakeBox());
            } else if (tokens[1] == "grid") {
                long divisions = 0;
                if (tokens.size() < 3 || !ToInt(tokens[2], divisions) || divisions < 1) {
                    return fail("mesh grid needs a positive division count");
                }
                out.meshes.push_back(MakeGrid(static_cast<int>(divisions)));
            } else {
                return fail("unknown mesh kind '" + tokens[1] + "'");
            }
        } else if (head == "texture") {
            if (tokens.size() < 2) {
                return fail("texture needs a kind");
            }
            if (tokens[1] == "file") {
                // `texture file 木纹.bmp` —— 一张**真的图**。这是 BMP 加载器进场景的那条路。
                if (tokens.size() < 3) {
                    return fail("texture file needs a path");
                }
                if (base_dir.empty()) {
                    return fail("this scene loads an image, so it has to be read with LoadScene "
                                "-- that is the one that knows which directory the path is "
                                "relative to");
                }
                // 路径一直到行尾：里面可以有空格。
                std::string picture = tokens[2];
                for (std::size_t index = 3; index < tokens.size(); ++index) {
                    picture += " " + tokens[index];
                }
                const bool absolute =
                    (!picture.empty() && (picture[0] == '/' || picture[0] == '\\')) ||
                    (picture.size() > 1 && picture[1] == ':');
                Texture image;
                std::string reason;
                if (!LoadBmp(absolute ? picture : base_dir + "/" + picture, image, reason)) {
                    return fail("texture file: " + reason);
                }
                out.textures.push_back(std::move(image));
                continue;
            }

            long size = 0;
            long cells = 0;
            if (tokens.size() < 4 || !ToInt(tokens[2], size) || !ToInt(tokens[3], cells) ||
                size < 1 || cells < 1) {
                return fail("texture needs a kind, a size and a cell count");
            }
            const auto extent = static_cast<std::uint32_t>(size);
            const auto count = static_cast<std::uint32_t>(cells);
            if (tokens[1] == "checker") {
                out.textures.push_back(MakeCheckerTexture(extent, count));
            } else if (tokens[1] == "normal") {
                out.textures.push_back(MakeRippleNormalTexture(extent, count));
            } else {
                return fail("unknown texture kind '" + tokens[1] + "'");
            }
        } else if (head == "material") {
            float tint[4];
            if (!Floats(tokens, 1, 4, tint)) {
                return fail("material needs a tint of four numbers");
            }
            Material material;
            material.tint = {tint[0], tint[1], tint[2], tint[3]};
            std::size_t at = 5;
            while (at < tokens.size()) {
                if (tokens[at] == "none") {
                    ++at;
                } else if (tokens[at] == "specular") {
                    if (at + 1 >= tokens.size() || !ToFloat(tokens[at + 1], material.specular)) {
                        return fail("material specular needs a number");
                    }
                    at += 2;
                } else if (tokens[at] == "texture") {
                    long index = 0;
                    if (at + 1 >= tokens.size() || !ToInt(tokens[at + 1], index) || index < 0 ||
                        static_cast<std::size_t>(index) >= out.textures.size()) {
                        return fail("material names a texture that is not there yet");
                    }
                    material.texture = static_cast<std::uint32_t>(index);
                    at += 2;
                } else if (tokens[at] == "normal") {
                    long index = 0;
                    if (at + 1 >= tokens.size() || !ToInt(tokens[at + 1], index) || index < 0 ||
                        static_cast<std::size_t>(index) >= out.textures.size()) {
                        return fail("material names a normal map that is not there yet");
                    }
                    material.normal_texture = static_cast<std::uint32_t>(index);
                    at += 2;
                } else {
                    return fail(
                        "material wants 'specular <f>', 'texture <index>', 'normal <index>' or "
                        "'none'");
                }
            }
            out.materials.push_back(material);
        } else if (head == "node") {
            long mesh = 0;
            long material = 0;
            long parent = kNoParent;
            if (tokens.size() < 4 || !ToInt(tokens[1], mesh) || !ToInt(tokens[2], material) ||
                !ToInt(tokens[3], parent)) {
                return fail("node needs mesh, material and parent indices");
            }
            float v[5];
            if (!Floats(tokens, 4, 5, v)) {
                return fail("node needs x, y, z, rotY and scale");
            }
            if (mesh < 0 || static_cast<std::size_t>(mesh) >= out.meshes.size()) {
                return fail("node names a mesh that is not there yet");
            }
            if (material < 0 || static_cast<std::size_t>(material) >= out.materials.size()) {
                return fail("node names a material that is not there yet");
            }
            if (parent < kNoParent ||
                (parent >= 0 && static_cast<std::size_t>(parent) >= out.nodes.size())) {
                return fail("node names a parent that does not come before it");
            }
            out.AddNode(static_cast<std::uint32_t>(mesh), static_cast<std::uint32_t>(material),
                        Translation(v[0], v[1], v[2]) * RotationY(Radians(v[3])) * Scale(v[4]),
                        static_cast<std::int32_t>(parent));
        } else if (head == "model") {
            // `model 椅子.model X Y Z ROTY SCALE` —— **一条指令就是一个摆好的东西。**
            //
            // 和 `mesh` 那族不一样：那几条约好一个网格，等人自己写 `node` 去摆；这一条自己就把
            // 节点摆好了。因为**一个模型有几种材质就是几个网格**（引擎那边一个节点一种材质），
            // 写文件的人没法知道是几个。
            if (tokens.size() < 7) {
                return fail("model needs a path, then x, y, z, rotY and scale");
            }
            if (base_dir.empty()) {
                return fail("this scene loads a model, so it has to be read with LoadScene -- "
                            "that is the one that knows which directory the path is relative to");
            }
            // **路径是"除了最后五个数之外的全部"**：最后五个永远是数，所以路径里有空格也不会被
            // 切错，而它不用引号。
            std::string path = tokens[1];
            for (std::size_t index = 2; index + 5 < tokens.size(); ++index) {
                path += " " + tokens[index];
            }
            float where[5];
            if (!Floats(tokens, tokens.size() - 5, 5, where)) {
                return fail("model needs x, y, z, rotY and scale after the path");
            }
            const bool absolute = (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
                                  (path.size() > 1 && path[1] == ':');
            const std::string full = absolute ? path : base_dir + "/" + path;

            MeshDescription description;
            std::string reason;
            if (!LoadModel(full, description, reason)) {
                return fail("model " + path + ": " + reason);
            }

            // **按材质拆开**：一个模型有几种颜色就是几个网格 + 几个节点，全都摆在同一处。
            // 拆的地方在这一层，管线和着色器一个字不用改 —— 见 `SplitByMaterial`。
            const Mat4 placed = Translation(where[0], where[1], where[2]) *
                                RotationY(Radians(where[3])) * Scale(where[4]);
            // 同一个图片文件不重复上传：两个材质用同一张图很常见。
            std::map<std::string, std::uint32_t> uploaded;
            for (const MaterialSplit& split : SplitByMaterial(description)) {
                Mesh mesh;
                if (!BakeMesh(split.description, mesh, reason)) {
                    return fail("model " + path + ": " + reason);
                }
                Material worn{split.material.tint, split.material.specular, kNoTexture, kNoTexture};
                if (split.material.texture != kNoTexture &&
                    split.material.texture < split.description.textures.size()) {
                    const MeshTexture& picture = split.description.textures[split.material.texture];
                    const auto seen = picture.path.empty() ? uploaded.end()
                                                           : uploaded.find(picture.path);
                    if (seen != uploaded.end()) {
                        worn.texture = seen->second;
                    } else {
                        worn.texture = static_cast<std::uint32_t>(out.textures.size());
                        out.textures.push_back(picture.image);
                        if (!picture.path.empty()) {
                            uploaded[picture.path] = worn.texture;
                        }
                    }
                }
                const auto index = static_cast<std::uint32_t>(out.meshes.size());
                out.meshes.push_back(std::move(mesh));
                const auto material = static_cast<std::uint32_t>(out.materials.size());
                out.materials.push_back(worn);
                out.AddNode(index, material, placed);
            }
        } else if (head == "group") {
            // A node that draws nothing: it exists to carry a transform its children
            // compose with, and an emitter parented to it is how a sound follows
            // something that moves.
            long parent = kNoParent;
            if (tokens.size() < 2 || !ToInt(tokens[1], parent)) {
                return fail("group needs a parent index");
            }
            float v[5];
            if (!Floats(tokens, 2, 5, v)) {
                return fail("group needs x, y, z, rotY and scale");
            }
            if (parent < kNoParent ||
                (parent >= 0 && static_cast<std::size_t>(parent) >= out.nodes.size())) {
                return fail("group names a parent that does not come before it");
            }
            out.AddNode(kNoMesh, 0,
                        Translation(v[0], v[1], v[2]) * RotationY(Radians(v[3])) * Scale(v[4]),
                        static_cast<std::int32_t>(parent));
        } else if (head == "sound") {
            // sound tone FREQUENCY SECONDS | sound noise SECONDS SEED
            //
            // Synthesised rather than loaded, like the textures above: a scene that
            // needs a file next to it is a scene that cannot be one file.
            if (tokens.size() < 4) {
                return fail("sound needs a kind and its two parameters");
            }
            if (tokens[1] == "tone") {
                float v[2];
                if (!Floats(tokens, 2, 2, v) || v[0] <= 0.0f || v[1] <= 0.0f) {
                    return fail("sound tone needs a frequency and a length, both positive");
                }
                out.sounds.push_back(MakeTone(v[0], v[1]));
            } else if (tokens[1] == "noise") {
                float length = 0.0f;
                long seed = 1;
                if (!ToFloat(tokens[2], length) || length <= 0.0f || !ToInt(tokens[3], seed)) {
                    return fail("sound noise needs a positive length and a seed");
                }
                out.sounds.push_back(MakeNoise(length, static_cast<std::uint32_t>(seed)));
            } else {
                return fail("unknown sound kind '" + tokens[1] + "'");
            }
        } else if (head == "emitter") {
            // emitter SOUND PARENT X Y Z [gain G] [reference R] [max M] [loop]
            long sound = 0;
            long parent = kNoParent;
            if (tokens.size() < 6 || !ToInt(tokens[1], sound) || !ToInt(tokens[2], parent)) {
                return fail("emitter needs a sound index, a parent and a position");
            }
            float v[3];
            if (!Floats(tokens, 3, 3, v)) {
                return fail("emitter needs x, y and z");
            }
            if (sound < 0 || static_cast<std::size_t>(sound) >= out.sounds.size()) {
                return fail("emitter names a sound that is not there yet");
            }
            if (parent < kNoParent ||
                (parent >= 0 && static_cast<std::size_t>(parent) >= out.nodes.size())) {
                return fail("emitter names a parent that does not come before it");
            }

            Emitter emitter;
            emitter.sound = static_cast<std::int32_t>(sound);
            emitter.parent = static_cast<std::int32_t>(parent);
            emitter.local = Translation(v[0], v[1], v[2]);
            std::size_t at = 6;
            while (at < tokens.size()) {
                if (tokens[at] == "loop") {
                    emitter.loop = true;
                    ++at;
                } else if (tokens[at] == "gain" || tokens[at] == "reference" ||
                           tokens[at] == "max") {
                    float value = 0.0f;
                    if (at + 1 >= tokens.size() || !ToFloat(tokens[at + 1], value)) {
                        return fail("emitter " + tokens[at] + " needs a number");
                    }
                    if (tokens[at] == "gain") {
                        emitter.gain = value;
                    } else if (tokens[at] == "reference") {
                        emitter.reference_distance = value;
                    } else {
                        emitter.max_distance = value;
                    }
                    at += 2;
                } else {
                    return fail("emitter wants 'gain <f>', 'reference <f>', 'max <f>' or 'loop'");
                }
            }
            out.emitters.push_back(emitter);
        } else if (head == "master") {
            float gain = 0.0f;
            if (tokens.size() < 2 || !ToFloat(tokens[1], gain) || gain < 0.0f) {
                return fail("master needs a gain of zero or more");
            }
            out.master_gain = gain;
        } else {
            return fail("unknown directive '" + head + "'");
        }
    }
    error.clear();
    return true;
}

bool LoadScene(const std::string& path, Scene& out, std::string& error) {
    // 走 `ReadWholeFile` 而不是直接 ifstream：Windows 上后者按 ANSI 代码页解释路径，中文路径会被
    // 读成别的字然后报"打不开"。这个项目和它的用户都在中文路径下干活。见 `file_io.h`。
    std::string contents;
    if (!ReadWholeFile(path, contents)) {
        error = "could not open " + path;
        return false;
    }
    // 场景里 `mesh file` 的相对路径，是相对**这个场景文件自己**所在的地方 —— 这样一份场景和它
    // 引用的模型可以整包一起挪走（那正是"整包挪到哪台机器上都对"那条规矩管的事）。
    const std::size_t slash = path.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return ParseScene(contents, out, error, base);
}

}  // namespace zlong::engine
