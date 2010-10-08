#!/bin/bash

FILES=`git ls-files | sed -e's/^/gfs2-utils\//'`
BRANCH=`git show-branch --current | sed -e's/.*\[//' -e's/\].*$//'`
DESC=`git describe 2>/dev/null`

if [ $? != 128 ]; then
  if [ ${BRANCH} == "master" ]; then
    VERSION=-${DESC}
  else
    VERSION=-${BRANCH}-${DESC}
  fi
fi

DEST=`pwd`/gfs2-utils${VERSION}.tar.gz

echo "Creating ${DEST}..."

(cd ..; tar -zcf ${DEST} ${FILES})

echo "Done."

