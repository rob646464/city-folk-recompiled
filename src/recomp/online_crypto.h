#pragma once
// Small cryptographic primitives for online play (docs/ONLINE.md "Security"): SHA-256,
// HMAC-SHA256, PBKDF2-HMAC-SHA256, ChaCha20 and an encrypt-then-MAC packet seal on top of them,
// plus OS random bytes. Written from the public specifications (FIPS 180-4, RFC 2104, RFC 8018,
// RFC 8439); no third-party code. Self-test: onlinecrypto::selfTest() (known-answer vectors).
#include <cstddef>
#include <cstdint>
#include <string>

namespace onlinecrypto {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Sha256 {
    Sha256();
    void update(const void* data, size_t len);
    void final(u8 out[32]);

private:
    void block(const u8* p);
    u32 h_[8];
    u8 buf_[64];
    u64 len_ = 0;
    size_t n_ = 0;
};

void sha256(const void* data, size_t len, u8 out[32]);
void hmacSha256(const void* key, size_t keyLen, const void* data, size_t len, u8 out[32]);
// Two-part message (header + body) without concatenating.
void hmacSha256(const void* key, size_t keyLen, const void* a, size_t aLen, const void* b, size_t bLen, u8 out[32]);
void pbkdf2Sha256(const void* pw, size_t pwLen, const void* salt, size_t saltLen, u32 iterations, u8* out, size_t outLen);
// XORs data with the ChaCha20 key stream (RFC 8439: 32-byte key, 12-byte nonce, block counter).
void chacha20(const u8 key[32], const u8 nonce[12], u32 counter, u8* data, size_t len);

// Fills out with bytes from the OS random generator (BCryptGenRandom / getrandom / urandom).
bool randomBytes(void* out, size_t len);

// Constant-time comparison.
bool equal(const void* a, const void* b, size_t len);

// Packet seal: ChaCha20 with a key derived for this direction, HMAC-SHA256 over the header and
// the ciphertext, truncated to 16 bytes. `nonce` must never repeat for one key (a per-direction
// packet counter).
constexpr size_t kTagSize = 16;
struct SealKey {
    u8 enc[32];
    u8 mac[32];
};
// Derives the two keys of one direction from a 32-byte session key and a label ("h2c", "c2h").
SealKey deriveSealKey(const u8 session[32], const char* label);
// Encrypts body in place and writes the tag (16 bytes) to tag.
void seal(const SealKey& k, u64 nonce, const u8* hdr, size_t hdrLen, u8* body, size_t bodyLen, u8 tag[kTagSize]);
// Checks the tag, then decrypts body in place; false (body untouched) when the tag is wrong.
bool open(const SealKey& k, u64 nonce, const u8* hdr, size_t hdrLen, u8* body, size_t bodyLen, const u8 tag[kTagSize]);

std::string hex(const void* data, size_t len);
bool selfTest(std::string* failure = nullptr);

}  // namespace onlinecrypto
