// 烛龙 (ZhuLong) - the entry store: the named byte strings a source publishes.
//
// The store is deliberately inert. It loads a source into a table, keeps every
// name and every value opaque, and answers lookups. It does not interpret a
// name or a value; anything that consumes an entry is a different layer, and the
// store has no opinion about it.
//
// The one convention it does fix -- what "a family" is -- is documented on
// FindIndexed and implemented in exactly one function, so it can be corrected in
// one place if it turns out to be wrong.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zlong::mount {

struct Entry {
    std::string name;
    std::vector<std::uint8_t> value;
};

/// Outcome of an indexed lookup. Four distinct answers, because "this source has
/// no such family" and "this source is missing this index" are different
/// problems with different fixes, and collapsing them buries the diagnosis.
enum class LookupKind : std::uint8_t {
    Found,
    /// No member of that family exists at all.
    FamilyMissing,
    /// The family exists, but this index does not.
    IndexMissing,
    /// The stem cannot be used: empty, or containing a NUL.
    BadStem,
};

const char* ToString(LookupKind kind) noexcept;

struct Lookup {
    LookupKind kind = LookupKind::FamilyMissing;
    const Entry* entry = nullptr;

    bool found() const noexcept { return kind == LookupKind::Found && entry != nullptr; }
};

class EntryStore {
public:
    EntryStore() = default;

    /// Build from name/value pairs. A duplicate name is refused: letting the
    /// last one silently win would hide a malformed source.
    static std::optional<EntryStore> FromEntries(std::vector<Entry> entries, std::string& error);

    /// The text form a source may publish: one "<name> = <hex bytes>" per line.
    /// '#' starts a comment, blank lines are ignored, and spaces inside the hex
    /// are allowed. Anything else on a line is refused, not skipped -- a line
    /// the reader did not understand must not turn into a silent gap.
    static std::optional<EntryStore> FromHexTable(std::span<const std::uint8_t> text,
                                                 std::string& error);

    /// Exact name lookup; nullptr when absent.
    const Entry* Find(std::string_view name) const noexcept;

    /// Family lookup.
    ///
    /// A family is the set of names of the form "<stem>_<two hex digits>" -- and
    /// that is the entire convention. Nothing here guesses at which underscore
    /// segment is a number, so a name whose last segment merely looks numeric
    /// (`..._bf`) is never mistaken for an indexed member.
    Lookup FindIndexed(std::string_view stem, std::uint32_t index) const noexcept;

    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }
    const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
    std::map<std::string, std::size_t, std::less<>> by_name_;
};

}  // namespace zlong::mount
