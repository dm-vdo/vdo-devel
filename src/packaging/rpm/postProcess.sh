#!/usr/bin/sh
#

if [[ $1 =~ \.[hc]$ ]]; then
  # Convert STATIC to nothing
  sed -i -E -e 's/^STATIC\b[ ]?//' $1
fi
