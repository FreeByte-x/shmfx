#include "shmfx/shm_security.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace shmfx {
namespace {

constexpr std::array<std::string_view, 5> kAllowedNamespaces = {"log", "tlm", "kv", "app", "sys"};
constexpr std::uint32_t kMaxNamespaceSegments = 1024;
constexpr std::size_t kSha256BlockBytes = 64;

struct Sha256State {
    std::array<std::uint32_t, 8> h{};
    std::array<std::uint8_t, kSha256BlockBytes> block{};
    std::uint64_t total_bytes = 0;
    std::size_t block_used = 0;
};

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

/// Returns true when a name tail character is accepted by the shmfx regex.
bool is_name_tail_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// Returns true when a namespace is allowed for user-created objects.
bool namespace_allowed(std::string_view ns) noexcept {
    return std::find(kAllowedNamespaces.begin(), kAllowedNamespaces.end(), ns) !=
           kAllowedNamespaces.end();
}

/// Returns a stable string_view over a fixed ABI name buffer.
std::string_view fixed_name_view(const char (&name)[SHMFX_NAME_BYTES]) noexcept {
    return {name, strnlen(name, SHMFX_NAME_BYTES)};
}

/// Rotates a 32-bit word right by n bits.
constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
    return (x >> n) | (x << (32u - n));
}

/// Reads a big-endian u32 from bytes.
std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
           (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}

/// Stores a u32 as big-endian bytes.
void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24u);
    p[1] = static_cast<std::uint8_t>(v >> 16u);
    p[2] = static_cast<std::uint8_t>(v >> 8u);
    p[3] = static_cast<std::uint8_t>(v);
}

/// Initializes SHA-256 state.
void sha256_init(Sha256State& state) noexcept {
    state.h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    state.total_bytes = 0;
    state.block_used = 0;
}

/// Compresses one SHA-256 block.
void sha256_transform(Sha256State& state, const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3u);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10u);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state.h[0];
    std::uint32_t b = state.h[1];
    std::uint32_t c = state.h[2];
    std::uint32_t d = state.h[3];
    std::uint32_t e = state.h[4];
    std::uint32_t f = state.h[5];
    std::uint32_t g = state.h[6];
    std::uint32_t h = state.h[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
    state.h[5] += f;
    state.h[6] += g;
    state.h[7] += h;
}

/// Adds bytes to SHA-256 state.
void sha256_update(Sha256State& state, const std::uint8_t* data, std::size_t size) noexcept {
    state.total_bytes += size;
    while (size > 0) {
        const std::size_t take = std::min(size, kSha256BlockBytes - state.block_used);
        std::memcpy(state.block.data() + state.block_used, data, take);
        state.block_used += take;
        data += take;
        size -= take;
        if (state.block_used == kSha256BlockBytes) {
            sha256_transform(state, state.block.data());
            state.block_used = 0;
        }
    }
}

/// Finalizes SHA-256 into a 32-byte digest.
std::array<std::uint8_t, SHMFX_HMAC_BYTES> sha256_final(Sha256State& state) noexcept {
    const std::uint64_t bit_len = state.total_bytes * 8u;
    state.block[state.block_used++] = 0x80u;
    if (state.block_used > 56) {
        std::fill(state.block.begin() + state.block_used, state.block.end(), 0);
        sha256_transform(state, state.block.data());
        state.block_used = 0;
    }
    std::fill(state.block.begin() + state.block_used, state.block.begin() + 56, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        state.block[56 + i] = static_cast<std::uint8_t>(bit_len >> ((7 - i) * 8));
    }
    sha256_transform(state, state.block.data());

    std::array<std::uint8_t, SHMFX_HMAC_BYTES> digest{};
    for (std::size_t i = 0; i < state.h.size(); ++i) {
        store_be32(digest.data() + i * 4, state.h[i]);
    }
    return digest;
}

/// Computes SHA-256 digest for one contiguous buffer.
std::array<std::uint8_t, SHMFX_HMAC_BYTES> sha256(std::span<const std::uint8_t> data) noexcept {
    Sha256State state{};
    sha256_init(state);
    sha256_update(state, data.data(), data.size());
    return sha256_final(state);
}

