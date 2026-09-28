# 内核-mount

# `archive.h`

`内核/mount/include/zlong/mount/archive.h`

```
烛龙 (ZhuLong) - archive structure parsing.

Two shapes, one parser: a "flat" archive (one table of named entries, one
string table, then the data) and a "partitioned" one (same shape, but each
entry also carries a hash-region size and the entry is wider).

============================================================================
These layouts are THIS PROJECT'S MODEL of the formats. Every field position
lives in ArchiveLayout as data, and the parser below is written only in terms
of that struct, so correcting a layout is an edit to the constants rather than
a rewrite. The tests pin the model with independently written byte vectors --
they prove the parser is self-consistent, not that it matches the real thing.
============================================================================

Structural parsing only: an entry is a named byte range. What is inside it is
the next layer's business.
```

```cpp
// Marks a field an archive layout does not have.
inline constexpr std::uint32_t kNoField = 0xFFFF'FFFFu;
// Upper bound on entries, so a corrupt count cannot ask for a huge allocation
// before any range check has run.
inline constexpr std::uint32_t kMaxArchiveEntries = 1u << 16;
// Where an archive's fields live, relative to the archive's start.
struct ArchiveLayout {
// Header, one table of 0x18-byte entries, one string table, then the data.
inline constexpr ArchiveLayout kFlatArchive{"PFS0", 4, 8, 16, 0x18, 0, 8, 16, kNoField};
// Same shape with the hashed-region size per entry and 0x40-byte entries.
inline constexpr ArchiveLayout kPartitionedArchive{"HFS0", 4, 8, 16, 0x40, 0, 8, 16, 20};
enum class ArchiveError {
const char* ToString(ArchiveError error) noexcept;
struct ArchiveEntry {
struct Archive {
    const ArchiveEntry* Find(std::string_view name) const;
```

---

# `entries.h`

`内核/mount/include/zlong/mount/entries.h`

```
烛龙 (ZhuLong) - the entry store: the named byte strings a source publishes.

The store is deliberately inert. It loads a source into a table, keeps every
name and every value opaque, and answers lookups. It does not interpret a
name or a value; anything that consumes an entry is a different layer, and the
store has no opinion about it.

The one convention it does fix -- what "a family" is -- is documented on
FindIndexed and implemented in exactly one function, so it can be corrected in
one place if it turns out to be wrong.
```

```cpp
struct Entry {
// Outcome of an indexed lookup. Four distinct answers, because "this source has
// no such family" and "this source is missing this index" are different
// problems with different fixes, and collapsing them buries the diagnosis.
enum class LookupKind : std::uint8_t {
const char* ToString(LookupKind kind) noexcept;
struct Lookup {
    bool found() const noexcept { return kind == LookupKind::Found && entry != nullptr; }
class EntryStore {
    // Build from name/value pairs. A duplicate name is refused: letting the
    // last one silently win would hide a malformed source.
    static std::optional<EntryStore> FromEntries(std::vector<Entry> entries, std::string& error);
    // The text form a source may publish: one "<name> = <hex bytes>" per line.
    // '#' starts a comment, blank lines are ignored, and spaces inside the hex
    // are allowed. Anything else on a line is refused, not skipped -- a line
    // the reader did not understand must not turn into a silent gap.
    static std::optional<EntryStore> FromHexTable(std::span<const std::uint8_t> text,
                                                 std::string& error);
    // Exact name lookup; nullptr when absent.
    const Entry* Find(std::string_view name) const noexcept;
    // Family lookup.
    //
    // A family is the set of names of the form "<stem>_<two hex digits>" -- and
    // that is the entire convention. Nothing here guesses at which underscore
    // segment is a number, so a name whose last segment merely looks numeric
    // (`..._bf`) is never mistaken for an indexed member.
    Lookup FindIndexed(std::string_view stem, std::uint32_t index) const noexcept;
    bool empty() const noexcept { return entries_.empty(); }
    const std::vector<Entry>& entries() const noexcept { return entries_; }
```

---

# `environment.h`

`内核/mount/include/zlong/mount/environment.h`

```
烛龙 (ZhuLong) - the system environment: what a mounted source set amounts to.

Two inputs, two separate paths, neither guessed from a filename:
* a system source -- a host directory, or a host image file -- mounted
read-only at "/system";
* an entry source -- a host file holding the entry table.

This is the whole of stage one: after LoadEnvironment succeeds, the resources
are in place and nothing above has to know where they came from.
```

