# file      : buildfile
# license   : MIT; see accompanying LICENSE file

./: {*/ -build/}                                    \
    doc{INSTALL NEWS README} legal{LICENSE AUTHORS} \
    manifest

# Don't install tests or the INSTALL file.
#
tests/:          install = false
doc{INSTALL}@./: install = false
