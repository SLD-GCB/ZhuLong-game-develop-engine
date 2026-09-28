#include "zlong/engine/model_file.h"

#include "zlong/engine/file_io.h"
#include "zlong/engine/image_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace zlong::engine {

namespace {

/// 一行拆成词。空白分隔，`#` 起注释。
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

/// 一个 float 写成**读回来还是它**的样子。
///
/// 九位有效数字是 float 的保证往返数。默认那六位不够：`0.123456789` 存成 `0.123457`，读回来
/// 就不是原来那个数了 —— 而"存一次读一次，模型悄悄变了一点"是那种很久以后才会有人怀疑的事。
std::string Number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    return text;
}

}  // namespace

std::string WriteModel(const MeshDescription& description) {
    std::ostringstream out;
    out << "# 一个模型：焊好的点、指着它们的面，和面穿着的材质。\n";
    out << "# 格式在 引擎/include/zlong/engine/model_file.h。\n";
    out << "mesh 4\n";
    if (description.materials.empty()) {
        // **一个模型至少要写出一条 `material`**，哪怕手上这份一个都没声明。否则写出来的文件里
        // `f` 那一行会说"我穿材质 0"，而读的人一个材质都还没见到 —— 那份文件读不回来。
        out << "material 1 1 1 1 0.25\n";
    }
    for (const MeshMaterial& material : description.materials) {
        out << "material " << Number(material.tint[0]) << " " << Number(material.tint[1]) << " "
            << Number(material.tint[2]) << " " << Number(material.tint[3]) << " "
            << Number(material.specular);
        // 贴图的路径写在最后。**是空的就不写** —— 那是程序里造出来的像素，没有"从哪儿来"。
        // （`SaveModel` 会先拦住这种情况，不让它悄悄少写一样东西。）
        if (material.texture != kNoTexture && material.texture < description.textures.size() &&
            !description.textures[material.texture].path.empty()) {
            out << " " << description.textures[material.texture].path;
        }
        out << "\n";
    }
    for (const Vec3& point : description.positions) {
        out << "v " << Number(point.x) << " " << Number(point.y) << " " << Number(point.z) << "\n";
    }
    for (const Face& face : description.faces) {
        out << "f " << (face.smooth ? "smooth" : "flat") << " " << face.count << " " << face.material;
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            out << " " << face.corners[corner];
        }
        for (std::uint32_t corner = 0; corner < face.count; ++corner) {
            out << " " << Number(face.uv[corner][0]) << " " << Number(face.uv[corner][1]);
        }
        out << "\n";
    }
    // 硬边一行一条，低号在前。**排在点的后面**：读的时候角只能指已经读到的点，硬边同理，排在这
    // 儿就一定是安全的。
    for (const Crease& crease : description.creases) {
        out << "crease " << crease.low << " " << crease.high << "\n";
    }
    return out.str();
}

