# -*- mode: python -*-
#
# %COPYRIGHT%
#
# %LICENSE%
#

"""
GDB command definition to load a shared library at a given address,
with appropriate adjustments for symbols in all sections.

For some reason, add-symbol-file doesn't automatically adjust the load
addresses of all of the sections, at least in the use case where we
have a core file but no executable(!), so we need to compute and
specify the load addresses for all the loadable sections..

Usage:
  gdb7 --core=core.foo
  source .../addlib.py
  add-library-file libuds.so 0x7fbd6a76c000
or:
  set $libc=0x7fbd6a419000
  add-library-file /usr/lib/debug/libc-2.7.so $libc

Figuring out the load address from the core file is left as an
exercise for the reader.
"""

# TODO: command-line arg completion, arg count checks, checking for
# errors from objdump.  Might also be better done with direct parsing
# of ELF format.  Should find a way to make read-only sections
# accessible using specified library image.  Support libc where the
# code and the debug data are stored in two different files.
# dont-repeat?

# Figure out how to automatically find the library in the address
# space?  My current techniques involve looking for a page with a
# certain byte at a certain offset, followed eight bytes later by
# another certain byte, followed eight bytes later...it requires
# examining the symbol table and relocations, and knowing which
# writable storage is not actually going to get changed once the
# relocations are performed.  (E.g., "const" string-pointer tables
# that aren't modified after load.)  It's not something I'm quite
# ready to automate in some generic way.

from __future__ import with_statement
import gdb
import re
import subprocess

# Don't need the LMA or Algn values
objdump_h_re=re.compile(r"^ +\d+ +([^ ]*) +([0-9a-f]+) +([0-9a-f]+) +[0-9a-f]+ +([0-9a-f]+) +2\*\*[0-9]+$")

# Uses "objdump -h" to return a list of all sections with some of their attributes.
def GetSectionHeaders(file):
    """Get the section header info from an object file or executable."""
    # run objdump -h
    sections=[]
    with subprocess.Popen(["objdump", "-h", file], 0,
                          None, None, subprocess.PIPE).stdout as pipe:
        # parse output
        pipe.readline() # blank line
        pipe.readline() # filename: format info
        pipe.readline() # blank line
        pipe.readline() # "Sections:"
        pipe.readline() # "Idx Name ..."
        while True:
            line1 = pipe.readline()
            if line1 == "":
                break
            line2 = pipe.readline()
            m = re.match(objdump_h_re, line1)
            name = m.group(1)
            size = int("0x" + m.group(2), 16)
            vma = int("0x" + m.group(3), 16)
            fileOff = int("0x" + m.group(4), 16)
            attrs = line2.replace(",", "").split()
            sections.append({ 'name': name, 'size': size, 'vma': vma, 'fileOff': fileOff, 'flags': attrs })
    return sections

class AddLibraryFile(gdb.Command):
    """Add symbols from a library file."""
    def __init__(self):
        super(AddLibraryFile,self).__init__("add-library-file",
                                            gdb.COMMAND_FILES)
    def invoke(self, arg, from_tty):
        args = gdb.string_to_argv(arg)
        libfile = args[0]
        headers = GetSectionHeaders(libfile)
        asfArgs = ""
        addend = gdb.parse_and_eval(args[1])
        if addend.type.code != gdb.TYPE_CODE_INT:
            raise gdb.GdbError("arg 2 should be an address")
        textaddr = None
        for h in headers:
            # Sections for which virtual memory address space is
            # allocated need to be relocated.
            if 'ALLOC' in h['flags']:
                if h['name'] == '.text':
                    textaddr = h['vma'] + addend
                else:
                    asfArgs = asfArgs + (" -s %s 0x%x"
                                         % (h['name'], h['vma'] + addend))
        gdb.execute("add-symbol-file %s 0x%x %s" % (libfile, textaddr, asfArgs))

AddLibraryFile()
