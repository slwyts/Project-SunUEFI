/* SPDX-License-Identifier: BSD-2-Clause */
#include "scmi_channel.h"

enum { STATUS = 4, FLAGS = 16, LENGTH = 20, HEADER = 24, PAYLOAD = 28,
       CHANNEL_FREE = 1, CHANNEL_ERROR = 2 };

int scmi_channel_init(struct scmi_channel *channel, const struct scmi_channel_io *io,
                      uint32_t bytes, uint32_t timeout_us)
{
    if (!channel || !io || !io->read || !io->write || !io->ring || !io->now_us || !io->wait ||
        bytes < PAYLOAD + 4 || (bytes & 3) || !timeout_us) return SCMI_PERF_INVALID;
    *channel = (struct scmi_channel){ .io = *io, .bytes = bytes, .timeout_us = timeout_us };
    return 0;
}

/* FREE, not an elapsed timeout or an error bit, returns TX ownership. */
static int wait_free(struct scmi_channel *c, uint64_t since, uint32_t *status)
{
    const struct scmi_channel_io *io = &c->io;
    for (;;) {
        *status = io->read(io->context, STATUS);
        if (*status == UINT32_MAX) return SCMI_PERF_PROTOCOL_ERROR;
        if (*status & CHANNEL_FREE) return 0;
        if (io->now_us(io->context) - since >= c->timeout_us) return SCMI_PERF_TIMEOUT;
        io->wait(io->context);
    }
}

int scmi_channel_drain(struct scmi_channel *c)
{
    if (!c || !c->io.now_us) return SCMI_PERF_INVALID;
    if (!c->pending) return 0;
    uint32_t status;
    int err = wait_free(c, c->io.now_us(c->io.context), &status);
    if (err) return err;
    uint32_t header = c->io.read(c->io.context, HEADER);
    c->pending = 0;
    return header == c->pending_header ? 0 : SCMI_PERF_PROTOCOL_ERROR;
}

int scmi_channel_message(void *context, unsigned protocol, unsigned command,
                         const uint32_t *args, size_t nargs, uint32_t *result,
                         size_t capacity, size_t *count)
{
    struct scmi_channel *c = context;
    if (!c || !c->io.now_us || !count || (!args && nargs) || (!result && capacity) ||
        protocol > 0xff || command > 0xff || nargs > (c->bytes - PAYLOAD) / 4)
        return SCMI_PERF_INVALID;
    *count = 0;
    int err = scmi_channel_drain(c);
    if (err) return err;
    const struct scmi_channel_io *io = &c->io;
    uint32_t status = io->read(io->context, STATUS);
    if (status == UINT32_MAX) return SCMI_PERF_PROTOCOL_ERROR;
    if (!(status & CHANNEL_FREE)) return SCMI_PERF_BUSY;
    uint32_t header = command | (protocol << 10) | ((++c->token & 0x3ffu) << 18);
    io->write(io->context, STATUS, 0);
    io->write(io->context, FLAGS, 0);  /* Synchronous reply; leave the RX channel to notifications. */
    io->write(io->context, LENGTH, 4 + (uint32_t)nargs * 4);
    io->write(io->context, HEADER, header);
    for (size_t i = 0; i < nargs; ++i) io->write(io->context, PAYLOAD + (unsigned)i * 4, args[i]);
    c->pending = 1;
    c->pending_header = header;
    io->ring(io->context, header);
    err = wait_free(c, io->now_us(io->context), &status);
    if (err) return err;
    c->pending = 0;
    uint32_t length = io->read(io->context, LENGTH);
    if ((status & CHANNEL_ERROR) || io->read(io->context, HEADER) != header ||
        length < 8 || length > c->bytes - HEADER || (length & 3)) return SCMI_PERF_PROTOCOL_ERROR;
    int32_t reply_status = (int32_t)io->read(io->context, PAYLOAD);
    if (reply_status) return reply_status < 0 ? reply_status : SCMI_PERF_PROTOCOL_ERROR;
    size_t words = (length - 8) / 4;
    if (words > capacity) return SCMI_PERF_NO_SPACE;
    for (size_t i = 0; i < words; ++i) result[i] = io->read(io->context, PAYLOAD + 4 + (unsigned)i * 4);
    *count = words;
    return 0;
}
