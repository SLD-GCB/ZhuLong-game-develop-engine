#include "zlong/engine/obj_file.h"

#include <array>
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

/// 一个 `f` 里的角，拆开之后的样子。下标已经**换算成 0 起**。
struct Corner {
    long position = -1;
    long uv = -1;              // -1 表示文件里没写这一段的 uv
};

enum class CornerResult {
    ok,
    not_a_number,              // 不是数，或者写了 0（OBJ 的下标从 1 数）
    out_of_range,              // 是个数，但指不到已经读到的点 / uv 上
};

/// 把 `a`、`a/b`、`a//c`、`a/b/c` 拆开，换算成 0 起的下标。
///
/// **负下标是"从已经读到的这一头倒数"** —— `-1` 是最后一个。所以换算只能发生在读到这一行的
/// 时候，也这就是 OBJ 要求点写在面前面的原因。
CornerResult ReadCorner(const std::string& token, long positions, long uvs, Corner& out) {
    const std::size_t first = token.find('/');
    const std::string position_text =
        first == std::string::npos ? token : token.substr(0, first);

    long position = 0;
    if (!ToInt(position_text, position) || position == 0) {
        return CornerResult::not_a_number;
    }
    if (position < 0) {
        position = positions + position + 1;
    }
    if (position <= 0 || position > positions) {
        return CornerResult::out_of_range;
    }
    out.position = position - 1;

    if (first == std::string::npos) {
        return CornerResult::ok;
    }
    const std::size_t second = token.find('/', first + 1);
    const std::string uv_text =
        token.substr(first + 1, second == std::string::npos ? std::string::npos
                                                            : second - first - 1);
    if (uv_text.empty()) {
        return CornerResult::ok;                 // `a//c`：有法线，没有 uv
    }
    long uv = 0;
    if (!ToInt(uv_text, uv) || uv == 0) {
        return CornerResult::not_a_number;
    }
    if (uv < 0) {
        uv = uvs + uv + 1;
    }
    if (uv <= 0 || uv > uvs) {
        return CornerResult::out_of_range;
    }
    out.uv = uv - 1;
    return CornerResult::ok;
}

}  // namespace

