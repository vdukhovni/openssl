/*
 * Copyright 2024 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/* Copyright (c) 2024, Google Inc. */

#include <assert.h>
#include <internal/common.h>
#include <internal/constant_time.h>
#include <internal/sha3.h>
#include <crypto/ml_kem.h>
#include <openssl/rand.h>

#define DEGREE              ML_KEM_DEGREE
#define BARRETT_SHIFT       (2 * ML_KEM_LOG2PRIME)
#ifdef SHA3_BLOCKSIZE
# define SHAKE128_BLOCKSIZE  SHA3_BLOCKSIZE(128)
#endif

/*
 * The scalar rejection-sampling buffer size needs to be a multiple of 3, but
 * is otherwise arbitrary, the preferred block size matches the internal buffer
 * size of SHAKE128, avoiding internal buffering and copying in SHAKE128. That
 * block size of (1600 - 256)/8 bytes, or 168, just happens to divide by 3!
 *
 * If the blocksize is unknown, or is not divisible by 3, 168 is used as a
 * fallback.
 */
#if defined(SHAKE128_BLOCKSIZE) && (SHAKE128_BLOCKSIZE) % 3 == 0
# define SCALAR_SAMPLING_BUFSIZE (SHAKE128_BLOCKSIZE)
#else
# define SCALAR_SAMPLING_BUFSIZE 168
#endif

/*
 * Structure of keys
 */
typedef struct ossl_ml_kem_scalar_st {
    /* On every function entry and exit, 0 <= c[i] < ML_KEM_PRIME. */
    uint16_t c[ML_KEM_DEGREE];
} scalar;

/* General form of public and private key storage */
#define DECLARE_ML_KEM_KEYDATA(name, rank, private_sz) \
    struct ossl_ml_kem_##name##_st { \
        /* Public vector |t| */ \
        scalar tbuf[(rank)]; \
        /* Pre-computed matrix |m| */ \
        scalar mbuf[(rank)*(rank)] \
        /* optional private key data */ \
        private_sz \
    }

