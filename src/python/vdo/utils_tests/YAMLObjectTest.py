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
import gettext
import sys
import unittest
import yaml

gettext.install('YAMLObjectTest')

sys.path.append("../..")
from vdo.utils import YAMLObject

########################################################################
class TestYAMLObject(YAMLObject):
  yaml_tag = "!test"

  ######################################################################
  @property
  def _yamlAttributeKeys(self):
    return ["one", "two", "three"]

########################################################################
class Test_YAMLObject(unittest.TestCase):

  ######################################################################
  def assertUnconditionally(self):
    self.assertTrue(False)

  ######################################################################
  def test_fromYAML(self):
    """Tests that we can create an instance from a YAML representation.
    """
    yamlRepresentation = """
!test
one:   1
two:   2
three: 3
"""

    instance = yaml.safe_load(yamlRepresentation)
    self.assertTrue(isinstance(instance, TestYAMLObject),
                    "instance is a TestYAMLObject")

  ######################################################################
  def test_toYAML(self):
    """Tests that we can generate a YAML representation.
    """
    instance = TestYAMLObject()
    instance.one = 1
    instance.two = 2
    instance.three = 3
    self.assertTrue(yaml.dump(instance) is not None,
                    "generated YAML representation")

  ######################################################################
  def test_toAndFromYAML(self):
    """Tests that we can create an instance from a YAML representation that
    is equal to the instance from which the representation was generated.
    """
    first = TestYAMLObject()
    first.one = 1
    first.two = 2
    first.three = 3

    second = yaml.safe_load(yaml.dump(first))
    self.assertTrue((second.one == first.one)
                    and (second.two == first.two)
                    and (second.three == first.three),
                    "constructed instance equals generating instance")

########################################################################
if __name__ == "__main__":
  unittest.main()
