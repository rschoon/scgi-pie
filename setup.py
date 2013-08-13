#!/usr/bin/env python3

from distutils.core import setup, Extension

setup(name='scgi-pie',
      version='1.0',
      description='lightweight and threaded SCGI server for Python WSGI applications',
      author='Robin Schoonover',
      author_email='robin@cornhooves.org',
      packages = ['scgi_pie'],
      ext_modules=[Extension('_scgi_pie', ['src/pie.c', 'src/buffer.c'])],
      scripts=['scripts/scgi-pie'],
)

