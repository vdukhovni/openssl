/*
 * Copyright 2024 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <openssl/err.h>
#include <openssl/proverr.h>
#include <openssl/rand.h>
#include <openssl/self_test.h>
#include "crypto/ml_kem.h"
#include "internal/param_build_set.h"
#include <openssl/param_build.h>
#include "prov/implementations.h"
#include "prov/providercommon.h"
#include "prov/provider_ctx.h"
#include "prov/securitycheck.h"

static OSSL_FUNC_keymgmt_new_fn ml_kem_512_new;
static OSSL_FUNC_keymgmt_new_fn ml_kem_768_new;
static OSSL_FUNC_keymgmt_new_fn ml_kem_1024_new;
static OSSL_FUNC_keymgmt_gen_fn ml_kem_gen;
static OSSL_FUNC_keymgmt_gen_init_fn ml_kem_512_gen_init;
static OSSL_FUNC_keymgmt_gen_init_fn ml_kem_768_gen_init;
static OSSL_FUNC_keymgmt_gen_init_fn ml_kem_1024_gen_init;
static OSSL_FUNC_keymgmt_gen_cleanup_fn ml_kem_gen_cleanup;
static OSSL_FUNC_keymgmt_gen_set_params_fn ml_kem_gen_set_params;
static OSSL_FUNC_keymgmt_gen_settable_params_fn ml_kem_gen_settable_params;
#ifndef FIPS_MODULE
static OSSL_FUNC_keymgmt_load_fn ml_kem_load;
#endif
static OSSL_FUNC_keymgmt_get_params_fn ml_kem_get_params;
static OSSL_FUNC_keymgmt_gettable_params_fn ml_kem_gettable_params;
static OSSL_FUNC_keymgmt_set_params_fn ml_kem_set_params;
static OSSL_FUNC_keymgmt_settable_params_fn ml_kem_settable_params;
static OSSL_FUNC_keymgmt_has_fn ml_kem_has;
static OSSL_FUNC_keymgmt_match_fn ml_kem_match;
static OSSL_FUNC_keymgmt_import_fn ml_kem_import;
static OSSL_FUNC_keymgmt_export_fn ml_kem_export;
static OSSL_FUNC_keymgmt_import_types_fn ml_kem_imexport_types;
static OSSL_FUNC_keymgmt_export_types_fn ml_kem_imexport_types;
static OSSL_FUNC_keymgmt_dup_fn ml_kem_dup;

static const int minimal_selection = OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS
    | OSSL_KEYMGMT_SELECT_PRIVATE_KEY;

typedef struct ml_kem_gen_ctx_st {
    OSSL_LIB_CTX *libctx;
    char *propq;
    int selection;
    int evp_type;
    uint8_t seedbuf[ML_KEM_SEED_BYTES];
    uint8_t *seed;
} PROV_ML_KEM_GEN_CTX;

#ifdef FIPS_MODULE
static int ml_kem_pairwise_test(const ML_KEM_KEY *key)
{
    int ret = 0;
    OSSL_SELF_TEST *st = NULL;
    OSSL_CALLBACK *cb = NULL;
    void *cbarg = NULL;
    unsigned char secret[ML_KEM_SHARED_SECRET_BYTES];
    unsigned char out[ML_KEM_SHARED_SECRET_BYTES];
    unsigned char entropy[ML_KEM_RANDOM_BYTES];
    unsigned char *ctext = NULL;
    const ML_KEM_VINFO *v = ossl_ml_kem_key_vinfo(key);
    int operation_result = 0;

    /* Unless we have both a public and private key, we can't do the test */
    if (!ossl_ml_kem_have_prvkey(key))
        return 1;

    /*
     * The functions `OSSL_SELF_TEST_*` will return directly if parameter `st`
     * is NULL.
     */
    OSSL_SELF_TEST_get_callback(key->libctx, &cb, &cbarg);

    st = OSSL_SELF_TEST_new(cb, cbarg);
    if (st == NULL)
        return 0;

    OSSL_SELF_TEST_onbegin(st, OSSL_SELF_TEST_TYPE_PCT,
                           OSSL_SELF_TEST_DESC_PCT_ML_KEM);

    /*
     * Initialise output buffers to avoid collecting random stack memory.
     * The `entropy' buffer is filled with an arbitrary non-zero value.
     */
    memset(out, 0, sizeof(out));
    memset(entropy, 0125, sizeof(entropy));

    ctext = OPENSSL_malloc(v->ctext_bytes);
    if (ctext == NULL)
        goto err;

    operation_result = ossl_ml_kem_encap_seed(ctext, v->ctext_bytes,
                                              secret, sizeof(secret),
                                              entropy, sizeof(entropy),
                                              key);
    if (operation_result != 1)
        goto err;

    OSSL_SELF_TEST_oncorrupt_byte(st, ctext);

    operation_result = ossl_ml_kem_decap(out, sizeof(out), ctext, v->ctext_bytes,
                                         key);
    if (operation_result != 1
            || memcmp(out, secret, sizeof(out)) != 0)
        goto err;

    ret = 1;
