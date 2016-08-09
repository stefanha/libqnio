/*
 * Network IO library for VxHS QEMU block driver (Veritas Technologies)
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2014-08-15 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "io_qnio.h"
#include "defs.h"

void
client_callback(struct qnio_msg *msg)
{
    iio_msg *reply;
    struct iovec iov;
    kvset_t *outps = NULL;
    qnio_stream *stream = NULL;
    uint32_t reason = IIO_REASON_DONE;
    struct ioapi_ctx *apictx = (struct ioapi_ctx *) msg->reserved;

    nioDbg("Got a response");

    reply = malloc(sizeof(iio_msg));

    if(msg->hinfo.err == QNIOERROR_CHANNEL_HUP) /* channel disconnect callback */
    {
        reason = IIO_REASON_HUP; 
        reply->iio_error = QNIOERROR_CHANNEL_HUP;
        reply->type = IIOM_DTYPE_NONE;
        apictx->io_cb(0, IIO_REASON_HUP, NULL, reply);
        free(msg);
        return;
    }

    reply->iio_error = msg->hinfo.err;
    if(reply->iio_error != QNIOERROR_SUCCESS)
    {
        reason = IIO_REASON_EVENT; 
    }
    reply->type = IIOM_DTYPE_NONE;

    if (msg->hinfo.data_type == DATA_TYPE_PS)
    {
        reply->iio_data.iio_ps = NULL;
        reply->iio_data.iio_json = NULL;
        if(msg->recv)
        {
            if(apictx->need_json)
            {
                iov = io_vector_at(msg->recv,0);
                stream = new_qnio_stream(0);
                kvset_unmarshal(iov.iov_base,&outps);
                kvset_print(stream,0,outps);
                reply->iio_data.iio_json = (char *)malloc(stream->size + 1);
                memcpy(reply->iio_data.iio_json,stream->buffer,stream->size);
                reply->iio_data.iio_json[stream->size] = '\0';
                qnio_delete_stream(stream);
                kvset_free(outps);
                reply->type = IIOM_DTYPE_JSON;
            }
            else
            {
                reply->iio_data.iio_ps = new_ps(0);
                iov = io_vector_at(msg->recv, 0);
                kvset_unmarshal(iov.iov_base, &reply->iio_data.iio_ps);
                reply->type = IIOM_DTYPE_PS;
            }
        }
    }
    else if(msg->hinfo.data_type == DATA_TYPE_RAW)
    {
        reply->iio_data.iio_buf.iio_nbytes = msg->hinfo.io_nbytes;
        if(msg->recv)
        {
            iov = io_vector_at(msg->recv, 0);
            reply->iio_data.iio_buf.iio_recv_buf = iov.iov_base;
            reply->iio_data.iio_buf.iio_len = iov.iov_len;
            reply->type = IIOM_DTYPE_BYTES;
        }
    }
    reply->iio_opcode = msg->hinfo.opcode;

    apictx->io_cb(msg->rfd, reason, msg->user_ctx, reply);

    if(msg->send)
        io_vector_delete(msg->send);

    if(msg->recv)
        io_vector_delete(msg->recv);

    qnio_free_io_pool_buf(msg);
    qnio_free_msg(msg);

    return;
}

struct ioapi_ctx *
iio_init(iio_cb_t cb)
{
    struct ioapi_ctx *apictx = NULL;

    if(cb == NULL)
    {
        nioDbg("callback is null");
        return NULL;
    }

    apictx = (struct ioapi_ctx *)malloc(sizeof (struct ioapi_ctx));
    if (NULL == apictx)
    {
        nioDbg ("Failed to allocate ioapi_ctx");
        return NULL;
    }

    safe_map_init(&apictx->channels);
    safe_map_init(&apictx->devices);
    apictx->next_fd = 1;
    apictx->io_cb = cb;

    apictx->qnioctx = qnio_client_init(client_callback);

    apictx->qnioctx->apictx = apictx;

    return apictx;
}

int32_t 
iio_open(struct ioapi_ctx *apictx, const char *uri, uint32_t flags)
{
    char host[NAME_SZ] = {0};
    char port[NAME_SZ] = {0};
    int32_t cfd = -1;
    int match = 0;
    int err;
    
    if(!uri)
        return -1;

    match = sscanf(uri, "of://%99[^:]:%99s", host, port);
    if(match != 2)
    {
        nioDbg("parse uri failed %d [%s] [%s]",match, host, port);
        return -1;
    }

    err = qnio_create_channel(apictx->qnioctx, host, port);

    if (err == QNIO_ERR_SUCCESS)
    { 
        cfd = ck_pr_faa_int(&(apictx->next_fd), 1);
        safe_map_insert(&apictx->channels, cfd, strdup(host));
        nioDbg("New channel is ready %d", cfd);
    }
    else if (err == QNIO_ERR_CHAN_EXISTS)
    {
        cfd = ck_pr_faa_int(&(apictx->next_fd), 1);
        safe_map_insert(&apictx->channels, cfd, strdup(host));
        nioDbg("Existing channel is ready %d", cfd);
    }
    return cfd;
}