```cpp
// The mount name a system source lives at.
inline constexpr std::string_view kSystemMountName = "system";
struct SystemEnvironment {
    bool empty() const noexcept { return mounts.mount_count() == 0 && entries.empty(); }
struct EnvironmentConfig {
enum class EnvironmentError {
const char* ToString(EnvironmentError error) noexcept;
```

---

# `mount.h`

`内核/mount/include/zlong/mount/mount.h`

```
烛龙 (ZhuLong) - the mount table: names to read-only trees.

A mounted source is addressed as "<name>/<path inside the tree>". Mount names
are a single segment, so a path's first segment selects the tree and the rest
is resolved inside it. The table owns the trees, and each tree keeps its own
backing devices alive.
```

```cpp
// Why a mount or a lookup failed. Explicit, because "it did not work" is not a
// diagnosis -- a bad mount name and a missing mount are different problems.
enum class MountError {
const char* ToString(MountError error) noexcept;
class MountTable {
    bool Mount(std::string name, ReadOnlyTree tree, MountError* error = nullptr);
    bool Unmount(std::string_view name);
    bool mounted(std::string_view name) const;
    const ReadOnlyTree* Find(std::string_view name) const;
    struct Resolved {
    bool Exists(std::string_view path) const;
```

---

# `reader.h`

`内核/mount/include/zlong/mount/reader.h`

```
烛龙 (ZhuLong) - partition readers: turning a container partition into a tree.

The mount layer does not know what is inside a partition, and does not want
to. It keeps an ordered list of readers, takes the first that claims a
partition, and the opaque reader is the floor -- so a partition is never
silently dropped, and supporting another layout is adding a reader rather
than touching this layer.
```

```cpp
class PartitionReader {
    virtual ~PartitionReader() = default;
    virtual const char* name() const noexcept = 0;
    // True for the reader that accepts anything. It exists so a partition
    // inside a recognised container is never dropped -- but a *source-level*
    // expansion must not end on it, or an unrecognised source would look
    // mounted when it is really one opaque blob.
    virtual bool is_floor() const noexcept { return false; }
    // Cheap: read only what is needed to decide, never the whole partition.
    virtual bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const = 0;
    // Expand the partition into a subtree. The returned tree keeps `device`
    // alive, and its file nodes are ranges of it -- nothing is copied.
    virtual std::optional<ReadOnlyTree> Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                              const ssd::PartitionInfo& info,
                                              std::string& error) const = 0;
// The floor: an unclaimed partition is just a byte range, so it becomes one
// file named after the partition. Cannot fail, so it can always be last.
class OpaquePartition final : public PartitionReader {
    const char* name() const noexcept override { return "opaque"; }
    bool is_floor() const noexcept override { return true; }
    bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const override;
// A partition that is itself a flat archive: its entries become the tree.
class FlatArchivePartition final : public PartitionReader {
    const char* name() const noexcept override { return "flat-archive"; }
    bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const override;
// An ordered set. First match wins.
class PartitionReaders {
    void Add(std::shared_ptr<const PartitionReader> reader);
    // Which reader would take this partition, or nullptr. Diagnostics only --
    // Expand runs the same choice again.
    const PartitionReader* Choose(const ssd::BlockDevice& device,
                                  const ssd::PartitionInfo& info) const;
    // {flat-archive, opaque}: specific first, floor last.
    static PartitionReaders Default();
```

---

# `source.h`

`内核/mount/include/zlong/mount/source.h`

```
烛龙 (ZhuLong) - opening a source: a host directory, or a host image file.

Host paths are UTF-8. ssd::FileBlockDevice opens with a narrow fstream path,
which on Windows goes through the ANSI code page, so a path with non-ASCII
characters fails there; HostFileDevice below carries a
std::filesystem::path instead, which converts UTF-8 correctly.
```

```cpp
enum class SourceError {
const char* ToString(SourceError error) noexcept;
// A host file as a byte-addressable, **read-only** device.
class HostFileDevice final : public ssd::BlockDevice {
    static std::optional<HostFileDevice> Open(const std::string& utf8_path);
    bool ReadAt(std::uint64_t offset, void* dst, std::size_t n) const override;
    // Always false: a source is opened for reading.
    bool WriteAt(std::uint64_t offset, const void* src, std::size_t n) override;
    struct Impl;
// Whether the path is a directory. Used to tell the two source forms apart --
// the choice is structural, never guessed from a filename.
bool IsHostDirectory(const std::string& utf8_path, SourceError* error = nullptr);
```

