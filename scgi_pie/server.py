
import signal
from threading import Thread

import _scgi_pie

class ServerThread(Thread):
    def __init__(self, app, sock, allow_buffering=False):
        self.listen_sock = sock

        self.request = _scgi_pie.Request(app, sock, allow_buffering)

        Thread.__init__(self)

    def run(self):
        self.request.accept_loop()

class WSGIServer(object):
    def __init__(self, app, socket, num_threads=4, **kwargs):
        if hasattr(socket, "detach"):
            socket = socket.detach()

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
