#include "zlong/mount/entries.h"

#include <utility>

namespace zlong::mount {

namespace {

constexpr char kHexLower[] = "0123456789abcdef";
constexpr char kHexUpper[] = "0123456789ABCDEF";

bool IsHexDigit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int HexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

std::string_view Trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// True when `name` is "<stem>_<two hex digits>". This is the single place the
/// family convention lives.
bool IsFamilyMember(std::string_view name, std::string_view stem) noexcept {
    if (name.size() != stem.size() + 3 || stem.empty()) {
        return false;
    }
    if (name.compare(0, stem.size(), stem) != 0 || name[stem.size()] != '_') {
        return false;
    }
    return IsHexDigit(name[stem.size() + 1]) && IsHexDigit(name[stem.size() + 2]);
}

std::string TwoHexDigits(std::uint32_t value, bool upper) {
    const char* digits = upper ? kHexUpper : kHexLower;
    std::string out;
    out.push_back(digits[(value >> 4) & 0xF]);
    out.push_back(digits[value & 0xF]);
    return out;
}

}  // namespace

const char* ToString(LookupKind kind) noexcept {
    switch (kind) {
    case LookupKind::Found:
        return "found";
    case LookupKind::FamilyMissing:
        return "the source has no such family";
    case LookupKind::IndexMissing:
        return "the family exists but this index does not";
    case LookupKind::BadStem:
        return "the family name is empty or contains a NUL";
    }
    return "unknown";
}

std::optional<EntryStore> EntryStore::FromEntries(std::vector<Entry> entries, std::string& error) {
    EntryStore store;
    store.entries_ = std::move(entries);

    for (std::size_t index = 0; index < store.entries_.size(); ++index) {
        const std::string& name = store.entries_[index].name;
        if (name.empty()) {
            error = "entry " + std::to_string(index) + " has an empty name";
            return std::nullopt;
        }
        if (name.find('\0') != std::string::npos) {
            error = "entry \"" + name + "\" has a NUL in its name";
            return std::nullopt;
        }
        const auto [position, inserted] = store.by_name_.emplace(name, index);
        (void)position;
        if (!inserted) {
            error = "the source names \"" + name + "\" more than once";
            return std::nullopt;
        }
    }

    error.clear();
    return store;
}

std::optional<EntryStore> EntryStore::FromHexTable(std::span<const std::uint8_t> text,
                                                  std::string& error) {
    std::vector<Entry> entries;
    const std::string_view source(reinterpret_cast<const char*>(text.data()), text.size());

    std::size_t line_number = 0;
    std::size_t cursor = 0;
    while (cursor <= source.size()) {
        const std::size_t newline = source.find('\n', cursor);
        const std::size_t end = newline == std::string_view::npos ? source.size() : newline;
        std::string_view line = source.substr(cursor, end - cursor);
        ++line_number;
        cursor = end + 1;

        line = Trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            error = "line " + std::to_string(line_number) + " has no '='";
            return std::nullopt;
        }
        const std::string_view name = Trim(line.substr(0, equals));
        std::string_view hex = Trim(line.substr(equals + 1));

        if (name.empty()) {
            error = "line " + std::to_string(line_number) + " has an empty name";
            return std::nullopt;
        }
        if (name.find('\0') != std::string_view::npos ||
            name.find(' ') != std::string_view::npos ||
            name.find('\t') != std::string_view::npos) {
            error = "line " + std::to_string(line_number) + " has whitespace in the name";
            return std::nullopt;
        }

        std::string digits;
        digits.reserve(hex.size());
        for (const char c : hex) {
            if (c == ' ' || c == '\t') {
                continue;
            }
            if (!IsHexDigit(c)) {
                error = "line " + std::to_string(line_number) + " has a non-hex digit";
                return std::nullopt;
            }
            digits.push_back(c);
        }
        if (digits.size() % 2 != 0) {
            error = "line " + std::to_string(line_number) + " has an odd number of hex digits";
            return std::nullopt;
        }

        Entry entry;
        entry.name = std::string(name);
        entry.value.reserve(digits.size() / 2);
        for (std::size_t index = 0; index < digits.size(); index += 2) {
            const int high = HexValue(digits[index]);
            const int low = HexValue(digits[index + 1]);
            entry.value.push_back(static_cast<std::uint8_t>((high << 4) | low));
        }
        entries.push_back(std::move(entry));
    }

    return FromEntries(std::move(entries), error);
}

const Entry* EntryStore::Find(std::string_view name) const noexcept {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &entries_[it->second];
}

Lookup EntryStore::FindIndexed(std::string_view stem, std::uint32_t index) const noexcept {
    if (stem.empty() || stem.find('\0') != std::string_view::npos) {
        return Lookup{LookupKind::BadStem, nullptr};
    }
    // The published form is two hex digits, so an index that cannot be written
    // that way cannot be in the family.
    if (index > 0xFF) {
        return Lookup{LookupKind::IndexMissing, nullptr};
    }

    if (const Entry* exact = Find(std::string(stem) + "_" + TwoHexDigits(index, false));
        exact != nullptr) {
        return Lookup{LookupKind::Found, exact};
    }
    if (const Entry* upper = Find(std::string(stem) + "_" + TwoHexDigits(index, true));
        upper != nullptr) {
        return Lookup{LookupKind::Found, upper};
    }

    for (const Entry& entry : entries_) {
        if (IsFamilyMember(entry.name, stem)) {
            return Lookup{LookupKind::IndexMissing, nullptr};
        }
    }
    return Lookup{LookupKind::FamilyMissing, nullptr};
}

}  // namespace zlong::mount
