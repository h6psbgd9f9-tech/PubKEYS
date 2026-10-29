// ============================================================================
//  hash.h  —  SHA-256, RIPEMD-160, Base58Check
// ============================================================================
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ------------------------------- SHA-256 ------------------------------------
namespace sha256d {

static const uint32_t K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Ctx {
    uint32_t h[8];
    void init() {
        h[0]=0x6a09e667; h[1]=0xbb67ae85; h[2]=0x3c6ef372; h[3]=0xa54ff53a;
        h[4]=0x510e527f; h[5]=0x9b05688c; h[6]=0x1f83d9ab; h[7]=0x5be0cd19;
    }
    void block(const uint8_t *p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
};

} // namespace sha256d

static inline void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

// SHA-256 для входа длиной < 56 байт (один блок) — быстрый путь
static inline void sha256_small(const uint8_t *data, size_t len, uint8_t out[32]) {
    // ГРАНИЦА ПРОВЕРЯЕТСЯ, А НЕ ПОДРАЗУМЕВАЕТСЯ.
    //
    // При len >= 56 байт 0x80 залезал в поле длины и дайджест выходил
    // неверным молча; при len == 64 -- запись в buf[64], то есть за пределы
    // массива на стеке. Сегодня единственный вызывающий (hash160) держит
    // len < 56, но функция лежит в общедоступном заголовке рядом с общей
    // sha256, и следующий вызывающий об ограничении узнает только отсюда.
    if (len >= 56) { sha256(data, len, out); return; }
    uint8_t buf[64] = {0};
    if (len) memcpy(buf, data, len);           // см. sha256: nullptr + 0 -- тоже UB
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[63 - i] = (uint8_t)(bits >> (8 * i));
    sha256d::Ctx c; c.init(); c.block(buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c.h[i] >> 24);
        out[i*4+1] = (uint8_t)(c.h[i] >> 16);
        out[i*4+2] = (uint8_t)(c.h[i] >> 8);
        out[i*4+3] = (uint8_t)(c.h[i]);
    }
}

// SHA-256 общего назначения
static inline void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256d::Ctx c; c.init();
    size_t i = 0;
    for (; i + 64 <= len; i += 64) c.block(data + i);
    uint8_t buf[128] = {0};
    size_t rem = len - i;
    // memcpy с нулевым указателем -- неопределённое поведение ДАЖЕ при нулевой
    // длине: стандарт требует годный указатель безусловно. sha256(nullptr, 0)
    // -- законный вызов (пустое сообщение), и санитайзер на нём срабатывает.
    if (rem) memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t total = (rem + 1 <= 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++) buf[total - 1 - j] = (uint8_t)(bits >> (8 * j));
    c.block(buf);
    if (total == 128) c.block(buf + 64);
    for (int j = 0; j < 8; j++) {
        out[j*4]   = (uint8_t)(c.h[j] >> 24);
        out[j*4+1] = (uint8_t)(c.h[j] >> 16);
        out[j*4+2] = (uint8_t)(c.h[j] >> 8);
        out[j*4+3] = (uint8_t)(c.h[j]);
    }
}

// ------------------------------ RIPEMD-160 ----------------------------------
namespace rmd {

static inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t f1(uint32_t x,uint32_t y,uint32_t z){return x^y^z;}
static inline uint32_t f2(uint32_t x,uint32_t y,uint32_t z){return (x&y)|(~x&z);}
static inline uint32_t f3(uint32_t x,uint32_t y,uint32_t z){return (x|~y)^z;}
static inline uint32_t f4(uint32_t x,uint32_t y,uint32_t z){return (x&z)|(y&~z);}
static inline uint32_t f5(uint32_t x,uint32_t y,uint32_t z){return x^(y|~z);}

static const int RL[80] = {
 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
 7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
 3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12,
 1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
 4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13};
static const int RR[80] = {
 5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12,
 6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
 15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13,
 8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
 12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11};
static const int SL[80] = {
 11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8,
 7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
 11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5,
 11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
 9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6};
static const int SR[80] = {
 8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6,
 9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
 9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5,
 15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
 8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11};

static const uint32_t KL[5] = {0x00000000,0x5A827999,0x6ED9EBA1,0x8F1BBCDC,0xA953FD4E};
static const uint32_t KR[5] = {0x50A28BE6,0x5C4DD124,0x6D703EF3,0x7A6D76E9,0x00000000};

} // namespace rmd