/// Converts a hex nibble into its integer value.
int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/// Parses a 64-character hex key into 32 raw bytes.
bool parse_hex_key(std::string_view hex, std::array<std::uint8_t, SHMFX_HMAC_BYTES>& out) noexcept {
    if (hex.size() != SHMFX_HMAC_BYTES * 2) {
        return false;
    }
    for (std::size_t i = 0; i < SHMFX_HMAC_BYTES; ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

/// Loads HMAC key from environment or the production key file.
bool load_hmac_key(std::array<std::uint8_t, SHMFX_HMAC_BYTES>& out) {
    if (const char* env = std::getenv("SHMFX_HMAC_KEY"); env != nullptr) {
        return parse_hex_key(env, out);
    }

    std::ifstream file("/etc/shmfx/hmac.key");
    if (!file) {
        return false;
    }
    std::string hex;
    file >> hex;
    return parse_hex_key(hex, out);
}

/// Computes HMAC-SHA256 for a buffer with a 32-byte key.
std::array<std::uint8_t, SHMFX_HMAC_BYTES> hmac_sha256(
    const std::array<std::uint8_t, SHMFX_HMAC_BYTES>& key,
    std::span<const std::uint8_t> data) noexcept {
    std::array<std::uint8_t, kSha256BlockBytes> ipad{};
    std::array<std::uint8_t, kSha256BlockBytes> opad{};
    for (std::size_t i = 0; i < kSha256BlockBytes; ++i) {
        const std::uint8_t b = i < key.size() ? key[i] : 0;
        ipad[i] = static_cast<std::uint8_t>(b ^ 0x36u);
        opad[i] = static_cast<std::uint8_t>(b ^ 0x5cu);
    }

    Sha256State inner{};
    sha256_init(inner);
    sha256_update(inner, ipad.data(), ipad.size());
    sha256_update(inner, data.data(), data.size());
    const auto inner_digest = sha256_final(inner);

    Sha256State outer{};
    sha256_init(outer);
    sha256_update(outer, opad.data(), opad.size());
    sha256_update(outer, inner_digest.data(), inner_digest.size());
    return sha256_final(outer);
}

/// Returns true when hmac[] contains any non-zero byte.
bool has_hmac_bytes(const ShmHeader& header) noexcept {
    return std::any_of(std::begin(header.hmac), std::end(header.hmac),
                       [](std::uint8_t b) { return b != 0; });
}

/// Checks digest equality without early exit.
bool digest_equal(const std::uint8_t* lhs,
                  const std::array<std::uint8_t, SHMFX_HMAC_BYTES>& rhs) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < rhs.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | (lhs[i] ^ rhs[i]));
    }
    return diff == 0;
}

} // namespace

Result<ParsedName> parse_and_validate_name(std::string_view name) noexcept {
    if (name == REGISTRY_NAME || name == REGISTRY_LOCK_NAME) {
        return ParsedName{{}, name, true};
    }

    constexpr std::string_view prefix = "/shmfx.";
    if (name.size() >= SHMFX_NAME_BYTES || name.substr(0, prefix.size()) != prefix) {
        return ErrorCode::InvalidName;
    }

    const std::size_t dot = name.find('.', prefix.size());
    if (dot == std::string_view::npos) {
        return ErrorCode::InvalidName;
    }

    const std::string_view ns = name.substr(prefix.size(), dot - prefix.size());
    const std::string_view leaf = name.substr(dot + 1);
    if (ns.size() < 2 || ns.size() > 15 || leaf.empty() || leaf.size() > 39) {
        return ErrorCode::InvalidName;
    }
    if (ns.front() < 'a' || ns.front() > 'z' || leaf.front() < 'a' || leaf.front() > 'z') {
        return ErrorCode::InvalidName;
    }
    if (!std::all_of(ns.begin() + 1, ns.end(), is_name_tail_char) ||
        !std::all_of(leaf.begin() + 1, leaf.end(), is_name_tail_char)) {
        return ErrorCode::InvalidName;
    }
    if (!namespace_allowed(ns)) {
        return ErrorCode::PermissionDenied;
    }
    return ParsedName{ns, leaf, false};
}

Result<void> validate_create_security(const CreateOptions& options,
                                      std::span<const RegistryEntry> existing_entries) noexcept {
    const auto parsed = parse_and_validate_name(options.name);
    if (!parsed) {
        return parsed.error();
    }
    if (options.total_size < SHMFX_HEADER_SIZE + options.meta_size ||
        options.total_size > MAX_SEGMENT_SIZE) {
        return ErrorCode::QuotaExceeded;
    }

    std::uint64_t total_bytes = options.total_size;
    std::uint32_t namespace_count = 0;
    for (const RegistryEntry& entry : existing_entries) {
        total_bytes += entry.total_size;
        const std::string_view entry_name = fixed_name_view(entry.name);
        if (!parsed.value().bootstrap && entry_name.substr(0, 7 + parsed.value().ns.size()) ==
                                             (std::string("/shmfx.") + std::string(parsed.value().ns))) {
            ++namespace_count;
        }
    }
    if (total_bytes > TOTAL_QUOTA_BYTES || namespace_count >= kMaxNamespaceSegments) {
        return ErrorCode::QuotaExceeded;
    }
    return {};
}

Result<void> apply_permissions(int fd, mode_t perm) noexcept {
    if (fchmod(fd, perm) != 0) {
        return ErrorCode::PermissionDenied;
    }
    return {};
}

Result<void> sign_header_if_enabled(ShmHeader& header) {
    if ((header.flags & HmacEnabled) == 0) {
        std::fill(std::begin(header.hmac), std::end(header.hmac), 0);
        return {};
    }

    std::array<std::uint8_t, SHMFX_HMAC_BYTES> key{};
    if (!load_hmac_key(key)) {
        return ErrorCode::PermissionDenied;
    }
    std::fill(std::begin(header.hmac), std::end(header.hmac), 0);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&header);
    const auto digest = hmac_sha256(key, {bytes, SHMFX_IMMUTABLE_BYTES});
    std::copy(digest.begin(), digest.end(), std::begin(header.hmac));
    return {};
}

Result<void> verify_header_hmac(const ShmHeader& header) {
    if ((header.flags & HmacEnabled) == 0 && !has_hmac_bytes(header)) {
        return {};
    }

    std::array<std::uint8_t, SHMFX_HMAC_BYTES> key{};
    if (!load_hmac_key(key)) {
        return ErrorCode::PermissionDenied;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&header);
    const auto digest = hmac_sha256(key, {bytes, SHMFX_IMMUTABLE_BYTES});
    if (!digest_equal(header.hmac, digest)) {
        return ErrorCode::CorruptedHeader;
    }
    return {};
}

} // namespace shmfx
