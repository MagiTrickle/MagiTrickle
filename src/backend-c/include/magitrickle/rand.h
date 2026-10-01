/* Cryptographically random bytes — shared by mt_id_random (config/id.c)
 * and the auth secret generator (api/auth.c). See rand.c for the
 * /dev/urandom-over-getrandom(2) choice (old-kernel compatibility). */
#ifndef MAGITRICKLE_RAND_H
#define MAGITRICKLE_RAND_H

#include <stddef.h>
#include <stdint.h>

#include "magitrickle/err.h"

/* Fills buf[0..len) with random bytes from /dev/urandom. MT_ERR_SYS if
 * the device can't be opened or read fully. */
mt_err_t mt_random_bytes(uint8_t *buf, size_t len);

#endif /* MAGITRICKLE_RAND_H */
