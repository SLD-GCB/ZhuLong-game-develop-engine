#include "zlong/engine/song_file.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "zlong/engine/file_io.h"

namespace zlong::engine {

namespace {

/// Split one line into tokens. Whitespace-separated; a token starting with '#' begins a comment
/// and ends the line.
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

bool ToDouble(const std::string& text, double& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtod(begin, &end);
    return end != begin && *end == '\0';
}

bool ToInt(const std::string& text, long& out) {
    const char* begin = text.c_str();
    char* end = nullptr;
    out = std::strtol(begin, &end, 10);
    return end != begin && *end == '\0';
}

}  // namespace

bool ParseSong(const std::string& text, SongSpec& out, std::string& error) {
    std::istringstream stream(text);
    std::string line;
    int number = 0;
    bool header = false;
    SongSpec song;
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

        if (!header) {
            if (head != "song" || tokens.size() != 2 || tokens[1] != "1") {
                return fail("一份曲子第一行应当是 `song 1`");
            }
            header = true;
            continue;
        }

        if (head == "tempo") {
            double bpm = 0.0;
            if (tokens.size() < 2 || !ToDouble(tokens[1], bpm) || !(bpm > 0.0)) {
                return fail("tempo 要一个正的速度（BPM）");
            }
            song.tempo_bpm = bpm;
        } else if (head == "loop") {
            double beats = 0.0;
            if (tokens.size() < 2 || !ToDouble(tokens[1], beats) || !(beats > 0.0)) {
                return fail("loop 要一个正的拍数（一遍多长）");
            }
            song.loop_beats = beats;
        } else if (head == "instrument") {
            long index = -1;
            if (tokens.size() < 2 || !ToInt(tokens[1], index) ||
                index != static_cast<long>(song.instruments.size())) {
                return fail("instrument 要一个从 0 连着的编号");
            }
            Instrument instrument;
            std::size_t at = 2;
            while (at < tokens.size()) {
                const std::string& key = tokens[at];
                if (key == "wave") {
                    if (at + 1 >= tokens.size()) {
                        return fail("instrument wave 要一个名字");
                    }
                    if (!SynthWaveFromName(tokens[at + 1], instrument.wave)) {
                        return fail("不认识的波形 '" + tokens[at + 1] +
                                    "'（sine / square / saw / triangle / noise）");
                    }
                    at += 2;
                } else if (key == "seed") {
                    long seed = 1;
                    if (at + 1 >= tokens.size() || !ToInt(tokens[at + 1], seed)) {
                        return fail("instrument seed 要一个整数");
                    }
                    instrument.seed = static_cast<std::uint32_t>(seed);
                    at += 2;
                } else if (key == "attack" || key == "decay" || key == "sustain" ||
                           key == "release" || key == "gain" || key == "cutoff") {
                    float value = 0.0f;
                    if (at + 1 >= tokens.size() || !ToFloat(tokens[at + 1], value)) {
                        return fail("instrument " + key + " 要一个数");
                    }
                    if (key == "attack") {
                        instrument.attack = value;
                    } else if (key == "decay") {
                        instrument.decay = value;
                    } else if (key == "sustain") {
                        instrument.sustain = value;
                    } else if (key == "release") {
                        instrument.release = value;
                    } else if (key == "gain") {
                        instrument.gain = value;
                    } else {
                        instrument.cutoff_hz = value;
                    }
                    at += 2;
                } else {
                    return fail("instrument 只认 wave / attack / decay / sustain / release / gain / "
                                "cutoff / seed，不认 '" + key + "'");
                }
            }
            if (!(instrument.attack >= 0.0f) || !(instrument.decay >= 0.0f) ||
                !(instrument.release >= 0.0f)) {
                return fail("instrument 的起音/衰减/释音不能是负的");
            }
            if (instrument.sustain < 0.0f || instrument.sustain > 1.0f) {
                return fail("instrument 的持续电平是 0..1");
            }
            if (!(instrument.gain >= 0.0f) || !(instrument.cutoff_hz >= 0.0f)) {
                return fail("instrument 的增益和低通不能是负的");
            }
            song.instruments.push_back(instrument);
        } else if (head == "note") {
            double start = 0.0;
            double beats = 0.0;
            double velocity = 0.0;
            long pitch = 0;
            long instrument = 0;
            if (tokens.size() < 6 || !ToDouble(tokens[1], start) || !ToDouble(tokens[2], beats) ||
                !ToInt(tokens[3], pitch) || !ToDouble(tokens[4], velocity) ||
                !ToInt(tokens[5], instrument)) {
                return fail("note 要五个数：起点(拍) 长度(拍) 音高(MIDI) 力度 乐器");
            }
            if (start < 0.0) {
                return fail("note 的起点不能是负的");
            }
            if (!(beats > 0.0)) {
                return fail("note 的长度要正的");
            }
            if (pitch < 0 || pitch > 127) {
                return fail("音高是 0..127 的 MIDI 号");
            }
            if (!(velocity >= 0.0)) {
                return fail("note 的力度不能是负的");
            }
            if (instrument < 0 || static_cast<std::size_t>(instrument) >= song.instruments.size()) {
                return fail("note 指的乐器还没出现（乐器要写在音符前面）");
            }
            Note made;
            made.start_beats = start;
            made.beats = beats;
            made.pitch = static_cast<int>(pitch);
            made.velocity = static_cast<float>(velocity);
            made.instrument = static_cast<int>(instrument);
            song.notes.push_back(made);
        } else {
            return fail("不认识的指令 '" + head + "'");
        }
    }

