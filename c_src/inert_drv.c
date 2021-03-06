/* Copyright (c) 2013, Michael Santos <michael.santos@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>

#include <sys/time.h>
#include <sys/resource.h>

#include <string.h>

#include <unistd.h>
#include <fcntl.h>

#include "erl_driver.h"
#include "ei.h"

#define INERT_FDSET     1
#define INERT_FDCLR     2

typedef struct {
    ErlDrvTermData caller;
    int mode;
} inert_state_t;

typedef struct {
    ErlDrvPort port;
    u_int32_t maxfd;
    inert_state_t *state;
} inert_drv_t;

typedef union {
    ErlDrvEvent ev;
    int32_t fd;
} inert_fd_t;

static void inert_drv_ready(ErlDrvData, ErlDrvEvent, int);
static ErlDrvSSizeT inert_errno(char **, ErlDrvSizeT *, int);
static ErlDrvSSizeT inert_copy(char **, ErlDrvSizeT *, char *, size_t);

    static ErlDrvData
inert_drv_start(ErlDrvPort port, char *buf)
{
    inert_drv_t *d = NULL;
    struct rlimit rlim = {0};

    d = driver_alloc(sizeof(inert_drv_t));
    if (!d)
        return ERL_DRV_ERROR_ERRNO;

    (void)memset(d, 0, sizeof(inert_drv_t));
    d->port = port;

    if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
        return ERL_DRV_ERROR_ERRNO;

    d->maxfd = rlim.rlim_cur;
    d->state = driver_alloc(rlim.rlim_cur * sizeof(inert_state_t));
    if (!d)
        return ERL_DRV_ERROR_ERRNO;

    (void)memset(d->state, 0, rlim.rlim_cur * sizeof(inert_state_t));

    return (ErlDrvData)d;
}

    static void
inert_drv_stop(ErlDrvData drv_data)
{
    inert_drv_t *d = (inert_drv_t *)drv_data;
    int fd = 0;

    for (fd = 0; fd < d->maxfd; fd++) {
        if (d->state[fd].mode & (ERL_DRV_READ|ERL_DRV_WRITE)) {
            inert_fd_t event = {0};
            event.fd = fd;
            (void)driver_select(d->port, event.ev, d->state[fd].mode, 0);
        }
    }

    driver_free(d->state);
    driver_free(d);
}

    static ErlDrvSSizeT
inert_drv_control(ErlDrvData drv_data, unsigned int command,
        char *buf, ErlDrvSizeT len,
        char **rbuf, ErlDrvSizeT rlen)
{
    inert_drv_t *d = (inert_drv_t *)drv_data;

    inert_fd_t event = {0};
    int mode = 0;
    int on = 1;

    if (len != 8)
        return -1;

    event.fd = ((unsigned char)buf[0] << 24)
        | ((unsigned char)buf[1] << 16)
        | ((unsigned char)buf[2] << 8)
        | (unsigned char)buf[3];

    mode = ((unsigned char)buf[4] << 24)
        | ((unsigned char)buf[5] << 16)
        | ((unsigned char)buf[6] << 8)
        | (unsigned char)buf[7];

    if (event.fd < 0 || event.fd >= d->maxfd || fcntl(event.fd, F_GETFD) < 0)
        return inert_errno(rbuf, &rlen, EBADFD);

    switch (command) {
        case INERT_FDSET:
            /* Successive calls to driver_select do not overwrite the
             * previous mode of an event. From testing, it looks like
             * the modes are OR'ed together.
             *
             * Reset the mode when applying a new mode (ignoring
             * ERL_DRV_USE).
             */
            if (driver_select(d->port, event.ev, ERL_DRV_READ|ERL_DRV_WRITE, 0) < 0)
                return -1;

            d->state[event.fd].mode = mode;
            d->state[event.fd].caller = driver_caller(d->port);

            break;
        case INERT_FDCLR:
            on = 0;
            d->state[event.fd].mode &= ~mode;
            break;
        default:
            return inert_errno(rbuf, &rlen, EINVAL);
    }

    *rbuf = NULL;
    return driver_select(d->port, event.ev, mode, on);
}

    static void
inert_drv_ready_input(ErlDrvData drv_data, ErlDrvEvent event)
{
    inert_drv_ready(drv_data, event, ERL_DRV_READ);
}

    static void