bool ParseModel(const std::string& text, MeshDescription& out, std::string& error,
                const std::string& base_dir) {
    out = MeshDescription{};
    std::istringstream stream(text);
    std::string line;
    int number = 0;
    bool saw_version = false;

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

        if (head == "mesh") {
            long version = 0;
            if (tokens.size() < 2 || !ToInt(tokens[1], version)) {
                return fail("mesh needs a version number");
            }
            if (version != 3 && version != 4) {
                return fail("this file is format " + std::to_string(version) +
                            ", and this build reads 3 and 4");
            }
            saw_version = true;
            continue;
        }

        if (head == "material") {
            // `material R G B A [高光]` —— 只有颜色和那个高光强度，见 `MeshMaterial`。
            if (tokens.size() < 5) {
                return fail("material needs R, G, B and A");
            }
            MeshMaterial material;
            if (!ToFloat(tokens[1], material.tint[0]) || !ToFloat(tokens[2], material.tint[1]) ||
                !ToFloat(tokens[3], material.tint[2]) || !ToFloat(tokens[4], material.tint[3])) {
                return fail("material has something in it that is not a number");
            }
            if (tokens.size() >= 6 && !ToFloat(tokens[5], material.specular)) {
                return fail("material's specular is not a number");
            }

            if (tokens.size() >= 7) {
                // 贴图在最后，而且**一直到行尾** —— 路径里可以有空格，一个词切不准。
                std::string picture = tokens[6];
                for (std::size_t index = 7; index < tokens.size(); ++index) {
                    picture += " " + tokens[index];
                }
                if (base_dir.empty()) {
                    return fail("this material samples \"" + picture +
                                "\", so the file has to be read with LoadModel -- that is the "
                                "one that knows which directory the path is relative to");
                }
                // 两个材质用同一张图是很常见的，所以先看有没有读过它 —— 读一遍就够，而且
                // `SplitByMaterial` 之后那份拷贝也能少一份。
                std::uint32_t shared = kNoTexture;
                for (std::size_t index = 0; index < out.textures.size(); ++index) {
                    if (out.textures[index].path == picture) {
                        shared = static_cast<std::uint32_t>(index);
                        break;
                    }
                }
                if (shared == kNoTexture) {
                    const bool absolute =
                        (!picture.empty() && (picture[0] == '/' || picture[0] == '\\')) ||
                        (picture.size() > 1 && picture[1] == ':');
                    Texture image;
                    std::string reason;
                    if (!LoadBmp(absolute ? picture : base_dir + "/" + picture, image, reason)) {
                        return fail("material's texture: " + reason);
                    }
                    shared = static_cast<std::uint32_t>(out.textures.size());
                    // **存的是文件里写的那条路径**（相对的），不是拼出来的那条 —— 存回去的时候要
                    // 原样写出来。
                    out.textures.push_back(MeshTexture{picture, std::move(image)});
                }
                material.texture = shared;
            }
            out.materials.push_back(material);
            continue;
        }

        if (head == "v") {
            if (tokens.size() < 4) {
                return fail("v needs three numbers");
            }
            Vec3 point{0.0f, 0.0f, 0.0f};
            if (!ToFloat(tokens[1], point.x) || !ToFloat(tokens[2], point.y) ||
                !ToFloat(tokens[3], point.z)) {
                return fail("v has something in it that is not a number");
            }
            out.positions.push_back(point);
            continue;
        }

        if (head == "f") {
            if (tokens.size() < 4) {
                return fail("f needs flat/smooth, how many corners, and which material");
            }
            Face face;
            if (tokens[1] == "smooth") {
                face.smooth = true;
            } else if (tokens[1] != "flat") {
                return fail("f says \"" + tokens[1] + "\"; it is flat or smooth");
            }

            long count = 0;
            if (!ToInt(tokens[2], count) || count < 3 || count > 4) {
                return fail("a face is three or four corners");
            }
            face.count = static_cast<std::uint32_t>(count);

            long material = 0;
            if (!ToInt(tokens[3], material) || material < 0) {
                return fail("a face's material is not a number");
            }
            if (material >= static_cast<long>(out.materials.size())) {
                return fail("this face wears material " + std::to_string(material) +
                            ", and only " + std::to_string(out.materials.size()) +
                            " have been declared so far (materials come first)");
            }
            face.material = static_cast<std::uint32_t>(material);

            const auto corners_at = 4;
            const auto uvs_at = corners_at + static_cast<std::size_t>(count);
            if (tokens.size() < uvs_at + static_cast<std::size_t>(count) * 2) {
                return fail("f needs " + std::to_string(count) + " corner numbers and " +
                            std::to_string(count) + " uvs after them");
            }

            for (long corner = 0; corner < count; ++corner) {
                long index = 0;
                if (!ToInt(tokens[corners_at + static_cast<std::size_t>(corner)], index) ||
                    index < 0) {
                    return fail("corner " + std::to_string(corner) + " is not a position number");
                }
                // 角只能指着**已经读到的**点，所以点要写在面前面。写出去的时候本来就是这么排的，
                // 而这条限制换来的是"第 12 行这个角是 99，前面只有 8 个点"这种能直接照着改的话。
                if (index >= static_cast<long>(out.positions.size())) {
                    return fail("corner " + std::to_string(corner) + " is position " +
                                std::to_string(index) + ", and only " +
                                std::to_string(out.positions.size()) +
                                " have been read so far (points come before faces)");
                }
                face.corners[corner] = static_cast<std::uint32_t>(index);
            }

            for (long corner = 0; corner < count; ++corner) {
                const std::size_t at = uvs_at + static_cast<std::size_t>(corner) * 2;
                if (!ToFloat(tokens[at], face.uv[corner][0]) ||
                    !ToFloat(tokens[at + 1], face.uv[corner][1])) {
                    return fail("corner " + std::to_string(corner) + " has a uv that is not a number");
                }
            }
            out.faces.push_back(face);
            continue;
        }

        if (head == "crease") {
            if (tokens.size() < 3) {
                return fail("crease needs two position numbers");
            }
            long low = 0;
            long high = 0;
            if (!ToInt(tokens[1], low) || !ToInt(tokens[2], high) || low < 0 || high < 0) {
                return fail("crease has something in it that is not a position number");
            }
            if (low == high) {
                return fail("a crease is an edge between two points, and this one names one point "
                            "twice");
            }
            // 同 `f`：硬边的两头也得是**已经读到的**点。写出去的时候排在点后面，所以本来就满足。
            const auto count = static_cast<long>(out.positions.size());
            if (low >= count || high >= count) {
                return fail("crease is between " + std::to_string(low) + " and " +
                            std::to_string(high) + ", and only " + std::to_string(count) +
                            " points have been read so far (points come before creases)");
            }
            // **低号在前**，这是 `Crease` 那份约定；文件里写反了不要紧，读的人扶正。
            out.creases.push_back(Crease{static_cast<std::uint32_t>(low < high ? low : high),
                                         static_cast<std::uint32_t>(low < high ? high : low)});
            continue;
        }

        return fail("I do not know the word \"" + head + "\"");
    }

    if (!saw_version) {
        error = "no \"mesh <version>\" line -- this does not look like a model at all";
        return false;
    }
    if (out.faces.empty()) {
        error = "there are no faces in it";
        return false;
    }
    // 一个材质都没声明的文件（手写的很常见）：给一个白的，于是"每个面都穿着点什么"这条成立。
    if (out.materials.empty()) {
        out.materials.push_back(MeshMaterial{});
    }
    // 硬边那张表**排好序、不重复** —— 这是 `MeshDescription::creases` 的约定，而文件是手改的，
    // 写重了、写乱了都是常事。这样下一个拿它去查的人（`Subdivide`）不用再自己防一手。
    std::sort(out.creases.begin(), out.creases.end());
    out.creases.erase(std::unique(out.creases.begin(), out.creases.end()), out.creases.end());
    error.clear();
    return true;
}