err:
    OPENSSL_free(ctext);
    OSSL_SELF_TEST_onend(st, ret);
    OSSL_SELF_TEST_free(st);
    return ret;
}
#endif  /* FIPS_MODULE */

static void *ml_kem_new(OSSL_LIB_CTX *libctx, char *propq, int evp_type)
{
    if (!ossl_prov_is_running())
        return NULL;
    return ossl_ml_kem_key_new(libctx, propq, evp_type);
}

static int ml_kem_has(const void *vkey, int selection)
{
    const ML_KEM_KEY *key = vkey;

    /* A NULL key MUST fail to have anything */
    if (!ossl_prov_is_running() || key == NULL)
        return 0;

    switch (selection & OSSL_KEYMGMT_SELECT_KEYPAIR) {
    case 0:
        return 1;
    case OSSL_KEYMGMT_SELECT_PUBLIC_KEY:
        return ossl_ml_kem_have_pubkey(key);
    default:
        return ossl_ml_kem_have_prvkey(key);
    }
}

static int ml_kem_match(const void *vkey1, const void *vkey2, int selection)
{
    const ML_KEM_KEY *key1 = vkey1;
    const ML_KEM_KEY *key2 = vkey2;

    if (!ossl_prov_is_running())
        return 0;

    /* All we have that can be compared is key material */
    if (!(selection & OSSL_KEYMGMT_SELECT_KEYPAIR))
        return 1;

    return ossl_ml_kem_pubkey_cmp(key1, key2);
}

static int ml_kem_export(void *vkey, int selection, OSSL_CALLBACK *param_cb,
                         void *cbarg)
{
    ML_KEM_KEY *key = vkey;
    OSSL_PARAM_BLD *tmpl = NULL;
    OSSL_PARAM *params = NULL;
    uint8_t *pubenc = NULL;
    uint8_t *prvenc = NULL;
    const ML_KEM_VINFO *v;
    size_t prvlen = 0;
    int ret = 0;

    if (!ossl_prov_is_running() || key == NULL)
        return 0;

    if ((selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0)
        return 0;

    /* Fail when no key material has yet been provided */
    if (!ossl_ml_kem_have_pubkey(key)) {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_KEY);
        return 0;
    }
    v = ossl_ml_kem_key_vinfo(key);

    if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) != 0) {
        pubenc = OPENSSL_malloc(v->pubkey_bytes);
        if (pubenc == NULL
            || !ossl_ml_kem_encode_public_key(pubenc, v->pubkey_bytes, key))
            goto err;
    }

    if (ossl_ml_kem_have_prvkey(key)
        && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) != 0) {
        /*
         * Allocated on the secure heap if configured, this is detected in
         * ossl_param_build_set_octet_string(), which will then also use the
         * secure heap.
         */
        if (ossl_ml_kem_have_seed(key)) {
            prvlen = ML_KEM_SEED_BYTES;
            if ((prvenc = OPENSSL_secure_zalloc(prvlen)) == NULL
                || !ossl_ml_kem_encode_key_seed(prvenc, prvlen, key))
                goto err;
        } else {
            prvlen = v->prvkey_bytes;
            if ((prvenc = OPENSSL_secure_zalloc(prvlen)) == NULL
                || !ossl_ml_kem_encode_private_key(prvenc, prvlen, key))
                goto err;
        }
    }

    tmpl = OSSL_PARAM_BLD_new();
    if (tmpl == NULL)
        goto err;

    /* The public key on request; it is always available when either is */
    if (pubenc != NULL
        && !ossl_param_build_set_octet_string(
               tmpl, params, OSSL_PKEY_PARAM_PUB_KEY, pubenc, v->pubkey_bytes))
            goto err;

    /*-
     * The private key on request, in the (d, z) seed format, when available,
     * otherwise in the FIPS 203 |dk| format.
     */
    if (prvenc != NULL
        && !ossl_param_build_set_octet_string(
                tmpl, params, OSSL_PKEY_PARAM_PRIV_KEY, prvenc, prvlen))
            goto err;

    params = OSSL_PARAM_BLD_to_param(tmpl);
    if (params == NULL)
        goto err;

    ret = param_cb(params, cbarg);
    OSSL_PARAM_free(params);

