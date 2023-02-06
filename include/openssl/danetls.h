/*
 * Copyright 2015-2023 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#ifndef OPENSSL_DANETLS_H
#define OPENSSL_DANETLS_H
# pragma once

# ifdef  __cplusplus
extern "C" {
# endif

/*-
 * Certificate usages:
 * https://tools.ietf.org/html/rfc6698#section-2.1.1
 */
# define DANETLS_USAGE_PKIX_TA   0
# define DANETLS_USAGE_PKIX_EE   1
# define DANETLS_USAGE_DANE_TA   2
# define DANETLS_USAGE_DANE_EE   3
# define DANETLS_USAGE_LAST      DANETLS_USAGE_DANE_EE

/*-
 * Selectors:
 * https://tools.ietf.org/html/rfc6698#section-2.1.2
 */
# define DANETLS_SELECTOR_CERT   0
# define DANETLS_SELECTOR_SPKI   1
# define DANETLS_SELECTOR_LAST   DANETLS_SELECTOR_SPKI

/*-
 * Matching types:
 * https://tools.ietf.org/html/rfc6698#section-2.1.3
 */
# define DANETLS_MATCHING_FULL   0
# define DANETLS_MATCHING_2256   1
# define DANETLS_MATCHING_2512   2
# define DANETLS_MATCHING_LAST   DANETLS_MATCHING_2512

# ifdef  __cplusplus
}
# endif
#endif
