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
  using serialization = manifest_serialization;
  using name_value = manifest_name_value;

  // package_manifest
  //
  package_manifest::
  package_manifest (parser& p)
      : package_manifest (p, p.next ()) // Delegate
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");
  }

  package_manifest::
  package_manifest (parser& p, const name_value& s)
  {
    // Make sure this is the start and we support the version.
    //
    if (!s.name.empty ())
      throw parsing (p.name (), s.name_line, s.name_column,
                     "start of package manifest expected");

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
                       "unknown name '" + n + "' in package manifest");
    }

    // Verify all non-optional values were specified.
    //
  }

  void package_manifest::
  serialize (serializer& s) const
  {
    s.next ("", "1"); // Start of manifest.
    s.next ("name", name);
    // ...
    s.next ("", ""); // End of manifest.
  }

  // repository_manifest
  //
  repository_manifest::
  repository_manifest (parser& p)
      : repository_manifest (p, p.next ()) // Delegate
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single repository manifest expected");
  }

  repository_manifest::
  repository_manifest (parser& p, const name_value& s)
  {
    // Make sure this is the start and we support the version.
    //
    if (!s.name.empty ())
      throw parsing (p.name (), s.name_line, s.name_column,
                     "start of repository manifest expected");

    if (s.value != "1")
      throw parsing (p.name (), s.value_line, s.value_column,
                     "unsupported format version");

    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "location")
        location = move (v);
      else
        throw parsing (p.name (), nv.value_line, nv.value_column,
                       "unknown name '" + n + "' in repository manifest");
    }

    // Verify all non-optional values were specified.
    //
    // - location can be omitted
  }

  void repository_manifest::
  serialize (serializer& s) const
  {
    s.next ("", "1"); // Start of manifest.

    if (!location.empty ())
      s.next ("location", location);

    s.next ("", ""); // End of manifest.
  }

  // manifests
  //
  manifests::
  manifests (parser& p)
  {
    bool rep (true); // Parsing repository list.

    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
    {
      if (rep)
      {
        repositories.push_back (repository_manifest (p, nv));

        // Manifest for local repository signals the end of the
        // repository list.
        //
        if (repositories.back ().location.empty ())
          rep = false;
      }
      else
        packages.push_back (package_manifest (p, nv));
    }
  }

  void manifests::
  serialize (serializer& s) const
  {
    if (repositories.empty () || !repositories.back ().location.empty ())
      throw serialization (s.name (), "local repository manifest expected");

    for (const repository_manifest& r: repositories)
      r.serialize (s);

    for (const package_manifest& p: packages)
      p.serialize (s);

    s.next ("", ""); // End of stream.
  }
}
