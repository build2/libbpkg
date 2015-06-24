// file      : bpkg/manifest.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest>

#include <string>
#include <ostream>
#include <sstream>
#include <cassert>
#include <cstring>   // strncmp()
#include <utility>   // move()
#include <algorithm> // find()
#include <stdexcept> // invalid_argument

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

  // Utility functions
  //
  static const strings priority_names ({"low", "medium", "high", "security"});
  static const string spaces (" \t");

  inline static bool
  space (char c)
  {
    return c == ' ' || c == '\t';
  }

  inline static bool
  digit (char c)
  {
    return c >= '0' && c <= '9';
  }

  inline static bool
  alpha (char c)
  {
    return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z';
  }

  static ostream&
  operator<< (ostream& o, const dependency& d)
  {
    o << d.name;

    if (d.version)
    {
      static const char* operations[] = {"==", "<", ">", "<=", ">="};

      o << " " << operations[static_cast<size_t> (d.version->operation)]
        << " " << d.version->value.string ();
    }

    return o;
  }

  // Resize v up to ';', return what goes after ';'.
  //
  inline static string
  add_comment (const string& v, const string& c)
  {
    return c.empty () ? v : (v + "; " + c);
  }

  static string
  split_comment (string& v)
  {
    using iterator = string::const_iterator;

    iterator b (v.begin ());
    iterator i (b);
    iterator ve (b); // End of value.
    iterator e (v.end ());

    // Find end of value (ve).
    //
    for (char c; i != e && (c = *i) != ';'; ++i)
      if (!space (c))
        ve = i + 1;

    // Find beginning of a comment (i).
    //
    if (i != e)
    {
      // Skip spaces.
      //
      for (++i; i != e && space (*i); ++i);
    }

    string c (i, e);
    v.resize (ve - b);
    return c;
  }

  template <typename T>
  static string
  concatenate (const T& s, const char* delim = ", ")
  {
    ostringstream o;
    for (auto b (s.begin ()), i (b), e (s.end ()); i != e; ++i)
    {
      if (i != b)
        o << delim;

      o << *i;
    }

    return o.str ();
  }

  // list_parser
  //
  class list_parser
  {
  public:
    using iterator = string::const_iterator;

  public:
    list_parser (iterator b, iterator e, char d = ',')
        : pos_ (b), end_ (e), delim_ (d) {}

    string
    next ();

  private:
    iterator pos_;
    iterator end_;
    char delim_;
  };

  string list_parser::
  next ()
  {
    string r;

    // Continue until get non empty list item.
    //
    while (pos_ != end_ && r.empty ())
    {
      // Skip spaces.
      //
      for (; pos_ != end_ && space (*pos_); ++pos_);

      iterator i (pos_);
      iterator e (pos_); // End of list item.

      for (char c; i != end_ && (c = *i) != delim_; ++i)
      {
        if (!space (c))
          e = i + 1;
      }

      if (e - pos_ > 0)
        r.assign (pos_, e);

      pos_ = i == end_ ? i : i + 1;
    }

    return r;
  }

  // version
  //
  version::
  version (const char* v): version () // Delegate
  {
    using std::string; // Otherwise compiler get confused with string() member.

    assert (v != nullptr);

    auto bad_arg ([](const string& d) {throw invalid_argument (d);});

    auto uint16 (
      [&bad_arg](const string& s, const char* what) -> uint16_t
      {
        unsigned long long v (stoull (s));

        if (v > UINT16_MAX) // From <cstdint>.
          bad_arg (string (what) + " should be 2-byte unsigned integer");

        return static_cast<uint16_t> (v);
      });

    auto add_canonical_component (
      [this, &bad_arg](const char* b, const char* e, bool numeric) -> bool
      {
        if (!canonical_.empty ())
          canonical_.append (1, '.');

        if (numeric)
        {
          if (e - b > 8)
            bad_arg ("8 digits maximum allowed in a component");

          canonical_.append (8 - (e - b), '0'); // Add padding spaces.

          string c (b, e);
          canonical_.append (c);
          return stoul (c) != 0;
        }
        else
        {
          // Don't use tolower to keep things locale independent.
          //
          static unsigned char shift ('a' - 'A');

          for (const char* i (b); i != e; ++i)
          {
            char c (*i);
            canonical_.append (1, c >= 'A' && c <='Z' ? c + shift : c);
          }

          return true;
        }
      });

    enum {epoch, upstream, revision} mode (epoch);

    const char* cb (v); // Begin of a component.
    const char* ub (v); // Begin of upstream component.
    const char* ue (v); // End of upstream component.
    const char* lnn (v - 1); // Last non numeric char.

    // Length of upstream version canonical representation without trailing
    // digit-only zero components.
    //
    size_t cl (0);

    const char* p (v);
    for (char c; (c = *p) != '\0'; ++p)
    {
      switch (c)
      {
      case '+':
        {
          if (mode != epoch || p == v)
            bad_arg ("unexpected '+' character position");

          if (lnn >= cb) // Contains non-digits.
            bad_arg ("epoch should be 2-byte unsigned integer");

          epoch_ = uint16 (string (cb, p), "epoch");
          mode = upstream;
          cb = p + 1;
          ub = cb;
          break;
        }

      case '-':
      case '.':
        {
          if (mode != epoch && mode != upstream || p == cb)
            bad_arg (string ("unexpected '") + c + "' character position");

          if (add_canonical_component (cb, p, lnn < cb))
            cl = canonical_.size ();

          ue = p;
          mode = c == '-' ? revision : upstream;
          cb = p + 1;
          break;
        }
      default:
        {
          if (!digit (c) && !alpha (c))
            bad_arg ("alpha-numeric characters expected in a component");
        }
      }

      if (!digit (c))
        lnn = p;
    }

    if (p == cb)
      bad_arg ("unexpected end");

    if (mode == revision)
    {
      if (lnn >= cb) // Contains non-digits.
        bad_arg ("revision should be 2-byte unsigned integer");

      revision_ = uint16 (cb, "revision");
    }
    else
    {
      if (add_canonical_component (cb, p, lnn < cb))
        cl = canonical_.size ();

      ue = p;
    }

    assert (ub != ue); // Can't happen if through all previous checks.
    upstream_.assign (ub, ue);
    canonical_.resize (cl);
  }

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
  package_manifest (parser& p, name_value nv)
  {
    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      bad_name ("start of package manifest expected");

    if (nv.value != "1")
      bad_value ("unsupported format version");

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "name")
      {
        if (!name.empty ())
          bad_name ("package name redefinition");

        if (v.empty ())
          bad_value ("empty package name");

        name = move (v);
      }
      else if (n == "version")
      {
        if (!version.empty ())
          bad_name ("package version redefinition");

        try
        {
          version = version_type (move (v));
        }
        catch (const invalid_argument& e)
        {
          bad_value (string ("invalid package version: ") + e.what ());
        }
      }
      else if (n == "summary")
      {
        if (!summary.empty ())
          bad_name ("package summary redefinition");

        if (v.empty ())
          bad_value ("empty package summary");

        summary = move (v);
      }
      else if (n == "tags")
      {
        if (!tags.empty ())
          bad_name ("package tags redefinition");

        list_parser lp (v.begin (), v.end ());
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
        {
          if (lv.find_first_of (spaces) != string::npos)
            bad_value ("only single-word tags allowed");

          tags.push_back (move (lv));
        }

        if (tags.empty ())
          bad_value ("empty package tags specification");
      }
      else if (n == "description")
      {
        if (description)
        {
          if (description->file)
            bad_name ("package description and description-file are "
                      "mutually exclusive");
          else
            bad_name ("package description redefinition");
        }

        if (v.empty ())
          bad_value ("empty package description");

        description = description_type (move (v));
      }
      else if (n == "description-file")
      {
        if (description)
        {
          if (description->file)
            bad_name ("package description-file redefinition");
          else
            bad_name ("package description-file and description are "
                      "mutually exclusive");
        }

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("no path in package description-file");

        description = description_type (move (v), move (c));
      }
      else if (n == "changes")
      {
        if (v.empty ())
          bad_value ("empty package changes specification");

        changes.emplace_back (move (v));
      }
      else if (n == "changes-file")
      {
        string c (split_comment (v));

        if (v.empty ())
          bad_value ("no path in package changes-file");

        changes.emplace_back (move (v), move (c));
      }
      else if (n == "url")
      {
        if (!url.empty ())
          bad_name ("project url redefinition");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("empty project url");

        url = url_type (move (v), move (c));
      }
      else if (n == "email")
      {
        if (!email.empty ())
          bad_name ("project email redefinition");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("empty project email");

        email = email_type (move (v), move (c));
      }
      else if (n == "package-url")
      {
        if (package_url)
          bad_name ("package url redefinition");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("empty package url");

        package_url = url_type (move (v), move (c));
      }
      else if (n == "package-email")
      {
        if (package_email)
          bad_name ("package email redefinition");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("empty package email");

        package_email = email_type (move (v), move (c));
      }
      else if (n == "priority")
      {
        if (priority)
          bad_name ("package priority redefinition");

        string c (split_comment (v));
        strings::const_iterator b (priority_names.begin ());
        strings::const_iterator e (priority_names.end ());
        strings::const_iterator i (find (b, e, v));

        if (i == e)
          bad_value ("invalid package priority");

        priority =
          priority_type (static_cast<priority_type::value_type> (i - b),
                         move (c));
      }
      else if (n == "license")
      {
        licenses l (split_comment (v));

        list_parser lp (v.begin (), v.end ());
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
          l.push_back (move (lv));

        if (l.empty ())
          bad_value ("empty package license specification");

        license_alternatives.push_back (move (l));
      }
      else if (n == "requires")
      {
        bool cond (!v.empty () && v[0] == '?');
        requirement_alternatives ra (cond, split_comment (v));
        string::const_iterator b (v.begin ());
        string::const_iterator e (v.end ());

        if (ra.conditional)
        {
          string::size_type p (v.find_first_not_of (spaces, 1));
          b = p == string::npos ? e : b + p;
        }

        list_parser lp (b, e, '|');
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
          ra.push_back (lv);

        if (ra.empty () && ra.comment.empty ())
          bad_value ("empty package requirement specification");

        requirements.push_back (move (ra));
      }
      else if (n == "depends")
      {
        bool cond (!v.empty () && v[0] == '?');
        dependency_alternatives da (cond, split_comment (v));
        string::const_iterator b (v.begin ());
        string::const_iterator e (v.end ());

        if (da.conditional)
        {
          string::size_type p (v.find_first_not_of (spaces, 1));
          b = p == string::npos ? e : b + p;
        }

        list_parser lp (b, e, '|');
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
        {
          using iterator = string::const_iterator;

          iterator b (lv.begin ());
          iterator i (b);
          iterator ne (b); // End of name.
          iterator e (lv.end ());

          // Find end of name (ne).
          //
          for (char c; i != e && (c = *i) != '=' && c != '<' && c != '>'; ++i)
          {
            if (!space (c))
              ne = i + 1;
          }

          if (i == e)
            da.push_back (dependency {lv});
          else
          {
            string nm (b, ne);

            if (nm.empty ())
              bad_value ("prerequisite package name not specified");

            // Got to version comparison.
            //
            const char* op (&*i);
            comparison operation;

            if (strncmp (op, "==", 2) == 0)
            {
              operation = comparison::eq;
              i += 2;
            }
            else if (strncmp (op, ">=", 2) == 0)
            {
              operation = comparison::ge;
              i += 2;
            }
            else if (strncmp (op, "<=", 2) == 0)
            {
              operation = comparison::le;
              i += 2;
            }
            else if (*op == '>')
            {
              operation = comparison::gt;
              ++i;
            }
            else if (*op == '<')
            {
              operation = comparison::lt;
              ++i;
            }
            else
              bad_value ("invalid prerequisite package version comparison");

            string::size_type pos = lv.find_first_not_of (spaces, i - b);

            if (pos == string::npos)
              bad_value ("no prerequisite package version specified");

            version_type v;

            try
            {
              v = version_type (lv.c_str () + pos);
            }
            catch (const invalid_argument& e)
            {
              bad_value (
                string ("invalid prerequisite package version: ") + e.what ());
            }

            dependency d{move (nm), version_comparison {move (v), operation}};
            da.push_back (move (d));
          }
        }

        if (da.empty ())
          bad_value ("empty package dependency specification");

        dependencies.push_back (da);
      }
      else
        bad_name ("unknown name '" + n + "' in package manifest");
    }

    // Verify all non-optional values were specified.
    //
    if (name.empty ())
      bad_value ("no package name specified");
    else if (version.empty ())
      bad_value ("no package version specified");
    else if (summary.empty ())
      bad_value ("no package summary specified");
    else if (url.empty ())
      bad_value ("no project url specified");
    else if (email.empty ())
      bad_value ("no project email specified");
    else if (license_alternatives.empty ())
      bad_value ("no project license specified");
  }

  void package_manifest::
  serialize (serializer& s) const
  {
    s.next ("", "1"); // Start of manifest.
    s.next ("name", name);
    s.next ("version", version.string ());

    if (priority)
    {
      priority::value_type v (*priority);
      assert (v < priority_names.size ());
      s.next ("priority", add_comment (priority_names[v], priority->comment));
    }

    s.next ("summary", summary);

    for (const auto& la: license_alternatives)
      s.next ("license", add_comment (concatenate (la), la.comment));

    if (!tags.empty ())
      s.next ("tags", concatenate (tags));

    if (description)
    {
      if (description->file)
        s.next ("description-file",
                add_comment (*description, description->comment));
      else
        s.next ("description", *description);
    }

    for (const auto& c: changes)
    {
      if (c.file)
        s.next ("changes-file", add_comment (c, c.comment));
      else
        s.next ("changes", c);
    }

    s.next ("url", add_comment (url, url.comment));

    if (package_url)
      s.next ("package-url", add_comment (*package_url, package_url->comment));

    s.next ("email", add_comment (email, email.comment));

    if (package_email)
      s.next ("package-email",
              add_comment (*package_email, package_email->comment));

    for (const auto& d: dependencies)
      s.next ("depends",
              (d.conditional ? "? " : "") +
              add_comment (concatenate (d, " | "), d.comment));

    for (const auto& r: requirements)
      s.next ("requires",
              (r.conditional ? "? " : "") +
              add_comment (concatenate (r, " | "), r.comment));

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
  repository_manifest (parser& p, name_value nv)
  {
    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      bad_name ("start of repository manifest expected");

    if (nv.value != "1")
      bad_value ("unsupported format version");

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "location")
        location = move (v);
      else
        bad_name ("unknown name '" + n + "' in repository manifest");
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
