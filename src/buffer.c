/*
 * Copyright (c) 2012-2014 Robin Schoonover
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "buffer.h"

#define MIN_SIZE                (1024)
#define MAX_PERSISTENT_SIZE     (4096)
#define MAX_SIZE                (2147483647)

/*
 * Currently we maintain a single buffer.  We realloc to increase the size.
 */

static int pull_data_until(PieBuffer *buf, size_t need);

int pie_buffer_init(PieBuffer *buffer) {
    buffer->buffer = NULL;
    buffer->buffer_size = 0;
    buffer->max_size = MAX_SIZE;
    
    buffer->reader = NULL;
    buffer->reader_udata = NULL;
    
    buffer->writer = NULL;
    buffer->writer_udata = NULL;

    pie_buffer_restart(buffer);
    
    return 0;
}

void pie_buffer_free_data(PieBuffer *buffer) {
    if(buffer->buffer != NULL)
        free(buffer->buffer);
    buffer->buffer = NULL;
    buffer->offset = 0;
    buffer->data_size = 0;
    buffer->buffer_size = 0;
}

void pie_buffer_restart(PieBuffer *buffer) {    
    buffer->offset = 0;
    buffer->data_size = 0;
    
    if(buffer->buffer_size > MAX_PERSISTENT_SIZE) {
        buffer->buffer_size = 0;
        free(buffer->buffer);
        buffer->buffer = NULL;
    }
}

void pie_buffer_set_reader(PieBuffer *buffer, buffer_pull_data *func, void *udata) {
    buffer->reader = func;
    buffer->reader_udata = udata;
}

void pie_buffer_set_maxsize(PieBuffer *buffer, size_t sz) {
    buffer->max_size = sz;
}

void pie_buffer_set_writer(PieBuffer *buffer, buffer_push_data *func, void *udata) {
    buffer->writer = func;
    buffer->writer_udata = udata;
}

ssize_t pie_buffer_recv(PieBuffer *buffer, int fd, size_t len) {
    char s[2048];
    ssize_t justread;
    
    if(len == 0)
        len = sizeof(s);

    while(len > 0) {
        if(len > sizeof(s))
            justread = sizeof(s);
        else
            justread = len;

        justread = recv(fd, s, justread, 0);
        if(justread > 0) {
            pie_buffer_append(buffer, s, justread);
            len -= justread;
        } else if(errno == EINTR) {
            justread = 0;
        } else {
            return justread;
        }
    }
    
    return 0;
}

ssize_t pie_buffer_send(PieBuffer *buffer, int fd, size_t len) {
    ssize_t justsent;
    ssize_t total = 0;

    pull_data_until(buffer, len);
    
    if(buffer->data_size - buffer->offset == 0)
        return 0;

    if(buffer->offset + len > buffer->data_size)
        len = buffer->data_size - buffer->offset + 1;

    while(len > 0) {
        char *p = buffer->buffer + buffer->offset;
        justsent = send(fd, p, len, 0);
        if(justsent <= 0) {
            if(justsent < 0 && errno == EINTR)
                continue;
            if(total > 0)
                return total;
            return justsent;
        }

        total += justsent;
        len -= justsent;
        buffer->offset += justsent;
    }

    return total;
}

int pie_buffer_append(PieBuffer *buffer, const char *data, size_t len) {
    size_t newlen;
    int i;
    
    /* move data to beginning of buffer */
    if(buffer->buffer != NULL && buffer->offset > 0 && len+buffer->offset>=buffer->buffer_size) {
        memmove(buffer->buffer,
                buffer->buffer+buffer->offset,
                buffer->data_size-buffer->offset);
    
        buffer->data_size -= buffer->offset;
        buffer->offset = 0;
    }
    
    /* we'll overflow our maximum limits */
    if(len >= buffer->max_size - buffer->data_size) {
        if(buffer->writer != NULL) {
            /* push all of current buffer */
            if(buffer->data_size > 0) {
                (*buffer->writer)(buffer,
                                  buffer->buffer+buffer->offset,
                                  buffer->data_size,
                                  buffer->writer_udata);
                buffer->offset = 0;
                buffer->data_size = 0;
            }
            
            /*
             * We have to push part of incoming data as well.  Push
             * blocks until we reach final block, and buffer that.
             * 
             * If we don't have to push any data, just fall through
             */
            if(len >= buffer->max_size) {
                for(i = 0; len - i > buffer->max_size; i += buffer->max_size) {
                    (*buffer->writer)(buffer,
                                 data + i,
                                 buffer->max_size,
                                 buffer->writer_udata);
                }
                
                /* adjust and fall through to buffer remaining */
                data = data + i;
                len = len - i;
            }
        } else {
            errno = ENOMEM;
            return -1;
        }
    }
    
    /* resize space */
    newlen = buffer->data_size + len;
    if(newlen >= buffer->buffer_size) {
        size_t newsize = ((len + buffer->data_size)/MIN_SIZE + 1)*MIN_SIZE;
        char *newbuf = realloc(buffer->buffer, newsize);

        if(newbuf == NULL) {
            errno = ENOMEM;
            return -1;
        }

        buffer->buffer = newbuf;
        buffer->buffer_size = newsize;
    }  
    
    /* store */
    memcpy(buffer->buffer + buffer->data_size, data, len);
    buffer->data_size = newlen;
    
    return 0;
}

