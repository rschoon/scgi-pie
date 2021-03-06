#
# Copyright (c) 2013-2015 Robin Schoonover
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import argparse
import importlib
import os
import sys
import threading

import scgi_pie

argp = argparse.ArgumentParser()

proc_argp = argp.add_argument_group(title='Process Options')
proc_argp.add_argument('--num-threads', '-t', type=int, help="Number of threads to spawn (defaults to 4)", default=4)
proc_argp.add_argument('--fd', type=int, help="Use inherited file descriptor as listen socket.  For use with tools such as spawn-fcgi.")
proc_argp.add_argument('--unix-socket', '--unix', '-s', type=argparse.FileType, help="Bind to Unix domain socket on path")
proc_argp.add_argument('--socket-mode', '-M', type=lambda a: int(a, 8), help="Change Unix domain socket path mode")
proc_argp.add_argument('--stack-size', type=int, help="Stack size of threads in bytes")
proc_argp.add_argument('--pipe', action='store_true', help='Use stdin/stdout for a single request instead of listening on a socket.')

python_argp = argp.add_argument_group(title='Python Options')
python_argp.add_argument('--add-dirname-to-path', action='store_true', help="Add path of wsgi app to sys.path")
python_argp.add_argument('--buffering', help="Allow buffering of response output.  "
                         "This violates WSGI spec, but can give a small performance boost")
python_argp.add_argument('--buffer-size', type=int, default=32768, help="Maximum size of buffers in bytes")
python_argp.add_argument('--validator', action='store_true', help='Add wsgiref.validator middleware')
python_argp.add_argument('--module', '-m', help="Load application from module path")
python_argp.add_argument('application', default=None)

args = argp.parse_args()

if args.buffer_size < 1024:
    sys.stderr.write("Buffer size is too small.\n")
    sys.exit(1) 

#
# Make/Get a socket
#

if args.pipe:
    sock = None
elif args.fd is not None:
    sock = args.fd
elif args.unix_socket is not None:
    import socket

    try:
        os.unlink(args.unix_socket)
    except:
        pass

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(args.unix_socket)

    if args.socket_mode:
        os.chmod(args.unix_socket, args.socket_mode)
else:
    sys.stderr.write("No listener given.\n")
    sys.exit(1) 

#
# Misc Process Setup
#

if args.stack_size is not None:
    threading.stack_size(args.stack_size)

#
# Force stderr to flush
#

class flushfile(object):
    def __init__(self, f):
        self.f = f
    def write(self, x):
        self.f.write(x)
        self.f.flush()
    def __getattr__(self,name):
        return object.__getattribute__(self.f, name)
sys.stderr = flushfile(sys.stderr)

#
# Load App
#

if args.module is None:
    if args.add_dirname_to_path:
        sys.path.insert(0, os.path.dirname(args.application))

    application = scgi_pie.load_app_from_file(args.application)
else:
    if args.add_dirname_to_path:
        sys.stderr.write("Warning: ignoring --add-dirname-to-path option because app from module")

    m = importlib.import_module(args.module)
    application = getattr(m, args.application or "application")


if args.validator:
    from wsgiref.validate import validator
    application = validator(application)

kwargs = {
    'allow_buffering' : args.buffering,
    'buffer_size' : args.buffer_size
}

#
# Run single?
#

if sock is None:
    scgi_pie.run_once(application, sys.stdin, sys.stdout, **kwargs)
    sys.exit(0)

#
# Create Server
#

server = scgi_pie.WSGIServer(
        application,
        sock,
        num_threads=args.num_threads,
        **kwargs
)

#
# Setup Signals
#

import signal

def handle_signal(signum, frame):
    server.halt()

signal.signal(signal.SIGPIPE, signal.SIG_IGN)
signal.signal(signal.SIGINT, handle_signal)
signal.signal(signal.SIGTERM, handle_signal)

#
# Run
#

server.run_forever()
