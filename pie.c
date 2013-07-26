
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <Python.h>

#include "scgi-pie.h"
#include "buffer.h"

#define MAX_HEADER_SIZE     16384

static PyThreadState *main_thr = NULL;
static PyObject *application = NULL;
static PyObject *pie_module = NULL;
static PyObject *wsgi_stderr = NULL;

static int input_TypeCheck(PyObject *self);
static int request_TypeCheck(PyObject *self);

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

    fprintf(stderr, "%s", buf);
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef ErrorMethods[] = {
    {"write", (PyCFunction)error_write, METH_VARARGS, "write"},
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
    int size;
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
    else
        return PyBytes_FromStringAndSize(p, justread);
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

    /* thread info */
    PyThreadState *py_thr;
    PyInterpreterState *py_main_is;

    int fd;

    struct {
        PieBuffer buffer;

        char *headers;
        int headers_space;

        PyObject *environ;
        InputObject *input;
    } req;

    struct {
        int headers_sent;
        PyObject *status;
        PyObject *headers;
    } resp;
} RequestObject;

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

static ssize_t request_write_raw(RequestObject *req, const char *buf, size_t count) {
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
    request_write_raw(req, "Status: ", 8);
    request_write_raw(req, PyBytes_AS_STRING(status), PyBytes_GET_SIZE(status));
    request_write_raw(req, "\r\n", 2);

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
    
        Py_BEGIN_ALLOW_THREADS
        request_write_raw(req, PyBytes_AS_STRING(name), PyBytes_GET_SIZE(name));
        request_write_raw(req, ": ", 2);
        request_write_raw(req, PyBytes_AS_STRING(value), PyBytes_GET_SIZE(value));
        request_write_raw(req, "\r\n", 2);
        Py_END_ALLOW_THREADS

        Py_DECREF(name);
        Py_DECREF(value);
    }
    request_write_raw(req, "\r\n", 2);
    
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
   
    Py_BEGIN_ALLOW_THREADS
    request_write_raw(req, PyBytes_AS_STRING(bytes), PyBytes_GET_SIZE(bytes));
    Py_END_ALLOW_THREADS

    Py_DECREF(bytes);          /* to_pybytes_latin1 inc ref'd it */
    Py_INCREF(Py_None);
    return Py_None;
}

static PyMethodDef RequestMethods[] = {
    {"start_response", (PyCFunction)request_start_response, METH_VARARGS | METH_KEYWORDS, ""},
    {"write", (PyCFunction)request_write, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL},
};

static PyTypeObject RequestType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "scgi_pie.Request",        /*tp_name*/
    sizeof(RequestObject),     /*tp_basicsize*/
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
    0,                         /*tp_init*/
    0,                         /*tp_alloc*/
    PyType_GenericNew,         /*tp_new*/
    0,                         /*tp_free*/
    0,                         /*tp_is_gc*/
};

static int request_TypeCheck(PyObject *self) {
    return PyObject_TypeCheck(self, &RequestType);
}

static void realloc_headers(RequestObject *req, int size) {
    if(size < 1024)
        size = 1024;

    req->req.headers = realloc(req->req.headers, size);
    req->req.headers_space = size;
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
        request_write_raw(req, err_headers, sizeof(err_headers)-1);
        req->resp.headers_sent = 1;
    }

    request_write_raw(req, err_body, sizeof(err_body)-1);

    if(error != NULL) {
        request_write_raw(req, error, strlen(error));
        request_write_raw(req, "\r\n", 2);
    }
}

static int load_headers(RequestObject *req) {
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

    if(header_size > MAX_HEADER_SIZE) {
        /* XXX there's a correct error code for this... */
        send_error(req, "header size unexpected size");
        return -1;
    } else if(header_size + 1 > req->req.headers_space)
        realloc_headers(req, header_size + 1);

    if(pie_buffer_getstr(buffer, req->req.headers, header_size) <= 0) {
        send_error(req, "Problems getting SCGI headers");
        return -1;
    }

    if(req->req.headers[header_size-1] == ',') {
        /* this is a bug */
        req->req.headers[header_size-1] = '\0';
    } else {
        pie_buffer_getchar(buffer);
        req->req.headers[header_size] = '\0';
    }

    return 0;
}