int pie_buffer_flush(PieBuffer *buffer) {
    int result = -1;

    if(buffer->writer != NULL) {    
        result = (*buffer->writer)(buffer, 
                                  buffer->buffer+buffer->offset,
                                  buffer->data_size,
                                  buffer->writer_udata);
    }
    buffer->offset = 0;
    buffer->data_size = 0;

    return result;
}

static int pull_data(PieBuffer *buf) {
    if(buf->reader != NULL) {
        return (*buf->reader)(buf, buf->reader_udata);
    }
    
    return -1;
}

static int pull_data_until(PieBuffer *buf, size_t need) {
    while(need > buf->data_size - buf->offset)
        if(pull_data(buf) < 0)
            return -1;
    return 0;
}

int pie_buffer_getchar(PieBuffer *buffer) {
    pull_data_until(buffer, 1);

    if(buffer->buffer != NULL && buffer->offset <= buffer->data_size) {
        return buffer->buffer[buffer->offset++];
    }
    return -1;
}

size_t pie_buffer_size(PieBuffer *buffer) {
    return buffer->data_size - buffer->offset;
}

ssize_t pie_buffer_unget(PieBuffer *buffer, size_t len) {
    if(buffer->offset + len > buffer->data_size) {
        errno = EINVAL;
        return -1;
    }

    buffer->offset -= len;

    return 0;
}

ssize_t pie_buffer_getptr(PieBuffer *buffer, char **p, size_t len) {
    pull_data_until(buffer, len);
    
    if(buffer->data_size - buffer->offset == 0)
        return 0;
    
    if(buffer->offset + len > buffer->data_size)
        len = buffer->data_size - buffer->offset + 1;
    *p = buffer->buffer + buffer->offset;
    buffer->offset += len;
    
    return len;
}

ssize_t pie_buffer_findchar(PieBuffer *buffer, char c, size_t hint) {
    size_t base;
    size_t i;
    
    if(hint > 0)
        pull_data_until(buffer, hint);

    base = buffer->offset;
    do {
        for(i = base; i < buffer->data_size; i++) {
            if(buffer->buffer[i] == c) {
                return i - buffer->offset;
            }
        }
    } while(pull_data(buffer) >= 0);
    
    return -1;
}

ssize_t pie_buffer_findnl(PieBuffer *buffer, size_t hint) {
    size_t base;
    size_t i;
    
    if(hint > 0)
        pull_data_until(buffer, hint);

    base = buffer->offset;
    do {
        for(i = base; i < buffer->data_size; i++) {
            if(buffer->buffer[i] == '\r' || buffer->buffer[i] == '\n') {
                return i - buffer->offset;
            }
        }
    } while(pull_data(buffer) >= 0);
    
    return -1;
}

char pie_buffer_peek(PieBuffer *buffer) {
    pull_data_until(buffer, 1);
    
    if(buffer->data_size == 0)
        return -1;
    
    return *(buffer->buffer + buffer->offset);
}

ssize_t pie_buffer_getstr(PieBuffer *buffer, char *str, size_t len) {
    char *p;
    ssize_t result;
    
    result = pie_buffer_getptr(buffer, &p, len);
    if(result <= 0)
        return result;
    
    memcpy(str, p, len);
    return len;
}