int32_t 
iio_devopen(struct ioapi_ctx *apictx, int32_t cfd, const char *devpath, uint32_t flags)
{
    char *channel;
    char chandev[NAME_SZ] = {0};
    int devfd;
    
    if(cfd < 0)
    {
        errno = EBADF;
        return -1;
    }
    if(!devpath)
        return -1;
    
    channel = (char *) safe_map_find(&apictx->channels, cfd);
    if(!channel)
    {
        errno = ENODEV;
        return -1;
    }

    devfd = ck_pr_faa_int(&(apictx->next_fd), 1);
    sprintf(chandev, "%s %s", channel, devpath);
    safe_map_insert(&apictx->devices, devfd, strdup(chandev));

    return devfd;
}

int32_t
iio_devclose(struct ioapi_ctx *apictx, int32_t cfd, int32_t rfd)
{
    char *chandev = NULL;

    chandev = (char *) safe_map_find(&apictx->devices, rfd);
    if(!chandev)
    {
        nioDbg("Could not find device for fd");
        errno = ENODEV;
        return -1;
    }
    safe_map_delete(&apictx->devices, rfd);
    return 0;
}

int32_t
iio_close(struct ioapi_ctx *apictx, uint32_t cfd)
{
    char *host = NULL;

    host = (char *) safe_map_find(&apictx->channels, cfd);
    if(!host)
    {
        nioDbg("Could not find channel for fd");
        errno = EBADF;
        return -1;
    }
    safe_map_delete(&apictx->channels, cfd);
    return 0;
}

int32_t
iio_read(struct ioapi_ctx *apictx, int32_t rfd, unsigned char *buf, uint64_t size, uint64_t offset, void *ctx, uint32_t flags)
{    
    char *chandev = NULL;
    struct qnio_msg *msg = NULL;
    char channel[NAME_SZ] = {0};
    char device[NAME_SZ] = {0};
    int err;
    struct iovec iov;

    chandev = (char *) safe_map_find(&apictx->devices, rfd);
    if(!chandev)
    {
        nioDbg("Could not find device for fd");
        errno = ENODEV;
        return -1;
    }

    sscanf(chandev,"%s %s", channel, device);

    msg = qnio_alloc_msg(apictx->qnioctx); 
    msg->reserved = apictx;

    msg->rfd = rfd;
    msg->hinfo.opcode = IOR_READ_REQUEST;
    msg->hinfo.io_offset = offset;
    msg->hinfo.io_size = size;
    msg->hinfo.io_flags |= IOR_SOURCE_TAG_APPIO;
    msg->hinfo.data_type = DATA_TYPE_RAW;
    iov.iov_base = buf;
    iov.iov_len = size;
    msg->recv = new_io_vector(1, NULL);
    io_vector_pushback(msg->recv, iov);
    msg->send = NULL;
    msg->hinfo.payload_size = 0;
    strncpy(msg->hinfo.target,device,strlen(device));
    msg->channel = channel;
    msg->user_ctx = ctx;
 
    if(flags & IIO_FLAG_ASYNC)
    {
        msg->hinfo.flags = QNIO_FLAG_REQ | QNIO_FLAG_REQ_NEED_RESP;
        err = qnio_send(apictx->qnioctx, msg);
        if(err != 0)
        {
            qnio_free_msg(msg);
        }
        return err;
    }
    else
    {
        err = qnio_send_recv(apictx->qnioctx, msg);
        qnio_free_msg(msg);
        return err;
    }

    return -1;
}

