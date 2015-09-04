// file      : tests/manifest/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <fstream>
#include <iostream>

#include <bpkg/manifest>
#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

using namespace std;
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
    ifstream ifs;
    ifs.exceptions (ifstream::badbit | ifstream::failbit);
    ifs.open (argv[1]);

    manifest_parser p (ifs, argv[1]);

#ifdef TEST_PACKAGES
    package_manifests ms (p);
#else
    repository_manifests ms (p);
#endif

    manifest_serializer s (cout, "stdout");
    ms.serialize (s);
  }
  catch (const ios_base::failure&)
  {
    cerr << "io failure" << endl;
    return 1;
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
