# 内核-ram

# `address_space.h`

`内核/RAM/include/zlong/ram/address_space.h`

```
烛龙 (ZhuLong) - building a stage-1 address space.

Mmu only walks page tables. This is the other half: it allocates tables,
writes descriptors and programs the translation regime, so a caller can map a
guest virtual range onto physical memory.

The descriptor format lives in mmu.h and the builders below use those same
constants, so the mapping side and the walk cannot disagree about bits[1:0].

Scope: 4 KB granule, TTBR0 (and TTBR1 pointed at the same root), one flat
address space. That is what running under HLE needs -- the guest's own EL1
kernel does not execute, so there is nobody to switch regimes.
```

```cpp
inline constexpr std::uint64_t kAf = kDescriptorAfBit;
inline constexpr std::uint64_t kUxn = kDescriptorUxnBit;
inline constexpr std::uint64_t kPxn = kDescriptorPxnBit;
// AP[2:1] live at bits[7:6]: bit 6 is read-only, bit 7 is EL0-accessible.
inline constexpr std::uint64_t Ap(bool el0_accessible, bool read_only) {
inline constexpr std::uint64_t RwEl0() { return kAf | Ap(true, false); }
inline constexpr std::uint64_t RoEl0() { return kAf | Ap(true, true); }
inline constexpr std::uint64_t RwEl1Only() { return kAf | Ap(false, false); }
inline constexpr std::uint64_t RoEl1Only() { return kAf | Ap(false, true); }
// A table descriptor, levels 0..2: bits[1:0] == 0b11.
inline constexpr std::uint64_t Table(std::uint64_t next) {
// A level-3 page: bits[1:0] == 0b11.
inline constexpr std::uint64_t Page(std::uint64_t pa, std::uint64_t attrs) {
// A block, levels 0..2 only: bits[1:0] == 0b01.
inline constexpr std::uint64_t Block(std::uint64_t pa, std::uint64_t attrs) {
class AddressSpace {
    explicit AddressSpace(Ram& ram);
    // Point TTBR0 and TTBR1 at a fresh level-0 table and turn the MMU on.
    // A t0sz of 16 gives a 48-bit VA, which covers everything DRAM sits at.
    bool enable(std::uint32_t t0sz = 16);
    bool map_page(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);
    bool map_block_2mb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);
    bool map_block_1gb(std::uint64_t va, std::uint64_t pa, std::uint64_t attrs);
    // Drop the level-3 page for `va`, leaving the tables in place. The entry is
    // zeroed, which the walk classifies as invalid -- so the format stays in one
    // place instead of being written out again here.
    bool unmap_page(std::uint64_t va);
    void store(std::uint64_t entry_pa, std::uint64_t descriptor);
```

---

# `mmu.h`

`内核/RAM/include/zlong/ram/mmu.h`

```
烛龙 (ZhuLong) - ARMv8-A stage-1 address translation.

Scope: 4 KB granule, 4-level walk, TTBR0/TTBR1 selected by TCR_EL1.T0SZ/T1SZ,
AP/AF/UXN/PXN permission checks, stage-1 only, no hardware access-flag
update, no contiguous hints.

Deliberately NOT using dynarmic's page_table/fastmem fast path: a non-null
page-table entry or fastmem-covered access becomes a bare host load/store
with no callback at all, and dynarmic exposes no write-watch or page-table-
miss hook. For guest RAM that would silently break self-modifying-code
detection (see 内核/cpu CodeWriteObserver). The software TLB below is the
performance mechanism instead.
```

