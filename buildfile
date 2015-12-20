# file      : buildfile
# copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = bpkg/ tests/
./: $d doc{LICENSE version} file{manifest}
include $d