static PyObject *setup_environ(RequestObject *req) {
    int https = 0;
    char *itr;
    PyObject *environ;
    PyObject *value_o;

    environ = Py_BuildValue("{sNsisisisOsOssssssssss}",
                            "wsgi.version", Py_BuildValue("(ii)", 1, 0),
                            "wsgi.multithread", 1,
                            "wsgi.multiprocess", 1,  /* who knows ? */
                            "wsgi.run_once", 0,
                            "wsgi.errors", wsgi_stderr,
                            "wsgi.input", req->req.input,
                            "SCRIPT_NAME", "",
                            "REQUEST_METHOD", "GET",
                            "PATH_INFO", "",
                            "QUERY_STRING", "",
                            "SERVER_PROTOCOL", "HTTP/1.1");

    for(itr = req->req.headers; *itr != '\0'; ) {
        char *name, *value;
        int namelen, valuelen;

        /* name */
        namelen = strlen(itr);
        name = itr;
        itr += namelen + 1;

        /* value */
        valuelen = strlen(itr);
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
            req->req.input->size = strtol(value, NULL, 10);
        } else if(!strncmp(name, "CONTENT_LENGTH", namelen)) {
            PyDict_SetItemString(environ, "CONTENT_LENGTH", value_o);
            req->req.input->size = strtol(value, NULL, 10);
        } else {
            PyDict_SetItemString(environ, name, value_o);
        }

        Py_DECREF(value_o);
        itr += valuelen + 1;
    }

    value_o = PyUnicode_FromString(https ? "https" : "http");
    PyDict_SetItemString(environ, "wsgi.url_scheme", value_o);
    Py_DECREF(value_o);

    req->req.environ = environ;
    return environ;
}

