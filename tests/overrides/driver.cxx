// file      : tests/overrides/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <vector>
#include <cstddef>  // size_t
#include <cstdint>  // uint64_t
#include <iostream>

#include <libbutl/utility.hxx>             // trim()
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>

#include <libbpkg/manifest.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usages: argv[0] [-n] (<name>:<value>)*
//
// Parse the package manifest from stdin, apply overrides passed as arguments,
// and serialize the resulting manifest to stdout.
//
// -n  pass the stream name to manifest_parser::override()
//
int
main (int argc, char* argv[])
{
  vector<manifest_name_value> overrides;

  string name;

  uint64_t l (1);
  for (int i (1); i != argc; ++i)
  {
    string a (argv[i]);

    if (a == "-n")
    {
      name = "args";
    }
    else
    {
      size_t p (a.find (':'));

      assert (p != string::npos);

      // Fill the values with the location information for the exception
      // description testing.
      //
      manifest_name_value nv {string (a, 0, p), string (a, p + 1),
                              l /* name_line */,  1     /* name_column */,
                              l /* value_line */, p + 2 /* value_column */,
                              0, 0, 0};

      ++l;

      trim (nv.name);
      trim (nv.value);

      assert (!nv.name.empty ());

      overrides.push_back (move (nv));
    }
  }

  cin.exceptions  (ios_base::failbit | ios_base::badbit);
  cout.exceptions (ios_base::failbit | ios_base::badbit);

  manifest_parser     p (cin,  "stdin");
  manifest_serializer s (cout, "stdout");

  try
  {
    package_manifest m (p);
    m.override (overrides, name);

    // While at it, test validate_overrides().
    //
    try
    {
      package_manifest::validate_overrides (overrides, name);
    }
    catch (const manifest_parsing&)
    {
      assert (false); // Validation must never fail if override succeeds.
    }

    m.serialize (s);
  }
  catch (const manifest_parsing& e)
  {
    cerr << e << endl;
    return 1;
  }
  catch (const manifest_serialization& e)
  {
    cerr << e << endl;
    return 1;
  }

  return 0;
}