    if (!header) {
        error = "这不是一份 .song（第一行应当是 `song 1`）";
        return false;
    }
    out = std::move(song);
    error.clear();
    return true;
}

bool LoadSong(const std::string& path, SongSpec& out, std::string& error) {
    std::string contents;
    if (!ReadWholeFile(path, contents)) {
        error = "读不了 " + path;
        return false;
    }
    if (!ParseSong(contents, out, error)) {
        error = path + "：" + error;
        return false;
    }
    return true;
}

bool SaveSong(const std::string& path, const SongSpec& spec, std::string& error) {
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

    text += "# 烛龙 (ZhuLong) - 一首曲子。第一行是版本，一行一条指令，`#` 起注释。\n";
    text += "# tempo 是速度（BPM）；loop 是一遍多少拍 —— 渲染出来的长度就是它。\n";
    text += "# instrument <编号> wave … attack … decay … sustain … release … gain … cutoff … seed …\n";
    text += "# note <起点拍> <长度拍> <音高 MIDI> <力度 0..1> <乐器编号>\n";
    add("song 1");
    add("tempo %.9g", spec.tempo_bpm);
    add("loop %.9g", spec.loop_beats);
    for (std::size_t index = 0; index < spec.instruments.size(); ++index) {
        const Instrument& instrument = spec.instruments[index];
        add("instrument %zu wave %s attack %.9g decay %.9g sustain %.9g release %.9g gain %.9g "
            "cutoff %.9g seed %u",
            index, SynthWaveName(instrument.wave), static_cast<double>(instrument.attack),
            static_cast<double>(instrument.decay), static_cast<double>(instrument.sustain),
            static_cast<double>(instrument.release), static_cast<double>(instrument.gain),
            static_cast<double>(instrument.cutoff_hz), instrument.seed);
    }
    for (const Note& note : spec.notes) {
        add("note %.9g %.9g %d %.9g %d", note.start_beats, note.beats, note.pitch,
            static_cast<double>(note.velocity), note.instrument);
    }

    if (!WriteWholeFile(path, text)) {
        error = "写不了 " + path;
        return false;
    }
    error.clear();
    return true;
}

}  // namespace zlong::engine
