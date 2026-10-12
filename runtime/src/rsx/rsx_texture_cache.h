#pragma once

// Guest texture bytes kept by the frame builder, so that a texture a game samples in thousands of draws is
// read from guest memory once instead of once per draw.
//
// Guest memory can change between draws, and the builder cannot watch it. An entry is therefore trusted only
// within an epoch, a stretch of the command stream in which the guest cannot have changed the texture behind
// the RSX's back: the builder starts a new epoch at every flip and every synchronization point, where the game
// learns how far the RSX has got before it reuses memory. Within an epoch an entry is used without touching
// guest memory. The first use in a later epoch reads the guest bytes and hashes them; an unchanged hash keeps
// the blob and its content id, a changed one stores a new blob under a new id. A write the RSX itself makes (a
// render target draw or clear, an inline blit word) ends the trust of the entries that overlap it at once,
// without a new epoch.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace dal {

// 64-bit hash of a byte range, 64 bytes per step with four independent multiply-fold lanes. Not a
// cryptographic hash: it only has to tell a changed texture from an unchanged one.
std::uint64_t rsxHashBytes(std::span<const std::uint8_t> bytes);

class RsxTextureCache {
public:
    // Everything that decides the copied bytes. `pitch` is the register value, before the 0 -> row fix-up;
    // `format` is the raw format byte (it carries the linear and unnormalized flags).
    struct Key {
        std::uint32_t location = 0, offset = 0, width = 0, height = 0, pitch = 0;
        std::uint8_t format = 0;
        bool operator==(const Key&) const = default;
    };

    struct Entry {
        std::shared_ptr<const std::vector<std::uint8_t>> data;
        std::uint64_t content_id = 0;
        std::uint64_t hash = 0;   // rsxHashBytes of the guest bytes `data` holds
        std::uint32_t pitch = 0;  // RsxTextureDesc::pitch to hand out (after the fix-up)
        // Bookkeeping, owned by the cache. [begin, end) = [key.offset, key.offset + data->size()) of `location`.
        std::uint32_t location = 0;
        std::uint64_t begin = 0, end = 0;
        std::uint64_t epoch = 0, serial = 0;  // epoch and write serial of the last validation
        std::uint64_t last_use = 0, last_flip = 0;
    };

    // Counters for the stop log; never reset.
    struct Stats {
        std::uint64_t epochs = 0;  // beginEpoch() calls, including those from onFlip and noteWrite overflow
        std::uint64_t hits = 0;    // uses served without reading guest memory
        std::uint64_t checks = 0;  // guest reads + hashes (first use in an epoch, or after an RSX write)
        std::uint64_t checked_bytes = 0;
        std::uint64_t stores = 0;  // new blobs (new content ids)
        std::uint64_t stored_bytes = 0;
    };

    // Default bounds: bytes of blobs kept, and flips an entry may go unused before it is dropped.
    static constexpr std::uint64_t kBudgetBytes = 256ull << 20;
    static constexpr std::uint64_t kIdleFlips = 600;

    explicit RsxTextureCache(std::uint64_t budget_bytes = kBudgetBytes);

    // The entry for `key`, valid or not (see fresh()); null when there is none. Counts as a use.
    Entry* find(const Key& key);
    // Whether the entry may be used without reading guest memory.
    bool fresh(const Entry& entry) const;
    // The entry has just been checked against the guest bytes: trust it until the epoch ends or a write hits it.
    void markFresh(Entry& entry);
    // Stores new content for `key` (creating the entry), names it with a new content id and marks it fresh.
    // Trims to the budget without dropping this entry.
    Entry& store(const Key& key, std::uint32_t pitch, std::uint64_t hash,
                 std::shared_ptr<const std::vector<std::uint8_t>> data);
    void erase(const Key& key);

    // The guest may have changed any texture since the last epoch began.
    void beginEpoch();
    // The RSX wrote [begin, end) of `location`.
    void noteWrite(std::uint32_t location, std::uint64_t begin, std::uint64_t end);
    // A flip ended: starts an epoch, and every 32 flips drops entries unused for kIdleFlips flips and entries
    // beyond the byte budget (least recently used first).
    void onFlip(std::uint64_t flip_index);

    void countHit() { stats_.hits++; }
    void countCheck(std::uint64_t bytes) {
        stats_.checks++;
        stats_.checked_bytes += bytes;
    }
    const Stats& stats() const { return stats_; }
    std::size_t entryCount() const { return entries_.size(); }
    std::uint64_t byteCount() const { return bytes_; }

    // A new id, unique in the process (builders share one counter, so a renderer fed by several never sees the
    // same id for different content).
    static std::uint64_t nextContentId();

private:
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };
    struct WriteRange {
        std::uint32_t location = 0;
        std::uint64_t begin = 0, end = 0, serial = 0;
    };
    // The write list is scanned for every texture use; past this many distinct ranges in one epoch a new
    // epoch is cheaper than the scan.
    static constexpr std::size_t kMaxWriteRanges = 64;

    // Drops least recently used entries other than `keep` until the blobs fit the budget.
    void trimBytes(const Entry* keep);
    static std::uint64_t entryBytes(const Entry& entry) { return entry.data ? entry.data->size() : 0; }

    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::uint64_t budget_bytes_;
    std::uint64_t bytes_ = 0;
    std::uint64_t epoch_ = 1, serial_ = 0, use_tick_ = 0, flip_ = 0;
    std::vector<WriteRange> writes_;
    Stats stats_;
};

}  // namespace dal
