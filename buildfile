# file      : buildfile
# copyright : Copyright (c) 2014-2019 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/} doc{INSTALL LICENSE NEWS README CONTRIBUTING.md} manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
