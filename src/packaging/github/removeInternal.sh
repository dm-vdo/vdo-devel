#!/bin/sh
#
# $Id$

if [[ $1 =~ \.[hc]$ ]]; then
  # Convert EXTERNAL_STATIC to static
  sed -i -e 's/^EXTERNAL_STATIC/static/' $1
fi
