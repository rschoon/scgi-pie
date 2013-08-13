
#include <signal.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <Python.h>

#include "buffer.h"

static int input_TypeCheck(PyObject *self);
static int request_TypeCheck(PyObject *self);

static PyObject *request_accept_loop(PyObject *self, PyObject *args);
static PyObject *request_halt_loop(PyObject *self, PyObject *args);
static int req_buffer_do_read(PieBuffer *buffer, void *udata);
static int resp_buffer_do_write(PieBuffer *buffer, const char *buf, size_t count, void *udata);

/*
 * Utility
 */

static PyObject* to_pybytes_latin1(PyObject *o, const char *name) {
    if(PyBytes_Check(o)) {
        Py_INCREF(o);
        return o;
    }

    if(PyUnicode_Check(o)) {
        o = PyUnicode_AsLatin1String(o);
        if(!o) {
            PyErr_Format(PyExc_TypeError, "expected %s to be a byte string, "
                        "but got non-latin1 unicode", name);
            return NULL;
        }
        return o;
    } else {
        PyErr_Format(PyExc_TypeError, "expected %s to be a byte string, but got %s",
            name, o->ob_type->tp_name);
        Py_DECREF(o);
        return NULL;
    }
}

/*
 * Error Object
 */

typedef struct {
    PyObject_HEAD
} ErrorObject;

