#pragma once

#include <cstdint>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace shmfx {

/// Stable framework error codes returned by public APIs.
enum class ErrorCode : std::int32_t {
    /// Operation completed successfully.
    Ok = 0,
    /// Name does not match the shmfx naming contract.
    InvalidName = -1,
    /// Header magic, layout, size, or integrity check failed.
    CorruptedHeader = -2,
    /// Requested POSIX shared-memory object was not found.
    NotFound = -3,
    /// Create was requested with exclusive semantics but the object exists.
    AlreadyExists = -4,
    /// Kernel permissions or namespace policy denied access.
    PermissionDenied = -5,
    /// Segment or framework quota would be exceeded.
    QuotaExceeded = -6,
    /// Robust mutex reported a prior owner death and requires recovery.
    PriorOwnerDead = -7,
    /// Non-blocking ring push failed because the ring is full.
    RingFull = -8,
    /// Header major version is incompatible with this library.
    VersionMismatch = -9,
    /// Segment is already in the terminal DEAD state.
    SegmentDead = -10,
};

/// Checks whether an ErrorCode represents success.
///
/// @param code Error code returned by a shmfx API.
/// @return true when @p code is ErrorCode::Ok.
[[nodiscard]] constexpr bool is_ok(ErrorCode code) noexcept {
    return code == ErrorCode::Ok;
}

/// Returns the stable symbolic name for an ErrorCode.
///
/// @param code Error code returned by a shmfx API.
/// @return Symbolic error name suitable for logs and diagnostics.
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok:
        return "Ok";
    case ErrorCode::InvalidName:
        return "ErrInvalidName";
    case ErrorCode::CorruptedHeader:
        return "ErrCorruptedHeader";
    case ErrorCode::NotFound:
        return "ErrNotFound";
    case ErrorCode::AlreadyExists:
        return "ErrAlreadyExists";
    case ErrorCode::PermissionDenied:
        return "ErrPermissionDenied";
    case ErrorCode::QuotaExceeded:
        return "ErrQuotaExceeded";
    case ErrorCode::PriorOwnerDead:
        return "ErrPriorOwnerDead";
    case ErrorCode::RingFull:
        return "ErrRingFull";
    case ErrorCode::VersionMismatch:
        return "ErrVersionMismatch";
    case ErrorCode::SegmentDead:
        return "ErrSegmentDead";
    }
    return "ErrUnknown";
}

/// Minimal move-oriented result type used until the project adopts std::expected.
///
/// Result<T> stores either a T value or an ErrorCode. It is intentionally small
/// and non-throwing at the API boundary; callers must check has_value() before
/// value().
///
/// @tparam T Value type returned by a successful operation.
template <class T>
class Result {
public:
    /// Creates a successful Result from a value.
    ///
    /// @param value Successful result payload.
    Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : ok_(true), error_(ErrorCode::Ok) {
        new (&storage_) T(std::move(value));
    }

    /// Creates a failed Result from an error code.
    ///
    /// @param error Failure code. ErrorCode::Ok must not be used here.
    Result(ErrorCode error) noexcept : ok_(false), error_(error) {}

    /// Moves another Result into this Result.
    ///
    /// @param other Source Result.
    Result(Result&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : ok_(other.ok_), error_(other.error_) {
        if (ok_) {
            new (&storage_) T(std::move(other.value()));
            other.reset();
        }
    }

    /// Destroys the contained value when present.
    ~Result() {
        reset();
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    /// Reports whether this Result contains a value.
    ///
    /// @return true when the operation succeeded.
    [[nodiscard]] bool has_value() const noexcept {
        return ok_;
    }

    /// Reports whether this Result contains a value.
    ///
    /// @return true when the operation succeeded.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    /// Returns the stored error code.
    ///
    /// @return ErrorCode::Ok when this Result contains a value.
    [[nodiscard]] ErrorCode error() const noexcept {
        return ok_ ? ErrorCode::Ok : error_;
    }

    /// Returns the contained value.
    ///
    /// @return Mutable reference to the success payload.
    T& value() noexcept {
        return *reinterpret_cast<T*>(&storage_);
    }

    /// Returns the contained value.
    ///
    /// @return Immutable reference to the success payload.
    const T& value() const noexcept {
        return *reinterpret_cast<const T*>(&storage_);
    }

private:
    /// Clears the contained value if present.
    void reset() noexcept {
        if (ok_) {
            value().~T();
            ok_ = false;
        }
    }

    bool ok_;
    ErrorCode error_;
    alignas(T) unsigned char storage_[sizeof(T)];
};

/// Void specialization for APIs that only report success or failure.
template <>
class Result<void> {
public:
    /// Creates a successful void Result.
    Result() noexcept : error_(ErrorCode::Ok) {}

    /// Creates a failed void Result from an error code.
    ///
    /// @param error Failure code. ErrorCode::Ok should not be used here.
    Result(ErrorCode error) noexcept : error_(error) {}

    /// Reports whether this Result represents success.
    ///
    /// @return true when the operation succeeded.
    [[nodiscard]] bool has_value() const noexcept {
        return error_ == ErrorCode::Ok;
    }

    /// Reports whether this Result represents success.
    ///
    /// @return true when the operation succeeded.
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    /// Returns the stored error code.
    ///
    /// @return ErrorCode::Ok when the operation succeeded.
    [[nodiscard]] ErrorCode error() const noexcept {
        return error_;
    }

private:
    ErrorCode error_;
};

} // namespace shmfx