inert_drv_ready_output(ErlDrvData drv_data, ErlDrvEvent event)
{
    inert_drv_ready(drv_data, event, ERL_DRV_WRITE);
}

    static void
inert_drv_ready(ErlDrvData drv_data, ErlDrvEvent event, int mode)
{
    inert_drv_t *d = (inert_drv_t *)drv_data;
    int32_t fd = ((inert_fd_t)event).fd;
    char *tag = NULL;

    (void)driver_select(d->port, event, mode, 0);
    d->state[fd].mode &= ~mode;

    switch (mode) {
        case ERL_DRV_READ:
            tag = "inert_read";
            break;
        case ERL_DRV_WRITE:
            tag = "inert_write";
            break;
        default:
            tag = "inert_error";
    }

    ErlDrvTermData res[] = {
        ERL_DRV_ATOM, driver_mk_atom(tag),
        ERL_DRV_PORT, driver_mk_port(d->port),
        ERL_DRV_INT, fd,
        ERL_DRV_TUPLE, 3
        };

    (void)erl_drv_send_term(
            driver_mk_port(d->port),
            d->state[fd].caller,
            res,
            sizeof(res) / sizeof(res[0])
            );
}

    static ErlDrvSSizeT
inert_errno(char **rbuf, ErlDrvSizeT *rlen, int errnum)
{
    char *errstr = NULL;

    errstr = erl_errno_id(errnum);

    return inert_copy(rbuf, rlen, errstr, strlen(errstr));
}

    static ErlDrvSSizeT
inert_copy(char **rbuf, ErlDrvSizeT *rlen, char *buf, size_t buflen)
{
    /* The max atom len is 255 bytes (not including the NULL) */
    if (buflen > 255)
        goto ERR;

    if (buflen > *rlen)
        *rbuf = driver_alloc(buflen);

    if (*rbuf == NULL)
        goto ERR;

    (void)memset(*rbuf, 0, *rlen);
    (void)memcpy(*rbuf, buf, buflen);

    *rlen = buflen;
    return buflen;

ERR:
    *rlen = 0;
    return -1;
}

ErlDrvEntry inert_driver_entry = {
    NULL,                           /* F_PTR init, called when driver is loaded */
    inert_drv_start,                /* L_PTR start, called when port is opened */
    inert_drv_stop,                 /* F_PTR stop, called when port is closed */
    NULL,                           /* F_PTR output, called when erlang has sent */
    inert_drv_ready_input,          /* F_PTR ready_input, called when input descriptor ready */
    inert_drv_ready_output,         /* F_PTR ready_output, called when output descriptor ready */
    "inert_drv",                    /* char *driver_name, the argument to open_port */
    NULL,                           /* F_PTR finish, called when unloaded */
    NULL,                           /* void *handle, Reserved by VM */
    inert_drv_control,              /* F_PTR control, port_command callback */
    NULL,                           /* F_PTR timeout, reserved */
    NULL,                           /* F_PTR outputv, reserved */
    NULL,                           /* F_PTR ready_async, only for async drivers */
    NULL,                           /* F_PTR flush, called when port is about
                                       to be closed, but there is data in driver
                                       queue */
    NULL,                           /* F_PTR call, much like control, sync call
                                       to driver */
    NULL,                           /* F_PTR event, called when an event selected
                                       by driver_event() occurs. */
    ERL_DRV_EXTENDED_MARKER,        /* int extended marker, Should always be
                                       set to indicate driver versioning */
    ERL_DRV_EXTENDED_MAJOR_VERSION, /* int major_version, should always be
                                       set to this value */
    ERL_DRV_EXTENDED_MINOR_VERSION, /* int minor_version, should always be
                                       set to this value */
    ERL_DRV_FLAG_USE_PORT_LOCKING|ERL_DRV_FLAG_SOFT_BUSY,  /* int driver_flags, see documentation */
    NULL,                           /* void *handle2, reserved for VM use */
    NULL,                           /* F_PTR process_exit, called when a
                                       monitored process dies */
    NULL                            /* F_PTR stop_select, called to close an
                                       event object */
};

DRIVER_INIT(inert_drv) /* must match name in driver_entry */
{
    return &inert_driver_entry;
}