static void send_result(RequestObject *req, PyObject *result) {
    PyObject *iter;
    PyObject *item, *converted;
    int checked_send_headers = 0;

    if(result == NULL) {
        request_print_info(req, stderr);
        fprintf(stderr, "Somehow got NULL from application.\n");
        return;
    }             

    iter = PyObject_GetIter(result);
    if(iter == NULL) {
        request_print_info(req, stderr);
        fprintf(stderr, "Got a non-iterable from application:");
        PyErr_Print();
        return;
    }
   
    /* Send body */
    while(!!(item = PyIter_Next(iter))) {
        if(!checked_send_headers) {
            request_send_headers(req);
            checked_send_headers = 1;
        }
 
        converted = to_pybytes_latin1(item, "data");
        if(converted != NULL) {
            Py_BEGIN_ALLOW_THREADS
            request_write_raw(req, PyBytes_AS_STRING(converted),
                                   PyBytes_GET_SIZE(converted));
            Py_END_ALLOW_THREADS

            Py_DECREF(converted);
        } else {
            if(PyErr_Occurred() != NULL) {
                request_print_info(req, stderr);
                PyErr_Print();
            }
        }

        Py_DECREF(item);
    }

    Py_DECREF(iter);
 
    if(!checked_send_headers)
        request_send_headers(req);
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


static void handle_request(RequestObject *req) {
    PyObject *start_response;
    PyObject *arglist;
    PyObject *result;
    PyObject *environ;

    /* setup */

    if(load_headers(req) < 0)
        return;
    
    PyEval_AcquireThread(req->py_thr);

    req->req.input = (InputObject *)PyObject_New(InputObject, &InputType);
    req->req.input->buffer = &req->req.buffer;
    req->req.input->size = 0;

    req->resp.headers_sent = 0;

    environ = setup_environ(req);
    
    /* perform call */

    start_response = PyObject_GetAttrString((PyObject*)req, "start_response");
    arglist = Py_BuildValue("(OO)", environ, start_response);
    result = PyObject_CallObject(application, arglist);
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

    PyEval_ReleaseThread(req->py_thr);
}

static int buffer_do_read(PieBuffer *buffer, void *udata) {
    RequestObject *request = (RequestObject *)udata;
    InputObject *input = request->req.input;
    char tmp[2048];
    ssize_t justread;

    if(input != NULL && input->size <= 0)
        return -1;  /* EOF */
    
    justread = recv(request->fd, tmp, sizeof(tmp), 0);
    if(justread < 0) {
        if(errno != EINTR)
            return -1;
    } else
        pie_buffer_append(buffer, tmp, justread);

    if(input != NULL)
        input->size -= justread;

    return 0;
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

static PyObject *init_module(void) {
    PyObject *m;

    if(PyType_Ready(&ErrorType) < 0)
        return NULL;

    if(PyType_Ready(&RequestType) < 0)
        return NULL;

    if(PyType_Ready(&InputType) < 0)
        return NULL;

    m = PyModule_Create(&ModuleDef);

    return m;
}

PyObject* get_fallback_app(int incref) {
    PyObject *app = PyObject_GetAttrString(pie_module, "fallback_app");
    if(incref)
        Py_INCREF(app);
    return app;
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

static void setup_venv(const char *path) {
    PyObject *prefix = NULL, *executable = NULL;
    PyObject *site = NULL, *method = NULL, *args = NULL, *result = NULL;

    if(path == NULL)
        return;

    /*
     * adjust sys so that site.py can do its work
     */
    prefix = PyUnicode_DecodeFSDefaultAndSize(path, strlen(path));
    executable = PyUnicode_FromFormat("%U/bin/python", prefix);

    PySys_SetObject("prefix", prefix);
    PySys_SetObject("executable", executable);

    Py_DECREF(prefix);
    Py_DECREF(executable);

    /*
     * load site, which does other paths
     */
    site = PyImport_ImportModule("site");
    if(site == NULL) {
        PyErr_Print();
        goto finish;
    }

    method = PyObject_GetAttrString(site, "main");
    if(method == NULL) {
        PyErr_Print();
        goto finish;
    }

    args = Py_BuildValue("()");
    if(args == NULL) {
        PyErr_Print();
        goto finish;
    }

    result = PyEval_CallObject(method, args);
    if(result == NULL) {
        PyErr_Print();
        goto finish;
    }

finish:
    Py_XDECREF(method);
    Py_XDECREF(args);
    Py_XDECREF(result);
    Py_XDECREF(site);
}

static PyObject *load_app(const char *path) {
    FILE *f;
    const char *dirname_p;
    PyObject *m, *d, *a;
    PyObject *v;
    PyObject *pth, *pth_to_add;

    if(path == NULL) {
        fprintf(stderr, "Application not set.\n");
        return get_fallback_app(1);
    }

    f = fopen(path, "r");
    if(f == NULL) {
        perror("Open application failed");
        return get_fallback_app(1);
    }

    if(global_state.add_dirname_to_path) {
        pth = PySys_GetObject("path");
        if(pth == NULL || !PyList_Check(pth)) {
            fprintf(stderr, "Unable to manipulate sys.path");
            return get_fallback_app(1);
        }

        dirname_p = strrchr(path, '/');
        if(dirname_p == NULL)
            pth_to_add = PyUnicode_DecodeUTF8(".", 1, "replace");
        else
            /* TODO: We need to figure out FS encoding or something here */
            pth_to_add = PyUnicode_DecodeUTF8(path, dirname_p - path, "replace");

        PyList_Insert(pth, 0, pth_to_add);
        Py_DECREF(pth_to_add);
    }
        
    m = new_blank_module("__wsgi_main__");
    d = PyModule_GetDict(m);
   
    v = PyUnicode_DecodeLatin1(path, strlen(path), "replace");
    PyDict_SetItemString(d, "__file__", v);
    Py_DECREF(v);
 
    v = PyRun_FileExFlags(f, path, Py_file_input, d, d, 0, NULL);

    if(PyErr_Occurred() != NULL)
        PyErr_Print();
    Py_XDECREF(v);              /* Decref result from code load, probably NoneType */
  
    fclose(f);

    /* Load application object */
    a = PyDict_GetItemString(d, "application");
    if(a == NULL || !PyCallable_Check(a)) {
        fprintf(stderr, "application missing or not a callable.\n");
        return get_fallback_app(1);
    }

    Py_INCREF(a);
    return a;
}

/*
 * Main functions
 */

void pie_init(void) {
    if(global_state.venv != NULL) {
        Py_NoSiteFlag = 1;      /* site is loaded in setup_venv */
        setenv("VIRTUAL_ENV", global_state.venv, 1);
    }

    Py_InitializeEx(0);
    PyEval_InitThreads();
    
    main_thr = PyThreadState_Get();
    pie_module = init_module();

    PyThreadState_Swap(main_thr);
    setup_venv(global_state.venv);
    application = load_app(global_state.app);

    wsgi_stderr = (PyObject*)PyObject_New(ErrorObject, &ErrorType);
    PySys_SetObject("stderr", wsgi_stderr);

    PyEval_ReleaseThread(main_thr);
}

void pie_main(void) {
    PyThreadState *py_thr;
    PyInterpreterState *py_main_is;
    RequestObject *request;

    py_main_is = main_thr->interp;
    py_thr = PyThreadState_New(py_main_is);

    PyEval_AcquireThread(py_thr);

    request = (RequestObject *)PyObject_New(RequestObject, &RequestType);

    request->py_main_is = py_main_is;
    request->py_thr = py_thr;
    request->req.input = NULL;
    request->req.headers = NULL;
    request->req.headers_space = 0;
    request->resp.headers_sent = 0;
    request->resp.status = NULL;
    request->resp.headers = NULL;

    pie_buffer_init(&request->req.buffer);
    pie_buffer_set_reader(&request->req.buffer, buffer_do_read, request);

    PyEval_ReleaseThread(py_thr);

    while(global_state.running) {
        int fd = accept(global_state.fd, NULL, NULL);
        if(fd >= 0) {
            request->fd = fd;
            handle_request(request);
            close(fd);
        } else if(errno != EMFILE && errno != ENFILE && errno != EINTR) {
            perror("accept");
            break;
        }

        /* free larger allocations */
        pie_buffer_restart(&request->req.buffer);
        if(request->req.headers_space > 4096) {
            free(request->req.headers);
            request->req.headers = NULL;
            request->req.headers_space = 0;
        }
    }

    free(request->req.headers);
    pie_buffer_free_data(&request->req.buffer);

    PyEval_AcquireThread(main_thr);
    PyThreadState_Clear(request->py_thr);
    PyThreadState_Delete(request->py_thr);
    Py_DECREF(request);
    PyEval_ReleaseThread(main_thr);
}

void pie_finish(void) {
    Py_Finalize();
}