err:
    OSSL_PARAM_BLD_free(tmpl);
    OPENSSL_secure_clear_free(prvenc, prvlen);
    OPENSSL_free(pubenc);
    return ret;
}

static const OSSL_PARAM *ml_kem_imexport_types(int selection)
{
    static const OSSL_PARAM key_types[] = {
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0),
        OSSL_PARAM_END
    };

    if ((selection & OSSL_KEYMGMT_SELECT_KEYPAIR) != 0)
        return key_types;
    return NULL;
}

static int ml_kem_key_fromdata(ML_KEM_KEY *key,
                               const OSSL_PARAM params[],
                               int include_private)
{
    const OSSL_PARAM *p = NULL;
    const uint8_t *d = NULL, *z = NULL;
    const void *pubenc = NULL, *prvenc = NULL;
    size_t publen = 0, prvlen = 0;
    const ML_KEM_VINFO *v;

    /* Invalid attempt to mutate a key, what is the right error to report? */
    if (key == NULL || ossl_ml_kem_have_pubkey(key))
        return 0;
    v = ossl_ml_kem_key_vinfo(key);

    /* What does the caller want to set? */
    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PUB_KEY);
    if (p != NULL &&
        OSSL_PARAM_get_octet_string_ptr(p, &pubenc, &publen) != 1)
        return 0;

    /*
     * Accept private keys in either expanded or seed form, distinguished by
     * length alone.  Accept either the "raw" or "encoded" parameters as the
     * input source, preferring the raw, which is expected to be the seed if
     * the caller supports seeds as a key format.
     */
    if (include_private) {
        p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PRIV_KEY);
        if (p == NULL)
            p = OSSL_PARAM_locate_const(params,
                                        OSSL_PKEY_PARAM_ENCODED_PRIVATE_KEY);
        if (p != NULL
            && OSSL_PARAM_get_octet_string_ptr(p, &prvenc, &prvlen) != 1)
            return 0;
        if (prvlen == ML_KEM_SEED_BYTES) {
            d = (uint8_t *)prvenc;
            z = d + ML_KEM_RANDOM_BYTES;
        } else if (prvlen != 0 && prvlen != v->prvkey_bytes) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY_LENGTH);
            return 0;
        }
    }

    /* The caller MUST specify at least one of the public or private keys. */
    if (publen == 0 && prvlen == 0) {
        ERR_raise(ERR_LIB_PROV, PROV_R_MISSING_KEY);
        return 0;
    }

    /*
     * When a pubkey is provided, its length MUST be correct, if a private key
     * is also provided, the public key will be otherwise ignored.  We could
     * look for a matching encoded block, but unclear this is useful.
     */
    if (publen != 0 && publen != v->pubkey_bytes) {
        ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY_LENGTH);
        return 0;
    }

    /*
     * If the private key is given, we'll ignore the public key data, taking
     * the embedded public key as authoritative.  For import, the private key
     * is in either (d, z) seed format or the FIPS 203 expanded format.
     */
    if (d != NULL)
        return ossl_ml_kem_genkey(d, z, NULL, 0, key);
    if (prvlen != 0)
        return ossl_ml_kem_parse_private_key(prvenc, prvlen, key);
    return ossl_ml_kem_parse_public_key(pubenc, publen, key);
}

