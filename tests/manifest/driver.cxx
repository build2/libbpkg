// file      : tests/manifest/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <iostream>

#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>
#include <libbutl/standard-version.hxx>

#include <libbpkg/manifest.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usages:
//
// argv[0] (-pp|-dp|-gp|-pr|-dr|-gr|-s) [-l]
// argv[0] -p [-c] [-i] [-l]
// argv[0] -ec <version>
// argv[0] -et <type> <name>
// argv[0] -v
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
// -v   print the libbpkg version
//
// In the second form read and parse the package manifest from stdin and
// serialize it to stdout.
//
// -c   complete the incomplete values (depends, <distribution>-version, etc)
// -i   ignore unknown
//
// Note: the above options should go after -p on the command line.
//
// -l
//    Don't break long lines while serializing a manifest.
//
// In the third form read and parse dependency constraints from stdin and
// roundtrip them to stdout together with their effective constraints,
// calculated using version passed as an argument.
//
// In the forth form print the effective type and the type sub-options to
// stdout (one per line) and exit.
//
// In the fifth form print the libbpkg version to stdout and exit.
//
int
main (int argc, char* argv[])
{
  assert (argc >= 2);
  string mode (argv[1]);

  cout.exceptions (ios_base::failbit | ios_base::badbit);

  if (mode == "-v")
  {
    cout << standard_version (LIBBPKG_VERSION_STR) << endl;
    return 0;
  }

  manifest_parser p (cin,  "stdin");

  try
  {
    if (mode == "-p")
    {
      bool complete_values (false);
      bool ignore_unknown (false);
      bool long_lines (false);

      for (int i (2); i != argc; ++i)
      {
        string o (argv[i]);

        if (o == "-c")
          complete_values = true;
        else if (o == "-i")
          ignore_unknown = true;
        else if (o == "-l")
          long_lines = true;
        else
          assert (false);
      }

      manifest_serializer s (cout, "stdout", long_lines);

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
        complete_values).serialize (s);
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
    else if (mode == "-et")
    {
      assert (argc == 4);

      optional<string> t (*argv[2] != '\0'
                          ? string (argv[2])
                          : optional<string> ());

      package_name n (argv[3]);

      cout << package_manifest::effective_type (t, n) << endl;

      for (const string& so: package_manifest::effective_type_sub_options (t))
        cout << so << endl;
    }
    else
    {
      bool long_lines (false);

      for (int i (2); i != argc; ++i)
      {
        string o (argv[i]);

        if (o == "-l")
          long_lines = true;
        else
          assert (false);
      }

      manifest_serializer s (cout, "stdout", long_lines);

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
