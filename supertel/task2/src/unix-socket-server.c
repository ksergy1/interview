#include "unix-socket-server.h"
#include "common.h"
#include "log.h"

#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>

#define BACKLOG 50

static
void close_connections(avl_tree_node_t *atn);
static inline
void close_connection(uss_connection_t *ussc);
static
void acceptor(int fd, io_svc_op_t op, uss_t *srv);
static
void data_may_be_sent(int fd, io_svc_op_t op, uss_connection_t *ussc);
static
void data_may_be_read(int fd, io_svc_op_t op, uss_connection_t *ussc);

void close_connections(avl_tree_node_t *atn) {
    if (!atn)
        return;

    close_connections(atn->left);
    close_connections(atn->right);

    close_connection((uss_connection_t *)atn->data);
}

void close_connection(uss_connection_t *ussc) {
    io_service_remove_job(ussc->host->iosvc, ussc->fd, IO_SVC_OP_READ);
    io_service_remove_job(ussc->host->iosvc, ussc->fd, IO_SVC_OP_WRITE);

    buffer_deinit(&ussc->read_task.b);
    buffer_deinit(&ussc->write_task.b);

    shutdown(ussc->fd, SHUT_RDWR);
    close(ussc->fd);
}

void acceptor(int fd, io_svc_op_t op, uss_t *srv) {
    int flags;
    avl_tree_node_t *atn;
    uss_connection_t *ussc;
    struct sockaddr_un addr;
    socklen_t addr_len;

    fd = accept(srv->fd, (struct sockaddr *)&addr, &addr_len);

    if (fd < 0) {
        LOG(LOG_LEVEL_ERROR,
            "Can't accept connection: %s\n",
            strerror(errno));

        return;
    }

    atn = avl_tree_add(&srv->connections, fd);
    ussc = (uss_connection_t *)atn->data;
    memset(ussc, 0, sizeof(*ussc));

    ussc->host = srv;
    ussc->fd = fd;
    ussc->eof = false;
    buffer_init(&ussc->read_task.b, 0, bp_non_shrinkable);
    buffer_init(&ussc->write_task.b, 0, bp_non_shrinkable);

    if (srv->acceptor &&
        !srv->acceptor(srv, ussc, srv->acceptor_ctx))
        unix_socket_server_close_connection(srv, ussc);
}

