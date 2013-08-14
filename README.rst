
Overview
========

scgi-pie is a lightweight and threaded SCGI server for Python WSGI applications.

At present it only supports Python 3.3 and newer.

Installing
==========

scgi-pie uses a setup.py script in the usual fashion, like so::

    $ python3.3 ./setup.py build
    # python3.3 ./setup.py install

Running
=======

Most configuration options are related to configuring the python environment.  
Since spawn-fcgi only sets up process parameters and creates a socket to listen
on, it can be used with ``scgi-pie --fd 0`` to set up and configure the process.
scgi-pie also provides the minimal ability to set up a unix domain socket using
the ``-s`` flag.  For more information, consult output from the ``--help`` flag.

At present, scgi-pie expects a mod-wsgi style .wsgi file.

Example
-------

test.wsgi::

    def application(environ, start_response):
        start_response('200 OK', [('Content-Type: text/plain')])
    return [(b'hello, world!')]

command line::

    spawn-fcgi -p 4040 -- /usr/bin/env scgi-pie /path/to/test.wsgi
