#!/usr/bin/sh
#

if [[ $1 =~ \.[hc]$ ]]; then
  # Convert EXTERNAL_STATIC to nothing
  sed -i -E -e 's/EXTERNAL_STATIC[ ]?//' $1
fi
