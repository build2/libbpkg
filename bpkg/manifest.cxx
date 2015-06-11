// file      : bpkg/manifest.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest>

#include <utility> // move()

#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

using namespace std;

namespace bpkg
{
  using parser = manifest_parser;
  using parsing = manifest_parsing;
  using serializer = manifest_serializer;
  using name_value = manifest_name_value;

  // manifest
  //
  manifest::
  manifest (parser& p): manifest (p, p.next ()) // Delegate.
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single manifest expected");
  }

  manifest::
  manifest (parser& p, const name_value& s)
  {
    // Make sure this is the start and we support the version.
    //
    if (!s.name.empty ())
      throw parsing (p.name (), s.name_line, s.name_column,
                     "start of manifest expected");

    if (s.value != "1")
      throw parsing (p.name (), s.value_line, s.value_column,
                     "unsupported format version");

    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "name")
        name = move (v);
      // ...
      else
        throw parsing (p.name (), nv.value_line, nv.value_column,
                       "unknown name " + n);
    }

    // Verify all non-optional values were specified.
    //
  }

  void manifest::
  serialize (serializer& s) const
  {
    s.next ("", "1"); // Start of manifest.
    s.next ("name", name);
    // ...
    s.next ("", ""); // End of manifest.
  }

  // manifests
  //
  manifests::
  manifests (parser& p)
  {
    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
      push_back (manifest (p, nv));
  }

  void manifests::
  serialize (serializer& s) const
  {
    for (const manifest& m: *this)
      m.serialize (s);

    s.next ("", ""); // End of stream.
  }
}