/* Declare variant-specific public and private storage */
#define DECLARE_ML_KEM_VARIANT_KEYDATA(bits) \
    DECLARE_ML_KEM_KEYDATA(bits##_puballoc, ML_KEM_##bits##_RANK,;); \
    DECLARE_ML_KEM_KEYDATA(bits##_prvalloc, ML_KEM_##bits##_RANK,;\
        scalar sbuf[ML_KEM_##bits##_RANK]; \
        uint8_t zbuf[ML_KEM_RANDOM_BYTES];)
DECLARE_ML_KEM_VARIANT_KEYDATA(512);
DECLARE_ML_KEM_VARIANT_KEYDATA(768);
DECLARE_ML_KEM_VARIANT_KEYDATA(1024);
#undef DECLARE_ML_KEM_VARIANT_KEYDATA
#undef DECLARE_ML_KEM_KEYDATA

typedef const ML_KEM_VINFO *vinfo_t;
typedef __owur
int (*cbd_t)(scalar *out, uint8_t in[ML_KEM_RANDOM_BYTES + 1],
             EVP_MD_CTX *mdctx, const ML_KEM_KEY *key);

/* The wire form of a losslessly encoded vector (12-bits per element). */
#define ML_KEM_VECTOR_BYTES(rank) \
    ((3 * ML_KEM_DEGREE / 2) * (rank))

/*
 * The expanded internal form stores each coefficient as a 16-bit unsigned int
 */
#define ML_KEM_VECALLOC_BYTES(rank) \
    (2 * ML_KEM_DEGREE * (rank))

/*
 * The wire-form public key consists of the lossless encoding of the vector
 * "t" = "A" * "s" + "e", followed by public seed "rho".
 */
#define ML_KEM_PUBKEY_BYTES(rank) \
    (ML_KEM_VECTOR_BYTES(rank) + ML_KEM_RANDOM_BYTES)

/*
 * Our internal serialised private key concatenates serialisations of "s", the
 * public key, the public key hash, and the failure secret "z".
 */
#define ML_KEM_PRVKEY_BYTES(rank) \
    (ML_KEM_VECTOR_BYTES(rank) + ML_KEM_PUBKEY_BYTES(rank) \
     + ML_KEM_PKHASH_BYTES + ML_KEM_RANDOM_BYTES)

/*
 * Encapsulation produces a vector "u" and a scalar "v", whose coordinates
 * (numbers modulo the ML-KEM prime "q") are lossily encoded using as "du" and
 * "dv" bits, respectively.  This encoding is the ciphertext input for
 * decapsulation.
 */
#define ML_KEM_U_VECTOR_BYTES(rank, du) \
    ((ML_KEM_DEGREE / 8) * (du) * (rank))
#define ML_KEM_V_SCALAR_BYTES(dv) \
    ((ML_KEM_DEGREE / 8) * (dv))
#define ML_KEM_CTEXT_BYTES(rank, du, dv) \
    (ML_KEM_U_VECTOR_BYTES(rank, du) + ML_KEM_V_SCALAR_BYTES(dv))

/*
 * Variant-specific sizes
 */
#define ML_KEM_512_VECTOR_BYTES \
    ML_KEM_VECTOR_BYTES(ML_KEM_512_RANK)
#define ML_KEM_768_VECTOR_BYTES \
    ML_KEM_VECTOR_BYTES(ML_KEM_768_RANK)
#define ML_KEM_1024_VECTOR_BYTES \
    ML_KEM_VECTOR_BYTES(ML_KEM_1024_RANK)

#define ML_KEM_512_PUBLIC_KEY_BYTES \
    ML_KEM_PUBKEY_BYTES(ML_KEM_512_RANK)
#define ML_KEM_768_PUBLIC_KEY_BYTES \
    ML_KEM_PUBKEY_BYTES(ML_KEM_768_RANK)
#define ML_KEM_1024_PUBLIC_KEY_BYTES \
    ML_KEM_PUBKEY_BYTES(ML_KEM_1024_RANK)

#define ML_KEM_512_PRIVATE_KEY_BYTES \
    ML_KEM_PRVKEY_BYTES(ML_KEM_512_RANK)
#define ML_KEM_768_PRIVATE_KEY_BYTES \
    ML_KEM_PRVKEY_BYTES(ML_KEM_768_RANK)
#define ML_KEM_1024_PRIVATE_KEY_BYTES \
    ML_KEM_PRVKEY_BYTES(ML_KEM_1024_RANK)

#define ML_KEM_512_U_VECTOR_BYTES \
    ML_KEM_U_VECTOR_BYTES(ML_KEM_512_RANK, ML_KEM_512_DU)
#define ML_KEM_768_U_VECTOR_BYTES \
    ML_KEM_U_VECTOR_BYTES(ML_KEM_768_RANK, ML_KEM_768_DU)
#define ML_KEM_1024_U_VECTOR_BYTES \
    ML_KEM_U_VECTOR_BYTES(ML_KEM_1024_RANK, ML_KEM_1024_DU)

#define ML_KEM_512_V_SCALAR_BYTES \
    ML_KEM_V_SCALAR_BYTES(ML_KEM_512_DV)
#define ML_KEM_768_V_SCALAR_BYTES \
    ML_KEM_V_SCALAR_BYTES(ML_KEM_768_DV)
#define ML_KEM_1024_V_SCALAR_BYTES \
    ML_KEM_V_SCALAR_BYTES(ML_KEM_1024_DV)

#define ML_KEM_512_CIPHERTEXT_BYTES \
    (ML_KEM_512_U_VECTOR_BYTES + ML_KEM_512_V_SCALAR_BYTES)
#define ML_KEM_768_CIPHERTEXT_BYTES \
    (ML_KEM_768_U_VECTOR_BYTES + ML_KEM_768_V_SCALAR_BYTES)
#define ML_KEM_1024_CIPHERTEXT_BYTES \
    (ML_KEM_1024_U_VECTOR_BYTES + ML_KEM_1024_V_SCALAR_BYTES)

/*
 * Per-variant fixed parameters
 */
static const ML_KEM_VINFO vinfo_map[3] = {
    {
        "ML-KEM-512",
        ML_KEM_512_VECTOR_BYTES,
        ML_KEM_512_PRIVATE_KEY_BYTES,
        ML_KEM_512_PUBLIC_KEY_BYTES,
        ML_KEM_512_CIPHERTEXT_BYTES,
        ML_KEM_512_U_VECTOR_BYTES,
        sizeof(struct ossl_ml_kem_512_puballoc_st),
        sizeof(struct ossl_ml_kem_512_prvalloc_st),
        ML_KEM_512,
        512,
        ML_KEM_512_RANK,
        ML_KEM_512_DU,
        ML_KEM_512_DV,
        ML_KEM_512_RNGSEC
    },
    {
        "ML-KEM-768",
        ML_KEM_768_VECTOR_BYTES,
        ML_KEM_768_PRIVATE_KEY_BYTES,
        ML_KEM_768_PUBLIC_KEY_BYTES,
        ML_KEM_768_CIPHERTEXT_BYTES,
        ML_KEM_768_U_VECTOR_BYTES,
        sizeof(struct ossl_ml_kem_768_puballoc_st),
        sizeof(struct ossl_ml_kem_768_prvalloc_st),
        ML_KEM_768,
        768,
        ML_KEM_768_RANK,
        ML_KEM_768_DU,
        ML_KEM_768_DV,
        ML_KEM_768_RNGSEC
    },
    {
        "ML-KEM-1024",
        ML_KEM_1024_VECTOR_BYTES,
        ML_KEM_1024_PRIVATE_KEY_BYTES,
        ML_KEM_1024_PUBLIC_KEY_BYTES,
        ML_KEM_1024_CIPHERTEXT_BYTES,
        ML_KEM_1024_U_VECTOR_BYTES,
        sizeof(struct ossl_ml_kem_1024_puballoc_st),
        sizeof(struct ossl_ml_kem_1024_prvalloc_st),
        ML_KEM_1024,
        1024,
        ML_KEM_1024_RANK,
        ML_KEM_1024_DU,
        ML_KEM_1024_DV,
        ML_KEM_1024_RNGSEC
    }
};

/*
 * Remainders modulo `kPrime`, for sufficiently small inputs, are computed in
 * constant time via Barrett reduction, and a final call to reduce_once(),
 * which reduces inputs that are at most 2*kPrime and is also constant-time.
 */
static const int kPrime = ML_KEM_PRIME;
static const unsigned kBarrettShift = BARRETT_SHIFT;
static const size_t   kBarrettMultiplier = (1 << BARRETT_SHIFT) / ML_KEM_PRIME;
static const uint16_t kHalfPrime = (ML_KEM_PRIME - 1) / 2;
static const uint16_t kInverseDegree = ML_KEM_INVERSE_DEGREE;

/*
 * Python helper:
 *
 * p = 3329
 * def bitreverse(i):
 *     ret = 0
 *     for n in range(7):
 *         bit = i & 1
 *         ret <<= 1
 *         ret |= bit
 *         i >>= 1
 *     return ret
 */

/*-
 * First precomputed array from Appendix A of FIPS 203, or else Python:
 * kNTTRoots = [pow(17, bitreverse(i), p) for i in range(128)]
 */
static const uint16_t kNTTRoots[128] = {
    1,    1729, 2580, 3289, 2642, 630,  1897, 848,  1062, 1919, 193,  797,
    2786, 3260, 569,  1746, 296,  2447, 1339, 1476, 3046, 56,   2240, 1333,
    1426, 2094, 535,  2882, 2393, 2879, 1974, 821,  289,  331,  3253, 1756,
    1197, 2304, 2277, 2055, 650,  1977, 2513, 632,  2865, 33,   1320, 1915,
    2319, 1435, 807,  452,  1438, 2868, 1534, 2402, 2647, 2617, 1481, 648,
    2474, 3110, 1227, 910,  17,   2761, 583,  2649, 1637, 723,  2288, 1100,
    1409, 2662, 3281, 233,  756,  2156, 3015, 3050, 1703, 1651, 2789, 1789,
    1847, 952,  1461, 2687, 939,  2308, 2437, 2388, 733,  2337, 268,  641,
    1584, 2298, 2037, 3220, 375,  2549, 2090, 1645, 1063, 319,  2773, 757,
    2099, 561,  2466, 2594, 2804, 1092, 403,  1026, 1143, 2150, 2775, 886,
    1722, 1212, 1874, 1029, 2110, 2935, 885,  2154,
};

/* InverseNTTRoots = [pow(17, -bitreverse(i), p) for i in range(128)] */
static const uint16_t kInverseNTTRoots[128] = {
    1,    1600, 40,   749,  2481, 1432, 2699, 687,  1583, 2760, 69,   543,
    2532, 3136, 1410, 2267, 2508, 1355, 450,  936,  447,  2794, 1235, 1903,
    1996, 1089, 3273, 283,  1853, 1990, 882,  3033, 2419, 2102, 219,  855,
    2681, 1848, 712,  682,  927,  1795, 461,  1891, 2877, 2522, 1894, 1010,
    1414, 2009, 3296, 464,  2697, 816,  1352, 2679, 1274, 1052, 1025, 2132,
    1573, 76,   2998, 3040, 1175, 2444, 394,  1219, 2300, 1455, 2117, 1607,
    2443, 554,  1179, 2186, 2303, 2926, 2237, 525,  735,  863,  2768, 1230,
    2572, 556,  3010, 2266, 1684, 1239, 780,  2954, 109,  1292, 1031, 1745,
    2688, 3061, 992,  2596, 941,  892,  1021, 2390, 642,  1868, 2377, 1482,
    1540, 540,  1678, 1626, 279,  314,  1173, 2573, 3096, 48,   667,  1920,
    2229, 1041, 2606, 1692, 680,  2746, 568,  3312,
};

/*
 * Second precomputed array from Appendix A of FIPS 203 (normalised positive),
 * or else Python:
 * ModRoots = [pow(17, 2*bitreverse(i) + 1, p) for i in range(128)]
 */
static const uint16_t kModRoots[128] = {
    17,   3312, 2761, 568,  583,  2746, 2649, 680,  1637, 1692, 723,  2606,
    2288, 1041, 1100, 2229, 1409, 1920, 2662, 667,  3281, 48,   233,  3096,
    756,  2573, 2156, 1173, 3015, 314,  3050, 279,  1703, 1626, 1651, 1678,
    2789, 540,  1789, 1540, 1847, 1482, 952,  2377, 1461, 1868, 2687, 642,
    939,  2390, 2308, 1021, 2437, 892,  2388, 941,  733,  2596, 2337, 992,
    268,  3061, 641,  2688, 1584, 1745, 2298, 1031, 2037, 1292, 3220, 109,
    375,  2954, 2549, 780,  2090, 1239, 1645, 1684, 1063, 2266, 319,  3010,
    2773, 556,  757,  2572, 2099, 1230, 561,  2768, 2466, 863,  2594, 735,
    2804, 525,  1092, 2237, 403,  2926, 1026, 2303, 1143, 2186, 2150, 1179,
    2775, 554,  886,  2443, 1722, 1607, 1212, 2117, 1874, 1455, 1029, 2300,
    2110, 1219, 2935, 394,  885,  2444, 2154, 1175,
};

/*
 * single_keccak hashes |inlen| bytes from |in| and writes |outlen| bytes of
 * output to |out|. If the |md| specifies a fixed-output function, like
 * SHA3-256, then |outlen| must be the correct length for that function.
 */
static __owur
int single_keccak(uint8_t *out, size_t outlen, const uint8_t *in, size_t inlen,
                  EVP_MD_CTX *mdctx)
{
    unsigned int sz = (unsigned int) outlen;

    if (!EVP_DigestUpdate(mdctx, in, inlen))
        return 0;
    if (EVP_MD_xof(EVP_MD_CTX_get0_md(mdctx)))
        return EVP_DigestFinalXOF(mdctx, out, outlen);
    return EVP_DigestFinal_ex(mdctx, out, &sz) && (size_t) sz == outlen;
}

/*
 * FIPS 203, Section 4.1, equation (4.3): PRF_eta. Takes 32+1 input bytes, and
 * uses SHAKE256 produce the input to SamplePolyCBD_eta: FIPS 203, algorithm 8.
 */
static __owur
int prf(uint8_t *out, size_t len, const uint8_t in[ML_KEM_RANDOM_BYTES + 1],
        EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    return EVP_DigestInit_ex(mdctx, key->shake256_md, NULL) &&
        single_keccak(out, len, in, ML_KEM_RANDOM_BYTES + 1, mdctx);
}

/*
 * FIPS 203, Section 4.1, equation (4.4): H.  SHA3-256 hash of a variable
 * length input, producing 32 bytes of output.
 */
static __owur
int hash_h(uint8_t out[ML_KEM_PKHASH_BYTES], const uint8_t *in, size_t len,
           EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    return EVP_DigestInit_ex(mdctx, key->sha3_256_md, NULL) &&
        single_keccak(out, ML_KEM_PKHASH_BYTES, in, len, mdctx);
}

/*
 * FIPS 203, Section 4.1, equation (4.5): G.  SHA3-512 hash of a variable
 * length input, producing 64 bytes of output, in particular the seeds
 * (d,z) for key generation.
 */
static __owur
int hash_g(uint8_t out[ML_KEM_SEED_BYTES], const uint8_t *in, size_t len,
           EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    return EVP_DigestInit_ex(mdctx, key->sha3_512_md, NULL) &&
        single_keccak(out, ML_KEM_SEED_BYTES, in, len, mdctx);
}

/*
 * FIPS 203, Section 4.1, equation (4.4): J. SHAKE256 taking a variable length
 * input to compute a 32-byte implicit rejection shared secret, of the same
 * length as the expected shared secret.  (Computed even on success to avoid
 * side-channel leaks).
 */
static __owur
int kdf(uint8_t out[ML_KEM_SHARED_SECRET_BYTES],
        const uint8_t z[ML_KEM_RANDOM_BYTES],
        const uint8_t *ctext, size_t len,
        EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    return EVP_DigestInit_ex(mdctx, key->shake256_md, NULL)
        && EVP_DigestUpdate(mdctx, z, ML_KEM_RANDOM_BYTES)
        && EVP_DigestUpdate(mdctx, ctext, len)
        && EVP_DigestFinalXOF(mdctx, out, ML_KEM_SHARED_SECRET_BYTES);
}

/*
 * FIPS 203, Section 4.2.2, Algorithm 7: "SampleNTT" (steps 3-17, steps 1, 2
 * are performed by the caller). Rejection-samples a Keccak stream to get
 * uniformly distributed elements in the range [0,q). This is used for matrix
 * expansion and only operates on public inputs.
 */
static __owur
int sample_scalar(scalar *out, EVP_MD_CTX *mdctx)
{
    int done = 0;
    uint8_t block[SCALAR_SAMPLING_BUFSIZE], *buf;
    uint8_t *end = block + sizeof(block);
    uint16_t d1, d2;

    while (done < DEGREE) {
        if (!EVP_DigestSqueeze(mdctx, block, sizeof(block)))
            return 0;
        /* Unrolled loop: three bytes in, two 12-bit *candidates* out */
        for (buf = block; done < DEGREE && buf < end;) {
            uint8_t b0 = *buf++;
            uint8_t b1 = *buf++;
            uint8_t b2 = *buf++;

            d1 = ((b1 & 0x0f) << 8) + b0;
            if (d1 < kPrime)
                out->c[done++] = d1;
            d2 = (b2 << 4) + (b1 >> 4);
            if (d2 < kPrime && done < DEGREE)
                out->c[done++] = d2;
        }
    }
    return 1;
}

/* reduce_once reduces 0 <= x < 2*kPrime, mod kPrime. */
static __owur uint16_t reduce_once(uint16_t x)
{
    const uint16_t subtracted = x - kPrime;
    uint16_t mask = 0u - (subtracted >> 15);

    assert(x < 2 * kPrime);
    /*
     * On Aarch64, omitting a |value_barrier_u16| results in a 2x speedup of
     * ML-KEM overall and Clang still produces constant-time code using `csel`.
     */
    return (mask & x) | (~mask & subtracted);
}

/*
 * Constant-time reduce x mod kPrime using Barrett reduction. x must be less
 * than kPrime + 2 * kPrime^2.  This is sufficient to reduce a product of
 * two already reduced u_int16 values, in fact it is sufficient for each
 * to be less than 2^12, because (kPrime * (2 * kPrime + 1)) > 2^24.
 */
static __owur uint16_t reduce(uint32_t x)
{
    uint64_t product = (uint64_t)x * kBarrettMultiplier;
    uint32_t quotient = (uint32_t)(product >> kBarrettShift);
    uint32_t remainder = x - quotient * kPrime;

    assert(x < kPrime + 2u * kPrime * kPrime);
    return reduce_once(remainder);
}

/*-
 * FIPS 203, Section 4.3, Algoritm 9: "NTT".
 * In-place number theoretic transform of a given scalar.  Note that ML-KEM's
 * kPrime 3329 does not have a 512th root of unity, so this transform leaves
 * off the last iteration of the usual FFT code, with the 128 relevant roots of
 * unity being stored in NTTRoots.  This means the output should be seen as 128
 * elements in GF(3329^2), with the coefficients of the elements being
 * consecutive entries in |s->c|.
 */
static void scalar_ntt(scalar *s)
{
    int offset = DEGREE;
    int k, step, i, j;
    uint32_t step_root;
    uint16_t odd, even;

    /*
     * `int` is used here because using `size_t` throughout caused a ~5%
     * slowdown with Clang 14 on Aarch64.
     */
    for (step = 1; step < DEGREE / 2; step <<= 1) {
        offset >>= 1;
        k = 0;
        for (i = 0; i < step; i++) {
            step_root = kNTTRoots[i + step];
            for (j = k; j < k + offset; j++) {
                odd = reduce(step_root * s->c[j + offset]);
                even = s->c[j];
                s->c[j] = reduce_once(odd + even);
                s->c[j + offset] = reduce_once(even - odd + kPrime);
            }
            k += 2 * offset;
        }
    }
}

/*-
 * FIPS 203, Section 4.3, Algoritm 10: "NTT^(-1)".
 * In-place inverse number theoretic transform of a given scalar, with pairs of
 * entries of s->v being interpreted as elements of GF(3329^2). Just as with
 * the number theoretic transform, this leaves off the first step of the normal
 * iFFT to account for the fact that 3329 does not have a 512th root of unity,
 * using the precomputed 128 roots of unity stored in InverseNTTRoots.
 *
 * FIPS 203, Algorithm 10, performs this transformation in a slightly different
 * manner, using the same NTTRoots table as the forward NTT transform.
 */
static void scalar_inverse_ntt(scalar *s)
{
    int step = DEGREE / 2;
    int offset, k, i, j;
    uint32_t step_root;
    uint16_t odd, even;

    /*
     * `int` is used here because using `size_t` throughout caused a ~5%
     * slowdown with Clang 14 on Aarch64.
     */
    for (offset = 2; offset < DEGREE; offset <<= 1) {
        step >>= 1;
        k = 0;
        for (i = 0; i < step; i++) {
            step_root = kInverseNTTRoots[i + step];
            for (j = k; j < k + offset; j++) {
                odd = s->c[j + offset];
                even = s->c[j];
                s->c[j] = reduce_once(odd + even);
                s->c[j + offset] = reduce(step_root * (even - odd + kPrime));
            }
            k += 2 * offset;
        }
    }
    for (i = 0; i < DEGREE; i++)
        s->c[i] = reduce(s->c[i] * kInverseDegree);
}

/* Addition updating the LHS scalar in-place. */
static void scalar_add(scalar *lhs, const scalar *rhs)
{
    int i;

    for (i = 0; i < DEGREE; i++)
        lhs->c[i] = reduce_once(lhs->c[i] + rhs->c[i]);
}

/* Subtraction updating the LHS scalar in-place. */
static void scalar_sub(scalar *lhs, const scalar *rhs)
{
    int i;

    for (i = 0; i < DEGREE; i++)
        lhs->c[i] = reduce_once(lhs->c[i] - rhs->c[i] + kPrime);
}

/*
 * Multiplying two scalars in the number theoretically transformed state. Since
 * 3329 does not have a 512th root of unity, this means we have to interpret
 * the 2*ith and (2*i+1)th entries of the scalar as elements of
 * GF(3329)[X]/(X^2 - 17^(2*bitreverse(i)+1)).
 *
 * The value of 17^(2*bitreverse(i)+1) mod 3329 is stored in the precomputed
 * ModRoots table. Note that our Barrett transform only allows us to multipy
 * two reduced numbers together, so we need some intermediate reduction steps,
 * even if an uint64_t could hold 3 multiplied numbers.
 */
static void scalar_mult(scalar *out, const scalar *lhs,
                        const scalar *rhs)
{
    int i;
    uint32_t real_real, img_img, real_img, img_real;

    for (i = 0; i < DEGREE / 2; i++) {
        real_real = (uint32_t)lhs->c[2 * i] * rhs->c[2 * i];
        img_img = (uint32_t)lhs->c[2 * i + 1] * rhs->c[2 * i + 1];
        real_img = (uint32_t)lhs->c[2 * i] * rhs->c[2 * i + 1];
        img_real = (uint32_t)lhs->c[2 * i + 1] * rhs->c[2 * i];
        out->c[2 * i] =
            reduce(real_real +
                   (uint32_t)reduce(img_img) * kModRoots[i]);
        out->c[2 * i + 1] = reduce(img_real + real_img);
    }
}

static ossl_inline
void scalar_mult_add(scalar *out, const scalar *lhs,
                     const scalar *rhs)
{
    scalar product;

    scalar_mult(&product, lhs, rhs);
    scalar_add(out, &product);
}

static const uint8_t kMasks[8] = {0x01, 0x03, 0x07, 0x0f,
                                  0x1f, 0x3f, 0x7f, 0xff};

/*-
 * FIPS 203, Section 4.2.1, Algorithm 5: "ByteEncode_d", for 2<=d<12.
 * Here |bits| is |d|.  For efficiency, we handle the d=1, and d=12 cases
 * separately.
 */
static void scalar_encode(uint8_t *out, const scalar *s, int bits)
{
    uint8_t out_byte = 0;
    int out_byte_bits = 0;
    int i, element_bits_done, chunk_bits, out_bits_remaining;
    uint16_t element;

    assert(bits < 12 && bits > 1);
    for (i = 0; i < DEGREE; i++) {
        element = s->c[i];
        element_bits_done = 0;
        while (element_bits_done < bits) {
            chunk_bits = bits - element_bits_done;
            out_bits_remaining = 8 - out_byte_bits;
            if (chunk_bits >= out_bits_remaining) {
                chunk_bits = out_bits_remaining;
                out_byte |= (element & kMasks[chunk_bits - 1]) << out_byte_bits;
                *out++ = out_byte;
                out_byte_bits = 0;
                out_byte = 0;
            } else {
                out_byte |= (element & kMasks[chunk_bits - 1]) << out_byte_bits;
                out_byte_bits += chunk_bits;
            }
            element_bits_done += chunk_bits;
            element >>= chunk_bits;
        }
    }
    if (out_byte_bits > 0)
        *out = out_byte;
}

/*
 * scalar_encode_12 is |scalar_encode| specialised for |bits| == 12.
 */
static void scalar_encode_12(uint8_t out[3 * DEGREE / 2], const scalar *s)
{
    const uint16_t *c = s->c;
    int i;

    for (i = 0; i < DEGREE / 2; ++i) {
        uint16_t c1 = *c++;
        uint16_t c2 = *c++;

        *out++ = (uint8_t) c1;
        *out++ = (uint8_t) (((c1 >> 8) & 0x0f) | ((c2 & 0x0f) << 4));
        *out++ = (uint8_t) (c2 >> 4);
    }
}

/*
 * scalar_encode_1 is |scalar_encode| specialised for |bits| == 1.
 */
static void scalar_encode_1(uint8_t out[DEGREE / 8], const scalar *s)
{
    int i, j;
    uint8_t out_byte;

    for (i = 0; i < DEGREE; i += 8) {
        out_byte = 0;
        for (j = 0; j < 8; j++)
            out_byte |= (s->c[i + j] & 1) << j;
        *out = out_byte;
        out++;
    }
}

/*-
 * FIPS 203, Section 4.2.1, Algorithm 6: "ByteDecode_d", for 2<=d<12.
 * Here |bits| is |d|.  For efficiency, we handle the d=1 and d=12 cases
 * separately.
 *
 * scalar_decode parses |DEGREE * bits| bits from |in| into |DEGREE| values in
 * |out|. It returns one on success and zero if any parsed value is >=
 * |kPrime|.
 *
 * Note: Used in decrypt_cpa(), which returns void and so does not check the
 * return value of this function.  But also used in vector_decode(), which
 * returns early when scalar_decode() fails.
 */
static int scalar_decode(scalar *out, const uint8_t *in, int bits)
{
    uint8_t in_byte = 0;
    int in_byte_bits_left = 0;
    int i, element_bits_done, chunk_bits;
    uint16_t element;

    if (!ossl_assert(bits < 12 && bits > 1))
        return 0;
    for (i = 0; i < DEGREE; i++) {
        element = 0;
        element_bits_done = 0;
        while (element_bits_done < bits) {
            if (in_byte_bits_left == 0) {
                in_byte = *in;
                in++;
                in_byte_bits_left = 8;
            }
            chunk_bits = bits - element_bits_done;
            if (chunk_bits > in_byte_bits_left)
                chunk_bits = in_byte_bits_left;
            element |= (in_byte & kMasks[chunk_bits - 1]) << element_bits_done;
            in_byte_bits_left -= chunk_bits;
            in_byte >>= chunk_bits;
            element_bits_done += chunk_bits;
        }
        if (element >= kPrime)
            return 0;
        out->c[i] = element;
    }
    return 1;
}

static __owur
int scalar_decode_12(scalar *out, const uint8_t in[3 * DEGREE / 2])
{
    int i;
    uint16_t *c = out->c;

    for (i = 0; i < DEGREE / 2; ++i) {
        uint8_t b1 = *in++;
        uint8_t b2 = *in++;
        uint8_t b3 = *in++;

        if ((*c++ = b1 | ((b2 & 0x0f) << 8)) >= kPrime
            || (*c++ = (b2 >> 4) | (b3 << 4)) >= kPrime)
            return 0;
    }
    return 1;
}

/* scalar_decode_1 is |scalar_decode| specialised for |bits| == 1. */
static void scalar_decode_1(scalar *out, const uint8_t in[DEGREE / 8])
{
    int i, j;
    uint8_t in_byte;

    for (i = 0; i < DEGREE; i += 8) {
        in_byte = *in;
        in++;
        for (j = 0; j < 8; j++) {
            out->c[i + j] = in_byte & 1;
            in_byte >>= 1;
        }
    }
}

/*
 * FIPS 203, Section 4.2.1, Equation (4.7): Compress_d.
 *
 * Compresses (lossily) an input |x| mod 3329 into |bits| many bits by grouping
 * numbers close to each other together. The formula used is
 * round(2^|bits|/kPrime*x) mod 2^|bits|.
 * Uses Barrett reduction to achieve constant time. Since we need both the
 * remainder (for rounding) and the quotient (as the result), we cannot use
 * |reduce| here, but need to do the Barrett reduction directly.
 */
static __owur uint16_t compress(uint16_t x, int bits)
{
    uint32_t shifted = (uint32_t)x << bits;
    uint64_t product = (uint64_t)shifted * kBarrettMultiplier;
    uint32_t quotient = (uint32_t)(product >> kBarrettShift);
    uint32_t remainder = shifted - quotient * kPrime;

    /*
     * Adjust the quotient to round correctly:
     *   0 <= remainder <= kHalfPrime round to 0
     *   kHalfPrime < remainder <= kPrime + kHalfPrime round to 1
     *   kPrime + kHalfPrime < remainder < 2 * kPrime round to 2
     */
    assert(remainder < 2u * kPrime);
    quotient += 1 & constant_time_lt_32(kHalfPrime, remainder);
    quotient += 1 & constant_time_lt_32(kPrime + kHalfPrime, remainder);
    return quotient & ((1 << bits) - 1);
}

/*
 * FIPS 203, Section 4.2.1, Equation (4.8): Decompress_d.

 * Decompresses |x| by using a close equi-distant representative. The formula
 * is round(kPrime/2^|bits|*x). Note that 2^|bits| being the divisor allows us
 * to implement this logic using only bit operations.
 */
static __owur uint16_t decompress(uint16_t x, int bits)
{
    uint32_t product = (uint32_t)x * kPrime;
    uint32_t power = 1 << bits;
    /* This is |product| % power, since |power| is a power of 2. */
    uint32_t remainder = product & (power - 1);
    /* This is |product| / power, since |power| is a power of 2. */
    uint32_t lower = product >> bits;

    /*
     * The rounding logic works since the first half of numbers mod |power|
     * have a 0 as first bit, and the second half has a 1 as first bit, since
     * |power| is a power of 2. As a 12 bit number, |remainder| is always
     * positive, so we will shift in 0s for a right shift.
     */
    return lower + (remainder >> (bits - 1));
}

/*-
 * FIPS 203, Section 4.2.1, Equation (4.7): "Compress_d".
 * In-place lossy rounding of scalars to 2^d bits.
 */
static void scalar_compress(scalar *s, int bits)
{
    int i;

    for (i = 0; i < DEGREE; i++)
        s->c[i] = compress(s->c[i], bits);
}

/*
 * FIPS 203, Section 4.2.1, Equation (4.8): "Decompress_d".
 * In-place approximate recovery of scalars from 2^d bit compression.
 */
static void scalar_decompress(scalar *s, int bits)
{
    int i;

    for (i = 0; i < DEGREE; i++)
        s->c[i] = decompress(s->c[i], bits);
}

/* Addition updating the LHS vector in-place. */
static void vector_add(scalar *lhs, const scalar *rhs, int rank)
{
    while (rank-- > 0)
        scalar_add(lhs++, rhs++);
}

/*
 * Encodes an entire vector into 32*|rank|*|bits| bytes. Note that since 256
 * (DEGREE) is divisible by 8, the individual vector entries will always fill a
 * whole number of bytes, so we do not need to worry about bit packing here.
 */
static void vector_encode(uint8_t *out, const scalar *a, int bits, int rank)
{
    int stride = bits * DEGREE / 8;

    for (; rank-- > 0; out += stride)
        scalar_encode(out, a++, bits);
}

/*
 * Decodes 32*|rank|*|bits| bytes from |in| into |out|. It returns one on
 * success or zero if any parsed value is >= |ML_KEM_PRIME|.
 *
 * Note: Used only in decrypt_cpa(), which returns void and so does not check
 * the return value of this function.  Side-channels are fine when the input
 * ciphertext to decap() is simply syntactically invalid.
 */
static void vector_decode(scalar *out, const uint8_t *in, int bits, int rank)
{
    int stride = bits * DEGREE / 8;

    for (; rank-- > 0; in += stride)
        if (!scalar_decode(out++, in, bits))
            return;
}

/* vector_encode(), specialised to bits == 12. */
static void vector_encode_12(uint8_t out[3 * DEGREE / 2], const scalar *a,
                             int rank)
{
    int stride = 3 * DEGREE / 2;

    for (; rank-- > 0; out += stride)
        scalar_encode_12(out, a++);
}

/* vector_decode(), specialised to bits == 12. */
static __owur
int vector_decode_12(scalar *out, const uint8_t in[3 * DEGREE / 2], int rank)
{
    int stride = 3 * DEGREE / 2;

    for (; rank-- > 0; in += stride)
        if (!scalar_decode_12(out++, in))
            return 0;
    return 1;
}

/* In-place compression of each scalar component */
static void vector_compress(scalar *a, int bits, int rank)
{
    while (rank-- > 0)
        scalar_compress(a++, bits);
}

/* In-place decompression of each scalar component */
static void vector_decompress(scalar *a, int bits, int rank)
{
    while (rank-- > 0)
        scalar_decompress(a++, bits);
}

/* The output scalar must not overlap with the inputs */
static void inner_product(scalar *out, const scalar *lhs, const scalar *rhs,
                          int rank)
{
    scalar_mult(out, lhs, rhs);
    while (--rank > 0)
        scalar_mult_add(out, ++lhs, ++rhs);
}

/* In-place NTT transform of a vector */
static void vector_ntt(scalar *a, int rank)
{
    while (rank-- > 0)
        scalar_ntt(a++);
}

/* In-place inverse NTT transform of a vector */
static void vector_inverse_ntt(scalar *a, int rank)
{
    while (rank-- > 0)
        scalar_inverse_ntt(a++);
}

/* Here, the output vector must not overlap with the inputs */
static void
matrix_mult(scalar *out, const scalar *m, const scalar *a, int rank)
{
    const scalar *ar;
    int i, j;

    for (i = rank; i-- > 0; ++out) {
        scalar_mult(out, m++, ar = a);
        for (j = rank - 1; j > 0; --j)
            scalar_mult_add(out, m++, ++ar);
    }
}

/* Here, the output vector must not overlap with the inputs */
static void
matrix_mult_transpose(scalar *out, const scalar *m, const scalar *a, int rank)
{
    const scalar *mc = m, *mr, *ar;
    int i, j;

    for (i = rank; i-- > 0; ++out)  {
        scalar_mult(out, mr = mc++, ar = a);
        for (j = rank; --j > 0; )
            scalar_mult_add(out, (mr += rank), ++ar);
    }
}

/*-
 * Expands the matrix from a seed for key generation and for encaps-CPA.
 * NOTE: FIPS 203 matrix "A" is the transpose of this matrix, computed
 * by appending the (i,j) indices to the seed in the opposite order!
 *
 * Where FIPS 203 computes t = A * s + e, we use the transpose of "m".
 */
static __owur
int matrix_expand(EVP_MD_CTX *mdctx, ML_KEM_KEY *key)
{
    scalar *out = key->m;
    uint8_t input[ML_KEM_RANDOM_BYTES + 2];
    int rank = key->vinfo->rank;
    int i, j;

    memcpy(input, key->rho, ML_KEM_RANDOM_BYTES);
    for (i = 0; i < rank; i++) {
        for (j = 0; j < rank; j++) {
            input[ML_KEM_RANDOM_BYTES] = i;
            input[ML_KEM_RANDOM_BYTES + 1] = j;
            if (!EVP_DigestInit_ex(mdctx, key->shake128_md, NULL)
                || !EVP_DigestUpdate(mdctx, input, sizeof(input))
                || !sample_scalar(out++, mdctx))
                return 0;
        }
    }
    return 1;
}

/*
 * Algorithm 7 from the spec, with eta fixed to two and the PRF call
 * included. Creates binominally distributed elements by sampling 2*|eta| bits,
 * and setting the coefficient to the count of the first bits minus the count of
 * the second bits, resulting in a centered binomial distribution. Since eta is
 * two this gives -2/2 with a probability of 1/16, -1/1 with probability 1/4,
 * and 0 with probability 3/8.
 */
static __owur
int cbd_2(scalar *out, uint8_t in[ML_KEM_RANDOM_BYTES + 1],
          EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    uint8_t randbuf[2 * 2 * DEGREE / 8];    /* 64 * eta */
    int i;
    uint8_t byte;
    uint16_t value;

    if (!prf(randbuf, sizeof(randbuf), in, mdctx, key))
        return 0;
    for (i = 0; i < DEGREE; i += 2) {
        byte = randbuf[i / 2];
        value = kPrime;
        value += (byte & 1) + ((byte >> 1) & 1);
        value -= ((byte >> 2) & 1) + ((byte >> 3) & 1);
        out->c[i] = reduce_once(value);
        byte >>= 4;
        value = kPrime;
        value += (byte & 1) + ((byte >> 1) & 1);
        value -= ((byte >> 2) & 1) + ((byte >> 3) & 1);
        out->c[i + 1] = reduce_once(value);
    }
    return 1;
}

/*
 * Algorithm 7 from the spec, with eta fixed to three and the PRF call
 * included. Creates binominally distributed elements by sampling 3*|eta| bits,
 * and setting the coefficient to the count of the first bits minus the count of
 * the second bits, resulting in a centered binomial distribution.
 */
static __owur
int cbd_3(scalar *out, uint8_t in[ML_KEM_RANDOM_BYTES + 1],
          EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    uint8_t randbuf[6 * DEGREE / 8];    /* 64 * eta */
    int i = 0, j = 0;
    uint8_t b1, b2, b3;
    uint16_t value;

#define bit0(b) (b & 1)
#define bitn(n, b) ((b >> n) & 1)

    if (!prf(randbuf, sizeof(randbuf), in, mdctx, key))
        return 0;
    /* Unrolled loop uses 3 bytes at a time, yielding 4 values (6 bits each) */
    while (j < (int) sizeof(randbuf)) {
        b1 = randbuf[j++];
        b2 = randbuf[j++];
        b3 = randbuf[j++];

        value = kPrime + bit0(b1) + bitn(1, b1) + bitn(2, b1);
        value -= bitn(3, b1)  + bitn(4, b1) + bitn(5, b1);
        out->c[i++] = reduce_once(value);

        value = kPrime + bitn(6, b1) + bitn(7, b1) + bit0(b2);
        value -= bitn(1, b2) + bitn(2, b2) + bitn(3, b2);
        out->c[i++] = reduce_once(value);

        value = kPrime + bitn(4, b2) + bitn(5, b2) + bitn(6, b2);
        value -= bitn(7, b2) + bit0(b3) + bitn(1, b3);
        out->c[i++] = reduce_once(value);

        value = kPrime + bitn(2, b3) + bitn(3, b3) + bitn(4, b3);
        value -= bitn(5, b3) + bitn(6, b3) + bitn(7, b3);
        out->c[i++] = reduce_once(value);
    }
#undef bit0
#undef bitn

    return 1;
}

/*
 * Generates a secret vector by using |cbd| with the given seed to generate
 * scalar elements and incrementing |counter| for each slot of the vector.
 */
static __owur
int gencbd_vector(scalar *out, cbd_t cbd, uint8_t *counter,
                  const uint8_t seed[ML_KEM_RANDOM_BYTES], int rank,
                  EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    uint8_t input[ML_KEM_RANDOM_BYTES + 1];

    memcpy(input, seed, ML_KEM_RANDOM_BYTES);
    while (rank-- > 0) {
        input[ML_KEM_RANDOM_BYTES] = (*counter)++;
        if (!cbd(out++, input, mdctx, key))
            return 0;
    }
    return 1;
}

/* The |ETA1| value for ML-KEM-512 is 3, the rest and all ETA2 values are 2. */
static cbd_t const cbd1[ML_KEM_1024 + 1] = { cbd_3, cbd_2, cbd_2 };

/*
 * FIPS 203, Section 5.2, Algorithm 14: K-PKE.Encrypt.
 *
 * Encrypts a message with given randomness to the ciphertext in |out|. Without
 * applying the Fujisaki-Okamoto transform this would not result in a CCA
 * secure scheme, since lattice schemes are vulnerable to decryption failure
 * oracles.
 *
 * The steps are re-ordered to make more efficient/localised use of storage.
 *
 * Note also that the input public key is assumed to hold a precomputed matrix
 * |A| (our key->m, with the public key holding an expanded (16-bit per scalar
 * coefficient) key->t vector.
 *
 * Caller passes storage in |tmp| for for two temporary vectors.
 */
static __owur
int encrypt_cpa(uint8_t out[ML_KEM_SHARED_SECRET_BYTES],
                const uint8_t message[DEGREE / 8],
                const uint8_t r[ML_KEM_RANDOM_BYTES], scalar *tmp,
                EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    vinfo_t vinfo = key->vinfo;
    cbd_t cbd_1 = cbd1[vinfo->variant];
    int rank = vinfo->rank;
    /* We can use tmp[0..rank-1] as storage for |y|, then |e1|, ... */
    scalar *y = &tmp[0], *e1 = y, *e2 = y, *mu = y;
    /* We can use tmp[rank]..tmp[2*rank - 1] for |u| */
    scalar *u = &tmp[rank];
    scalar v;
    uint8_t input[ML_KEM_RANDOM_BYTES + 1];
    uint8_t counter = 0;
    int du = vinfo->du;
    int dv = vinfo->dv;

    /* FIPS 203 "y" vector */
    if (!gencbd_vector(y, cbd_1, &counter, r, rank, mdctx, key))
        return 0;
    vector_ntt(y, rank);
    /* FIPS 203 "v" scalar */
    inner_product(&v, key->t, y, rank);
    scalar_inverse_ntt(&v);
    /* FIPS 203 "u" vector */
    matrix_mult(u, key->m, y, rank);
    vector_inverse_ntt(u, rank);

    /* All done with |y|, now free to reuse tmp[0] for FIPS 203 |e1| */
    if (!gencbd_vector(e1, cbd_2, &counter, r, rank, mdctx, key))
        return 0;
    vector_add(u, e1, rank);
    vector_compress(u, du, rank);
    vector_encode(out, u, du, rank);

    /* All done with |e1|, now free to reuse tmp[0] for FIPS 203 |e2| */
    memcpy(input, r, ML_KEM_RANDOM_BYTES);
    input[ML_KEM_RANDOM_BYTES] = counter;
    if (!cbd_2(e2, input, mdctx, key))
        return 0;
    scalar_add(&v, e2);

    /* All Done with |e2|, now free to reuse tmp[0] for FIPS 203 |mu| */
    scalar_decode_1(mu, message);
    scalar_decompress(mu, 1);
    scalar_add(&v, mu);
    scalar_compress(&v, dv);
    scalar_encode(out + vinfo->u_vector_bytes, &v, dv);
    return 1;
}

/*
 * FIPS 203, Section 5.3, Algorithm 15: K-PKE.Decrypt.
 */
static void
decrypt_cpa(uint8_t out[ML_KEM_SHARED_SECRET_BYTES],
            const uint8_t *ctext, scalar *u, const ML_KEM_KEY *key)
{
    vinfo_t vinfo = key->vinfo;
    scalar v, mask;
    int rank = vinfo->rank;
    int du = vinfo->du;
    int dv = vinfo->dv;

    vector_decode(u, ctext, du, rank);
    vector_decompress(u, du, rank);
    vector_ntt(u, rank);
    scalar_decode(&v, ctext + vinfo->u_vector_bytes, dv);
    scalar_decompress(&v, dv);
    inner_product(&mask, key->s, u, rank);
    scalar_inverse_ntt(&mask);
    scalar_sub(&v, &mask);
    scalar_compress(&v, 1);
    scalar_encode_1(out, &v);
}

/*-
 * FIPS 203, Section 7.1, Algorithm 19: "ML-KEM.KeyGen".
 * FIPS 203, Section 7.2, Algorithm 20: "ML-KEM.Encaps".
 *
 * Fills the |out| buffer with the |ek| output of "ML-KEM.KeyGen", or,
 * equivalently, the |ek| input of "ML-KEM.Encaps", i.e. returns the
 * wire-format of an ML-KEM public key.
 */
static void encode_pubkey(uint8_t *out, const ML_KEM_KEY *key)
{
    const uint8_t *rho = key->rho;
    vinfo_t vinfo = key->vinfo;

    vector_encode_12(out, key->t, vinfo->rank);
    memcpy(out + vinfo->vector_bytes, rho, ML_KEM_RANDOM_BYTES);
}

/*-
 * FIPS 203, Section 7.1, Algorithm 19: "ML-KEM.KeyGen".
 *
 * Fills the |out| buffer with the |dk| output of "ML-KEM.KeyGen".
 * This matches the input format of parse_prvkey() below.
 */
static void encode_prvkey(uint8_t *out, const ML_KEM_KEY *key)
{
    vinfo_t vinfo = key->vinfo;

    vector_encode_12(out, key->s, vinfo->rank);
    out += vinfo->vector_bytes;
    encode_pubkey(out, key);
    out += vinfo->pubkey_bytes;
    memcpy(out, key->pkhash, ML_KEM_PKHASH_BYTES);
    out += ML_KEM_PKHASH_BYTES;
    memcpy(out, key->z, ML_KEM_RANDOM_BYTES);
}

/*-
 * FIPS 203, Section 7.1, Algorithm 19: "ML-KEM.KeyGen".
 * FIPS 203, Section 7.2, Algorithm 20: "ML-KEM.Encaps".
 *
 * This function parses the |in| buffer as the |ek| output of "ML-KEM.KeyGen",
 * or, equivalently, the |ek| input of "ML-KEM.Encaps", i.e. decodes the
 * wire-format of the ML-KEM public key.
 */
static int parse_pubkey(const uint8_t *in, EVP_MD_CTX *mdctx, ML_KEM_KEY *key)
{
    vinfo_t vinfo = key->vinfo;

    /* Decode and check |t| */
    if (!vector_decode_12(key->t, in, vinfo->rank))
        return 0;
    /* Save the matrix |m| recovery seed |rho| */
    memcpy(key->rho, in + vinfo->vector_bytes, ML_KEM_RANDOM_BYTES);
    /*
     * Pre-compute the public key hash, needed for both encap and decap.
     * Also pre-compute the matrix expansion, stored with the public key.
     */
    return hash_h(key->pkhash, in, vinfo->pubkey_bytes, mdctx, key)
        && matrix_expand(mdctx, key);
}

/*
 * FIPS 203, Section 7.1, Algorithm 19: "ML-KEM.KeyGen".
 *
 * Parses the |in| buffer as a |dk| output of "ML-KEM.KeyGen".
 * This matches the output format of encode_prvkey() above.
 */
static int parse_prvkey(const uint8_t *in, EVP_MD_CTX *mdctx, ML_KEM_KEY *key)
{
    vinfo_t vinfo = key->vinfo;

    /* Decode and check |s|. */
    if (!vector_decode_12(key->s, in, vinfo->rank))
        return 0;
    in += vinfo->vector_bytes;

    if (!parse_pubkey(in, mdctx, key))
        return 0;
    in += vinfo->pubkey_bytes;

    /* Check public key hash. */
    if (memcmp(key->pkhash, in, ML_KEM_PKHASH_BYTES) != 0)
        return 0;
    in += ML_KEM_PKHASH_BYTES;

    memcpy(key->z, in, ML_KEM_RANDOM_BYTES);
    return 1;
}

/*
 * FIPS 203, Section 6.1, Algorithm 16: "ML-KEM.KeyGen_internal".
 *
 * The implementation of Section 5.1, Algorithm 13, "K-PKE.KeyGen(d)" is
 * inlined.
 *
 * The caller MUST also pass a pre-allocated scratch buffer |tmp| with room for
 * at least one "vector" (rank * sizeof(scalar)), and a pre-allocated digest
 * context that is not shared with any concurrent computation.
 *
 * This function outputs the serialised wire-form |ek| public key into  the
 * provided |pubenc| buffer, and generates the content of the |rho|, |pkhash|,
 * |t|, |m|, |s| and |z| components of the private |key| (which must have
 * preallocated space for these).
 *
 * Keys are computed from a 32-byte random |d| plus the 1 byte rank for
 * domain separation.  These are concatenated and hashed to produce a pair of
 * 32-byte seeds public "rho", used to generate the matrix, and private "sigma",
 * used to generate the secret vector |s|.
 *
 * The second random input |z| is copied verbatim into the Fujisaki-Okamoto
 * (FO) transform "implicit-rejection" secret (the |z| component of the private
 * key), which thwarts chosen-ciphertext attacks, provided decap() runs in
 * constant time, with no side channel leaks, on all well-formed (valid length,
 * and correctly encoded) ciphertext inputs.
 */
static __owur
int genkey(const uint8_t d[ML_KEM_RANDOM_BYTES],
           const uint8_t z[ML_KEM_RANDOM_BYTES],
           scalar *tmp, EVP_MD_CTX *mdctx,
           uint8_t *pubenc, ML_KEM_KEY *key)
{
    uint8_t hashed[2 * ML_KEM_RANDOM_BYTES];
    const uint8_t *const sigma = hashed + ML_KEM_RANDOM_BYTES;
    uint8_t augmented_seed[ML_KEM_RANDOM_BYTES + 1];
    vinfo_t vinfo = key->vinfo;
    cbd_t cbd_1 = cbd1[vinfo->variant];
    int rank = vinfo->rank;
    uint8_t counter = 0;

    /*
     * Use the "d" seed salted with the rank to derive the public and private
     * seeds rho and sigma.
     */
    memcpy(augmented_seed, d, ML_KEM_RANDOM_BYTES);
    augmented_seed[ML_KEM_RANDOM_BYTES] = (uint8_t) rank;
    if (!hash_g(hashed, augmented_seed, sizeof(augmented_seed), mdctx, key))
        return 0;
    memcpy(key->rho, hashed, ML_KEM_RANDOM_BYTES);
    if (!matrix_expand(mdctx, key)
        || !gencbd_vector(key->s, cbd_1, &counter, sigma, rank, mdctx, key))
        return 0;
    vector_ntt(key->s, rank);
    /* FIPS 203 |e| vector */
    if (!gencbd_vector(tmp, cbd_1, &counter, sigma, rank, mdctx, key))
        return 0;
    vector_ntt(tmp, rank);

    /* Fill in the public key */
    matrix_mult_transpose(key->t, key->m, key->s, rank);
    vector_add(key->t, tmp, rank);
    encode_pubkey(pubenc, key);
    if (!hash_h(key->pkhash, pubenc, vinfo->pubkey_bytes, mdctx, key))
        return 0;

    /* Save "z" portion of seed for "implicit rejection" on failure */
    memcpy(key->z, z, ML_KEM_RANDOM_BYTES);
    return 1;
}

/*-
 * FIPS 203, Section 6.2, Algorithm 17: "ML-KEM.Encaps_internal".
 * This is the deterministic version with randomness supplied externally.
 *
 * The caller must pass space for two vectors in |tmp|.
 * The |ctext| buffer have space for the ciphertext of the ML-KEM variant
 * of the provided key.
 */
static
int encap(uint8_t *ctext, uint8_t secret[ML_KEM_SHARED_SECRET_BYTES],
          const uint8_t entropy[ML_KEM_RANDOM_BYTES],
          scalar *tmp, EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    uint8_t input[ML_KEM_RANDOM_BYTES + ML_KEM_PKHASH_BYTES];
    uint8_t Kr[ML_KEM_SHARED_SECRET_BYTES + ML_KEM_RANDOM_BYTES];
    uint8_t *r = Kr + ML_KEM_SHARED_SECRET_BYTES;

#if ML_KEM_SEED_BYTES != ML_KEM_SHARED_SECRET_BYTES + ML_KEM_RANDOM_BYTES
# error "ML-KEM keygen seed length != shared secret + random bytes length"
#endif

    memcpy(input, entropy, ML_KEM_RANDOM_BYTES);
    memcpy(input + ML_KEM_RANDOM_BYTES, key->pkhash, ML_KEM_PKHASH_BYTES);
    if (!hash_g(Kr, input, sizeof(input), mdctx, key)
        || !encrypt_cpa(ctext, entropy, r, tmp, mdctx, key))
        return 0;
    memcpy(secret, Kr, ML_KEM_SHARED_SECRET_BYTES);
    return 1;
}

/*
 * FIPS 203, Section 6.3, Algorithm 18: ML-KEM.Decaps_internal
 *
 * Barring failure of the supporting SHA3/SHAKE primitives, this is fully
 * deterministic, the randomness for the FO transform is extracted during
 * private key generation.
 *
 * The caller must pass space for two vectors in |tmp|.
 * The |ctext| and |tmp_ctext| buffers must each have space for the ciphertext
 * of the key's ML-KEM variant.
 */
static
int decap(uint8_t secret[ML_KEM_SHARED_SECRET_BYTES],
          const uint8_t *ctext, uint8_t *tmp_ctext, scalar *tmp,
          EVP_MD_CTX *mdctx, const ML_KEM_KEY *key)
{
    uint8_t decrypted[ML_KEM_SHARED_SECRET_BYTES + ML_KEM_PKHASH_BYTES];
    uint8_t failure_key[ML_KEM_RANDOM_BYTES];
    uint8_t Kr[ML_KEM_SHARED_SECRET_BYTES + ML_KEM_RANDOM_BYTES];
    uint8_t *r = Kr + ML_KEM_SHARED_SECRET_BYTES;
    const uint8_t *pkhash = key->pkhash;
    vinfo_t vinfo = key->vinfo;
    int i;
    uint8_t mask;

#if ML_KEM_SHARED_SECRET_BYTES != ML_KEM_RANDOM_BYTES
# error "Invalid unequal lengths of ML-KEM shared secret and random inputs"
#endif

    /*
     * If our KDF is unavailable, fail early! Otherwise, keep going ignoring
     * any further errors, returning success, and whatever we got for a shared
     * secret.  The decrypt_cpa() function is just arithmetic on secret data,
     * so should not be subject to failure that makes its output predictable.
     *
     * We guard against "should never happen" catastrophic failure of the
     * "pure" function |hash_g| by overwriting the shared secret with the
     * content of the failure key and returning early, if nevertheless hash_g
     * fails.  This is not constant-time, but a failure of |hash_g| already
     * implies loss of side-channel resistance.
     *
     * The same action is taken, if also |encrypt_cpa| should catastrophically
     * fail, due to failure of the |PRF| underlyign the CBD functions.
     */
    if (!kdf(failure_key, key->z, ctext, vinfo->ctext_bytes, mdctx, key))
        return 0;
    decrypt_cpa(decrypted, ctext, tmp, key);
    memcpy(decrypted + ML_KEM_SHARED_SECRET_BYTES, pkhash, ML_KEM_PKHASH_BYTES);
    if (!hash_g(Kr, decrypted, sizeof(decrypted), mdctx, key)
        || !encrypt_cpa(tmp_ctext, decrypted, r, tmp, mdctx, key)) {
        memcpy(secret, failure_key, ML_KEM_SHARED_SECRET_BYTES);
        return 1;
    }
    mask = constant_time_eq_int_8(0,
        CRYPTO_memcmp(ctext, tmp_ctext, vinfo->ctext_bytes));
    for (i = 0; i < ML_KEM_SHARED_SECRET_BYTES; i++)
        secret[i] = constant_time_select_8(mask, Kr[i], failure_key[i]);
    return 1;
}

/*
 * After allocating storage for public or private key data, update the key
 * component pointers to reference that storage.
 */
static __owur
int add_storage(scalar *p, int private, ML_KEM_KEY *key)
{
    int rank = key->vinfo->rank;

    if (p == NULL)
        return 0;
    /* A public key needs space for |t| and |m| */
    key->m = (key->t = p) + rank;
    /* A private key also needs space for |s| and |z| */
    if (private)
        key->z = (uint8_t *)(rank + (key->s = key->m + rank * rank));
    else
        key->z = (uint8_t *)(key->s = NULL);
    return 1;
}

/*
 * After freeing the storage associated with a key that failed to be
 * constructed, reset the internal pointers back to NULL.
 */
static void
free_storage(ML_KEM_KEY *key)
{
    if (key->t == NULL)
        return;
    OPENSSL_free(key->t);
    key->z = (uint8_t *)(key->s = key->m = key->t = NULL);
}

/*
 * ----- API exported to the provider
 *
 * Parameters with an implicit fixed length in the internal static API of each
 * variant have an explicit checked length argument at this layer.
 */

/* Retrieve the parameters of one of the ML-KEM variants */
vinfo_t ossl_ml_kem_get_vinfo(int variant)
{
    if (variant > ML_KEM_1024)
        return NULL;
    return &vinfo_map[variant];
}

ML_KEM_KEY *ossl_ml_kem_key_new(OSSL_LIB_CTX *libctx, const char *properties,
                                int variant)
{
    vinfo_t vinfo = ossl_ml_kem_get_vinfo(variant);
    ML_KEM_KEY *key;

#define SIZELT(t1, t2)             ((sizeof(t1) < sizeof(t2)) ? -1 : 1)
#define SIZE_UINT_LT_UINT32        SIZELT(unsigned int, uint32_t)
#define CHECK_SIZE_UINT_GE_UINT32  ((void)sizeof(char[SIZE_UINT_LT_UINT32]))

    /* Precondition of ML-KEM implementation correctness */
    CHECK_SIZE_UINT_GE_UINT32;

    if (vinfo == NULL)
        return NULL;

    if ((key = OPENSSL_malloc(sizeof(*key))) == NULL)
        return NULL;

    key->vinfo = vinfo;
    key->libctx = libctx;
    key->shake128_md = EVP_MD_fetch(libctx, "SHAKE128", properties);
    key->shake256_md = EVP_MD_fetch(libctx, "SHAKE256", properties);
    key->sha3_256_md = EVP_MD_fetch(libctx, "SHA3-256", properties);
    key->sha3_512_md = EVP_MD_fetch(libctx, "SHA3-512", properties);
    key->s = key->m = key->t = NULL;
    key->z = NULL;

    if (key->shake128_md != NULL
        && key->shake256_md != NULL
        && key->sha3_256_md != NULL
        && key->sha3_512_md != NULL)
    return key;

    ossl_ml_kem_key_free(key);
    return NULL;
}

ML_KEM_KEY *ossl_ml_kem_key_dup(const ML_KEM_KEY *key, int selection)
{
    int ok = 0;
    ML_KEM_KEY *ret;

    if (key == NULL
        || (ret = OPENSSL_memdup(key, sizeof(*key))) == NULL)
        return NULL;

    /* Clear selection bits we can't fulfill */
    if (!ossl_ml_kem_have_pubkey(key))
        selection = 0;
    else if (!ossl_ml_kem_have_prvkey(key))
        selection &= ~OSSL_KEYMGMT_SELECT_PRIVATE_KEY;

    switch (selection & OSSL_KEYMGMT_SELECT_KEYPAIR) {
    case 0:
        ret->z = (uint8_t *)(ret->s = ret->m = ret->t = NULL);
        ok = 1;
        break;
    case OSSL_KEYMGMT_SELECT_PUBLIC_KEY:
        ok = add_storage(OPENSSL_memdup(key->t, key->vinfo->puballoc), 0, ret);
        break;
    case OSSL_KEYMGMT_SELECT_PRIVATE_KEY:
        ok = add_storage(OPENSSL_memdup(key->t, key->vinfo->prvalloc), 1, ret);
        break;
    }

    if (!ok) {
        OPENSSL_free(ret);
        return NULL;
    }

    EVP_MD_up_ref(ret->shake128_md);
    EVP_MD_up_ref(ret->shake256_md);
    EVP_MD_up_ref(ret->sha3_256_md);
    EVP_MD_up_ref(ret->sha3_512_md);

    return ret;
}

void ossl_ml_kem_key_free(ML_KEM_KEY *key)
{
    if (key == NULL)
        return;

    EVP_MD_free(key->shake128_md);
    EVP_MD_free(key->shake256_md);
    EVP_MD_free(key->sha3_256_md);
    EVP_MD_free(key->sha3_512_md);

    /*-
     * Cleanse any sensitive data:
     * - The private vector |s| is immediately followed by the FO failure
     *   secret |z|, we can cleanse both in one call.
     */
    if (key->s != NULL)
        OPENSSL_cleanse(key->s, key->vinfo->vector_bytes + ML_KEM_RANDOM_BYTES);

    /* Free the key material */
    OPENSSL_free(key->t);
    OPENSSL_free(key);
}

/* Serialise the public component of an ML-KEM key */
int ossl_ml_kem_encode_public_key(uint8_t *out, size_t len,
                                  const ML_KEM_KEY *key)
{
    if (!ossl_ml_kem_have_pubkey(key)
        || len != key->vinfo->pubkey_bytes)
        return 0;
    encode_pubkey(out, key);
    return 1;
}

/* Serialise an ML-KEM private key */
int ossl_ml_kem_encode_private_key(uint8_t *out, size_t len,
                                   const ML_KEM_KEY *key)
{
    if (!ossl_ml_kem_have_prvkey(key)
        || len != key->vinfo->prvkey_bytes)
        return 0;
    encode_prvkey(out, key);
    return 1;
}

/* Parse input as a public key */
int ossl_ml_kem_parse_public_key(const uint8_t *in, size_t len, ML_KEM_KEY *key)
{
    EVP_MD_CTX *mdctx = NULL;
    vinfo_t vinfo;
    int ret = 0;

    /* Keys with key material are immutable */
    if (key == NULL || ossl_ml_kem_have_pubkey(key))
        return 0;
    vinfo = key->vinfo;

    if (len != vinfo->pubkey_bytes
        || (mdctx = EVP_MD_CTX_new()) == NULL)
        return 0;

    if (add_storage(OPENSSL_malloc(vinfo->puballoc), 0, key))
        ret = parse_pubkey(in, mdctx, key);

    if (!ret)
        free_storage(key);
    EVP_MD_CTX_free(mdctx);
    return ret;
}

/* Parse input as a new private key  */
int ossl_ml_kem_parse_private_key(const uint8_t *in, size_t len,
                                  ML_KEM_KEY *key)
{
    EVP_MD_CTX *mdctx = NULL;
    vinfo_t vinfo;
    int ret = 0;

    /* Keys with key material are immutable */
    if (key == NULL || ossl_ml_kem_have_pubkey(key))
        return 0;
    vinfo = key->vinfo;

    if (len != vinfo->prvkey_bytes
        || (mdctx = EVP_MD_CTX_new()) == NULL)
        return 0;

    if (add_storage(OPENSSL_malloc(vinfo->prvalloc), 1, key))
        ret = parse_prvkey(in, mdctx, key);

    if (!ret)
        free_storage(key);
    EVP_MD_CTX_free(mdctx);
    return ret;
}

/*
 * Generate a new keypair from a given seed, giving a deterministic result for
 * running tests.  The caller can elect to not collect the encoded public key.
 */
int ossl_ml_kem_genkey_seed(const uint8_t *seed, size_t seedlen,
                            uint8_t *pubenc, size_t publen,
                            ML_KEM_KEY *key)
{
    EVP_MD_CTX *mdctx = NULL;
    vinfo_t vinfo;
    int ret = 0;

    if (key == NULL || ossl_ml_kem_have_pubkey(key))
        return 0;
    vinfo = key->vinfo;

    if (seed == NULL || seedlen != ML_KEM_SEED_BYTES
        || (pubenc != NULL && publen != vinfo->pubkey_bytes)
        || (mdctx = EVP_MD_CTX_new()) == NULL)
        return 0;

    if (add_storage(OPENSSL_malloc(vinfo->prvalloc), 1, key)) {
        const uint8_t *d = seed;
        const uint8_t *z = seed + ML_KEM_RANDOM_BYTES;

        /*-
         * This avoids the need to handle allocation failures for two (max 2KB
         * each) vectors and (if the caller does not want the public key) an
         * encoded public key (max 1568 bytes), that are never retained on
         * return from this function.
         * We stack-allocate these.
         */
#       define case_genkey_seed(bits) \
        case ML_KEM_##bits: \
            if (pubenc != NULL) \
            { \
                scalar tmp[ML_KEM_##bits##_RANK]; \
                                                             \
                ret = genkey(d, z, tmp, mdctx, pubenc, key); \
            } else { \
                scalar tmp[ML_KEM_##bits##_RANK]; \
                uint8_t encbuf[ML_KEM_##bits##_PUBLIC_KEY_BYTES]; \
                                                             \
                ret = genkey(d, z, tmp, mdctx, encbuf, key); \
            } \
            break
        switch (vinfo->variant) {
        case_genkey_seed(512);
        case_genkey_seed(768);
        case_genkey_seed(1024);
        }
#       undef case_genkey_seed
    }

    if (!ret)
        free_storage(key);
    EVP_MD_CTX_free(mdctx);
    return ret;
}

/*
 * Generate a new keypair from a random seed, using the library context's
 * private DRBG.  The caller can elect to not collect the seed or the encoded
 * public key.
 */
int ossl_ml_kem_genkey_rand(uint8_t *seed, size_t seedlen,
                            uint8_t *pubenc, size_t publen,
                            ML_KEM_KEY *key)
{
    uint8_t tmpseed[ML_KEM_SEED_BYTES];
    uint8_t *sptr = seed == NULL ? tmpseed : seed;

    if (key == NULL
        || ossl_ml_kem_have_pubkey(key)
        || (seed != NULL && seedlen != ML_KEM_SEED_BYTES))
        return 0;

    if (RAND_priv_bytes_ex(key->libctx, sptr, sizeof(tmpseed),
                           key->vinfo->secbits) <= 0)
        return 0;

    return ossl_ml_kem_genkey_seed(sptr, sizeof(tmpseed), pubenc, publen, key);
}

/*
 * FIPS 203, Section 6.2, Algorithm 17: ML-KEM.Encaps_internal
 * This is the deterministic version with randomness supplied externally.
 */
int ossl_ml_kem_encap_seed(uint8_t *ctext, size_t clen,
                           uint8_t *shared_secret, size_t slen,
                           const uint8_t *entropy, size_t elen,
                           const ML_KEM_KEY *key)
{
    vinfo_t vinfo;
    EVP_MD_CTX *mdctx;
    int ret = 0;

    if (!ossl_ml_kem_have_pubkey(key))
        return 0;
    vinfo = key->vinfo;

    if (ctext == NULL || clen != vinfo->ctext_bytes
        || shared_secret == NULL || slen != ML_KEM_SHARED_SECRET_BYTES
        || entropy == NULL || elen != ML_KEM_RANDOM_BYTES
        || key == NULL || (mdctx = EVP_MD_CTX_new()) == NULL)
        return 0;

    /*-
     * This avoids the need to handle allocation failures for two (max 2KB
     * each) vectors, that are never retained on return from this function.
     * We stack-allocate these.
     */
#   define case_encap_seed(bits) \
    case ML_KEM_##bits: \
        { \
            scalar tmp[2 * ML_KEM_##bits##_RANK]; \
                                                                         \
            ret = encap(ctext, shared_secret, entropy, tmp, mdctx, key); \
            break; \
        }
    switch (vinfo->variant) {
    case_encap_seed(512);
    case_encap_seed(768);
    case_encap_seed(1024);
    }
#   undef case_encap_seed

    EVP_MD_CTX_free(mdctx);
    return ret;
}

int ossl_ml_kem_encap_rand(uint8_t *ctext, size_t clen,
                           uint8_t *shared_secret, size_t slen,
                           const ML_KEM_KEY *key)
{
    uint8_t r[ML_KEM_RANDOM_BYTES];

    if (key == NULL)
        return 0;

    if (RAND_bytes_ex(key->libctx, r, ML_KEM_RANDOM_BYTES,
                      key->vinfo->secbits) < 1)
        return 0;

    return ossl_ml_kem_encap_seed(ctext, clen, shared_secret, slen,
                                  r, sizeof(r), key);
}

int ossl_ml_kem_decap(uint8_t *shared_secret, size_t slen,
                      const uint8_t *ctext, size_t clen,
                      const ML_KEM_KEY *key)
{
    vinfo_t vinfo;
    EVP_MD_CTX *mdctx;
    int ret = 0;

    /* Need a private key here */
    if (!ossl_ml_kem_have_prvkey(key))
        return 0;
    vinfo = key->vinfo;

    if (shared_secret == NULL || slen != ML_KEM_SHARED_SECRET_BYTES
        || ctext == NULL || clen != vinfo->ctext_bytes
        || (mdctx = EVP_MD_CTX_new()) == NULL) {
        RAND_bytes_ex(key->libctx, shared_secret,
                      ML_KEM_SHARED_SECRET_BYTES, vinfo->secbits);
        return 0;
    }

    /*-
     * This avoids the need to handle allocation failures for two (max 2KB
     * each) vectors and an encoded ciphertext (max 1568 bytes), that are never
     * retained on return from this function.
     * We stack-allocate these.
     */
#   define case_decap(bits) \
    case ML_KEM_##bits: \
        { \
            uint8_t cbuf[ML_KEM_##bits##_CIPHERTEXT_BYTES]; \
            scalar tmp[2 * ML_KEM_##bits##_RANK]; \
                                                                      \
            ret = decap(shared_secret, ctext, cbuf, tmp, mdctx, key); \
            EVP_MD_CTX_free(mdctx); \
            return ret; \
        }
    switch (vinfo->variant) {
    case_decap(512);
    case_decap(768);
    case_decap(1024);
    }
    return 0;
#   undef case_decap
}

int ossl_ml_kem_pubkey_cmp(const ML_KEM_KEY *key1, const ML_KEM_KEY *key2)
{
    /* No match if either or both public keys are not available */
    if (!ossl_ml_kem_have_pubkey(key1) || !ossl_ml_kem_have_pubkey(key2))
        return 0;

    /*
     * This handles any unexpected differences the ML-KEM variant rank, barring
     * SHA3-256 hash collisions, the keys are also the same size.
     */
    return memcmp(key1->pkhash, key2->pkhash, ML_KEM_PKHASH_BYTES) == 0;
}
