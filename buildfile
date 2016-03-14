# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = bpkg/ tests/
./: $d doc{LICENSE README version} file{manifest}
include $d

doc{INSTALL*}: install = false
