#!/bin/sh
#
# $Id$

if [[ $1 =~ \.[hc]$ ]]; then
  # Convert STATIC to static
  sed -i -e 's/^STATIC/static/' $1
fi
