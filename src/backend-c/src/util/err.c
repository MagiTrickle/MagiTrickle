#include "magitrickle/err.h"

#include <errno.h>

const char *mt_err_str(mt_err_t err)
{
    switch (err) {
    case MT_OK:          return "ok";
    case MT_ERR_NOMEM:   return "out of memory";
    case MT_ERR_INVAL:   return "invalid argument";
    case MT_ERR_IO:      return "i/o error";
    case MT_ERR_AGAIN:   return "try again";
    case MT_ERR_LIMIT:   return "limit reached";
    case MT_ERR_TIMEOUT: return "timeout";
    case MT_ERR_CLOSED:  return "closed";
    case MT_ERR_EXIST:   return "already exists";
    case MT_ERR_NOENT:   return "not found";
    case MT_ERR_PROTO:   return "protocol error";
    case MT_ERR_STATE:   return "invalid state";
    case MT_ERR_SYS:     return "system error";
    case MT_ERR_UPSTREAM: return "upstream fetch failed";
    case MT_ERR_CANCELED: return "canceled";
    }
    return "unknown error";
}

mt_err_t mt_err_from_errno(int errnum)
{
    switch (errnum) {
    case 0:           return MT_OK;
    case ENOMEM:      return MT_ERR_NOMEM;
    case EINVAL:      return MT_ERR_INVAL;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINTR:       return MT_ERR_AGAIN;
    case ETIMEDOUT:   return MT_ERR_TIMEOUT;
    case EEXIST:      return MT_ERR_EXIST;
    case ENOENT:
    case ESRCH:       return MT_ERR_NOENT;
    case EPROTO:
    case EBADMSG:     return MT_ERR_PROTO;
    case EPIPE:
    case ECONNRESET:
    case EIO:         return MT_ERR_IO;
    default:          return MT_ERR_SYS;
    }
}
