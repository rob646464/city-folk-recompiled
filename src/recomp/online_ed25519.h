#pragma once
// Ed25519 signatures (RFC 8032) and X25519 key agreement (RFC 7748) for online friends and mail
// (docs/ONLINE.md "Friends and mail"), with SHA-512 (FIPS 180-4). Our own implementation in the
// compact 16x16-bit-limb style; constants derived from the curve definition
// (scratch script); known-answer tests in selfTest().
#include <cstddef>
#include <cstdint>
#include <string>

namespace onlinecrypto {

void sha512(const void* data, size_t len, uint8_t out[64]);

// 32-byte seed -> 32-byte public key (the 64-byte expanded secret is recomputed per signature).
void ed25519PublicKey(const uint8_t seed[32], uint8_t pub[32]);
void ed25519Sign(const uint8_t seed[32], const void* msg, size_t len, uint8_t sig[64]);
bool ed25519Verify(const uint8_t pub[32], const void* msg, size_t len, const uint8_t sig[64]);

// X25519: out = scalar * point (u coordinate); base point 9 when point is null.
void x25519(const uint8_t scalar[32], const uint8_t* point, uint8_t out[32]);

bool edSelfTest(std::string* failure = nullptr);

}  // namespace onlinecrypto
