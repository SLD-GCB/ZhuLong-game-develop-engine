#include "zlong/engine/scene_file.h"

#include "zlong/engine/file_io.h"
#include "zlong/engine/image_file.h"
#include "zlong/engine/model_file.h"
#include "zlong/engine/song_file.h"
#include "zlong/engine/synth.h"
#include "zlong/engine/wav_file.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
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

/// 波形的名字和号在 `synth.h` 里（`SynthWaveName` / `SynthWaveFromName`）—— 场景文本和曲子的
/// `.song` 文件都要它，所以只留一份。

/// 路径切分，给写出器用。
///
/// **不用 `std::filesystem`** —— 它在这个平台上遇到中文会抛（见 `file_io.h`），而这些字符串从头到尾
/// 都是 UTF-8 的字节，自己切几下就够了。
bool IsAbsolutePath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return true;
    }
    return path.size() >= 2 && path[1] == ':';
}

std::string DirOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string FileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string StemOf(const std::string& path) {
    std::string name = FileNameOf(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        name = name.substr(0, dot);
    }
    return name.empty() ? std::string("scene") : name;
}

/// `dir` 和 `name` 拼一条路径；`dir` 空或者 "." 就是当前的 `name`。
std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") {
        return name;
    }
    return dir + "/" + name;
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
            // sound tone FREQUENCY SECONDS | sound noise SECONDS SEED | sound wav PATH
            //
            // 前两种是**合成**出来的，像上面那些贴图：一个需要旁边有文件的场景，就不是"一个文件"了。
            // 第三种是**真的声音**（`wav_file.h`），而它需要旁边有文件 —— 那条路径一直到行尾，相对
            // 场景文件自己的目录，和 `texture file` / `model` 一条规矩。
            if (tokens.size() < 2) {
                return fail("sound needs a kind");
            }
            if (tokens[1] == "wav") {
                if (tokens.size() < 3) {
                    return fail("sound wav needs a path");
                }
                if (base_dir.empty()) {
                    return fail("this scene loads a wav, so it has to be read with LoadScene -- "
                                "that is the one that knows which directory the path is relative "
                                "to");
                }
                std::string file = tokens[2];
                for (std::size_t index = 3; index < tokens.size(); ++index) {
                    file += " " + tokens[index];
                }
                const bool absolute =
                    (!file.empty() && (file[0] == '/' || file[0] == '\\')) ||
                    (file.size() > 1 && file[1] == ':');
                audio::Clip clip;
                std::string reason;
                if (!LoadWav(absolute ? file : base_dir + "/" + file, clip, reason)) {
                    return fail("sound wav: " + reason);
                }
                out.sounds.push_back(std::move(clip));
                continue;
            }
            if (tokens[1] == "song") {
                // `sound song 主题.song` —— 一首曲子（`song_file.h`），渲染成一段。路径一直到行尾、
                // 相对场景文件自己的目录，和 `sound wav` / `model` 一条规矩。
                if (tokens.size() < 3) {
                    return fail("sound song needs a path");
                }
                if (base_dir.empty()) {
                    return fail("this scene loads a song, so it has to be read with LoadScene -- "
                                "that is the one that knows which directory the path is relative "
                                "to");
                }
                std::string file = tokens[2];
                for (std::size_t index = 3; index < tokens.size(); ++index) {
                    file += " " + tokens[index];
                }
                const bool absolute =
                    (!file.empty() && (file[0] == '/' || file[0] == '\\')) ||
                    (file.size() > 1 && file[1] == ':');
                SongSpec song;
                std::string reason;
                if (!LoadSong(absolute ? file : base_dir + "/" + file, song, reason)) {
                    return fail("sound song: " + reason);
                }
                audio::Clip clip = RenderSong(song);
                if (!clip.Valid()) {
                    return fail("sound song: 这首曲子里没有音符（或者一遍的长度不是正的）");
                }
                out.sounds.push_back(std::move(clip));
                continue;
            }
            if (tokens[1] == "synth") {
                // sound synth [wave NAME] [f0 F] [f1 F] [seconds F] [attack F] [decay F] [sustain F]
                //             [release F] [gain F] [cutoff F] [seed N]
                //
                // 关键字后面跟一个值 —— 和 `emitter` 的 `gain G`、`material` 的 `specular F` 同一种
                // 写法（不是 `key=value`）。**省掉的用默认值**，所以手写一行可以只写要动的几个数；
                // `SaveScene` 则总是把十一个全写出来。
                SynthSpec spec;
                std::size_t at = 2;
                while (at < tokens.size()) {
                    const std::string& key = tokens[at];
                    if (key == "wave") {
                        if (at + 1 >= tokens.size()) {
                            return fail("sound synth wave needs a name");
                        }
                        if (!SynthWaveFromName(tokens[at + 1], spec.wave)) {
                            return fail("unknown synth wave '" + tokens[at + 1] +
                                        "' (sine / square / saw / triangle / noise)");
                        }
                        at += 2;
                    } else if (key == "seed") {
                        long seed = 1;
                        if (at + 1 >= tokens.size() || !ToInt(tokens[at + 1], seed)) {
                            return fail("sound synth seed needs a whole number");
                        }
                        spec.seed = static_cast<std::uint32_t>(seed);
                        at += 2;
                    } else if (key == "f0" || key == "f1" || key == "seconds" || key == "attack" ||
                               key == "decay" || key == "sustain" || key == "release" ||
                               key == "gain" || key == "cutoff") {
                        float value = 0.0f;
                        if (at + 1 >= tokens.size() || !ToFloat(tokens[at + 1], value)) {
                            return fail("sound synth " + key + " needs a number");
                        }
                        if (key == "f0") {
                            spec.start_hz = value;
                        } else if (key == "f1") {
                            spec.end_hz = value;
                        } else if (key == "seconds") {
                            spec.seconds = value;
                        } else if (key == "attack") {
                            spec.attack = value;
                        } else if (key == "decay") {
                            spec.decay = value;
                        } else if (key == "sustain") {
                            spec.sustain = value;
                        } else if (key == "release") {
                            spec.release = value;
                        } else if (key == "gain") {
                            spec.gain = value;
                        } else {
                            spec.cutoff_hz = value;
                        }
                        at += 2;
                    } else {
                        return fail("sound synth wants wave / f0 / f1 / seconds / attack / decay / "
                                    "sustain / release / gain / cutoff / seed, not '" + key + "'");
                    }
                }
                if (!(spec.seconds > 0.0f)) {
                    return fail("sound synth needs a positive length");
                }
                if (!(spec.start_hz > 0.0f) || !(spec.end_hz > 0.0f)) {
                    return fail("sound synth needs positive frequencies");
                }
                if (!(spec.gain >= 0.0f)) {
                    return fail("sound synth gain cannot be negative");
                }
                if (!(spec.cutoff_hz >= 0.0f)) {
                    return fail("sound synth cutoff cannot be negative");
                }
                if (spec.sustain < 0.0f || spec.sustain > 1.0f) {
                    return fail("sound synth sustain is a level from 0 to 1");
                }
                out.sounds.push_back(RenderSynth(spec));
                continue;
            }
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
                } else if (tokens[at] == "global") {
                    // **不跟位置走**：满音量、居中。背景音乐就是这样（`Emitter::spatial`）。
                    emitter.spatial = false;
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
                    return fail("emitter wants 'gain <f>', 'reference <f>', 'max <f>', 'loop' or "
                                "'global'");
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

bool SaveScene(const std::string& path, const SceneDescription& description, std::string& error) {
    const std::string base = DirOf(path);
    const std::string stem = StemOf(path);

    std::vector<std::string> model_files;
    model_files.reserve(description.objects.size());

    for (std::size_t index = 0; index < description.objects.size(); ++index) {
        MeshDescription mesh = description.objects[index].mesh;   // 拷一份改路径，不动调用方那份
        const std::string& source_dir = description.objects[index].source_dir;

        // 贴图：解析成能开的路径 -> 拷到场景目录 -> 路径改成文件名。找不到就报错、不写（见头文件）。
        for (MeshTexture& texture : mesh.textures) {
            if (texture.path.empty()) {
                continue;   // 程序里造的图，没有文件可拷
            }
            const std::string from =
                IsAbsolutePath(texture.path) ? texture.path : JoinPath(source_dir, texture.path);
            const std::string name = FileNameOf(texture.path);
            const std::string to = JoinPath(base, name);
            if (from != to) {
                std::string bytes;
                if (!ReadWholeFile(from, bytes)) {
                    error = "texture not found: " + from;
                    return false;
                }
                if (!WriteWholeFile(to, bytes)) {
                    error = "could not copy texture to " + to;
                    return false;
                }
            }
            texture.path = name;
        }

        const std::string file = stem + "_" + std::to_string(index) + ".model";
        if (!SaveModel(JoinPath(base, file), mesh, error)) {
            return false;
        }
        model_files.push_back(file);
    }

    // 声音文件也要拷到场景目录旁边，和贴图一样 —— 一份场景要能整包搬走。要找的那个文件不在了就
    // 报错、不写（`sound wav` / `sound song` 写出去而没有那个文件，读回来就是个开不了的东西）。
    std::vector<std::string> sound_file_names(description.sounds.size());
    for (std::size_t index = 0; index < description.sounds.size(); ++index) {
        const SceneSoundInput& sound = description.sounds[index];
        if (sound.kind != SceneSoundInput::Kind::Wav && sound.kind != SceneSoundInput::Kind::Song) {
            continue;
        }
        if (sound.path.empty()) {
            error = "a scene sound points at a file but has no path";
            return false;
        }
        const std::string from =
            IsAbsolutePath(sound.path) ? sound.path : JoinPath(sound.source_dir, sound.path);
        const std::string name = FileNameOf(sound.path);
        const std::string to = JoinPath(base, name);
        if (from != to) {
            std::string bytes;
            if (!ReadWholeFile(from, bytes)) {
                error = "sound not found: " + from;
                return false;
            }
            if (!WriteWholeFile(to, bytes)) {
                error = "could not copy sound to " + to;
                return false;
            }
        }
        sound_file_names[index] = name;
    }

    // 场景文本。字段的写法照着 `ParseScene` 认的那几种 —— 相机是一行一个字段。
    std::string text;
    const auto add = [&text](const char* format, ...) {
        char buffer[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        text += buffer;
        text += '\n';
    };

    const Camera& camera = description.camera;
    add("camera eye %.9g %.9g %.9g", static_cast<double>(camera.eye.x),
        static_cast<double>(camera.eye.y), static_cast<double>(camera.eye.z));
    add("camera target %.9g %.9g %.9g", static_cast<double>(camera.target.x),
        static_cast<double>(camera.target.y), static_cast<double>(camera.target.z));
    add("camera up %.9g %.9g %.9g", static_cast<double>(camera.up.x),
        static_cast<double>(camera.up.y), static_cast<double>(camera.up.z));
    add("camera fov %.9g", static_cast<double>(camera.fov_y_radians * 180.0f / kPi));
    add("camera near %.9g", static_cast<double>(camera.near_z));
    add("camera far %.9g", static_cast<double>(camera.far_z));
    add("background %.9g %.9g %.9g %.9g", static_cast<double>(description.background[0]),
        static_cast<double>(description.background[1]), static_cast<double>(description.background[2]),
        static_cast<double>(description.background[3]));
    add("ambient %.9g %.9g %.9g", static_cast<double>(description.ambient.x),
        static_cast<double>(description.ambient.y), static_cast<double>(description.ambient.z));
    add("shadow %u", description.shadow_map_size);
    for (const Light& light : description.lights) {
        add("light %.9g %.9g %.9g %.9g %.9g %.9g", static_cast<double>(light.direction.x),
            static_cast<double>(light.direction.y), static_cast<double>(light.direction.z),
            static_cast<double>(light.colour.x), static_cast<double>(light.colour.y),
            static_cast<double>(light.colour.z));
    }
    for (std::size_t index = 0; index < model_files.size(); ++index) {
        const SceneObjectInput& object = description.objects[index];
        // 摆放：平移 + 均匀缩放（绕 Y 转写 0 —— 旋转已经烘进点的坐标里了，见头文件）。
        add("model %s %.9g %.9g %.9g 0 %.9g", model_files[index].c_str(),
            static_cast<double>(object.position.x), static_cast<double>(object.position.y),
            static_cast<double>(object.position.z), static_cast<double>(object.scale));
    }

    // 声音：先每一段自己，再摆它们的人，最后整场的音量。**顺序不能换** —— `ParseScene` 要求
    // `emitter` 指的那段声音已经读过了。
    for (std::size_t index = 0; index < description.sounds.size(); ++index) {
        const SceneSoundInput& sound = description.sounds[index];
        switch (sound.kind) {
        case SceneSoundInput::Kind::Tone:
            add("sound tone %.9g %.9g", static_cast<double>(sound.frequency),
                static_cast<double>(sound.seconds));
            break;
        case SceneSoundInput::Kind::Noise:
            add("sound noise %.9g %u", static_cast<double>(sound.seconds), sound.seed);
            break;
        case SceneSoundInput::Kind::Wav:
            add("sound wav %s", sound_file_names[index].c_str());
            break;
        case SceneSoundInput::Kind::Song:
            add("sound song %s", sound_file_names[index].c_str());
            break;
        case SceneSoundInput::Kind::Synth: {
            const SynthSpec& spec = sound.synth;
            add("sound synth wave %s f0 %.9g f1 %.9g seconds %.9g attack %.9g decay %.9g "
                "sustain %.9g release %.9g gain %.9g cutoff %.9g seed %u",
                SynthWaveName(spec.wave), static_cast<double>(spec.start_hz),
                static_cast<double>(spec.end_hz), static_cast<double>(spec.seconds),
                static_cast<double>(spec.attack), static_cast<double>(spec.decay),
                static_cast<double>(spec.sustain), static_cast<double>(spec.release),
                static_cast<double>(spec.gain), static_cast<double>(spec.cutoff_hz), spec.seed);
            break;
        }
        }
    }
    for (const SceneEmitterInput& emitter : description.emitters) {
        // `loop` 和 `global` 只在真是的时候写 —— 它们是开关，不跟数。
        add("emitter %d %d %.9g %.9g %.9g gain %.9g reference %.9g max %.9g%s%s",
            static_cast<int>(emitter.sound), static_cast<int>(emitter.parent),
            static_cast<double>(emitter.position.x), static_cast<double>(emitter.position.y),
            static_cast<double>(emitter.position.z), static_cast<double>(emitter.gain),
            static_cast<double>(emitter.reference_distance),
            static_cast<double>(emitter.max_distance), emitter.loop ? " loop" : "",
            emitter.spatial ? "" : " global");
    }
    add("master %.9g", static_cast<double>(description.master_gain));

    if (!WriteWholeFile(path, text)) {
        error = "could not write " + path;
        return false;
    }
    error.clear();
    return true;
}

}  // namespace zlong::engine
