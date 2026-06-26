/// Dragon Runtime - Argon2id (RFC 9106) password hashing.
///
/// Its OWN translation unit, mirroring runtime_ed25519.cpp / runtime_crypto.cpp:
/// the linker pulls this object ONLY when a program references the argon2id
/// entry point - a program that never hashes a password pays nothing.
///
/// Argon2id is memory-hard and perf-critical, so the whole core lives in C++
/// (legitimate FFI per the dogfooding policy, same as ed25519/scrypt). The .dr
/// layer (stdlib/argon2id.dr) is a thin API wrapper.
///
/// mbedTLS in this build provides NO BLAKE2b, so BLAKE2b (RFC 7693) is
/// implemented here from scratch, then the Argon2 variable-length hash H'
/// (RFC 9106 §3.2), the compression function G over 1024-byte blocks
/// (§3.5/§3.6), the data-independent/dependent indexing for Argon2id (§3.4.1.2),
/// and the t-pass / p-lane / 4-slice memory fill + finalize.
///
/// All intermediate state (blocks, H' buffers, BLAKE2b state) is wiped with
/// argon2_secure_zero before release.

#include "runtime_internal.h"
#include <cstring>
#include <cstdlib>
#include <cstdint>

// Self-contained secure zeroize. argon2id implements its own BLAKE2b and has no
// other mbedTLS dependency, so it must NOT pull in mbedtls_platform_zeroize:
// codegen only links the mbedTLS engine for TLS/crypto symbols, so an
// argon2id-only user program would fail to link (and we don't want to drag the
// whole TLS engine into every password-hashing binary). A volatile function
// pointer to memset defeats dead-store elimination, the same guarantee
// mbedtls_platform_zeroize provides.
static void* (*const volatile argon2_memset_v)(void*, int, size_t) = memset;
static void argon2_secure_zero(void* p, size_t n) {
    if (p && n) argon2_memset_v(p, 0, n);
}

extern "C" {

//===-------------------------------------------------------------------===//
// BLAKE2b (RFC 7693)
//===-------------------------------------------------------------------===//

static const uint64_t blake2b_IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t blake2b_sigma[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3 },
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8 },
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13 },
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9 },
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11 },
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10 },
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5 },
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3 }
};

static inline uint64_t rotr64(uint64_t x, int c) {
    return (x >> c) | (x << (64 - c));
}

static inline uint64_t load64(const uint8_t* p) {
    return ((uint64_t)p[0])        | ((uint64_t)p[1] << 8)  |
           ((uint64_t)p[2] << 16)  | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32)  | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48)  | ((uint64_t)p[7] << 56);
}

static inline void store64(uint8_t* p, uint64_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32); p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48); p[7] = (uint8_t)(v >> 56);
}

static inline void store32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define BLAKE2B_BLOCKBYTES 128
#define BLAKE2B_OUTBYTES   64

typedef struct {
    uint64_t h[8];
    uint64_t t[2];               // message byte counter
    uint64_t f[2];               // finalization flags
    uint8_t  buf[BLAKE2B_BLOCKBYTES];
    size_t   buflen;
    size_t   outlen;
} blake2b_state;

#define B2B_G(r, i, a, b, c, d)                                  \
    do {                                                         \
        a = a + b + m[blake2b_sigma[r][2*i+0]];                  \
        d = rotr64(d ^ a, 32);                                   \
        c = c + d;                                               \
        b = rotr64(b ^ c, 24);                                   \
        a = a + b + m[blake2b_sigma[r][2*i+1]];                  \
        d = rotr64(d ^ a, 16);                                   \
        c = c + d;                                               \
        b = rotr64(b ^ c, 63);                                   \
    } while (0)

