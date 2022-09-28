#!/usr/bin/python

#
# %COPYRIGHT%
#
# %LICENSE%
#

#
# $Id$
#
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
from datetime import *
import gettext
import sys
import unittest
gettext.install('CommandTest')

sys.path.append("../..")
from vdo.utils import Command, CommandError, runCommand
from vdo.utils import tryCommandsUntilSuccess

class Test_Command(unittest.TestCase):

  def test_commandName(self):
    nc = Command(['echo', 'foo', 'on', 'you'])
    self.assertEqual(nc.commandName(), 'echo', "simple command name")

  def test_success(self):
    nc = Command(['echo', 'foo', 'on', 'you'])
    s = nc.run()
    self.assertEqual(s.rstrip(), 'foo on you', "stdout from echo")
    self.assertEqual(runCommand(['echo', 'foo', 'on', 'you']).rstrip(),
                     'foo on you', "stdout from echo")
    self.assertEqual(runCommand(['echo', 'foo', 'on', 'you'], strip=True),
                     'foo on you', "stdout from echo")

  def test_call_errors(self):
    nc = Command(['BOGUS', 'foo', 'on', 'you'])
    self.assertRaises(CommandError, nc.run)
    self.assertEqual(nc.run(noThrow=True), '', "noThrow error")
    self.assertEqual(runCommand(['BOGUS', 'foo', 'on', 'you'], noThrow=True),
                     '', "noThrow error")

  def test_setenv(self):
    self.assertTrue(runCommand(['date'],
                               environment={ 'TZ' : 'UTC' }).find('UTC'),
                    'environment variable')

  def test_retries(self):
    t = (datetime.now() + timedelta(seconds=15)).strftime("%H:%M:%S")[:-1]
    runCommand(['date', '|', 'grep', t], shell=True, retries=21)
    t = (datetime.now() + timedelta(seconds=15)).strftime("%H:%M:%S")[:-1]
    self.assertRaises(CommandError, runCommand,
                      ['date', '|', 'grep', t], shell=True, retries=2)

  def test_shell(self):
    self.assertEqual(runCommand(['ls', '/etc', '|', 'grep', 'permabit'],
                                shell=True, strip=True),
                     'permabit', "shell pipeline")

  def test_tryCommandsUntilSuccess(self):
    self.assertEqual(tryCommandsUntilSuccess([['./nonexistentCommand'],
                                              ['./nonexistentCommand2'],
                                              ['echo', 'first'],
                                              ['./nonexistentCommand3'],
                                              ['echo', 'second']], strip=True),
                     'first')

    self.assertRaises(CommandError, tryCommandsUntilSuccess,
                      [['./nonexistentCommand'],
                       ['./nonexistentCommand2'],
                       ['./nonexistentCommand3']], strip=True)


if __name__ == "__main__":
  unittest.main()
