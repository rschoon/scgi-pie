#
# Copyright (c) 2013 Robin Schoonover
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

import signal
from threading import Thread
import os

import _scgi_pie

class ServerThread(Thread):
    def __init__(self, app, sock, allow_buffering=False, buffer_size=32768):
        self.listen_sock = sock

        self.request = _scgi_pie.Request(app, sock, allow_buffering, buffer_size)

        Thread.__init__(self)

    def run(self):
        self.request.accept_loop()

class WSGIServer(object):
    def __init__(self, app, socket, num_threads=4, **kwargs):
        if hasattr(socket, "detach"):
            socket = socket.detach()

        self.socket = socket
        self.threads = []
        for i in range(num_threads):
            self.threads.append(ServerThread(app, socket, **kwargs))

    def run_forever(self):
        for thr in self.threads:
            thr.start()

        for thr in self.threads:
            thr.join()

    def halt(self):
        oldh = signal.signal(signal.SIGINT, lambda i,f: None)
        signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGINT})

        for thr in self.threads:
            thr.request.halt_loop()

        signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGINT})
        signal.signal(signal.SIGINT, oldh)

    def close(self):
        if self.socket is not None:
            os.close(self.socket)
            self.socket = None

    def __del__(self):
        self.close()
