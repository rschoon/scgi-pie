#!/usr/bin/env python3

import re
import sys
import sysconfig

pyver = sysconfig.get_config_var('VERSION')
getvar = sysconfig.get_config_var

#
# cflags
#

flags = ['-I' + sysconfig.get_path('include'),
         '-I' + sysconfig.get_path('platinclude') ]
flags.extend(getvar('CFLAGS').split())
flags = ' '.join(flags)

#
# libs
#

libs = getvar('LIBS').split() + getvar('SYSLIBS').split()
libs.append('-lpython' + pyver + sys.abiflags)
libs = ' '.join(libs)

#
#
#

print("cflags: %s"%flags)
print("ldflags: %s"%libs)

with open("Makefile.in", "r") as fin:
    with open("Makefile", "w") as fout:
        text = fin.read()
        last = 0
        for m in re.finditer(r"(@@(?P<name>.*?)@@)", text):
            fout.write(text[last:m.start()])
            if m.group("name") == "CFLAGS":
                fout.write(flags)
            elif m.group("name") == "LDFLAGS":
                fout.write(libs)
            last = m.end()
        fout.write(text[last:])

print("\nMakefile written.")