// RIPEMD-160 для входа ровно 32 байта (наш случай) и общего назначения
static inline void ripemd160(const uint8_t *data, size_t len, uint8_t out[20]) {
    using namespace rmd;
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};

    size_t total = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> buf(total, 0);
    if (len) memcpy(buf.data(), data, len);   // см. sha256: nullptr + 0 -- тоже UB
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 8 + i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < total; off += 64) {
        uint32_t X[16];
        for (int i = 0; i < 16; i++)
            X[i] = (uint32_t)buf[off+i*4] | ((uint32_t)buf[off+i*4+1]<<8) |
                   ((uint32_t)buf[off+i*4+2]<<16) | ((uint32_t)buf[off+i*4+3]<<24);
        uint32_t al=h[0],bl=h[1],cl=h[2],dl=h[3],el=h[4];
        uint32_t ar=al,br=bl,cr=cl,dr=dl,er=el;
        for (int j = 0; j < 80; j++) {
            int r = j / 16;
            uint32_t t;
            uint32_t fl = (r==0)?f1(bl,cl,dl):(r==1)?f2(bl,cl,dl):(r==2)?f3(bl,cl,dl):
                          (r==3)?f4(bl,cl,dl):f5(bl,cl,dl);
            t = rol(al + fl + X[RL[j]] + KL[r], SL[j]) + el;
            al=el; el=dl; dl=rol(cl,10); cl=bl; bl=t;

            uint32_t fr = (r==0)?f5(br,cr,dr):(r==1)?f4(br,cr,dr):(r==2)?f3(br,cr,dr):
                          (r==3)?f2(br,cr,dr):f1(br,cr,dr);
            t = rol(ar + fr + X[RR[j]] + KR[r], SR[j]) + er;
            ar=er; er=dr; dr=rol(cr,10); cr=br; br=t;
        }
        uint32_t tt = h[1] + cl + dr;
        h[1] = h[2] + dl + er;
        h[2] = h[3] + el + ar;
        h[3] = h[4] + al + br;
        h[4] = h[0] + bl + cr;
        h[0] = tt;
    }
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(h[i]);
        out[i*4+1] = (uint8_t)(h[i] >> 8);
        out[i*4+2] = (uint8_t)(h[i] >> 16);
        out[i*4+3] = (uint8_t)(h[i] >> 24);
    }
}

// HASH160 = RIPEMD160(SHA256(x))
static inline void hash160(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint8_t sh[32];
    if (len < 56) sha256_small(data, len, sh);
    else sha256(data, len, sh);
    ripemd160(sh, 32, out);
}

// ------------------------------- Base58 -------------------------------------
static const char *B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static inline std::string base58_encode(const uint8_t *data, size_t len) {
    std::vector<uint8_t> digits;
    digits.reserve(len * 138 / 100 + 1);
    for (size_t i = 0; i < len; i++) {
        int carry = data[i];
        for (size_t j = 0; j < digits.size(); j++) {
            int v = digits[j] * 256 + carry;
            digits[j] = (uint8_t)(v % 58);
            carry = v / 58;
        }
        while (carry) { digits.push_back((uint8_t)(carry % 58)); carry /= 58; }
    }
    std::string s;
    for (size_t i = 0; i < len && data[i] == 0; i++) s += '1';
    for (size_t i = digits.size(); i-- > 0;) s += B58[digits[i]];
    if (s.empty()) s = "1";
    return s;
}

static inline bool base58_decode(const std::string &s, std::vector<uint8_t> &out) {
    std::vector<uint8_t> bytes;
    for (char c : s) {
        const char *p = strchr(B58, c);
        if (!p || c == 0) return false;
        int carry = (int)(p - B58);
        for (size_t j = 0; j < bytes.size(); j++) {
            int v = bytes[j] * 58 + carry;
            bytes[j] = (uint8_t)(v & 0xFF);
            carry = v >> 8;
        }
        while (carry) { bytes.push_back((uint8_t)(carry & 0xFF)); carry >>= 8; }
    }
    out.clear();
    for (size_t i = 0; i < s.size() && s[i] == '1'; i++) out.push_back(0);
    for (size_t i = bytes.size(); i-- > 0;) out.push_back(bytes[i]);
    return true;
}

static inline std::string base58check(const uint8_t *payload, size_t len) {
    std::vector<uint8_t> buf(payload, payload + len);
    uint8_t h1[32], h2[32];
    sha256(buf.data(), buf.size(), h1);
    sha256(h1, 32, h2);
    buf.insert(buf.end(), h2, h2 + 4);
    return base58_encode(buf.data(), buf.size());
}

// P2PKH-адрес из HASH160
static inline std::string addr_from_hash160(const uint8_t h[20]) {
    uint8_t p[21];
    p[0] = 0x00;
    memcpy(p + 1, h, 20);
    return base58check(p, 21);
}

// WIF из приватного ключа
static inline std::string wif_from_key(const uint8_t key[32], bool compressed) {
    uint8_t p[34];
    p[0] = 0x80;
    memcpy(p + 1, key, 32);
    if (compressed) { p[33] = 0x01; return base58check(p, 34); }
    return base58check(p, 33);
}

static inline std::string to_hex(const uint8_t *d, size_t n) {
    static const char *H = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
    return s;
}