static int ml_kem_import(void *vkey, int selection, const OSSL_PARAM params[])
{
    ML_KEM_KEY *key = vkey;
    int include_private;
    int res;

    if (!ossl_prov_is_running() || key == NULL)
        return 0;

    if ((selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0)
        return 0;

    include_private = selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY ? 1 : 0;
    res = ml_kem_key_fromdata(key, params, include_private);
#ifdef FIPS_MODULE
    if (res > 0 && include_private && !ml_kem_pairwise_test(key)) {
        ossl_set_error_state(OSSL_SELF_TEST_TYPE_PCT);
        res = 0;
    }
#endif  /* FIPS_MODULE */
    return res;
}

static const OSSL_PARAM *ml_kem_gettable_params(void *provctx)
{
    static const OSSL_PARAM arr[] = {
        OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL),
        OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
        OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PRIVATE_KEY, NULL, 0),
        OSSL_PARAM_END
    };

    return arr;
}

#ifndef FIPS_MODULE
void *ml_kem_load(const void *reference, size_t reference_sz)
{
    ML_KEM_KEY *key = NULL;

    if (ossl_prov_is_running() && reference_sz == sizeof(key)) {
        /* The contents of the reference is the address to our object */
        key = *(ML_KEM_KEY **)reference;
        /* We grabbed, so we detach it */
        *(ML_KEM_KEY **)reference = NULL;
        return key;
    }
    return NULL;
}
#endif

/*
 * It is assumed the key is guaranteed non-NULL here, and is from this provider
 */
static int ml_kem_get_params(void *vkey, OSSL_PARAM params[])
{
    ML_KEM_KEY *key = vkey;
    const ML_KEM_VINFO *v = ossl_ml_kem_key_vinfo(key);
    OSSL_PARAM *p;

    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_BITS);
    if (p != NULL)
        if (!OSSL_PARAM_set_int(p, v->bits))
            return 0;

    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_SECURITY_BITS);
    if (p != NULL)
        if (!OSSL_PARAM_set_int(p, v->secbits))
            return 0;

    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_MAX_SIZE);
    if (p != NULL)
        if (!OSSL_PARAM_set_int(p, v->ctext_bytes))
            return 0;

    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
    if (p != NULL && ossl_ml_kem_have_pubkey(key)) {
        if (p->data_type != OSSL_PARAM_OCTET_STRING)
            return 0;
        p->return_size = v->pubkey_bytes;
        if (p->data != NULL) {
            if (p->data_size < p->return_size)
                return 0;
            if (!ossl_ml_kem_encode_public_key(p->data, p->return_size, key))
                return 0;
        }
    }

    p = OSSL_PARAM_locate(params, OSSL_PKEY_PARAM_ENCODED_PRIVATE_KEY);
    if (p != NULL && ossl_ml_kem_have_prvkey(key)) {
        if (p->data_type != OSSL_PARAM_OCTET_STRING)
            return 0;
        p->return_size = v->prvkey_bytes;
        if (p->data != NULL) {
            if (p->data_size < p->return_size)
                return 0;
            if (!ossl_ml_kem_encode_private_key(p->data, p->return_size, key))
                return 0;
        }
    }

    return 1;
}

static const OSSL_PARAM *ml_kem_settable_params(void *provctx)
{
    static const OSSL_PARAM arr[] = {
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PRIVATE_KEY, NULL, 0),
        OSSL_PARAM_END
    };

    return arr;
}

static int ml_kem_set_params(void *vkey, const OSSL_PARAM params[])
{
    ML_KEM_KEY *key = vkey;
    const OSSL_PARAM *p;
    const void *pubenc = NULL, *prvenc = NULL;
    size_t publen = 0, prvlen = 0;

    if (ossl_param_is_empty(params))
        return 1;

    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ENCODED_PRIVATE_KEY);
    if (p != NULL
        && (OSSL_PARAM_get_octet_string_ptr(p, &prvenc, &prvlen) != 1
            || prvlen != key->vinfo->prvkey_bytes)) {
        ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY);
        return 0;
    }

    if (prvlen == 0) {
        p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY);
        if (p != NULL
            && (OSSL_PARAM_get_octet_string_ptr(p, &pubenc, &publen) != 1
                || publen != key->vinfo->pubkey_bytes)) {
            ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_KEY);
            return 0;
        }
    }

    if (publen == 0 && prvlen == 0)
        return 1;

    /* Key mutation is reportedly generally not allowed */
    if (ossl_ml_kem_have_pubkey(key)) {
        ERR_raise_data(ERR_LIB_PROV,
                       PROV_R_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE,
                       "ML-KEM keys cannot be mutated");
        return 0;
    }

    if (prvlen)
        return ossl_ml_kem_parse_private_key(prvenc, prvlen, key);
    else
        return ossl_ml_kem_parse_public_key(pubenc, publen, key);
}