static void blake2b_compress(blake2b_state* S, const uint8_t block[BLAKE2B_BLOCKBYTES]) {
    uint64_t m[16];
    uint64_t v[16];
    for (int i = 0; i < 16; i++) m[i] = load64(block + i * 8);
    for (int i = 0; i < 8;  i++) v[i] = S->h[i];
    v[8]  = blake2b_IV[0];
    v[9]  = blake2b_IV[1];
    v[10] = blake2b_IV[2];
    v[11] = blake2b_IV[3];
    v[12] = blake2b_IV[4] ^ S->t[0];
    v[13] = blake2b_IV[5] ^ S->t[1];
    v[14] = blake2b_IV[6] ^ S->f[0];
    v[15] = blake2b_IV[7] ^ S->f[1];

    for (int r = 0; r < 12; r++) {
        B2B_G(r, 0, v[0], v[4], v[8],  v[12]);
        B2B_G(r, 1, v[1], v[5], v[9],  v[13]);
        B2B_G(r, 2, v[2], v[6], v[10], v[14]);
        B2B_G(r, 3, v[3], v[7], v[11], v[15]);
        B2B_G(r, 4, v[0], v[5], v[10], v[15]);
        B2B_G(r, 5, v[1], v[6], v[11], v[12]);
        B2B_G(r, 6, v[2], v[7], v[8],  v[13]);
        B2B_G(r, 7, v[3], v[4], v[9],  v[14]);
    }
    for (int i = 0; i < 8; i++) S->h[i] ^= v[i] ^ v[i + 8];

    argon2_secure_zero(m, sizeof(m));
    argon2_secure_zero(v, sizeof(v));
}

// Initialize BLAKE2b for digest length `outlen` (1..64) and optional key.
static int blake2b_init(blake2b_state* S, size_t outlen,
                        const uint8_t* key, size_t keylen) {
    if (outlen == 0 || outlen > BLAKE2B_OUTBYTES) return -1;
    if (keylen > BLAKE2B_OUTBYTES) return -1;
    memset(S, 0, sizeof(*S));
    for (int i = 0; i < 8; i++) S->h[i] = blake2b_IV[i];
    // Parameter block: digest_length(1) | key_length(1) | fanout(1)=1 |
    // depth(1)=1 | leaf_length(4)=0 | node_offset(8)=0 | node_depth(1)=0 |
    // inner_length(1)=0 | reserved(14)=0 | salt(16)=0 | personal(16)=0.
    // Only the low 8 bytes (h[0]) are non-zero here.
    uint64_t param0 = (uint64_t)outlen | ((uint64_t)keylen << 8) |
                      ((uint64_t)1 << 16) | ((uint64_t)1 << 24);
    S->h[0] ^= param0;
    S->outlen = outlen;
    if (keylen > 0) {
        uint8_t block[BLAKE2B_BLOCKBYTES];
        memset(block, 0, sizeof(block));
        memcpy(block, key, keylen);
        // feed the keyed first block (full 128 bytes)
        S->t[0] += BLAKE2B_BLOCKBYTES;
        blake2b_compress(S, block);
        argon2_secure_zero(block, sizeof(block));
    }
    return 0;
}

static void blake2b_update(blake2b_state* S, const uint8_t* in, size_t inlen) {
    if (inlen == 0) return;
    size_t left = S->buflen;
    size_t fill = BLAKE2B_BLOCKBYTES - left;
    if (inlen > fill) {
        // top off the buffer and compress (there is more to come, so this is
        // never the final block)
        memcpy(S->buf + left, in, fill);
        S->t[0] += BLAKE2B_BLOCKBYTES;
        if (S->t[0] < BLAKE2B_BLOCKBYTES) S->t[1]++;
        blake2b_compress(S, S->buf);
        S->buflen = 0;
        in += fill;
        inlen -= fill;
        while (inlen > BLAKE2B_BLOCKBYTES) {
            S->t[0] += BLAKE2B_BLOCKBYTES;
            if (S->t[0] < BLAKE2B_BLOCKBYTES) S->t[1]++;
            blake2b_compress(S, in);
            in += BLAKE2B_BLOCKBYTES;
            inlen -= BLAKE2B_BLOCKBYTES;
        }
    }
    memcpy(S->buf + S->buflen, in, inlen);
    S->buflen += inlen;
}

static void blake2b_final(blake2b_state* S, uint8_t* out, size_t outlen) {
    uint8_t buffer[BLAKE2B_OUTBYTES];
    memset(buffer, 0, sizeof(buffer));
    S->t[0] += S->buflen;
    if (S->t[0] < S->buflen) S->t[1]++;
    S->f[0] = (uint64_t)-1;                 // last block flag
    memset(S->buf + S->buflen, 0, BLAKE2B_BLOCKBYTES - S->buflen);
    blake2b_compress(S, S->buf);
    for (int i = 0; i < 8; i++) store64(buffer + i * 8, S->h[i]);
    memcpy(out, buffer, outlen);
    argon2_secure_zero(buffer, sizeof(buffer));
}

