#include "greatest.h"

#include <poll.h>

#include "magitrickle/cancel.h"

static bool fd_readable(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, 0) == 1 && (p.revents & POLLIN) != 0;
}

TEST starts_lowered(void) {
    mt_cancel_t *c = mt_cancel_new();
    ASSERT(c != NULL);
    ASSERT(!mt_cancel_raised(c));
    ASSERT(!fd_readable(mt_cancel_fd(c)));
    mt_cancel_free(c);
    PASS();
}

TEST raise_is_visible_both_ways(void) {
    mt_cancel_t *c = mt_cancel_new();
    mt_cancel_raise(c);
    ASSERT(mt_cancel_raised(c));
    /* A thread parked in poll() on a child's pipes must wake up too. */
    ASSERT(fd_readable(mt_cancel_fd(c)));
    mt_cancel_free(c);
    PASS();
}

TEST clear_lowers_and_drains(void) {
    mt_cancel_t *c = mt_cancel_new();
    mt_cancel_raise(c);
    mt_cancel_clear(c);
    ASSERT(!mt_cancel_raised(c));
    ASSERT(!fd_readable(mt_cancel_fd(c)));
    mt_cancel_free(c);
    PASS();
}

/* Requests pile up faster than passes run; the pipe must not fill and
 * block the requester. */
TEST repeated_raises_do_not_fill_the_pipe(void) {
    mt_cancel_t *c = mt_cancel_new();
    for (int i = 0; i < 100000; i++) { mt_cancel_raise(c); }
    ASSERT(mt_cancel_raised(c));
    mt_cancel_clear(c);
    ASSERT(!mt_cancel_raised(c));
    ASSERT(!fd_readable(mt_cancel_fd(c)));
    mt_cancel_free(c);
    PASS();
}

TEST reraise_after_clear(void) {
    mt_cancel_t *c = mt_cancel_new();
    mt_cancel_raise(c);
    mt_cancel_clear(c);
    mt_cancel_raise(c);
    ASSERT(mt_cancel_raised(c));
    ASSERT(fd_readable(mt_cancel_fd(c)));
    mt_cancel_free(c);
    PASS();
}

TEST null_is_never_raised(void) {
    ASSERT(!mt_cancel_raised(NULL));
    ASSERT_EQ(-1, mt_cancel_fd(NULL));
    mt_cancel_raise(NULL);
    mt_cancel_clear(NULL);
    mt_cancel_free(NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(starts_lowered);
    RUN_TEST(raise_is_visible_both_ways);
    RUN_TEST(clear_lowers_and_drains);
    RUN_TEST(repeated_raises_do_not_fill_the_pipe);
    RUN_TEST(reraise_after_clear);
    RUN_TEST(null_is_never_raised);
    GREATEST_MAIN_END();
}