static PyObject *error_write(PyObject *self, PyObject *args) {
    const char *buf;

    if(!PyArg_ParseTuple(args, "s", &buf))
        return NULL;

    Py_BEGIN_ALLOW_THREADS
    fprintf(stderr, "%s", buf);
    Py_END_ALLOW_THREADS
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *error_writelines(PyObject *self, PyObject *args) {
    PyObject *iter;
    PyObject *arg1;
    PyObject *line;
    PyObject *lineB;

    if(!PyArg_ParseTuple(args, "O", &arg1))
        return NULL;

    iter = PyObject_GetIter(arg1);
    if(iter == NULL)
        return NULL;
   
    while(!!(line = PyIter_Next(iter))) {
        lineB = to_pybytes_latin1(line, "line");
        if(lineB != NULL) {
            Py_BEGIN_ALLOW_THREADS
            fwrite(PyBytes_AS_STRING(lineB), 1, PyBytes_GET_SIZE(lineB), stderr);
            Py_END_ALLOW_THREADS

            Py_DECREF(lineB);
        } else {
            Py_XDECREF(line);
            Py_XDECREF(iter);
            return NULL;
        }

        Py_DECREF(line);
    }

    Py_DECREF(iter);
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *error_flush(PyObject *self, PyObject *args) {
    Py_BEGIN_ALLOW_THREADS
    fflush(stderr);
    Py_END_ALLOW_THREADS
    
    Py_INCREF(Py_None);
    return Py_None;
}


static PyMethodDef ErrorMethods[] = {
    {"write", (PyCFunction)error_write, METH_VARARGS, "write"},
    {"writelines", (PyCFunction)error_writelines, METH_VARARGS, "writelines"},
    {"flush", (PyCFunction)error_flush, METH_VARARGS, "flush"},
    {NULL, NULL, 0, NULL},
};


static PyTypeObject ErrorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "scgi_pie.Error",          /*tp_name*/
    sizeof(ErrorObject),       /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    0,                         /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "wsgi.error",              /*tp_doc */
    0,                         /*tp_traverse */
    0,                         /*tp_clear */
    0,                         /*tp_richcompare */
    0,                         /*tp_weaklistoffset */
    0,                         /*tp_iter */
    0,                         /*tp_iternext */
    ErrorMethods,              /*tp_methods */
    0,                         /*tp_members*/
    0,                         /*tp_getset*/
    0,                         /*tp_base*/
    0,                         /*tp_dict*/
    0,                         /*tp_descr_get*/
    0,                         /*tp_descr_set*/
    0,                         /*tp_dictoffset*/
    0,                         /*tp_init*/
    0,                         /*tp_alloc*/
    PyType_GenericNew,         /*tp_new*/
    0,                         /*tp_free*/
    0,                         /*tp_is_gc*/
};

#if 0
static int error_TypeCheck(PyObject *self) {
    return PyObject_TypeCheck(self, &ErrorType);
}
#endif

/*
 * Input Object
 */

typedef struct {
    PyObject_HEAD

    PieBuffer *buffer;
    int size;       /* remaining to send to python */
} InputObject;

static PyObject *input_close(PyObject *self, PyObject *args) {
    if(input_TypeCheck(self))
        ((InputObject*)self)->buffer = NULL;

    Py_INCREF(Py_None);
    return Py_None;
}

static PieBuffer *input_get_buffer(PyObject *self) {
    PieBuffer *result;

    if(!input_TypeCheck(self)) {
        PyErr_SetString(PyExc_TypeError, "expected input object");
        return NULL;
    }

    result = ((InputObject*)self)->buffer;
    if(result == NULL)
        PyErr_SetString(PyExc_RuntimeError, "input object is closed (no buffer)");

    return result;
}

static PyObject *input_read(PyObject *self, PyObject *args) {
    int size = -1;
    size_t justread;
    PieBuffer *buf;
    char *p;
   
    if(!PyArg_ParseTuple(args, "|i", &size))
        return NULL;
 
    buf = input_get_buffer(self);
    if(buf == NULL)
        return NULL;

    if(size < 0)
        size = ((InputObject*)self)->size;
 
    justread = pie_buffer_getptr(buf, &p, size);
    if(justread <= 0)
        return PyBytes_FromString("");
    else
        return PyBytes_FromStringAndSize(p, justread);
}

static PyObject *input_readline(PyObject *self, PyObject *args) {
    int hint_size = -1;
    ssize_t loc;
    size_t justread;
    char *p;
    PieBuffer *buf;

    if(!PyArg_ParseTuple(args, "|i", &hint_size))
        return NULL;

    buf = input_get_buffer(self);
    if(buf == NULL)
        return NULL;

    loc = pie_buffer_findnl(buf, hint_size > 0 ? hint_size : 0); 
    if(loc < 0) {
        loc = pie_buffer_size(buf);
        if(loc > 0)
            loc = loc - 1;
        else
            return PyBytes_FromString("");
    }

    justread = pie_buffer_getptr(buf, &p, loc + 1);
    if(justread <= 0)
        return PyBytes_FromString("");
    else {
        ((InputObject*)self)->size -= justread;
        return PyBytes_FromStringAndSize(p, justread);
    }
}

static PyObject *input_readlines(PyObject *self, PyObject *args) {
    PyObject *line;
    PyObject *list = NULL;
    PyObject *emptyargs = NULL;
  
    list = PyList_New(0);
    emptyargs = Py_BuildValue("()");
    if(list == NULL || emptyargs == NULL) {
        Py_XDECREF(list);
        Py_XDECREF(emptyargs);
        return NULL;
    }
 
    for(;;) {
        line = input_readline(self, emptyargs);
        if(line == NULL)
            break;
 
        PyList_Append(list, line);
        Py_DECREF(line);
    }
    Py_DECREF(emptyargs);

    if(PyErr_Occurred() != NULL && !PyErr_ExceptionMatches(PyExc_EOFError)) {
        Py_DECREF(list);
        return NULL;
    }
    PyErr_Clear();
    return list;
}

static PyObject *input_iter(InputObject *self) {
    if(self->buffer == NULL) {
        PyErr_SetString(PyExc_EOFError, "object is closed");
        return NULL;
    }

    Py_INCREF(self);
    return (PyObject*)self;
}

static PyObject *input_iternext(InputObject *self) {
    PyObject *arglist;
    PyObject *result;
 
    if(self->buffer == NULL) {
        PyErr_SetString(PyExc_EOFError, "object is closed");
        return NULL;
    }

    /* call readline and return result */
    arglist = Py_BuildValue("()");
    result = input_readline((PyObject*)self, arglist);
    Py_DECREF(arglist);

    if(result == NULL) 
        return NULL;
    else if(!PyBytes_Check(result)) {
        PyErr_SetString(PyExc_RuntimeError, "strangely, didn't get bytes");
        Py_DECREF(result);
    } else if (PyBytes_Size(result) == 0) {
        PyErr_SetObject(PyExc_StopIteration, Py_None);
        return NULL;
    }
    return result;
}

static PyObject *input_getclosed(PyObject *self, void *closure) {
    if(input_TypeCheck(self) && ((InputObject*)self)->buffer != NULL) {
        Py_INCREF(Py_False);
        return Py_False;
    } else {
        Py_INCREF(Py_True);
        return Py_True;
    }
}

static PyMethodDef InputMethods[] = {
    {"close", (PyCFunction)input_close, METH_VARARGS, "close"},
    {"read", (PyCFunction)input_read, METH_VARARGS, "read"},
    {"readline", (PyCFunction)input_readline, METH_VARARGS, "readline"},
    {"readlines", (PyCFunction)input_readlines, METH_VARARGS, "readlines"},
    {NULL, NULL, 0, NULL},
};

static PyGetSetDef InputGetSet[] = {
    {"closed", (getter)input_getclosed, NULL, "is closed", NULL},
    {NULL}
};

static PyTypeObject InputType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "scgi_pie.Input",          /*tp_name*/
    sizeof(InputObject),       /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    0,                         /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "wsgi.input",              /*tp_doc */
    0,                         /*tp_traverse */
    0,                         /*tp_clear */
    0,                         /*tp_richcompare */
    0,                         /*tp_weaklistoffset */
    (getiterfunc)input_iter,   /*tp_iter */
    (iternextfunc)input_iternext, /*tp_iternext */
    InputMethods,              /*tp_methods */
    0,                         /*tp_members*/
    InputGetSet,               /*tp_getset*/
    0,                         /*tp_base*/
    0,                         /*tp_dict*/
    0,                         /*tp_descr_get*/
    0,                         /*tp_descr_set*/
    0,                         /*tp_dictoffset*/
    0,                         /*tp_init*/
    0,                         /*tp_alloc*/
    PyType_GenericNew,         /*tp_new*/
    0,                         /*tp_free*/
    0,                         /*tp_is_gc*/
};

