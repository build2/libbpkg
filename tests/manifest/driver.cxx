// file      : tests/manifest/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <cassert>
#include <iostream>

#include <butl/manifest-parser>
#include <butl/manifest-serializer>

#include <bpkg/manifest>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usage: argv[0] (-p|-r|-s)
//
// Read and parse manifest from STDIN and serialize it to STDOUT. The
// following options specify the manifest type.
//
// -p  parse package manifest list
// -r  parse repository manifest list
// -s  parse signature manifest
//
int
main (int argc, char* argv[])
{
  assert (argc == 2);
  string opt (argv[1]);

  cin.exceptions  (ios_base::failbit | ios_base::badbit);
  cout.exceptions (ios_base::failbit | ios_base::badbit);

  manifest_parser     p (cin,  "stdin");
  manifest_serializer s (cout, "stdout");

  if (opt == "-p")
    package_manifests (p).serialize (s);
  else if (opt == "-r")
    repository_manifests (p).serialize (s);
  else if (opt == "-s")
    signature_manifest (p).serialize (s);
  else
    assert (false);
}