---

# `tree.h`

`内核/mount/include/zlong/mount/tree.h`

```
烛龙 (ZhuLong) - the read-only tree: the mount layer's neutral data model.

A file is a byte range over a block device that the tree keeps alive, not a
copy: a tree over a large image costs only its node list plus the shared
device handle. Everything above this layer speaks in terms of this type -- a
mounted source IS a ReadOnlyTree.

This layer is host-side and runs before any guest does, so it depends on
nothing above zlong::ssd.
```

```cpp
// Why a tree operation failed. Explicit codes, because "it did not work" is
// not a diagnosis.
enum class TreeError {
const char* ToString(TreeError error) noexcept;
// One node. Directories own their children; a file points at a byte range of a
// device that must outlive it.
struct Node {
    enum class Kind : std::uint8_t { Directory, File };
Node MakeDirectory(std::string name, std::vector<Node> children);
Node MakeFile(std::string name, const ssd::BlockDevice* device, std::uint64_t offset,
              std::uint64_t size);
// A read-only view of a directory hierarchy.
//
// Children are sorted by name on construction, so iteration order is
// deterministic and lookup is a binary search.
class ReadOnlyTree {
    // A source whose whole contents are already in memory. Copies the bytes
    // (one device per file), so this is for tests and small sources -- an
    // image-backed tree built by an archive reader shares one device instead.
    struct MemoryFile {
    static std::optional<ReadOnlyTree> FromMemoryFiles(std::vector<MemoryFile> files,
                                                      std::string& error);
    // An explicit file list over one shared device -- what an archive reader
    // produces. No copying: each file is a range of `device`. `path` may
    // contain '/' to nest.
    struct RangeFile {
    static std::optional<ReadOnlyTree> FromRanges(std::shared_ptr<const ssd::BlockDevice> device,
                                                 std::vector<RangeFile> files,
                                                 std::string& error);
    // Read a host directory (UTF-8 path) into a tree, recursively.
    //
    // A host directory has no single device behind it, so each file is read
    // into memory. An image-backed tree built by an archive reader shares one
    // device instead and copies nothing.
    static std::optional<ReadOnlyTree> FromHostDirectory(const std::string& path,
                                                        std::string& error);
    const Node& root() const noexcept { return root_; }
    bool empty() const noexcept { return root_.children.empty(); }
    const Node* Find(std::string_view path, TreeError* error = nullptr) const;
    bool Exists(std::string_view path) const;
    const std::vector<std::shared_ptr<const ssd::BlockDevice>>& devices() const noexcept {
```

---

# `zip.h`

`内核/mount/include/zlong/mount/zip.h`

```
烛龙 (ZhuLong) - ZIP reading, for a source shipped as a zip.

Structural parsing only: the central directory is parsed to a list of named
byte ranges, and a stored entry is a range of the zip itself -- nothing is
copied. An entry that is actually compressed is refused by name and method
rather than skipped, because a silently missing entry is worse than an error.

============================================================================
SCOPE: this parses the central directory and the local headers. Stored entries
are supported (which is what a firmware package uses -- its payload is already
incompressible). A deflated entry is a named error, not a wrong answer; adding
inflate later is a change to ZipToRanges and nothing else.
============================================================================
```

```cpp
// The local file header is 30 bytes, before the name and extra fields.
inline constexpr std::size_t kZipLocalHeaderBytes = 30;
// The central directory header is 46 bytes, before the name and extra fields.
inline constexpr std::size_t kZipCentralHeaderBytes = 46;
inline constexpr std::uint32_t kZipLocalSignature = 0x04034b50;
inline constexpr std::uint32_t kZipCentralSignature = 0x02014b50;
inline constexpr std::uint32_t kZipEndSignature = 0x06054b50;
inline constexpr std::uint16_t kZipMethodStored = 0;
inline constexpr std::uint16_t kZipMethodDeflate = 8;
// Upper bound on entries, so a corrupt count cannot ask for a huge allocation.
inline constexpr std::uint32_t kMaxZipEntries = 1u << 16;
enum class ZipError {
const char* ToString(ZipError error) noexcept;
struct ZipEntry {
    bool stored() const noexcept { return method == kZipMethodStored; }
struct Zip {
    const ZipEntry* Find(std::string_view name) const;
// Whether the device starts with a local file header. Cheap enough to use as a
// source-form probe.
bool LooksLikeZip(const ssd::BlockDevice& device);
```

---