static int input_TypeCheck(PyObject *self) {
    return PyObject_TypeCheck(self, &InputType);
}

/*
 * Request Object
 */

typedef struct {
    PyObject_HEAD

    int fd;

    struct loop_state {
        int quitting;
        int in_accept;
        long thread_id;

        PyObject *application;
        PyObject *stderr;
        int allow_buffering;
        int listen_fd;
    } loop_state;

    struct {
        PieBuffer buffer;

        PyObject *environ;
        InputObject *input;
        int input_size;       /* remaining from scgi */
    } req;

    struct {
        PieBuffer buffer;

        int headers_sent;
        PyObject *status;
        PyObject *headers;
    } resp;
} RequestObject;

static PyObject *request_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    RequestObject *req;

    req = (RequestObject *)type->tp_alloc(type, 0);
    if(req != NULL) {
        req->fd = -1;

        req->loop_state.quitting = 0;
        req->loop_state.in_accept = 0;
        req->loop_state.thread_id = 0;

        req->loop_state.application = NULL;
        req->loop_state.allow_buffering = 0;
        req->loop_state.listen_fd = -1;
        req->loop_state.stderr = PySys_GetObject("stderr");
        Py_INCREF(req->loop_state.stderr);

        req->req.input = NULL;
        req->resp.headers_sent = 0;
        req->resp.status = NULL;
        req->resp.headers = NULL;

        pie_buffer_init(&req->req.buffer);
        pie_buffer_set_reader(&req->req.buffer, req_buffer_do_read, req);

        pie_buffer_init(&req->resp.buffer);
        pie_buffer_set_writer(&req->resp.buffer, resp_buffer_do_write, req);
    }

    return (PyObject *)req;
}

static int request_init(PyObject *self, PyObject *args, PyObject *kwds) {
    RequestObject *req = (RequestObject *)self;
    static char *kwlist[] = { "application", "listen_socket", "allow_buffering", NULL };

    if(!PyArg_ParseTupleAndKeywords(args, kwds, "Oip", kwlist,
                                    &req->loop_state.application,
                                    &req->loop_state.listen_fd,
                                    &req->loop_state.allow_buffering))
        return -1; 

    return 0;
}

static void request_dealloc(PyObject* self) {
    RequestObject *req = (RequestObject *)self;

    Py_CLEAR(req->loop_state.application);
    Py_CLEAR(req->loop_state.stderr);
    Py_CLEAR(req->req.input);
    Py_CLEAR(req->resp.status);
    Py_CLEAR(req->resp.headers);

    pie_buffer_free_data(&req->req.buffer);
    pie_buffer_free_data(&req->resp.buffer);
}

