/* Compiler-independent netlink attribute iteration.
 *
 * libmnl's mnl_attr_for_each* macros pass a pointer difference to an int
 * parameter.  The project used GCC diagnostic pragmas around the macro
 * headers to suppress -Wconversion.  Entware's GCC 8.4 parsed a pragma
 * between a macro-expanded for statement and its body as an empty loop,
 * so valid attributes were skipped at runtime.  Keeping the iteration in
 * ordinary functions avoids both the narrowing warning and that target
 * compiler ambiguity.
 */
#ifndef MAGITRICKLE_NLATTR_ITER_H
#define MAGITRICKLE_NLATTR_ITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/netlink.h>

typedef struct mt_nlattr_iter {
    const uint8_t *cursor;
    size_t remaining;
} mt_nlattr_iter_t;

/* Initializes an iterator over attributes following an nlmsg payload
 * header of extra_header_len bytes (e.g. sizeof(struct rtmsg)). */
bool mt_nlattr_iter_init_nlmsg(mt_nlattr_iter_t *it, const struct nlmsghdr *h,
                               size_t extra_header_len);

/* Initializes an iterator over attributes inside a nested attribute. */
bool mt_nlattr_iter_init_nested(mt_nlattr_iter_t *it, const struct nlattr *nest);

/* Returns the next structurally valid attribute, or false at the end or
 * on malformed/truncated input.  The returned pointer is borrowed. */
bool mt_nlattr_iter_next(mt_nlattr_iter_t *it, const struct nlattr **out);

#endif /* MAGITRICKLE_NLATTR_ITER_H */