// One-shot unkeyed BLAKE2b.
static void blake2b(uint8_t* out, size_t outlen,
                    const uint8_t* in, size_t inlen) {
    blake2b_state S;
    blake2b_init(&S, outlen, nullptr, 0);
    blake2b_update(&S, in, inlen);
    blake2b_final(&S, out, outlen);
    argon2_secure_zero(&S, sizeof(S));
}

//===-------------------------------------------------------------------===//
// Argon2 variable-length hash H' (RFC 9106 §3.2)
//===-------------------------------------------------------------------===//
//
// H'^T(A): if T <= 64, BLAKE2b(LE32(T) || A, T). Otherwise let r = ceil(T/32)-2;
// V_1 = BLAKE2b(LE32(T) || A, 64); V_{i+1} = BLAKE2b(V_i, 64); the output is the
// first 32 bytes of each V_1..V_r, then the final (T - 32*r) bytes of V_{r+1}.
static void argon2_H_prime(uint8_t* out, uint32_t outlen,
                           const uint8_t* in, size_t inlen) {
    uint8_t lenbuf[4];
    store32(lenbuf, outlen);

    if (outlen <= BLAKE2B_OUTBYTES) {
        blake2b_state S;
        blake2b_init(&S, outlen, nullptr, 0);
        blake2b_update(&S, lenbuf, 4);
        blake2b_update(&S, in, inlen);
        blake2b_final(&S, out, outlen);
        argon2_secure_zero(&S, sizeof(S));
        return;
    }

    // long output path
    uint8_t V[BLAKE2B_OUTBYTES];
    {
        blake2b_state S;
        blake2b_init(&S, BLAKE2B_OUTBYTES, nullptr, 0);
        blake2b_update(&S, lenbuf, 4);
        blake2b_update(&S, in, inlen);
        blake2b_final(&S, V, BLAKE2B_OUTBYTES);
        argon2_secure_zero(&S, sizeof(S));
    }
    uint32_t pos = 0;
    memcpy(out + pos, V, 32);
    pos += 32;
    uint32_t remaining = outlen - 32;       // bytes still to write
    while (remaining > 64) {
        blake2b(V, BLAKE2B_OUTBYTES, V, BLAKE2B_OUTBYTES);
        memcpy(out + pos, V, 32);
        pos += 32;
        remaining -= 32;
    }
    // final block: write the last `remaining` (<=64) bytes
    blake2b(V, BLAKE2B_OUTBYTES, V, BLAKE2B_OUTBYTES);
    memcpy(out + pos, V, remaining);
    argon2_secure_zero(V, sizeof(V));
}

//===-------------------------------------------------------------------===//
// Compression function G over 1024-byte blocks (RFC 9106 §3.5/§3.6)
//===-------------------------------------------------------------------===//

#define ARGON2_QWORDS_IN_BLOCK 128       // 1024 bytes / 8
#define ARGON2_BLOCK_SIZE      1024

typedef struct { uint64_t v[ARGON2_QWORDS_IN_BLOCK]; } block;

static inline uint64_t fBlaMka(uint64_t x, uint64_t y) {
    // The modular-multiply mixing of the Argon2 G rounds:
    //   x + y + 2 * (lower-32-of-x) * (lower-32-of-y)
    const uint64_t m = 0xFFFFFFFFULL;
    uint64_t xy = (x & m) * (y & m);
    return x + y + 2 * xy;
}

// The BLAKE2b round function GB used inside the permutation P (§3.6).
#define ARGON2_G(a, b, c, d)                       \
    do {                                           \
        a = fBlaMka(a, b);                         \
        d = rotr64(d ^ a, 32);                     \
        c = fBlaMka(c, d);                         \
        b = rotr64(b ^ c, 24);                     \
        a = fBlaMka(a, b);                         \
        d = rotr64(d ^ a, 16);                     \
        c = fBlaMka(c, d);                         \
        b = rotr64(b ^ c, 63);                     \
    } while (0)

