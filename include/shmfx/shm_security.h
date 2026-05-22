#pragma once

#include "shmfx/shm_error.h"
#include "shmfx/shm_header.h"
#include "shmfx/shm_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace shmfx {

/// Parsed and validated shmfx POSIX shm name.
struct ParsedName {
    /// Namespace component between /shmfx. and the final dot.
    std::string_view ns;
    /// Object name component after the final dot.
    std::string_view leaf;
    /// Whether this is a framework bootstrap object outside the normal regex.
    bool bootstrap = false;
};

/// Validates a POSIX shm object name and extracts namespace components.
///
/// @param name Candidate POSIX shm object name.
/// @return ParsedName on success, otherwise InvalidName or PermissionDenied.
[[nodiscard]] Result<ParsedName> parse_and_validate_name(std::string_view name) noexcept;

/// Validates size, namespace, and quota for a create request.
///
/// @param options Create options supplied by the caller.
/// @param existing_entries Current registry snapshot used for quota checks.
/// @return Success or failure code.
[[nodiscard]] Result<void> validate_create_security(
    const CreateOptions& options,
    std::span<const RegistryEntry> existing_entries) noexcept;

/// Applies final POSIX permissions after a segment has been created.
///
/// @param fd POSIX shm file descriptor.
/// @param perm Desired final mode.
/// @return Success or permission failure.
[[nodiscard]] Result<void> apply_permissions(int fd, mode_t perm) noexcept;

/// Computes and stores HMAC-SHA256 for a header when HmacEnabled is set.
///
/// @param header Header whose immutable block is signed.
/// @return Success, or PermissionDenied when no usable key is configured.
[[nodiscard]] Result<void> sign_header_if_enabled(ShmHeader& header);

/// Verifies HMAC-SHA256 for a header when HmacEnabled is set or hmac[] is non-zero.
///
/// @param header Header to validate.
/// @return Success, CorruptedHeader on mismatch, or PermissionDenied when key is missing.
[[nodiscard]] Result<void> verify_header_hmac(const ShmHeader& header);

} // namespace shmfx
