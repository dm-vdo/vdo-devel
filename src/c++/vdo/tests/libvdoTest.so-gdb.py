# Define a new command "vdo-dump" for GDB to debug a running VDO test.
# (Not useful for core files.)

# Invoke gdb with '-iex "add-auto-load-safe-path ."' to get it to load this
# Python file. Or you can create ~/.config/gdb/gdbinit with an
# add-auto-load-safe-path command pointing to this directory, or using "/" to
# disable the security protection altogether.

import gdb

class VDODumpCommand(gdb.Command):
    """VDO "dump" command

    Calls vdo_dump on the supplied VDO device pointer (which must look
    like one "word" in the UI, for silly reasons) and an optional list
    of (unquoted) dump parameters like "viopool", "queues", "all",
    etc.

    """
    def __init__(self):
        super(VDODumpCommand, self).__init__ ("vdo-dump", gdb.COMMAND_USER)

    def invoke(self, argument, from_tty):
        # FIXME reject non-ASCII input
        args = gdb.string_to_argv(argument)
        if len(args) < 1:
            raise gdb.GdbError("usage: vdo-dump <vdo-pointer> [<dump-arg>...]")
        vdo_arg = args[0]
        args[0] = "dummy"
        # FIXME validate the args??
        # (That would take care of the ASCII and special-char cases...)
        # FIXME quote any special characters
        c_string = '\\0'.join(args)
        lengths = list(map(len, args))
        offsets = list(map(lambda n: sum(lengths[:n]) + n, range(len(args))))
        c_string_loc = gdb.parse_and_eval("(char *) \"" + c_string + "\"")
        gdb.set_convenience_variable("_temp", c_string_loc)
        c_argv_elements = list(map(lambda n: f"$_temp+{offsets[n]},", range(len(offsets))))
        c_argv_init = "&{ " + " ".join(c_argv_elements + ["(char *)0"]) + " }"
        c_argv_loc = gdb.parse_and_eval(c_argv_init)
        gdb.set_convenience_variable("_temp2", c_argv_loc)
        gdb.execute(f"call vdo_dump({vdo_arg}, {len(args)}, $_temp2, \"gdb\")")

VDODumpCommand()
