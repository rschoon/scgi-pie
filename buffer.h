/*
 * Copyright (c) 2012-2013 Robin Schoonover
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

#ifndef PIE_BUFFER_H
#define PIE_BUFFER_H

#include <sys/types.h>

typedef struct PieBuffer PieBuffer;

typedef int (buffer_pull_data)(PieBuffer*, void *);
typedef int (buffer_push_data)(PieBuffer*, const char *, size_t, void *);

struct PieBuffer {   
    char *buffer;
    size_t offset;
    size_t data_size;
    size_t buffer_size;
    size_t max_size;
    
    buffer_pull_data *reader;
    void *reader_udata;
    
    buffer_push_data *writer;
    void *writer_udata;
};

int pie_buffer_init(PieBuffer *buffer);
void pie_buffer_free_data(PieBuffer *buffer);
void pie_buffer_restart(PieBuffer *buffer);
void pie_buffer_set_reader(PieBuffer *buffer, buffer_pull_data *func, void *udata);
void pie_buffer_set_maxsize(PieBuffer *buffer, size_t sz);
void pie_buffer_set_writer(PieBuffer *buffer, buffer_push_data *func, void *udata);
ssize_t pie_buffer_recv(PieBuffer *buffer, int fd, size_t len);
int pie_buffer_append(PieBuffer *buffer, const char *data, size_t len);
int pie_buffer_flush(PieBuffer *buffer);
char pie_buffer_peek(PieBuffer *buffer);
ssize_t pie_buffer_findnl(PieBuffer *buffer, size_t hint);
size_t pie_buffer_size(PieBuffer *buffer);
int pie_buffer_getchar(PieBuffer *buffer);
ssize_t pie_buffer_getstr(PieBuffer *buffer, char *str, size_t len);
ssize_t pie_buffer_getptr(PieBuffer *buffer, char **p, size_t len);


#endif