void data_may_be_sent(int fd, io_svc_op_t op, uss_connection_t *ussc) {
    size_t bytes_wrote = ussc->write_task.currently_wrote;
    size_t bytes_to_write = ussc->write_task.b.user_size - bytes_wrote;
    ssize_t current_write;
    int err;

    errno = 0;
    while (bytes_to_write) {
        current_write = send(ussc->fd,
                             ussc->write_task.b.data + bytes_wrote,
                             bytes_to_write,
                             MSG_DONTWAIT | MSG_NOSIGNAL);

        if (current_write < 0) {
            if (errno == EINTR) {
                errno = 0;
                continue;
            }
            else
                break;
        }

        bytes_wrote += current_write;
        bytes_to_write -= current_write;
    }

    ussc->write_task.currently_wrote = bytes_wrote;

    err = errno;

    if (bytes_to_write && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;

    io_service_remove_job(ussc->host->iosvc, fd, IO_SVC_OP_WRITE);

    if (ussc->write_task.writer)
        ussc->write_task.writer(ussc->host, ussc, err, ussc->write_task.ctx);
}

void data_may_be_read(int fd, io_svc_op_t op, uss_connection_t *ussc) {
    size_t bytes_read = ussc->read_task.currently_read;
    size_t bytes_to_read;
    ssize_t current_read;
    bool eof = false;
    int err = 0;
    int pending, rc;

    bytes_to_read = ussc->read_task.b.user_size -ussc->read_task.b.offset
                    - bytes_read;

    rc = ioctl(fd, FIONREAD, &pending);

    if (0 == pending) /* EOF */ {
        ussc->eof = true;

        io_service_remove_job(ussc->host->iosvc, fd, IO_SVC_OP_READ);

        if (ussc->read_task.reader)
            ussc->read_task.reader(ussc->host, ussc, err, ussc->read_task.ctx);

        return;
    }

    if (pending > bytes_to_read)
        pending = bytes_to_read;

    errno = 0;
    while (bytes_to_read && !eof) {
        current_read = recv(ussc->fd,
                            ussc->read_task.b.data
                                + ussc->read_task.b.offset + bytes_read,
                            bytes_to_read,
                            MSG_DONTWAIT | MSG_NOSIGNAL);

        if (current_read < 0) {
            if (errno == EINTR) {
                errno = 0;
                continue;
            }
            else
                break;
        }

        if (current_read == 0) /* EOF */
            eof = true;

        bytes_read += current_read;
        bytes_to_read -= current_read;
    }

    ussc->read_task.currently_read = bytes_read;

    err = errno;

    ussc->eof = eof;
    if (eof) {
        io_service_remove_job(ussc->host->iosvc, fd, IO_SVC_OP_READ);

        if (ussc->read_task.reader)
            ussc->read_task.reader(ussc->host, ussc, err, ussc->read_task.ctx);

        return;
    }

    if (bytes_to_read && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;

    io_service_remove_job(ussc->host->iosvc, fd, IO_SVC_OP_READ);

    if (ussc->read_task.reader)
        ussc->read_task.reader(ussc->host, ussc, err, ussc->read_task.ctx);
}

/******************* API *******************/
bool unix_socket_server_init(uss_t *srv,
                             const char *name,
                             size_t name_len,
                             io_service_t *iosvc) {
    assert(srv && name && name_len);

    memset(srv, 0, sizeof(*srv));

    srv->iosvc = iosvc;
    srv->acceptor = NULL;

    srv->fd = allocate_unix_socket(name, name_len);

    if (srv->fd < 0)
        return false;

    avl_tree_init(&srv->connections, true, sizeof(uss_connection_t));

    return true;
}

void unix_socket_server_deinit(uss_t *srv) {
    assert(srv);

    close(srv->fd);
    close_connections(srv->connections.root);
}

bool unix_socket_server_listen(uss_t *srv,
                               uss_acceptor_t _acceptor, void *ctx) {
    assert(srv);

    srv->acceptor = _acceptor;
    srv->acceptor_ctx = ctx;

    if (listen(srv->fd, BACKLOG)) {
        LOG(LOG_LEVEL_WARN, "Can't listen: %s\n",
            strerror(errno));

        return false;
    }

    io_service_post_job(
        srv->iosvc, srv->fd, IO_SVC_OP_READ, !IOSVC_JOB_ONESHOT,
        (iosvc_job_function_t)acceptor,
        srv
    );
}

void unix_socket_server_close_connection(uss_t *srv, uss_connection_t *conn) {
    assert(srv && conn);

    avl_tree_remove(&srv->connections, conn->fd);

    close_connection(conn);
}

void unix_socket_server_send(uss_t *srv, uss_connection_t *conn,
                             const void *d, size_t sz,
                             uss_writer_t writer, void *ctx) {
    assert(srv && conn);

    conn->write_task.currently_wrote = 0;
    buffer_realloc(&conn->write_task.b, sz);
    memcpy(conn->write_task.b.data, d, sz);
    conn->write_task.writer = writer;
    conn->write_task.ctx = ctx;

    io_service_post_job(
        srv->iosvc, conn->fd, IO_SVC_OP_WRITE, !IOSVC_JOB_ONESHOT,
        (iosvc_job_function_t)data_may_be_sent,
        conn
    );
}

void unix_socket_server_recv(uss_t *srv, uss_connection_t *conn,
                             size_t sz,
                             uss_reader_t reader, void *ctx) {
    assert(srv && conn);

    conn->read_task.currently_read = 0;
    conn->read_task.b.offset = conn->read_task.b.user_size;
    buffer_realloc(&conn->read_task.b, conn->read_task.b.offset + sz);
    conn->read_task.reader = reader;
    conn->read_task.ctx = ctx;

    io_service_post_job(
        srv->iosvc, conn->fd, IO_SVC_OP_READ, !IOSVC_JOB_ONESHOT,
        (iosvc_job_function_t)data_may_be_read,
        conn
    );
}
