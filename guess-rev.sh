#!/bin/bash

REV=unknown

which svnversion > /dev/null 2>&1 && REV=`svnversion`

echo -n $REV