static int ml_kem_gen_set_params(void *vgctx, const OSSL_PARAM params[])
{
    PROV_ML_KEM_GEN_CTX *gctx = vgctx;
    const OSSL_PARAM *p;

    if (gctx == NULL)
        return 0;
    if (ossl_param_is_empty(params))
        return 1;

    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PROPERTIES);
    if (p != NULL) {
        if (p->data_type != OSSL_PARAM_UTF8_STRING)
            return 0;
        OPENSSL_free(gctx->propq);
        if ((gctx->propq = OPENSSL_strdup(p->data)) == NULL)
            return 0;
    }

    p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_ML_KEM_SEED);
    if (p != NULL) {
        size_t len = ML_KEM_SEED_BYTES;

        gctx->seed = gctx->seedbuf;
        if (OSSL_PARAM_get_octet_string(p, (void **)&gctx->seed, len, &len)
            && len == ML_KEM_SEED_BYTES)
            return 1;

        /* Possibly, but less likely wrong data type */
        ERR_raise(ERR_LIB_PROV, PROV_R_INVALID_SEED_LENGTH);
        gctx->seed = NULL;
        return 0;
    }
    return 1;
}

static void *ml_kem_gen_init(void *provctx, int selection,
                             const OSSL_PARAM params[], int evp_type)
{
    PROV_ML_KEM_GEN_CTX *gctx = NULL;

    /*
     * We can only generate private keys, check that the selection is
     * appropriate.
     */
    if (!ossl_prov_is_running()
        || (selection & minimal_selection) == 0
        || (gctx = OPENSSL_zalloc(sizeof(*gctx))) == NULL)
        return NULL;

    gctx->selection = selection;
    gctx->evp_type = evp_type;
    if (provctx != NULL)
        gctx->libctx = PROV_LIBCTX_OF(provctx);
    if (ml_kem_gen_set_params(gctx, params))
        return gctx;

    ml_kem_gen_cleanup(gctx);
    return NULL;
}

static const OSSL_PARAM *ml_kem_gen_settable_params(ossl_unused void *vgctx,
                                                    ossl_unused void *provctx)
{
    static OSSL_PARAM settable[] = {
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ML_KEM_SEED, NULL, 0),
        OSSL_PARAM_END
    };
    return settable;
}