static PyObject *request_start_response(PyObject *self, PyObject *args, PyObject *keywds) {
    PyObject *status;
    RequestObject *req;
    PyObject *headers = NULL, *exc_info = NULL;
    static char *kwlist[] = {"status", "headers", "exc_info", NULL};
    int has_exc_info;
  
    req = (RequestObject *)self;
    if(!PyArg_ParseTupleAndKeywords(args, keywds, "OO|O", kwlist,
                                    &status, &headers, &exc_info))
        return NULL; 
    
    if(!request_TypeCheck(self)) {
        PyErr_SetString(PyExc_TypeError, "expected request object");
        return NULL;
    }

    status = to_pybytes_latin1(status, "status");
    if(status == NULL)
        return NULL;
 
    if(!PyList_Check(headers)) {
        PyErr_SetString(PyExc_TypeError, "headers needs to be a list");

        Py_DECREF(status);
        return NULL;
    }

    has_exc_info = (exc_info != NULL && exc_info != Py_None);
    if(req->resp.headers_sent) {
        Py_DECREF(status);
        if(has_exc_info) {
            PyErr_SetString(PyExc_RuntimeError, "headers already sent");
            return NULL;
        } else {
            PyObject *exc_info_unpacked[] = {NULL, NULL, NULL};
            if (!PyArg_ParseTuple(exc_info, "OOO", exc_info_unpacked,
                                                   exc_info_unpacked+1,
                                                   exc_info_unpacked+2))
                return NULL;

            PyErr_Restore(exc_info_unpacked[0], exc_info_unpacked[1], exc_info_unpacked[2]);
            return NULL;
        }
    } 
    
    if(req->resp.headers != NULL && !has_exc_info) {
        PyErr_SetString(PyExc_RuntimeError, "headers already set");
   
        Py_DECREF(status);
        return NULL;
    }

    /* save header/status
     *
     * Note: our use of to_pybytes_latin1() already increased the reference
     * count of status, so just steal that reference. 
     */

    Py_XDECREF(req->resp.headers);
    Py_XDECREF(req->resp.status);
    req->resp.headers = headers;
    req->resp.status = status;
    Py_INCREF(headers);

    return PyObject_GetAttrString(self, "write");
}

static int request_send_headers(RequestObject *req) {
    int i;
    PyObject *item;
    PyObject *name, *value;
    PyObject *headers;
    PyObject *status;

    if(req->resp.headers_sent) {
        return 0;
    }

    headers = req->resp.headers;
    status = req->resp.status;
    if(headers == NULL || status == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "start_response never called with headers");
        return -1;
    }

    /* send status */
    pie_buffer_append(&req->resp.buffer, "Status: ", 8);
    pie_buffer_append(&req->resp.buffer, PyBytes_AS_STRING(status), PyBytes_GET_SIZE(status));
    pie_buffer_append(&req->resp.buffer, "\r\n", 2);

    /* send rest headers */
    for(i = 0; i < PyList_Size(headers); i++) {
        item = PyList_GetItem(headers, i);
    
        if(!PyTuple_Check(item)) {
            PyErr_SetString(PyExc_TypeError, "a non-tuple found in headers");
            return -1;
        }

        if(!PyArg_ParseTuple(item, "OO", &name, &value))
            return -1;

        name = to_pybytes_latin1(name, "header name");
        if(name == NULL)
            return -1;
     
        value = to_pybytes_latin1(value, "header value");
        if(value == NULL) {
            Py_DECREF(name);
            return -1;
        }
    
        pie_buffer_append(&req->resp.buffer, PyBytes_AS_STRING(name), PyBytes_GET_SIZE(name));
        pie_buffer_append(&req->resp.buffer, ": ", 2);
        pie_buffer_append(&req->resp.buffer, PyBytes_AS_STRING(value), PyBytes_GET_SIZE(value));
        pie_buffer_append(&req->resp.buffer, "\r\n", 2);

        Py_DECREF(name);
        Py_DECREF(value);
    }
    pie_buffer_append(&req->resp.buffer, "\r\n", 2);

    Py_BEGIN_ALLOW_THREADS
    pie_buffer_flush(&req->resp.buffer);
    Py_END_ALLOW_THREADS

    req->resp.headers_sent = 1;
 
    return 0;
}