```cpp
using zlong::cpu::MemoryFaultKind;
using zlong::cpu::VAddr;
// ---------------------------------------------------------------------------
// The descriptor format, written down once.
//
// `mmu.cpp` walks on these and the mapping side builds on them, so there is one
// definition of the format rather than two that can drift apart.
//
// ARMv8-A type bits:
// bits[1:0] == 0b11  ->  a Table at levels 0..2, and a Page at level 3
// bits[1:0] == 0b01  ->  a Block, which exists only at levels 0..2
// otherwise          ->  not a valid descriptor
//
// Reading "bit 1 set means leaf" is the tempting mistake, and it is silent: L3
// pages still work (0b11 is a page there), while tables and blocks simply swap
// meanings -- so a real guest page table walks into garbage.
// ---------------------------------------------------------------------------
inline constexpr std::uint64_t kDescriptorValidBit = 1ULL << 0;
inline constexpr std::uint64_t kDescriptorTypeBit = 1ULL << 1;
inline constexpr std::uint64_t kDescriptorAfBit = 1ULL << 10;
inline constexpr std::uint64_t kDescriptorPxnBit = 1ULL << 53;
inline constexpr std::uint64_t kDescriptorUxnBit = 1ULL << 54;
inline constexpr std::uint64_t kDescriptorAddressMask = 0x0000'FFFF'FFFF'F000ULL;
enum class DescriptorType : std::uint8_t {
constexpr DescriptorType ClassifyDescriptor(std::uint64_t raw) noexcept {
struct TranslateResult {
class Mmu {
    static constexpr std::size_t kPageShift = 12;
    static constexpr std::uint64_t kPageSize = 1ULL << kPageShift;
    static constexpr std::uint64_t kPageMask = kPageSize - 1;
    // Translate a guest virtual address. `el0` selects the privilege level used
    // for permission checks.
    TranslateResult translate(VAddr va, bool is_write, bool is_fetch, bool el0);
    // Drop every cached translation. Called when the guest writes a register
    // that affects translation, executes TLBI, or modifies page-table memory.
    void invalidate_all() noexcept { generation_.fetch_add(1, std::memory_order_acq_rel); }
    // True when this 4 KB page has been visited as a translation table during a
    // walk. A write to such a page may have changed permissions anywhere, so the
    // caller flushes everything.
    bool is_walked_page(std::uint64_t pa) const noexcept;
    // Convenience for tests/kernel: program a translation regime.
    void set_translation(std::uint64_t ttbr0, std::uint64_t tcr, std::uint64_t sctlr) noexcept;
    struct Descriptor {
    TranslateResult walk(VAddr va, bool is_write, bool is_fetch, bool el0, Descriptor& out);
    bool check_permissions(const Descriptor& desc, bool is_write, bool is_fetch, bool el0,
                           TranslateResult& result) const;
    void mark_walked_page(std::uint64_t pa, std::size_t n) noexcept;
```

---

# `physical.h`

`内核/RAM/include/zlong/ram/physical.h`

```
烛龙 (ZhuLong) - guest physical address space.

Physical memory is deliberately dumb: a list of regions, each RAM region
backed by host memory, plus a bump allocator. Address translation lives one
level up, in Mmu.
```

```cpp
enum class RegionKind : std::uint8_t {
struct PhysicalRegion {
// NVIDIA Tegra X1 DRAM base.
inline constexpr std::uint64_t kTegraDramBase = 0x8000'0000ULL;
// Switch main memory: 4 GiB LPDDR4.
inline constexpr std::uint64_t kDefaultDramSize = 4ULL * 1024 * 1024 * 1024;
// Alignment of the host backing store for guest RAM. Vulkan's
// VK_EXT_external_memory_host requires an imported host pointer to satisfy
// VkPhysicalDeviceExternalMemoryHostPropertiesEXT::minImportedHostPointerAlignment
// (4 KiB class on NVIDIA), which a plain std::vector does not guarantee. We
// over-align so any sane requirement is already met.
inline constexpr std::size_t kHostAlignment = 64 * 1024;
// Frees a block obtained from AllocateAligned.
struct AlignedFree {
    void operator()(std::uint8_t* pointer) const noexcept;
using AlignedBuffer = std::unique_ptr<std::uint8_t, AlignedFree>;
// Allocate `size` zero-filled bytes aligned to at least `alignment`.
AlignedBuffer AllocateAligned(std::size_t size, std::size_t alignment);
class PhysicalMemory {
    explicit PhysicalMemory(std::vector<PhysicalRegion> regions);
    const std::vector<PhysicalRegion>& regions() const noexcept { return regions_; }
    // True when [pa, pa + n) lies entirely inside a RAM region.
    bool mapped(std::uint64_t pa, std::size_t n) const noexcept;
    bool is_executable(std::uint64_t pa) const noexcept;
    bool read(std::uint64_t pa, void* dst, std::size_t n) const;
    bool write(std::uint64_t pa, const void* src, std::size_t n);
    bool read64(std::uint64_t pa, std::uint64_t& out) const;
    bool write64(std::uint64_t pa, std::uint64_t value);
    const std::uint8_t* host_pointer(std::uint64_t pa) const noexcept;
    // The alignment guaranteed for every host_pointer() result. The GPU layer
    // needs this to decide whether a direct host import is possible.
    static constexpr std::size_t host_alignment() noexcept { return kHostAlignment; }
    struct Backing {
    Backing* find(std::uint64_t pa) noexcept;
    const Backing* find(std::uint64_t pa) const noexcept;
```