static void *ml_kem_gen(void *vgctx, OSSL_CALLBACK *osslcb, void *cbarg)
{
    PROV_ML_KEM_GEN_CTX *gctx = vgctx;
    ML_KEM_KEY *key;
    uint8_t *nopub = NULL;
    uint8_t *seed = gctx->seed;
    uint8_t *d = seed != NULL ? seed : NULL;
    uint8_t *z = seed != NULL ? seed + ML_KEM_RANDOM_BYTES : NULL;
    int genok = 0;

    if (gctx == NULL
        || (gctx->selection & OSSL_KEYMGMT_SELECT_KEYPAIR) ==
            OSSL_KEYMGMT_SELECT_PUBLIC_KEY
        || (key = ml_kem_new(gctx->libctx, gctx->propq, gctx->evp_type)) == NULL)
        return NULL;

    if ((gctx->selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == 0)
        return key;

    genok = ossl_ml_kem_genkey(d, z, nopub, 0, key);

    /* Erase the single-use seed */
    if (seed != NULL)
        OPENSSL_cleanse(seed, ML_KEM_SEED_BYTES);
    gctx->seed = NULL;

    if (genok) {
#ifdef FIPS_MODULE
        if (!ml_kem_pairwise_test(key)) {
            ossl_set_error_state(OSSL_SELF_TEST_TYPE_PCT);
            ossl_ml_kem_key_free(key);
            return NULL;
        }
#endif  /* FIPS_MODULE */
        return key;
    }

    ossl_ml_kem_key_free(key);
    return NULL;
}

static void ml_kem_gen_cleanup(void *vgctx)
{
    PROV_ML_KEM_GEN_CTX *gctx = vgctx;

    if (gctx->seed != NULL)
        OPENSSL_cleanse(gctx->seed, ML_KEM_RANDOM_BYTES);
    OPENSSL_free(gctx->propq);
    OPENSSL_free(gctx);
}

static void *ml_kem_dup(const void *vkey, int selection)
{
    const ML_KEM_KEY *key = vkey;

    if (!ossl_prov_is_running())
        return NULL;

    return ossl_ml_kem_key_dup(key, selection);
}

#ifndef FIPS_MODULE
# define DISPATCH_LOAD_FN \
        { OSSL_FUNC_KEYMGMT_LOAD, (OSSL_FUNC) ml_kem_load },
#else
# define DISPATCH_LOAD_FN   /* Non-FIPS only */
#endif

#define DECLARE_VARIANT(bits) \
    static void *ml_kem_##bits##_new(void *provctx) \
    { \
        return ml_kem_new(provctx == NULL ? NULL : PROV_LIBCTX_OF(provctx), \
                          NULL, EVP_PKEY_ML_KEM_##bits); \
    } \
    static void *ml_kem_##bits##_gen_init(void *provctx, int selection, \
                                          const OSSL_PARAM params[]) \
    { \
        return ml_kem_gen_init(provctx, selection, params, \
                               EVP_PKEY_ML_KEM_##bits); \
    } \
    const OSSL_DISPATCH ossl_ml_kem_##bits##_keymgmt_functions[] = { \
        { OSSL_FUNC_KEYMGMT_NEW, (OSSL_FUNC) ml_kem_##bits##_new }, \
        { OSSL_FUNC_KEYMGMT_FREE, (OSSL_FUNC) ossl_ml_kem_key_free }, \
        { OSSL_FUNC_KEYMGMT_GET_PARAMS, (OSSL_FUNC) ml_kem_get_params }, \
        { OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS, (OSSL_FUNC) ml_kem_gettable_params }, \
        { OSSL_FUNC_KEYMGMT_SET_PARAMS, (OSSL_FUNC) ml_kem_set_params }, \
        { OSSL_FUNC_KEYMGMT_SETTABLE_PARAMS, (OSSL_FUNC) ml_kem_settable_params }, \
        { OSSL_FUNC_KEYMGMT_HAS, (OSSL_FUNC) ml_kem_has }, \
        { OSSL_FUNC_KEYMGMT_MATCH, (OSSL_FUNC) ml_kem_match }, \
        { OSSL_FUNC_KEYMGMT_GEN_INIT, (OSSL_FUNC) ml_kem_##bits##_gen_init }, \
        { OSSL_FUNC_KEYMGMT_GEN_SET_PARAMS, (OSSL_FUNC) ml_kem_gen_set_params }, \
        { OSSL_FUNC_KEYMGMT_GEN_SETTABLE_PARAMS, (OSSL_FUNC) ml_kem_gen_settable_params }, \
        { OSSL_FUNC_KEYMGMT_GEN, (OSSL_FUNC) ml_kem_gen }, \
        { OSSL_FUNC_KEYMGMT_GEN_CLEANUP, (OSSL_FUNC) ml_kem_gen_cleanup }, \
        DISPATCH_LOAD_FN \
        { OSSL_FUNC_KEYMGMT_DUP, (OSSL_FUNC) ml_kem_dup }, \
        { OSSL_FUNC_KEYMGMT_IMPORT, (OSSL_FUNC) ml_kem_import }, \
        { OSSL_FUNC_KEYMGMT_IMPORT_TYPES, (OSSL_FUNC) ml_kem_imexport_types }, \
        { OSSL_FUNC_KEYMGMT_EXPORT, (OSSL_FUNC) ml_kem_export }, \
        { OSSL_FUNC_KEYMGMT_EXPORT_TYPES, (OSSL_FUNC) ml_kem_imexport_types }, \
        OSSL_DISPATCH_END \
    }
DECLARE_VARIANT(512);
DECLARE_VARIANT(768);
DECLARE_VARIANT(1024);