bool ParseObj(const std::string& text, MeshDescription& out, ImportReport& report,
              std::string& error) {
    out = MeshDescription{};
    report = ImportReport{};

    std::vector<Vec3> positions;
    std::vector<std::array<float, 2>> uvs;

    std::istringstream stream(text);
    std::string line;
    int number = 0;
    // OBJ 的默认是 `s off`。没有 `s` 的文件因此一律是平的 —— 见 `obj_file.h` 里为什么不去倒推。
    bool smooth = false;

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

        if (head == "v") {
            if (tokens.size() < 4) {
                return fail("v needs three numbers");
            }
            Vec3 point{0.0f, 0.0f, 0.0f};
            if (!ToFloat(tokens[1], point.x) || !ToFloat(tokens[2], point.y) ||
                !ToFloat(tokens[3], point.z)) {
                return fail("v has something in it that is not a number");
            }
            positions.push_back(point);
            continue;
        }

        if (head == "vt") {
            if (tokens.size() < 3) {
                return fail("vt needs two numbers");
            }
            std::array<float, 2> uv{0.0f, 0.0f};
            if (!ToFloat(tokens[1], uv[0]) || !ToFloat(tokens[2], uv[1])) {
                return fail("vt has something in it that is not a number");
            }
            uvs.push_back(uv);
            continue;
        }

        if (head == "vn") {
            // 读得出来，但没有地方放：这个引擎的法线是烘出来的。见 `obj_file.h`。
            continue;
        }

        if (head == "f") {
            if (tokens.size() < 4) {
                return fail("f needs at least three corners");
            }
            std::vector<Corner> corners;
            corners.reserve(tokens.size() - 1);
            for (std::size_t index = 1; index < tokens.size(); ++index) {
                Corner corner;
                switch (ReadCorner(tokens[index], static_cast<long>(positions.size()),
                                   static_cast<long>(uvs.size()), corner)) {
                case CornerResult::not_a_number:
                    return fail("the corner \"" + tokens[index] + "\" is not a point number");
                case CornerResult::out_of_range:
                    return fail("the corner \"" + tokens[index] + "\" points past the " +
                                std::to_string(positions.size()) + " points and " +
                                std::to_string(uvs.size()) + " uvs read so far");
                case CornerResult::ok:
                    break;
                }
                corners.push_back(corner);
            }
            ++report.faces_in_file;

            // 一个角从 `corners` 里取出来，装成模型的一份面角。
            const auto corner_at = [&](std::size_t index, std::uint32_t slot, Face& face) {
                face.corners[slot] = static_cast<std::uint32_t>(corners[index].position);
                const long uv = corners[index].uv;
                face.uv[slot][0] = uv >= 0 ? uvs[static_cast<std::size_t>(uv)][0] : 0.0f;
                face.uv[slot][1] = uv >= 0 ? uvs[static_cast<std::size_t>(uv)][1] : 0.0f;
            };

            if (corners.size() <= 4) {
                Face face;
                face.count = static_cast<std::uint32_t>(corners.size());
                face.smooth = smooth;
                for (std::uint32_t slot = 0; slot < face.count; ++slot) {
                    corner_at(slot, slot, face);
                }
                out.faces.push_back(face);
                if (smooth) {
                    ++report.faces_smooth;
                } else {
                    ++report.faces_flat;
                }
            } else {
                // 五个角以上。从**第一个角**扇开 —— 扇一个凹陷的多边形会切出细长的三角形，这是
                // 已知的代价；但这是别人的文件，形状留着比替它猜怎么切好。
                for (std::size_t index = 1; index + 1 < corners.size(); ++index) {
                    Face face;
                    face.count = 3;
                    face.smooth = smooth;
                    corner_at(0, 0, face);
                    corner_at(index, 1, face);
                    corner_at(index + 1, 2, face);
                    out.faces.push_back(face);
                }
                ++report.faces_fanned;
                if (smooth) {
                    report.faces_smooth += corners.size() - 2;
                } else {
                    report.faces_flat += corners.size() - 2;
                }
            }
            continue;
        }

        if (head == "s") {
            if (tokens.size() < 2) {
                return fail("s needs \"off\" or a smoothing group number");
            }
            long group = 0;
            smooth = !(tokens[1] == "off" || (ToInt(tokens[1], group) && group == 0));
            continue;
        }

        // 剩下的都是**认得出但不属于一份"网格"的**（`o` / `g` 物体和组名、`usemtl` / `mtllib`
        // 材质），以及 OBJ 里那些画线画点的扩展。跳过并记一笔，而不是拒绝整个文件 —— 别人的导出
        // 器多写一行，不该让整份文件读不进来。
        ++report.lines_ignored;
    }

    if (out.faces.empty()) {
        error = "there are no faces in it";
        return false;
    }

    // 文件里有、但没有任何面用到的点：**丢掉**。
    //
    // 留着它们不叫忠实，叫留了一群**看不见、却点得到**的点 —— 顶点拾取是按屏幕距离找的，那些点
    // 投影在哪儿只有上帝知道。所以重排一遍下标，把丢掉的个数记在账上。
    std::vector<bool> used(positions.size(), false);
    for (const Face& face : out.faces) {
        for (std::uint32_t slot = 0; slot < face.count; ++slot) {
            used[face.corners[slot]] = true;
        }
    }
    std::vector<std::uint32_t> moved(positions.size(), 0);
    std::vector<Vec3> kept;
    kept.reserve(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        if (used[index]) {
            moved[index] = static_cast<std::uint32_t>(kept.size());
            kept.push_back(positions[index]);
        }
    }
    for (Face& face : out.faces) {
        for (std::uint32_t slot = 0; slot < face.count; ++slot) {
            face.corners[slot] = moved[face.corners[slot]];
        }
    }

    report.points_in_file = positions.size();
    report.points_kept = kept.size();
    report.points_dropped = positions.size() - kept.size();
    out.positions = std::move(kept);

    // **一个白的，占着 0 号。** `usemtl` 现在还是跳过的（见 `obj_file.h`），所以导进来的东西一律
    // 是这一个颜色 —— 但"每个面都穿着点什么"这条得成立，不然场景那边会拿到一个没有材质的模型。
    out.materials.push_back(MeshMaterial{});

    error.clear();
    return true;
}

bool LoadObj(const std::string& path, MeshDescription& out, ImportReport& report,
             std::string& error) {
    std::string contents;
    if (!ReadWholeFile(path, contents)) {
        error = "could not read " + path;
        return false;
    }
    return ParseObj(contents, out, report, error);
}

}  // namespace zlong::engine
