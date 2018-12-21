// file      : tests/manifest/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
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

// Usages:
//
// argv[0] (-pp|-dp|-gp|-pr|-dr|-gr|-s)
// argv[0] [-c] -p
// argv[0] -ec <version>
//
// In the first form read and parse manifest list from stdin and serialize it
// to stdout. The following options specify the manifest type.
//
// -pp  parse pkg package manifest list
// -dp  parse dir package manifest list
// -gp  parse git package manifest list
// -pr  parse pkg repository manifest list
// -dr  parse dir repository manifest list
// -gr  parse git repository manifest list
// -s   parse signature manifest
//
// In the second form read and parse the package manifest from stdin and
// serialize it to stdout. Complete the dependency constraints if -c is
// specified. Note: -c, if specified, should go before -p on the command line.
//
// In the third form read and parse dependency constraints from stdin and
// roundtrip them to stdout together with their effective constraints,
// calculated using version passed as an argument.
//
int
main (int argc, char* argv[])
{
  assert (argc <= 3);
  string opt (argv[1]);

  bool complete_depends (opt == "-c");

  if (complete_depends)
  {
    opt = argv[2];
    assert (opt == "-p");
  }

  assert ((opt == "-ec" || complete_depends) == (argc == 3));

  cout.exceptions (ios_base::failbit | ios_base::badbit);

  manifest_parser     p (cin,  "stdin");
  manifest_serializer s (cout, "stdout");

  try
  {
    if (opt == "-ec")
    {
      version v (argv[2]);

      cin.exceptions (ios_base::badbit);

      string s;
      while (!eof (getline (cin, s)))
      {
        dependency_constraint c (s);
        dependency_constraint ec (c.effective (v));

        assert (c.complete () == (c == ec));

        cout << c << " " << ec << endl;
      }
    }
    else
    {
      cin.exceptions (ios_base::failbit | ios_base::badbit);

      if (opt == "-p")
        pkg_package_manifest (p,
                              false /* ignore_unknown */,
                              complete_depends).serialize (s);
      else if (opt == "-pp")
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
  }
  catch (const manifest_parsing& e)
  {
    cerr << e << endl;
    return 1;
  }

  return 0;
}
