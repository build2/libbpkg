// file      : tests/manifest/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <cassert>
#include <iostream>

#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

#include <libbpkg/manifest.hxx>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usage: argv[0] (-pp|-dp|-gp|-pr|-dr|-gr|-s)
//
// Read and parse manifest from STDIN and serialize it to STDOUT. The
// following options specify the manifest type.
//
// -pp  parse pkg package manifest list
// -dp  parse dir package manifest list
// -gp  parse git package manifest list
// -pr  parse pkg repository manifest list
// -dr  parse dir repository manifest list
// -gr  parse git repository manifest list
// -s   parse signature manifest
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

  if (opt == "-pp")
    pkg_package_manifests (p).serialize (s);
  else if (opt == "-dp")
    dir_package_manifests (p).serialize (s);
  else if (opt == "-gp")
    git_package_manifests (p).serialize (s);
  else if (opt == "-pr")
    pkg_repository_manifests (p).serialize (s);
  else if (opt == "-dr")
    dir_repository_manifests (p).serialize (s);
  else if (opt == "-gr")
    git_repository_manifests (p).serialize (s);
  else if (opt == "-s")
    signature_manifest (p).serialize (s);
  else
    assert (false);
}