---

# `ram.h`

`内核/RAM/include/zlong/ram/ram.h`

```
烛龙 (ZhuLong) - the RAM layer: guest physical memory plus a real MMU,
exposed to the CPU layer as a GuestMemory bus.

Addresses arriving here are guest VIRTUAL addresses (dynarmic does no
translation). Everything the four cores touch goes through this object.
```

```cpp
class Ram final : public zlong::cpu::GuestMemory {
    // No default argument on purpose: the default layout is 4 GiB and backing it
    // allocates eagerly, so callers state what they want.
    explicit Ram(std::vector<PhysicalRegion> regions);
    explicit Ram(std::uint64_t dram_size);
    PhysicalMemory& physical() noexcept { return physical_; }
    Mmu& mmu() noexcept { return mmu_; }
    SystemRegisters& registers() noexcept { return registers_; }
    // Privilege used for permission checks on this host thread. The kernel
    // calls this when it enters an exception level; default is EL0, which is
    // where guest application code runs.
    void set_current_el(std::uint32_t el) noexcept;
    bool TryWrite8(VAddr address, std::uint8_t value) override;
    bool TryWrite16(VAddr address, std::uint16_t value) override;
    bool TryWrite32(VAddr address, std::uint32_t value) override;
    bool TryWrite64(VAddr address, std::uint64_t value) override;
    bool TryWrite128(VAddr address, zlong::cpu::Vector value) override;
    zlong::cpu::FaultInfo DescribeFault(VAddr address, bool is_write, bool is_fetch) const override;
    zlong::cpu::SystemInsnResult HandleSystemInstruction(std::size_t core, VAddr pc,
                                                         std::uint32_t instruction,
                                                         std::uint64_t rt_value) override;
    template <typename T>
    std::optional<T> read(VAddr address);
    template <typename T>
    bool write(VAddr address, T value);
    bool write(VAddr address, T value);
    bool write_system_register(std::uint32_t key, std::uint64_t value);
    void record_fault(VAddr va, bool is_write, bool is_fetch, const TranslateResult& result);
    bool el0() const noexcept { return current_el() == 0; }
```

---

# `registers.h`

`内核/RAM/include/zlong/ram/registers.h`

```
烛龙 (ZhuLong) - system registers that dynarmic's A64 frontend does not
implement.

dynarmic implements MRS/MSR for only nine registers (CNTFRQ_EL0, CNTPCT_EL0,
CTR_EL0, DCZID_EL0, FPCR, FPSR, NZCV, TPIDR_EL0, TPIDRRO_EL0). Everything
else lands in UserCallbacks::InterpreterFallback, which is where the memory
layer picks these up. The encodings below were verified by assembling the
corresponding mnemonics.

Scope note: only registers that belong to *memory management* live here. The
exception-state registers (ESR_EL1, FAR_EL1, VBAR_EL1, ELR_EL1, SPSR_EL1)
belong to the kernel's exception model and are deliberately left unhandled --
under HLE the guest's EL1 kernel code does not run.
```

```cpp
// Packed (op1, CRn, CRm, op2) key for a system register.
constexpr std::uint32_t SystemRegKey(std::uint32_t op1, std::uint32_t crn, std::uint32_t crm,
                                     std::uint32_t op2) noexcept {
class SystemRegisters {
    // Cortex-A57 MIDR_EL1: implementer 0x41 (ARM), variant 0, arch 0xF,
    // part 0xD07, revision 0.
    static constexpr std::uint64_t kMidrEl1 = 0x410F'D070ULL;
    void set_ttbr0_el1(std::uint64_t value) noexcept {
    void set_ttbr1_el1(std::uint64_t value) noexcept {
    void set_tcr_el1(std::uint64_t value) noexcept {
    void set_mair_el1(std::uint64_t value) noexcept {
    void set_sctlr_el1(std::uint64_t value) noexcept {
    void set_cpacr_el1(std::uint64_t value) noexcept {
    // SCTLR_EL1.M: when clear, translation is off and VA == PA.
    bool mmu_enabled() const noexcept {
```

---
