#!/bin/sh
# Generate debian/changelog.upstream, debian/generated-m4.list,
# and debian/generated-po.list.
#
# Uses debian/changelog, the git revision log, and .gitignore
# files from the current checkout.

set -e

sh debian/changelog.upstream.sh
cp -f m4/.gitignore debian/generated-m4.list
cp -f po/.gitignore debian/generated-po.list
