// 烛龙 (ZhuLong) - partition readers: turning a container partition into a tree.
//
// The mount layer does not know what is inside a partition, and does not want
// to. It keeps an ordered list of readers, takes the first that claims a
// partition, and the opaque reader is the floor -- so a partition is never
// silently dropped, and supporting another layout is adding a reader rather
// than touching this layer.

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "zlong/mount/tree.h"
#include "zlong/ssd/block_device.h"
#include "zlong/ssd/container.h"

namespace zlong::mount {

class PartitionReader {
public:
    virtual ~PartitionReader() = default;

    virtual const char* name() const noexcept = 0;

    /// True for the reader that accepts anything. It exists so a partition
    /// inside a recognised container is never dropped -- but a *source-level*
    /// expansion must not end on it, or an unrecognised source would look
    /// mounted when it is really one opaque blob.
    virtual bool is_floor() const noexcept { return false; }

    /// Cheap: read only what is needed to decide, never the whole partition.
    virtual bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const = 0;

    /// Expand the partition into a subtree. The returned tree keeps `device`
    /// alive, and its file nodes are ranges of it -- nothing is copied.
    virtual std::optional<ReadOnlyTree> Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                              const ssd::PartitionInfo& info,
                                              std::string& error) const = 0;
};

/// The floor: an unclaimed partition is just a byte range, so it becomes one
/// file named after the partition. Cannot fail, so it can always be last.
class OpaquePartition final : public PartitionReader {
public:
    const char* name() const noexcept override { return "opaque"; }
    bool is_floor() const noexcept override { return true; }
    bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const override;
    std::optional<ReadOnlyTree> Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                      const ssd::PartitionInfo& info,
                                      std::string& error) const override;
};

/// A partition that is itself a flat archive: its entries become the tree.
class FlatArchivePartition final : public PartitionReader {
public:
    const char* name() const noexcept override { return "flat-archive"; }
    bool Matches(const ssd::BlockDevice& device, const ssd::PartitionInfo& info) const override;
    std::optional<ReadOnlyTree> Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                      const ssd::PartitionInfo& info,
                                      std::string& error) const override;
};

/// An ordered set. First match wins.
class PartitionReaders {
public:
    void Add(std::shared_ptr<const PartitionReader> reader);

    std::size_t size() const noexcept { return readers_.size(); }
    std::vector<std::string> names() const;

    /// Which reader would take this partition, or nullptr. Diagnostics only --
    /// Expand runs the same choice again.
    const PartitionReader* Choose(const ssd::BlockDevice& device,
                                  const ssd::PartitionInfo& info) const;

    /// Expands with the first matching reader. Returns nullopt with the reason
    /// only when nothing matched at all -- a matching reader's own failure is
    /// reported through `error` too, so the two are not confused.
    std::optional<ReadOnlyTree> Expand(std::shared_ptr<const ssd::BlockDevice> device,
                                      const ssd::PartitionInfo& info, std::string& error) const;

    /// {flat-archive, opaque}: specific first, floor last.
    static PartitionReaders Default();

private:
    std::vector<std::shared_ptr<const PartitionReader>> readers_;
};

/// Expand a whole device into one flat file list.
///
/// The device's partitions are used when it parses as a container; otherwise the
/// whole device is treated as a single partition. Partitions are expanded by
/// `readers` and their trees flattened, prefixed with the partition id when
/// there is more than one -- so two partitions that happen to name a file the
/// same way cannot collide.
std::optional<std::vector<ReadOnlyTree::RangeFile>> ExpandDeviceToFiles(
    const std::shared_ptr<const ssd::BlockDevice>& device, const PartitionReaders& readers,
    std::string& error);

}  // namespace zlong::mount
