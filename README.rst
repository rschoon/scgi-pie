
Installing
==========

Currently, the configure script is python, and generates flags based on the
version of python used to run it, so run it like this::

    $ python3.3 ./configure.py

This will generate a Makefile::

    $ make
    # make install

If a virtual environment is used to run the configure script, the install 
location will default to the environment.

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
