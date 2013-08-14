#!/usr/bin/env python3

import os
from distutils.core import setup, Extension

extra_compile_args = None
if os.name == 'posix':
    # XXX gcc only
    extra_compile_args = ['-fvisibility=hidden']

setup(
    name='scgi-pie',
    version='1.0',
    description='lightweight and threaded SCGI server for Python WSGI applications',
    author='Robin Schoonover',
    author_email='robin@cornhooves.org',
    packages = ['scgi_pie'],
    ext_modules=[
        Extension('_scgi_pie', ['src/pie.c', 'src/buffer.c'],
                  extra_compile_args=extra_compile_args)
    ],
    scripts=['scripts/scgi-pie'],
)

