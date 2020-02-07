// file      : tests/manifest/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <cassert>
#include <iostream>

#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>
#include <libbutl/standard-version.mxx>

#include <libbpkg/manifest.hxx>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usages:
//
// argv[0] (-pp|-dp|-gp|-pr|-dr|-gr|-s)
// argv[0] -p -c -i
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
// serialize it to stdout.
//
// -c   complete the dependency constraints
// -i   ignore unknown
//
// Note: the above options should go after -p on the command line.
//
// In the third form read and parse dependency constraints from stdin and
// roundtrip them to stdout together with their effective constraints,
// calculated using version passed as an argument.
//
int
main (int argc, char* argv[])
{
  assert (argc >= 2);
  string mode (argv[1]);

  cout.exceptions (ios_base::failbit | ios_base::badbit);

  manifest_parser     p (cin,  "stdin");
  manifest_serializer s (cout, "stdout");

  try
  {
    if (mode == "-p")
    {
      bool complete_dependencies (false);
      bool ignore_unknown (false);

      for (int i (2); i != argc; ++i)
      {
        string o (argv[i]);

        if (o == "-c")
          complete_dependencies = true;
        else if (o == "-i")
          ignore_unknown = true;
        else
          assert (false);
      }

      cin.exceptions (ios_base::failbit | ios_base::badbit);

      package_manifest (
        p,
        [] (version& v)
        {
          // Emulate populating the snapshot information for the latest
          // snapshot.
          //
          if (butl::optional<standard_version> sv =
              parse_standard_version (v.string ()))
          {
            if (sv->latest_snapshot ())
            {
              sv->snapshot_sn = 123;
              v = version (sv->string ());
            }
          }
        },
        ignore_unknown,
        complete_dependencies).serialize (s);
    }
    else if (mode == "-ec")
    {
      assert (argc == 3);

      version v (argv[2]);

      cin.exceptions (ios_base::badbit);

      string s;
      while (!eof (getline (cin, s)))
      {
        version_constraint c (s);
        version_constraint ec (c.effective (v));

        assert (c.complete () == (c == ec));

        cout << c << " " << ec << endl;
      }
    }
    else
    {
      assert (argc == 2);

      cin.exceptions (ios_base::failbit | ios_base::badbit);

      if (mode == "-pp")
        pkg_package_manifests (p).serialize (s);
      else if (mode == "-dp")
        dir_package_manifests (p).serialize (s);
      else if (mode == "-gp")
        git_package_manifests (p).serialize (s);
      else if (mode == "-pr")
        pkg_repository_manifests (p).serialize (s);
      else if (mode == "-dr")
        dir_repository_manifests (p).serialize (s);
      else if (mode == "-gr")
        git_repository_manifests (p).serialize (s);
      else if (mode == "-s")
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
  catch (const invalid_argument& e)
  {
    cerr << e << endl;
    return 1;
  }

  return 0;
}
