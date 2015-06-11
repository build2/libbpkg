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
    ifs.open (argv[1], ifstream::in | ifstream::binary);

    manifest_parser p (ifs, argv[1]);
    manifests ms (p);

    manifest_serializer s (cout, "stdout");
    ms.serialize (s);
  }
  catch (const ios_base::failure&)
  {
    cerr << "io failure" << endl;
    return 1;
  }
  catch (const std::exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