int32_t
iio_writev(struct ioapi_ctx *apictx, int32_t rfd, struct iovec *iov, int iovcnt, uint64_t offset, void *ctx, uint32_t flags)
{
    char *chandev = NULL;
    struct qnio_msg *msg = NULL;
    char channel[NAME_SZ] = {0};
    char device[NAME_SZ] = {0};
    int i, err;
    uint64_t io_size = 0;

    chandev = (char *) safe_map_find(&apictx->devices, rfd);
    if(!chandev)
    {
        nioDbg("Could not find device for fd");
        errno = ENODEV;
        return -1;
    }

    sscanf(chandev,"%s %s", channel, device);

    msg = qnio_alloc_msg(apictx->qnioctx); 
    msg->reserved = apictx;

    msg->rfd = rfd;
    msg->hinfo.opcode = IOR_WRITE_REQUEST;
    msg->hinfo.data_type = DATA_TYPE_RAW;
    msg->send = new_io_vector(1, NULL);
    msg->recv = NULL;
    msg->hinfo.payload_size = 0;
    for(i=0; i<iovcnt; i++)
    {
        io_vector_pushback(msg->send, iov[i]);
        io_size += iov[i].iov_len;
    }

    if (io_size > IIO_IO_BUF_SIZE)
    {
        /* return payload too big */
        io_vector_delete(msg->send);
        qnio_free_msg(msg);
        errno = EFBIG;
        return (-1);
    }

    msg->hinfo.payload_size = io_size;
    msg->hinfo.io_offset = offset;
    msg->hinfo.io_flags |= IOR_SOURCE_TAG_APPIO;
    strncpy(msg->hinfo.target,device,strlen(device));
    msg->channel = channel;
    msg->user_ctx = ctx;
 
    if(flags & IIO_FLAG_ASYNC)
    {
        msg->hinfo.flags = QNIO_FLAG_REQ;
        if(flags & IIO_FLAG_DONE)
        {
            msg->hinfo.flags |= QNIO_FLAG_REQ_NEED_ACK;
        }
        err = qnio_send(apictx->qnioctx, msg);
        if(err != 0)
        {
            qnio_free_msg(msg);
        }
        return err;
    }
    else
    {
        err = qnio_send_recv(apictx->qnioctx, msg);
        qnio_free_msg(msg);
        return err;
    }
}

int32_t 
iio_ioctl_json(struct ioapi_ctx *apictx, int32_t rfd, uint32_t opcode, char *injson, char **outjson, void *ctx, uint32_t flags)
{
    char *chandev = NULL;
    struct qnio_msg *msg = NULL;
    char channel[NAME_SZ] = {0};
    char device[NAME_SZ] = {0};
    struct iovec data, out;
    int err;
    kvset_t *inps = NULL;
    kvset_t *outps = NULL;
    qnio_stream *stream = NULL;

    chandev = (char *) safe_map_find(&apictx->devices, rfd);
    if(!chandev)
    {
        nioDbg("Could not find device for fd");
        errno = ENODEV;
        return -1;
    }

    sscanf(chandev,"%s %s", channel, device);
    if (injson != NULL)
    {
        inps = parse_json(injson);
        if(inps == NULL)
        {
            nioDbg("Parse json failed");
            return -1;
        }
    }

    msg = qnio_alloc_msg(apictx->qnioctx); 
    msg->reserved = apictx;

    msg->rfd = rfd;
    msg->hinfo.opcode = opcode;
    msg->hinfo.data_type = DATA_TYPE_PS;
    msg->hinfo.payload_size = 0;
    data.iov_len = 0;
    if (inps != NULL)
    {
        msg->send = new_io_vector(1, NULL);
        data.iov_base = kvset_marshal(inps, (int *)&(data.iov_len));
        io_vector_pushback(msg->send, data);
        kvset_free(inps);
    }
    msg->recv = NULL;
    msg->hinfo.payload_size = data.iov_len;
    strncpy(msg->hinfo.target,device,strlen(device));
    msg->channel = channel;
    msg->user_ctx = ctx;
 
    if(flags & IIO_FLAG_ASYNC)
    {
        msg->hinfo.flags = QNIO_FLAG_REQ;
        if(flags & IIO_FLAG_DONE)
        {
            msg->hinfo.flags |= QNIO_FLAG_REQ_NEED_RESP;
        }
        err = qnio_send(apictx->qnioctx, msg);
        if(err != 0)
        {
            qnio_free_io_pool_buf(msg);
            qnio_free_msg(msg);
        }
        return err;
    }
    else
    {
        err = qnio_send_recv(apictx->qnioctx, msg);
        if(err == 0)
        {
            if (msg->recv)
            {
                out = io_vector_at(msg->recv, 0);
                stream = new_qnio_stream(0);
                kvset_unmarshal(out.iov_base, &outps);
                kvset_print(stream, 0, outps); 
                *outjson = (char *) malloc(stream->size + 1);
                memcpy(*outjson, stream->buffer, stream->size);
		((char *)*outjson)[stream->size] = '\0';
                qnio_delete_stream(stream);
                kvset_free(outps);
                io_vector_delete(msg->recv);
            }
        }
        qnio_free_io_pool_buf(msg);
        if (msg->send)
        {
            io_vector_delete(msg->send);
        }
        qnio_free_msg(msg);
        return err;
    }
}