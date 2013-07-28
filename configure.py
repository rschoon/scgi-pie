#!/usr/bin/env python3

import argparse
import re
import sys
import sysconfig
import os

pyver = sysconfig.get_config_var('VERSION')
getvar = sysconfig.get_config_var
is_venv = sys.base_prefix != sys.prefix

mkvar = {}      # makefile variables

#
# Args
#

parser = argparse.ArgumentParser(description='configure build')
parser.add_argument('--prefix', dest='prefix')
parser.add_argument('--bindir', dest='bindir')

args = parser.parse_args()

#
# install paths
#

print(args.prefix, sys.exec_prefix, sys.prefix)

if args.prefix is None:
    if is_venv:
        mkvar['PREFIX'] = sys.exec_prefix
    else:
        mkvar['PREFIX'] = '/usr/local'
else:
    mkvar['PREFIX'] = args.prefix
mkvar['BINDIR'] = args.bindir or os.path.join(mkvar['PREFIX'], 'bin')

#
# binary name
#

mkvar['NAME'] = 'scgi-pie'
if is_venv:
    mkvar['INSTALL_NAME'] = 'scgi-pie'
    mkvar['LN_COMMAND'] = ''
else:
    mkvar['INSTALL_NAME'] = 'scgi-pie'+pyver
    mkvar['LN_COMMAND'] = 'ln -s %s %s'%(os.path.join(mkvar['BINDIR'], 'scgi-pie'+pyver),
                                         os.path.join(mkvar['BINDIR'], 'scgi-pie'))

#
# cflags
#

flags = ['-I' + sysconfig.get_path('include'),
         '-I' + sysconfig.get_path('platinclude') ]
flags.extend(getvar('CFLAGS').split())
mkvar['CFLAGS'] = ' '.join(flags)

#
# libs
#

libs = getvar('LIBS').split() + getvar('SYSLIBS').split()
libs.append('-lpython' + pyver + sys.abiflags)
libs.append('-lrt')
mkvar['LDFLAGS'] = ' '.join(libs)

#
#
#

print("cflags: %s"%mkvar['CFLAGS'])
print("ldflags: %s"%mkvar['LDFLAGS'])
print("prefix: %s"%mkvar['PREFIX'])

with open("Makefile.in", "r") as fin:
    with open("Makefile", "w") as fout:
        text = fin.read()
        last = 0
        for m in re.finditer(r"(@@(?P<name>.*?)@@)", text):
            fout.write(text[last:m.start()])

            name = m.group("name")
            value = mkvar.get(name)
            if value is not None:
                fout.write(value)
            else:
                print("*** Unknown var: %s\n"%name)
                fout.write("@@%s@@"%name)

            last = m.end()
        fout.write(text[last:])

print("\nMakefile written.")

os.system("make clean")