// Permutation P operating on 16 64-bit words v0..v15 (§3.6).
#define ARGON2_P(v0,v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12,v13,v14,v15) \
    do {                                                               \
        ARGON2_G(v0, v4, v8,  v12);                                    \
        ARGON2_G(v1, v5, v9,  v13);                                    \
        ARGON2_G(v2, v6, v10, v14);                                    \
        ARGON2_G(v3, v7, v11, v15);                                    \
        ARGON2_G(v0, v5, v10, v15);                                    \
        ARGON2_G(v1, v6, v11, v12);                                    \
        ARGON2_G(v2, v7, v8,  v13);                                    \
        ARGON2_G(v3, v4, v9,  v14);                                    \
    } while (0)

// G(X, Y): R = X ^ Y; apply P column-wise over each of the 8 rows of 16 words,
// then row-wise over each of the 8 columns, then result = Z ^ R. When
// `with_xor` is set, the destination already holds a value to XOR in too (used
// for t>0 / passes that overwrite existing blocks).
static void fill_block(const block* prev, const block* ref, block* next,
                       int with_xor) {
    block R, Z;
    for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
        R.v[i] = prev->v[i] ^ ref->v[i];
    memcpy(&Z, &R, sizeof(block));
    if (with_xor) {
        for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
            R.v[i] ^= next->v[i];
    }

    // Apply P to each of the 8 rows (column step). Each row is 16 words; the
    // 16 words map to the 8x2 matrix as v[2k] / v[2k+1] interleave per §3.6.
    for (int i = 0; i < 8; i++) {
        uint64_t* p = &Z.v[16 * i];
        ARGON2_P(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                 p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    }
    // Apply P to each of the 8 columns (row step). Column i gathers the two
    // words 2*i and 2*i+1 from each of the 8 rows -> 16 words.
    for (int i = 0; i < 8; i++) {
        uint64_t* p = Z.v;
        ARGON2_P(p[2*i], p[2*i+1],
                 p[2*i+16], p[2*i+17],
                 p[2*i+32], p[2*i+33],
                 p[2*i+48], p[2*i+49],
                 p[2*i+64], p[2*i+65],
                 p[2*i+80], p[2*i+81],
                 p[2*i+96], p[2*i+97],
                 p[2*i+112], p[2*i+113]);
    }
    for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
        next->v[i] = Z.v[i] ^ R.v[i];

    argon2_secure_zero(&R, sizeof(R));
    argon2_secure_zero(&Z, sizeof(Z));
}

//===-------------------------------------------------------------------===//
// Indexing (RFC 9106 §3.4.1.1 / §3.4.1.2)
//===-------------------------------------------------------------------===//

#define ARGON2_SYNC_POINTS 4
#define ARGON2_ID 2          // type code embedded in H0 and address blocks

// Argon2 layout context (single struct passed around the fill).
typedef struct {
    block*   memory;         // lane_length * lanes blocks
    uint32_t passes;         // t_cost
    uint32_t lanes;          // p
    uint32_t lane_length;    // segment_length * SYNC_POINTS
    uint32_t segment_length; // m' / (lanes * SYNC_POINTS)
    uint32_t memory_blocks;  // m'
} argon2_instance;

typedef struct {
    uint32_t pass;
    uint32_t lane;
    uint32_t slice;
    uint32_t index;          // index of the block within the segment
} argon2_position;

// Generate the next address block for data-independent (Argon2i) indexing
// (§3.4.1.2). `input_block` carries the counter state; `address_block` receives
// the 128 pseudo-random J1/J2 pairs; `zero_block` is an all-zero block used as
// the "prev" feed for the two compression invocations.
static void next_addresses(block* address_block, block* input_block,
                           const block* zero_block) {
    input_block->v[6]++;
    fill_block(zero_block, input_block, address_block, 0);
    fill_block(zero_block, address_block, address_block, 0);
}

// Compute the reference block index for the block at (position) given the
// pseudo-random value `pseudo_rand` (low 64 bits: J1 in low 32, J2 in high 32).
static uint32_t index_alpha(const argon2_instance* inst,
                            const argon2_position* pos,
                            uint32_t pseudo_rand,
                            int same_lane) {
    // reference_area_size: the number of blocks that may be referenced.
    // Mirrors the reference implementation's index_alpha exactly, including the
    // "subtract one more when index==0 and referencing another lane" rule.
    uint32_t reference_area_size;
    if (pos->pass == 0) {
        if (pos->slice == 0) {
            // first slice, first pass: only blocks already produced in this
            // segment (index - 1).
            reference_area_size = pos->index - 1;
        } else if (same_lane) {
            reference_area_size = pos->slice * inst->segment_length
                                  + pos->index - 1;
        } else {
            reference_area_size = pos->slice * inst->segment_length
                                  + (pos->index == 0 ? (uint32_t)-1 : 0);
        }
    } else {
        if (same_lane) {
            reference_area_size = inst->lane_length
                                  - inst->segment_length + pos->index - 1;
        } else {
            reference_area_size = inst->lane_length - inst->segment_length
                                  + (pos->index == 0 ? (uint32_t)-1 : 0);
        }
    }

    // Map the 32-bit pseudo_rand to a relative position with the spec's
    // non-uniform mapping (favours recent blocks).
    uint64_t relative_position = pseudo_rand;
    relative_position = relative_position * relative_position >> 32;
    relative_position = reference_area_size - 1
                        - (reference_area_size * relative_position >> 32);

    // start_position: where the referenceable window begins for this segment.
    uint32_t start_position = 0;
    if (pos->pass != 0) {
        start_position = (pos->slice == ARGON2_SYNC_POINTS - 1)
                         ? 0
                         : (pos->slice + 1) * inst->segment_length;
    }
    uint32_t absolute_position =
        (uint32_t)((start_position + relative_position) % inst->lane_length);
    return absolute_position;
}

//===-------------------------------------------------------------------===//
// Segment fill
//===-------------------------------------------------------------------===//

static void fill_segment(argon2_instance* inst, const argon2_position* pos_in) {
    argon2_position position = *pos_in;

    // Argon2id: data-independent addressing for the first two slices of the
    // first pass; data-dependent otherwise.
    int data_independent =
        (position.pass == 0 && position.slice < ARGON2_SYNC_POINTS / 2);

    block address_block, input_block, zero_block;
    if (data_independent) {
        memset(&zero_block, 0, sizeof(zero_block));
        memset(&input_block, 0, sizeof(input_block));
        memset(&address_block, 0, sizeof(address_block));
        input_block.v[0] = position.pass;
        input_block.v[1] = position.lane;
        input_block.v[2] = position.slice;
        input_block.v[3] = inst->memory_blocks;
        input_block.v[4] = inst->passes;
        input_block.v[5] = ARGON2_ID;     // type
        // v[6] is the counter, bumped by next_addresses (starts at 0 -> 1).
    }

    uint32_t starting_index = 0;
    if (position.pass == 0 && position.slice == 0) {
        starting_index = 2;               // blocks 0 and 1 are seeded already
        if (data_independent) {
            // produce the first address block (counter -> 1)
            next_addresses(&address_block, &input_block, &zero_block);
        }
    }

    // offset of the first block of this segment within the lane
    uint32_t curr_offset = position.lane * inst->lane_length
                           + position.slice * inst->segment_length
                           + starting_index;
    uint32_t prev_offset;
    if (curr_offset % inst->lane_length == 0) {
        prev_offset = curr_offset + inst->lane_length - 1;  // wrap to lane end
    } else {
        prev_offset = curr_offset - 1;
    }

    for (uint32_t i = starting_index; i < inst->segment_length;
         i++, curr_offset++, prev_offset++) {
        if (curr_offset % inst->lane_length == 1) {
            // moved to a new lane row boundary; prev is the block right before
            prev_offset = curr_offset - 1;
        }

        // pseudo-random value driving the reference index
        uint64_t pseudo_rand;
        if (data_independent) {
            if (i % ARGON2_QWORDS_IN_BLOCK == 0) {
                next_addresses(&address_block, &input_block, &zero_block);
            }
            pseudo_rand = address_block.v[i % ARGON2_QWORDS_IN_BLOCK];
        } else {
            pseudo_rand = inst->memory[prev_offset].v[0];
        }

        // which lane the reference block comes from
        uint32_t ref_lane;
        if (position.pass == 0 && position.slice == 0) {
            ref_lane = position.lane;     // can only reference own lane
        } else {
            ref_lane = (uint32_t)((pseudo_rand >> 32) % inst->lanes);
        }

        position.index = i;
        int same_lane = (ref_lane == position.lane);
        uint32_t ref_index = index_alpha(inst, &position,
                                         (uint32_t)(pseudo_rand & 0xFFFFFFFFULL),
                                         same_lane);

        block* ref_block =
            &inst->memory[ref_lane * inst->lane_length + ref_index];
        block* curr_block = &inst->memory[curr_offset];

        // first pass writes fresh; later passes XOR into the existing block
        int with_xor = (position.pass != 0);
        fill_block(&inst->memory[prev_offset], ref_block, curr_block, with_xor);
    }

    if (data_independent) {
        argon2_secure_zero(&address_block, sizeof(address_block));
        argon2_secure_zero(&input_block, sizeof(input_block));
        argon2_secure_zero(&zero_block, sizeof(zero_block));
    }
}

//===-------------------------------------------------------------------===//
// Top-level Argon2id
//===-------------------------------------------------------------------===//

static inline const uint8_t* b_data(DragonBytes* b) {
    static const uint8_t empty[1] = {0};
    return (b && b->data) ? b->data : empty;
}
static inline uint32_t b_len(DragonBytes* b) {
    return b ? (uint32_t)b->len : 0;
}

// Build H0 = BLAKE2b( LE32(p) || LE32(tag_len) || LE32(m) || LE32(t) ||
//   LE32(version=0x13) || LE32(type=Argon2id) || LE32(|P|) || P ||
//   LE32(|S|) || S || LE32(|K|) || K || LE32(|X|) || X ), 64 ).
static void compute_H0(uint8_t H0[BLAKE2B_OUTBYTES],
                       DragonBytes* pwd, DragonBytes* salt,
                       DragonBytes* secret, DragonBytes* ad,
                       uint32_t t, uint32_t m, uint32_t p,
                       uint32_t tag_len) {
    blake2b_state S;
    blake2b_init(&S, BLAKE2B_OUTBYTES, nullptr, 0);
    uint8_t buf[4];

    store32(buf, p);        blake2b_update(&S, buf, 4);
    store32(buf, tag_len);  blake2b_update(&S, buf, 4);
    store32(buf, m);        blake2b_update(&S, buf, 4);
    store32(buf, t);        blake2b_update(&S, buf, 4);
    store32(buf, 0x13);     blake2b_update(&S, buf, 4);   // version
    store32(buf, ARGON2_ID);blake2b_update(&S, buf, 4);   // type = Argon2id

    store32(buf, b_len(pwd));    blake2b_update(&S, buf, 4);
    blake2b_update(&S, b_data(pwd), b_len(pwd));
    store32(buf, b_len(salt));   blake2b_update(&S, buf, 4);
    blake2b_update(&S, b_data(salt), b_len(salt));
    store32(buf, b_len(secret)); blake2b_update(&S, buf, 4);
    blake2b_update(&S, b_data(secret), b_len(secret));
    store32(buf, b_len(ad));     blake2b_update(&S, buf, 4);
    blake2b_update(&S, b_data(ad), b_len(ad));

    blake2b_final(&S, H0, BLAKE2B_OUTBYTES);
    argon2_secure_zero(&S, sizeof(S));
}

/// Raw Argon2id tag. Returns a fresh DragonBytes (refcount 1) of `tag_len`
/// bytes, or raises ValueError (code 90) on bad parameters.
DragonBytes* dragon_argon2id_raw(DragonBytes* pwd, DragonBytes* salt,
                                 DragonBytes* secret, DragonBytes* ad,
                                 int64_t t_cost, int64_t m_cost_kib,
                                 int64_t parallelism, int64_t tag_len) {
    if (parallelism < 1 || parallelism > 0xFFFFFF) {
        dragon_raise_exc_cstr(90, "argon2id: parallelism must be in [1, 2^24)");
        return nullptr;
    }
    if (tag_len < 4) {
        dragon_raise_exc_cstr(90, "argon2id: tag length must be >= 4 bytes");
        return nullptr;
    }
    if (t_cost < 1) {
        dragon_raise_exc_cstr(90, "argon2id: time cost must be >= 1");
        return nullptr;
    }
    if (m_cost_kib < 8 * parallelism) {
        dragon_raise_exc_cstr(90, "argon2id: memory cost must be >= 8 * parallelism KiB");
        return nullptr;
    }

    uint32_t p = (uint32_t)parallelism;
    uint32_t t = (uint32_t)t_cost;
    uint32_t m = (uint32_t)m_cost_kib;
    uint32_t outlen = (uint32_t)tag_len;

    // m' = 4 * p * floor(m / (4p)); each lane has lane_length blocks.
    uint32_t segment_length = m / (p * ARGON2_SYNC_POINTS);
    uint32_t memory_blocks = segment_length * p * ARGON2_SYNC_POINTS;
    uint32_t lane_length = segment_length * ARGON2_SYNC_POINTS;

    block* memory = (block*)calloc(memory_blocks, sizeof(block));
    if (!memory) {
        dragon_raise_exc_cstr(90, "argon2id: out of memory");
        return nullptr;
    }

    // H0
    uint8_t H0[BLAKE2B_OUTBYTES + 8];     // 64 + room for LE32(lane)|LE32(0/1)
    compute_H0(H0, pwd, salt, secret, ad, t, m, p, outlen);

    // Seed the first two blocks of every lane:
    //   B[i][0] = H'(H0 || LE32(0) || LE32(i))
    //   B[i][1] = H'(H0 || LE32(1) || LE32(i))
    uint8_t blockhash[ARGON2_BLOCK_SIZE];
    for (uint32_t lane = 0; lane < p; lane++) {
        store32(H0 + 64, 0);
        store32(H0 + 68, lane);
        argon2_H_prime(blockhash, ARGON2_BLOCK_SIZE, H0, 72);
        for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
            memory[lane * lane_length + 0].v[i] = load64(blockhash + i * 8);

        store32(H0 + 64, 1);
        store32(H0 + 68, lane);
        argon2_H_prime(blockhash, ARGON2_BLOCK_SIZE, H0, 72);
        for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
            memory[lane * lane_length + 1].v[i] = load64(blockhash + i * 8);
    }
    argon2_secure_zero(blockhash, sizeof(blockhash));
    argon2_secure_zero(H0, sizeof(H0));

    argon2_instance inst;
    inst.memory = memory;
    inst.passes = t;
    inst.lanes = p;
    inst.lane_length = lane_length;
    inst.segment_length = segment_length;
    inst.memory_blocks = memory_blocks;

    // Fill: t passes, 4 slices each, p lanes per slice.
    for (uint32_t pass = 0; pass < t; pass++) {
        for (uint32_t slice = 0; slice < ARGON2_SYNC_POINTS; slice++) {
            for (uint32_t lane = 0; lane < p; lane++) {
                argon2_position pos;
                pos.pass = pass;
                pos.lane = lane;
                pos.slice = slice;
                pos.index = 0;
                fill_segment(&inst, &pos);
            }
        }
    }

    // Finalize: C = XOR of the last block of every lane, tag = H'(C, tag_len).
    block final_block;
    memcpy(&final_block, &memory[lane_length - 1], sizeof(block));
    for (uint32_t lane = 1; lane < p; lane++) {
        block* last = &memory[lane * lane_length + (lane_length - 1)];
        for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
            final_block.v[i] ^= last->v[i];
    }

    uint8_t final_bytes[ARGON2_BLOCK_SIZE];
    for (int i = 0; i < ARGON2_QWORDS_IN_BLOCK; i++)
        store64(final_bytes + i * 8, final_block.v[i]);

    uint8_t* tag = (uint8_t*)malloc(outlen);
    if (!tag) {
        argon2_secure_zero(memory, (size_t)memory_blocks * sizeof(block));
        free(memory);
        argon2_secure_zero(&final_block, sizeof(final_block));
        argon2_secure_zero(final_bytes, sizeof(final_bytes));
        dragon_raise_exc_cstr(90, "argon2id: out of memory");
        return nullptr;
    }
    argon2_H_prime(tag, outlen, final_bytes, ARGON2_BLOCK_SIZE);

    DragonBytes* out = dragon_bytes_new(tag, outlen);

    // Wipe every intermediate buffer.
    argon2_secure_zero(memory, (size_t)memory_blocks * sizeof(block));
    free(memory);
    argon2_secure_zero(&final_block, sizeof(final_block));
    argon2_secure_zero(final_bytes, sizeof(final_bytes));
    argon2_secure_zero(tag, outlen);
    free(tag);

    return out;
}

}  // extern "C"