bool LoadModel(const std::string& path, MeshDescription& out, std::string& error) {
    std::string contents;
    if (!ReadWholeFile(path, contents)) {
        error = "could not read " + path;
        return false;
    }
    // 贴图的相对路径是相对**这个 .model 自己**的 —— 和场景里 `model` 那条指令同一个道理，于是
    // 一份模型和它的贴图可以整包一起挪走。
    const std::size_t slash = path.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    return ParseModel(contents, out, error, base);
}

bool SaveModel(const std::string& path, const MeshDescription& description, std::string& error) {
    // 先看一眼有没有"存不回去"的东西：一张不是从文件来的贴图没有路径可写，而**悄悄少写一样**比
    // 直接说不行坏得多（存一次贴图就没了，谁也不知道）。
    for (const MeshMaterial& material : description.materials) {
        if (material.texture == kNoTexture) {
            continue;
        }
        if (material.texture >= description.textures.size()) {
            error = "a material names texture " + std::to_string(material.texture) + ", and there "
                    "are only " + std::to_string(description.textures.size());
            return false;
        }
        if (description.textures[material.texture].path.empty()) {
            error = "a material samples a texture that did not come from a file, so there is no "
                    "path to write -- swap it for one that did";
            return false;
        }
    }
    if (!WriteWholeFile(path, WriteModel(description))) {
        error = "could not write " + path;
        return false;
    }
    error.clear();
    return true;
}

}  // namespace zlong::engine
