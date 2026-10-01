#include "greatest.h"

#include <pthread.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#include "magitrickle/loop.h"

/* one-shot timer stops the loop */
static void stop_timer_cb(mt_loop_t *loop, void *ud)
{
    int *fired = ud;
    (*fired)++;
    mt_loop_stop(loop);
}

TEST oneshot_timer_fires_and_stops(void)
{
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int fired = 0;
    ASSERT_EQ(MT_OK,
              mt_loop_add_timer(loop, 10, 0, stop_timer_cb, &fired, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(1, fired);
    mt_loop_destroy(loop);
    PASS();
}

struct periodic_ctx {
    int fires;
};

static void periodic_cb(mt_loop_t *loop, void *ud)
{
    struct periodic_ctx *ctx = ud;
    ctx->fires++;
    if (ctx->fires >= 3) {
        mt_loop_stop(loop);
    }
}

TEST periodic_timer_fires_repeatedly(void)
{
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    struct periodic_ctx ctx = {0};
    ASSERT_EQ(MT_OK,
              mt_loop_add_timer(loop, 5, 5, periodic_cb, &ctx, NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(3, ctx.fires);
    mt_loop_destroy(loop);
    PASS();
}

struct pipe_ctx {
    char buf[16];
    ssize_t n;
};

static void pipe_cb(mt_loop_t *loop, int fd, uint32_t events, void *ud)
{
    struct pipe_ctx *ctx = ud;
    (void)events;
    ctx->n = read(fd, ctx->buf, sizeof(ctx->buf));
    mt_loop_stop(loop);
}

TEST fd_readable_event_delivers_data(void)
{
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));

    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    struct pipe_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_EQ(MT_OK, mt_loop_add_fd(loop, fds[0], EPOLLIN, pipe_cb, &ctx));

    ASSERT_EQ(5, write(fds[1], "hello", 5));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(5, ctx.n);
    ASSERT_EQ(0, memcmp(ctx.buf, "hello", 5));

    ASSERT_EQ(MT_OK, mt_loop_del_fd(loop, fds[0]));
    close(fds[0]);
    close(fds[1]);
    mt_loop_destroy(loop);
    PASS();
}

static void post_cb(mt_loop_t *loop, void *ud)
{
    int *hit = ud;
    (*hit)++;
    mt_loop_stop(loop);
}

static void *poster_thread(void *arg)
{
    mt_loop_t *loop = arg;
    /* give the loop a moment to start */
    struct timespec delay = {0, 10000000L};
    nanosleep(&delay, NULL);
    static int hit = 0;
    mt_loop_post(loop, post_cb, &hit);
    return &hit;
}

TEST cross_thread_post_executes_on_loop(void)
{
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));

    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, poster_thread, loop));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));

    void *ret = NULL;
    pthread_join(th, &ret);
    ASSERT_EQ(1, *(int *)ret);
    mt_loop_destroy(loop);
    PASS();
}

TEST del_timer_prevents_firing(void)
{
    mt_loop_t *loop = NULL;
    ASSERT_EQ(MT_OK, mt_loop_create(&loop));
    int fired = 0;
    int cancel_id = 0;
    ASSERT_EQ(MT_OK, mt_loop_add_timer(loop, 50, 0, stop_timer_cb, &fired,
                                       &cancel_id));
    ASSERT_EQ(MT_OK, mt_loop_del_timer(loop, cancel_id));
    /* a second timer stops the loop; the cancelled one must not fire */
    int stop_fired = 0;
    ASSERT_EQ(MT_OK,
              mt_loop_add_timer(loop, 80, 0, stop_timer_cb, &stop_fired,
                                NULL));
    ASSERT_EQ(MT_OK, mt_loop_run(loop));
    ASSERT_EQ(0, fired);
    ASSERT_EQ(1, stop_fired);
    mt_loop_destroy(loop);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(oneshot_timer_fires_and_stops);
    RUN_TEST(periodic_timer_fires_repeatedly);
    RUN_TEST(fd_readable_event_delivers_data);
    RUN_TEST(cross_thread_post_executes_on_loop);
    RUN_TEST(del_timer_prevents_firing);
    GREATEST_MAIN_END();
}