static PyObject *request_write(PyObject *self, PyObject *args) {
    PyObject *bytes;
    RequestObject *req;

    if(!request_TypeCheck(self)) {
        PyErr_SetString(PyExc_TypeError, "expected request object");
        return NULL;
    }
    req = (RequestObject*)self;
    
    if(request_send_headers(req) < 0)
        return NULL;

    if(!PyArg_ParseTuple(args, "O", &bytes))
        return NULL;

    bytes = to_pybytes_latin1(bytes, "data");
    if(bytes == NULL)
        return NULL;
    
    pie_buffer_append(&req->resp.buffer, PyBytes_AS_STRING(bytes), PyBytes_GET_SIZE(bytes));
    if(!req->loop_state.allow_buffering) {
        Py_BEGIN_ALLOW_THREADS
        pie_buffer_flush(&req->resp.buffer);
        Py_END_ALLOW_THREADS
    }

    Py_DECREF(bytes);          /* to_pybytes_latin1 inc ref'd it */
    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef RequestMethods[] = {
    {"accept_loop", (PyCFunction)request_accept_loop, METH_VARARGS, ""},
    {"halt_loop", (PyCFunction)request_halt_loop, METH_VARARGS, ""},
    {"start_response", (PyCFunction)request_start_response, METH_VARARGS | METH_KEYWORDS, ""},
    {"write", (PyCFunction)request_write, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject RequestType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_scgi_pie.Request",        /*tp_name*/
    sizeof(RequestObject),     /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)request_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    0,                         /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,        /*tp_flags*/
    "Request Object",          /*tp_doc */
    0,                         /*tp_traverse */
    0,                         /*tp_clear */
    0,                         /*tp_richcompare */
    0,                         /*tp_weaklistoffset */
    0,                         /*tp_iter */
    0,                         /*tp_iternext */
    RequestMethods,            /*tp_methods */
    0,                         /*tp_members*/
    0,                         /*tp_getset*/
    0,                         /*tp_base*/
    0,                         /*tp_dict*/
    0,                         /*tp_descr_get*/
    0,                         /*tp_descr_set*/
    0,                         /*tp_dictoffset*/
    request_init,              /*tp_init*/
    0,                         /*tp_alloc*/
    request_new,               /*tp_new*/
    0,                         /*tp_free*/
    0,                         /*tp_is_gc*/
};

static int request_TypeCheck(PyObject *self) {
    return PyObject_TypeCheck(self, &RequestType);
}

static void request_print_info(RequestObject *req, FILE *out) {
    if(req->req.environ != NULL) {
        PyObject *value;

        fprintf(out, "\n");

        value = PyDict_GetItemString(req->req.environ, "SCRIPT_NAME");
        if(value != NULL && PyUnicode_Check(value))
            fprintf(out, "SN=%s ", PyUnicode_AsUTF8(value));

        value = PyDict_GetItemString(req->req.environ, "PATH_INFO");
        if(value != NULL && PyUnicode_Check(value))
            fprintf(out, "PI=%s ", PyUnicode_AsUTF8(value));

         fprintf(out, "\n");
    }
}

static void send_error(RequestObject *req, const char *error) {
    static const char err_headers[] = "Status: 500 Internal Server Error\r\n"
                                   "Content-Type: text/plain\r\n\r\n";
    static const char err_body[] = "An internal server error has occured.\r\n\r\n";

    if(!req->resp.headers_sent) {
        pie_buffer_append(&req->resp.buffer, err_headers, sizeof(err_headers)-1);
        req->resp.headers_sent = 1;
    }

    pie_buffer_append(&req->resp.buffer, err_body, sizeof(err_body)-1);

    if(error != NULL) {
        pie_buffer_append(&req->resp.buffer, error, strlen(error));
        pie_buffer_append(&req->resp.buffer, "\r\n", 2);
    }

    pie_buffer_flush(&req->resp.buffer);
}

static int load_headers(RequestObject *req, char ** headers) {
    PieBuffer *buffer = &req->req.buffer;
    int header_size = 0;
    int c = '\0';

    while(c != ':') {
        c = pie_buffer_getchar(buffer);
        if(c < 0) {
            send_error(req, "Problems getting SCGI header size");
            return -1;
        }
    
        if(c >= '0' && c <= '9')
            header_size = header_size * 10 + (c - '0');
    }

    if(pie_buffer_getptr(buffer, headers, header_size) <= 0) {
        send_error(req, "Problems getting SCGI headers");
        return -1;
    }

    return header_size;
}

static PyObject *setup_environ(RequestObject *req, char * headers, int header_size) {
    int https = 0;
    char *itr, *end;
    PyObject *environ;
    PyObject *value_o;
    int content_length = -1;

    environ = Py_BuildValue("{sNsisisisOsOssssssssss}",
                            "wsgi.version", Py_BuildValue("(ii)", 1, 0),
                            "wsgi.multithread", 1,
                            "wsgi.multiprocess", 1,  /* who knows ? */
                            "wsgi.run_once", 0,
                            "wsgi.errors", req->loop_state.stderr,
                            "wsgi.input", req->req.input,
                            "SCRIPT_NAME", "",
                            "REQUEST_METHOD", "GET",
                            "PATH_INFO", "",
                            "QUERY_STRING", "",
                            "SERVER_PROTOCOL", "HTTP/1.1");

    end = headers + header_size;   /* redundant but safe */

    for(itr = headers; *itr != '\0' && itr < end; ) {
        char *name, *value;
        int namelen, valuelen;

        /* name */
        namelen = strnlen(itr, end - itr);
        name = itr;
        itr += namelen + 1;

        if(itr >= end)
            break;

        /* value */
        valuelen = strnlen(itr, end - itr);
        value = itr;

        if(!strncmp(name, "HTTPS", namelen)) {
            if(strncmp(value, "0", valuelen) && strncmp(value, "off", valuelen)) {
                https = 1;
            }
        }

        value_o = PyUnicode_DecodeLatin1(value, valuelen, "replace");    /* XXX latin1? */
        if(!strncmp(name, "HTTP_CONTENT_TYPE", namelen)) {
            PyDict_SetItemString(environ, "CONTENT_TYPE", value_o);
        } else if(!strncmp(name, "HTTP_CONTENT_LENGTH", namelen)) {
            PyDict_SetItemString(environ, "CONTENT_LENGTH", value_o);
            content_length = strtol(value, NULL, 10);
        } else if(!strncmp(name, "CONTENT_LENGTH", namelen)) {
            PyDict_SetItemString(environ, "CONTENT_LENGTH", value_o);
            content_length = strtol(value, NULL, 10);
        } else if(!strncmp(name, "HTTP_HOST", namelen)) {
            PyDict_SetItemString(environ, "SERVER_NAME", value_o);
            PyDict_SetItemString(environ, name, value_o);
        } else {
            PyDict_SetItemString(environ, name, value_o);
        }

        Py_DECREF(value_o);
        itr += valuelen + 1;
    }

    value_o = PyUnicode_FromString(https ? "https" : "http");
    PyDict_SetItemString(environ, "wsgi.url_scheme", value_o);
    Py_DECREF(value_o);

    req->req.input->size = content_length;
    req->req.input_size = content_length;

    req->req.environ = environ;
    return environ;
}


static void send_result(RequestObject *req, PyObject *result) {
    PyObject *iter;
    PyObject *item;
    int checked_send_headers = 0;

    if(result == NULL) {
        request_print_info(req, stderr);
        fprintf(stderr, "Somehow got NULL from application.\n");
        return;
    }             

    iter = PyObject_GetIter(result);
    if(iter == NULL) {
        request_print_info(req, stderr);
        fprintf(stderr, "Got a non-iterable from application:\n");
        PyErr_Print();
        return;
    }
   
    /* Send body */
    while(!!(item = PyIter_Next(iter))) {
        PyObject *converted;
        const char *bytes;
        int byteslen;
 
        converted = to_pybytes_latin1(item, "data");
        Py_DECREF(item);

        if(converted == NULL) {
            if(PyErr_Occurred() != NULL) {
                request_print_info(req, stderr);
                PyErr_Print();
                PyErr_Clear();
            }
            break;
        }

        byteslen = PyBytes_GET_SIZE(converted);
        bytes = PyBytes_AS_STRING(converted);

        if(byteslen > 0) {
            if(!checked_send_headers) {
                request_send_headers(req);
                checked_send_headers = 1;
            }

            pie_buffer_append(&req->resp.buffer, bytes, byteslen);
            if(!req->loop_state.allow_buffering) {
                Py_BEGIN_ALLOW_THREADS
                pie_buffer_flush(&req->resp.buffer);
                Py_END_ALLOW_THREADS
            }
        }

        Py_DECREF(converted);
    }

    if(PyErr_Occurred() != NULL) {
        request_print_info(req, stderr);
        fprintf(stderr, "Iterator returned an error:\n");
        PyErr_Print();
    }

    Py_DECREF(iter);

    if(!checked_send_headers)
        request_send_headers(req);

    if(pie_buffer_size(&req->resp.buffer) > 0) {
        Py_BEGIN_ALLOW_THREADS
        pie_buffer_flush(&req->resp.buffer);
        Py_END_ALLOW_THREADS
    }
}

static void close_result(RequestObject *req, PyObject *result) {
    PyObject *method;
    PyObject *args;
    PyObject *rv;

    if(PyObject_HasAttrString(result, "close")) {
        method = PyObject_GetAttrString(result, "close");
        args = Py_BuildValue("()");
        rv = PyEval_CallObject(method, args);

        if(rv == NULL) {
            request_print_info(req, stderr);
            PyErr_Print();
            PyErr_Clear();
        }
            
        Py_DECREF(args);
        Py_DECREF(method);
        Py_XDECREF(rv);
    }
}

static void handle_request(RequestObject *req, PyThreadState *py_thr) {
    PyObject *start_response;
    PyObject *arglist;
    PyObject *result;
    PyObject *environ;
    char *headers;
    int header_size;

    /* setup */

    header_size = load_headers(req, &headers);
    
    PyEval_AcquireThread(py_thr);

    req->req.input = (InputObject *)PyObject_New(InputObject, &InputType);
    req->req.input->buffer = &req->req.buffer;
    req->req.input->size = 0;

    req->resp.headers_sent = 0;

    environ = setup_environ(req, headers, header_size);
    
    if(headers[header_size-1] != ',')
        pie_buffer_getchar(&req->req.buffer);
    req->req.input_size -= pie_buffer_size(&req->req.buffer);

    /* perform call */

    start_response = PyObject_GetAttrString((PyObject*)req, "start_response");
    arglist = Py_BuildValue("(OO)", environ, start_response);
    result = PyObject_CallObject(req->loop_state.application, arglist);
    if(PyErr_Occurred() != NULL) {
        request_print_info(req, stderr);
        PyErr_Print();
        send_error(req, "uncaught exception");
    } else {
        send_result(req, result);
        close_result(req, result);
    }

    /* clean up */
    Py_XDECREF(start_response);
    Py_XDECREF(result);
    Py_XDECREF(arglist);

    Py_CLEAR(req->req.environ);
    Py_CLEAR(req->resp.status);
    Py_CLEAR(req->resp.headers);

    req->req.input->buffer = NULL;
    Py_CLEAR(req->req.input);

    req->resp.headers_sent = 1;
    req->fd = -1;

    PyEval_ReleaseThread(py_thr);
}

static int req_buffer_do_read(PieBuffer *buffer, void *udata) {
    RequestObject *request = (RequestObject *)udata;
    char tmp[2048];
    ssize_t justread;
    
    if(request->req.input != NULL && request->req.input_size <= 0)
        return -1;

    justread = recv(request->fd, tmp, sizeof(tmp), 0);
    if(justread < 0) {
        if(errno != EINTR)
            return -1;
    } else if(justread == 0) {
        return -1;
    } else
        pie_buffer_append(buffer, tmp, justread);

    request->req.input_size -= justread;

    if(request->fd < 0)
        return -1;

    return 0;
}

static int resp_buffer_do_write(PieBuffer *buffer, const char *buf, size_t count, void *udata) {
    RequestObject *req = (RequestObject *)udata;
    ssize_t wrote;
    ssize_t left = count;

    while(left > 0) {
        wrote = write(req->fd, buf, left);
        if(wrote <= 0) {
            if(errno == EINTR)
                wrote = 0;
            else
                return wrote;
        }

        left -= wrote;
        buf += wrote;
    }
    return count;

    return 0;
}

/*
 * Loader
 */

static PyObject *new_blank_module(const char *name) {
    PyObject *m, *d;
    m = PyImport_AddModule(name);
    if (m == NULL)
        Py_FatalError("can't create blank module module");
    d = PyModule_GetDict(m);
    if(PyDict_GetItemString(d, "__builtins__") == NULL) {
        PyObject *bimod = PyImport_ImportModule("builtins");
        if (bimod == NULL || PyDict_SetItemString(d, "__builtins__", bimod) != 0)
            Py_FatalError("can't add __builtins__ to app module");
            Py_XDECREF(bimod);
    }
    return m;
}
static PyObject *load_app(const char *path) {
    FILE *f;
    PyObject *m, *d, *a;
    PyObject *v;

    if(path == NULL) {
        PyErr_SetString(PyExc_ImportError, "application path not set");
        return NULL;
    }

    f = fopen(path, "r");
    if(path == NULL) {
        return PyErr_SetFromErrno(PyExc_OSError);
    }

    m = new_blank_module("__wsgi_main__");
    d = PyModule_GetDict(m);
   
    v = PyUnicode_DecodeLatin1(path, strlen(path), "replace");
    PyDict_SetItemString(d, "__file__", v);
    Py_DECREF(v);
 
    v = PyRun_FileExFlags(f, path, Py_file_input, d, d, 0, NULL);
    Py_XDECREF(v);              /* Decref result from code load, probably NoneType */
  
    fclose(f);

    if(PyErr_Occurred() != NULL)
        return NULL;

    /* Load application object */
    a = PyDict_GetItemString(d, "application");
    if(a == NULL || !PyCallable_Check(a)) {
        PyErr_SetString(PyExc_ImportError, "application missing or not a callable");
        return NULL;
    }
    Py_INCREF(a);

    return a;
}

static PyObject* m_load_app(PyObject *self, PyObject *args) {
    const char *path;

    if(!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    return load_app(path);
}

/*
 * Main Loop
 */

static PyObject *request_accept_loop(PyObject *self, PyObject *args) {
    RequestObject *request;
    PyThreadState *py_thr;

    if(!request_TypeCheck(self)) {
        PyErr_SetString(PyExc_TypeError, "expected request object");
        return NULL;
    }
    request = (RequestObject *)self;

    request->loop_state.in_accept = 1;
    request->loop_state.thread_id = PyThreadState_Get()->thread_id;

    py_thr = PyEval_SaveThread();

    while(!request->loop_state.quitting) {
        int fd = accept(request->loop_state.listen_fd, NULL, NULL);
        if(fd >= 0) {
            request->fd = fd;
            handle_request(request, py_thr);
            request->fd = -1;
            close(fd);
        } else if(errno != EMFILE && errno != ENFILE && errno != EINTR) {
            PyEval_RestoreThread(py_thr);
            return PyErr_SetFromErrno(PyExc_OSError);
        }

        pie_buffer_restart(&request->req.buffer);
        pie_buffer_restart(&request->resp.buffer);
    }

    PyEval_RestoreThread(py_thr);

    request->loop_state.in_accept = 0;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *request_halt_loop(PyObject *self, PyObject *args) {
    RequestObject *req = (RequestObject *)self;

    if(!request_TypeCheck(self)) {
        PyErr_SetString(PyExc_TypeError, "expected request object");
        return NULL;
    }
    req->loop_state.quitting = 1;
    if(!req->loop_state.in_accept) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    req->loop_state.listen_fd = -1;
    
    if(pthread_kill(req->loop_state.thread_id, SIGINT) < 0)
        perror("pthread_kill");
    
    Py_INCREF(Py_None);
    return Py_None;
}

/*
 * Module
 */ 

static PyObject *m_fallback_app(PyObject *self, PyObject *args) {
    PyObject *environ, *start_response;
    PyObject *sr_args;
    PyObject *result;

    if(!PyArg_ParseTuple(args, "OO", &environ, &start_response)) {
        return NULL;
    }

    if(!PyCallable_Check(start_response)) {
        fprintf(stderr, "(fallback) start_response wasn't callable?");
        return NULL;
    }
 
    sr_args = Py_BuildValue("s[(ss)]", "500 Internal Server Error", 
                                       "Content-Type", "text/plain");

    result = PyEval_CallObject(start_response, sr_args);
    Py_DECREF(sr_args);
    Py_XDECREF(result);

    return Py_BuildValue("[s]", "Internal Server Error.\n"
                                "No application callable was able to be loaded.\n");
}

static PyMethodDef ModuleMethods[] = {
    {"load_app", (PyCFunction)m_load_app, METH_VARARGS, ""},
    {"fallback_app", (PyCFunction)m_fallback_app, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef ModuleDef = {
    {}, /* m_base */
    "scgi_pie",  /* m_name */
    0,  /* m_doc */
    0,  /* m_size */
    ModuleMethods,  /* m_methods */
    0,  /* m_reload */
    0,  /* m_traverse */
    0,  /* m_clear */
    0,  /* m_free */
};


PyMODINIT_FUNC
PyInit__scgi_pie(void)
{
    PyObject *m;

    if(PyType_Ready(&ErrorType) < 0)
        return NULL;

    if(PyType_Ready(&RequestType) < 0)
        return NULL;

    if(PyType_Ready(&InputType) < 0)
        return NULL;

    m = PyModule_Create(&ModuleDef);
    if (m == NULL)
        return NULL;

    PyModule_AddObject(m, "Request", (PyObject *)&RequestType);

    return m;
}