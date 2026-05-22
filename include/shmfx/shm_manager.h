#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_header.h"
#include "shmfx/shm_types.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace shmfx {

/// RAII view over a mapped shmfx segment.
class ShmHandle {
public:
    /// Creates an empty handle.
    ShmHandle() noexcept = default;

    /// Moves a mapped segment handle.
    ///
    /// @param other Source handle; left empty after the move.
    ShmHandle(ShmHandle&& other) noexcept;

    /// Releases the current mapping and moves another handle into this one.
    ///
    /// @param other Source handle; left empty after the move.
    /// @return This handle.
    ShmHandle& operator=(ShmHandle&& other) noexcept;

    ShmHandle(const ShmHandle&) = delete;
    ShmHandle& operator=(const ShmHandle&) = delete;

    /// Detaches the mapping and decrements RW ref_count when needed.
    ~ShmHandle();

    /// Returns whether this handle currently owns a mapping.
    ///
    /// @return true when mapped.
    [[nodiscard]] bool valid() const noexcept;

    /// Returns the mapped segment header.
    ///
    /// @return Mutable header reference.
    [[nodiscard]] ShmHeader& header() noexcept;

    /// Returns the mapped segment header.
    ///
    /// @return Immutable header reference.
    [[nodiscard]] const ShmHeader& header() const noexcept;

    /// Returns the user metadata region described by the header.
    ///
    /// @return Mutable byte span over metadata.
    [[nodiscard]] std::span<std::byte> metadata() noexcept;

    /// Returns the user metadata region described by the header.
    ///
    /// @return Immutable byte span over metadata.
    [[nodiscard]] std::span<const std::byte> metadata() const noexcept;

    /// Returns the payload region described by the header.
    ///
    /// @return Mutable byte span over payload.
    [[nodiscard]] std::span<std::byte> payload() noexcept;

    /// Returns the payload region described by the header.
    ///
    /// @return Immutable byte span over payload.
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;

    /// Returns the segment name copied into the header.
    ///
    /// @return POSIX shm object name.
    [[nodiscard]] std::string_view name() const noexcept;

    /// Returns the attach mode used by this handle.
    ///
    /// @return ReadOnly or ReadWrite.
    [[nodiscard]] AttachMode mode() const noexcept;

private:
    friend class ShmManager;
    friend class Registry;

    /// Creates a mapped handle from raw mmap state.
    ///
    /// @param base Mapped base address.
    /// @param size Mapped size in bytes.
    /// @param fd POSIX shm file descriptor.
    /// @param mode Attach mode for ref_count handling.
    /// @param owner Whether this handle created the segment.
    ShmHandle(void* base, std::size_t size, int fd, AttachMode mode, bool owner) noexcept;

    /// Releases the current mapping.
    void reset() noexcept;

    void* base_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
    AttachMode mode_ = AttachMode::ReadOnly;
    bool owner_ = false;
};

/// Factory for creating, attaching, and force-destroying shmfx segments.
class ShmManager {
public:
    /// Creates a new segment and publishes it to the registry.
    ///
    /// @param options Segment creation options.
    /// @return RW owner handle or an error code.
    [[nodiscard]] static Result<ShmHandle> create(const CreateOptions& options);

    /// Attaches to an existing segment by name.
    ///
    /// @param name POSIX shm object name.
    /// @param mode Mapping and ref_count mode.
    /// @return Segment handle or an error code.
    [[nodiscard]] static Result<ShmHandle> attach(std::string_view name,
                                                  AttachMode mode = AttachMode::ReadWrite);

    /// Force-unlinks a segment and removes it from the registry.
    ///
    /// @param name POSIX shm object name.
    /// @return Success or failure code.
    [[nodiscard]] static Result<void> destroy(std::string_view name);
};

} // namespace shmfx
