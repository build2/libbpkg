// file      : tests/manifest/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <iostream>

#include <butl/fdstream>
#include <butl/manifest-parser>
#include <butl/manifest-serializer>

#include <bpkg/manifest>

using namespace std;
using namespace butl;
using namespace bpkg;

int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    cerr << "usage: " << argv[0] << " <file>" << endl;
    return 1;
  }

  try
  {
    ifdstream ifs (argv[1]);
    manifest_parser p (ifs, argv[1]);

#ifdef TEST_PACKAGES
    package_manifests ms (p);
#elif TEST_REPOSITORIES
    repository_manifests ms (p);
#else
    signature_manifest ms (p);
#endif

    stdout_fdmode (fdstream_mode::binary); // Write in binary mode.
    manifest_serializer s (cout, "stdout");
    ms.serialize (s);
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
