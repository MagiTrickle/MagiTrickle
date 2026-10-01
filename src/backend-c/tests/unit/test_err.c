#include "greatest.h"

#include <errno.h>
#include <string.h>

#include "magitrickle/err.h"

TEST every_code_has_a_string(void)
{
    for (int e = MT_OK; e <= MT_ERR_CANCELED; e++) {
        const char *s = mt_err_str((mt_err_t)e);
        ASSERT(s != NULL);
        ASSERT(strlen(s) > 0);
        ASSERT(strcmp(s, "unknown error") != 0);
    }
    PASS();
}

TEST errno_mapping(void)
{
    ASSERT_EQ(MT_OK, mt_err_from_errno(0));
    ASSERT_EQ(MT_ERR_NOMEM, mt_err_from_errno(ENOMEM));
    ASSERT_EQ(MT_ERR_AGAIN, mt_err_from_errno(EAGAIN));
    ASSERT_EQ(MT_ERR_AGAIN, mt_err_from_errno(EINTR));
    ASSERT_EQ(MT_ERR_TIMEOUT, mt_err_from_errno(ETIMEDOUT));
    ASSERT_EQ(MT_ERR_NOENT, mt_err_from_errno(ENOENT));
    ASSERT_EQ(MT_ERR_EXIST, mt_err_from_errno(EEXIST));
    ASSERT_EQ(MT_ERR_SYS, mt_err_from_errno(E2BIG));
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(every_code_has_a_string);
    RUN_TEST(errno_mapping);
    GREATEST_MAIN_END();
}
