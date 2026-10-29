#include "sha256.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <array>
#include <cstring>

namespace fileguard {

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t choose(uint32_t e, uint32_t f, uint32_t g) {
    return (e & f) ^ (~e & g);
}

inline uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline uint32_t sig0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sig1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t theta0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t theta1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

const std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

struct SHA256Context {
    std::array<uint32_t, 8> state = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint64_t bitlen = 0;
    std::array<uint8_t, 64> buffer{};
    size_t datalen = 0;

    void transform(const uint8_t data[64]) {
        std::array<uint32_t, 64> m;
        for (size_t i = 0, j = 0; i < 16; ++i, j += 4) {
            m[i] = (static_cast<uint32_t>(data[j]) << 24) |
                   (static_cast<uint32_t>(data[j + 1]) << 16) |
                   (static_cast<uint32_t>(data[j + 2]) << 8) |
                   (static_cast<uint32_t>(data[j + 3]));
        }
        for (size_t i = 16; i < 64; ++i) {
            m[i] = theta1(m[i - 2]) + m[i - 7] + theta0(m[i - 15]) + m[i - 16];
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (size_t i = 0; i < 64; ++i) {
            uint32_t t1 = h + sig1(e) + choose(e, f, g) + K[i] + m[i];
            uint32_t t2 = sig0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            buffer[datalen++] = data[i];
            if (datalen == 64) {
                transform(buffer.data());
                bitlen += 512;
                datalen = 0;
            }
        }
    }

    void finalize(uint8_t hash[32]) {
        size_t i = datalen;
        if (datalen < 56) {
            buffer[i++] = 0x80;
            while (i < 56) buffer[i++] = 0x00;
        } else {
            buffer[i++] = 0x80;
            while (i < 64) buffer[i++] = 0x00;
            transform(buffer.data());
            memset(buffer.data(), 0, 56);
        }

        bitlen += datalen * 8;
        for (int b = 7; b >= 0; --b) {
            buffer[56 + (7 - b)] = static_cast<uint8_t>((bitlen >> (b * 8)) & 0xff);
        }
        transform(buffer.data());

        for (size_t idx = 0; idx < 8; ++idx) {
            hash[idx * 4]     = static_cast<uint8_t>((state[idx] >> 24) & 0xff);
            hash[idx * 4 + 1] = static_cast<uint8_t>((state[idx] >> 16) & 0xff);
            hash[idx * 4 + 2] = static_cast<uint8_t>((state[idx] >> 8) & 0xff);
            hash[idx * 4 + 3] = static_cast<uint8_t>(state[idx] & 0xff);
        }
    }
};

} // namespace

std::string sha256_bytes(const uint8_t* data, size_t len) {
    SHA256Context ctx;
    ctx.update(data, len);
    uint8_t hash[32];
    ctx.finalize(hash);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < 32; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

std::string sha256_buffer(const std::vector<uint8_t>& buf) {
    return sha256_bytes(buf.data(), buf.size());
}

std::string sha256_string(const std::string& str) {
    return sha256_bytes(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    SHA256Context ctx;
    char buf[16384];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
        ctx.update(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(file.gcount()));
    }
    uint8_t hash[32];
    ctx.finalize(hash);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < 32; ++i) {
        oss << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

} // namespace fileguard
