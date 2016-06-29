// file      : bpkg/manifest.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/manifest>

#include <string>
#include <ostream>
#include <sstream>
#include <cassert>
#include <cstring>   // strncmp(), strcmp()
#include <utility>   // move()
#include <cstdint>   // uint64_t, uint16_t, UINT16_MAX
#include <iterator>  // back_insert_iterator
#include <algorithm> // find(), transform()
#include <stdexcept> // invalid_argument

#include <butl/path>
#include <butl/base64>

#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

using namespace std;
using namespace butl;

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
  static const strings repository_role_names (
    {"base", "prerequisite", "complement"});

  static const string spaces (" \t");

  inline static bool
  space (char c) noexcept
  {
    return c == ' ' || c == '\t';
  }

  inline static bool
  digit (char c) noexcept
  {
    return c >= '0' && c <= '9';
  }

  inline static bool
  alpha (char c) noexcept
  {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }

  inline static bool
  valid_sha256 (const string& s) noexcept
  {
    if (s.size () != 64)
      return false;

    for (const auto& c: s)
    {
      if ((c < 'a' || c > 'f' ) && !digit (c))
        return false;
    }

    return true;
  }

  // Replace std::tolower to keep things locale independent.
  //
  inline static char
  lowercase (char c) noexcept
  {
    const unsigned char shift ('a' - 'A');
    return c >= 'A' && c <='Z' ? c + shift : c;
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
  version (uint16_t e, std::string u, optional<std::string> l, uint16_t r)
      : epoch (e),
        upstream (move (u)),
        release (move (l)),
        revision (r),
        canonical_upstream (
          data_type (upstream.c_str (), data_type::parse::upstream).
            canonical_upstream),
        canonical_release (
          data_type (release ? release->c_str () : nullptr,
                     data_type::parse::release).
            canonical_release)
  {
    // Check members constrains.
    //
    if (upstream.empty ()) // Constructing empty version.
    {
      if (epoch != 0)
        throw invalid_argument ("epoch for empty version");

      if (!release || !release->empty ())
        throw invalid_argument ("not-empty release for empty version");

      if (revision != 0)
        throw invalid_argument ("revision for empty version");
    }
    else if (release && release->empty () && revision != 0)
      // Empty release signifies the earliest possible release. Revision is
      // meaningless in such a context.
      //
      throw invalid_argument ("revision for earliest possible release");
  }

  // Builder of the upstream or release version part canonical representation.
  //
  struct canonical_part: string
  {
    string
    final () const {return substr (0, len_);}

    void
    add (const char* begin, const char* end, bool numeric)
    {
      if (!empty ())
        append (1, '.');

      bool zo (false); // Digit-only zero component.
      if (numeric)
      {
        if (end - begin > 8)
          throw invalid_argument ("8 digits maximum allowed in a component");

        append (8 - (end - begin), '0'); // Add padding zeros.

        string c (begin, end);
        append (c);
        zo = stoul (c) == 0;
      }
      else
      {
        for (const char* i (begin); i != end; ++i)
          append (1, lowercase (*i));
      }

      if (!zo)
        len_ = size ();
    }

  private:
    size_t len_ = 0; // Length without the trailing digit-only zero components.
  };

  version::data_type::
  data_type (const char* v, parse pr): epoch (0), revision (0)
  {
    // Otherwise compiler gets confused with string() member.
    //
    using std::string;

    if (pr == parse::release && v == nullptr)
    {
      // Special case: final version release part.
      //
      canonical_release = "~";
      return;
    }

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

    enum class mode {epoch, upstream, release, revision};
    mode m (pr == parse::full
            ? mode::epoch
            : pr == parse::upstream
              ? mode::upstream
              : mode::release);

    canonical_part canon_upstream;
    canonical_part canon_release;

    canonical_part* canon_part (
      pr == parse::release ? &canon_release : &canon_upstream);

    const char* cb (v); // Begin of a component.
    const char* ub (v); // Begin of upstream part.
    const char* ue (v); // End of upstream part.
    const char* rb (v); // Begin of release part.
    const char* re (v); // End of release part.
    const char* lnn (v - 1); // Last non numeric char.

    const char* p (v);
    for (char c; (c = *p) != '\0'; ++p)
    {
      switch (c)
      {
      case '~':
        {
          if (pr != parse::full)
            bad_arg ("unexpected '~' character");

          // Process the epoch part.
          //
          if (m != mode::epoch || p == v)
            bad_arg ("unexpected '~' character position");

          if (lnn >= cb) // Contains non-digits.
            bad_arg ("epoch should be 2-byte unsigned integer");

          epoch = uint16 (string (cb, p), "epoch");

          m = mode::upstream;
          cb = p + 1;
          ub = cb;
          break;
        }

      case '+':
      case '-':
      case '.':
        {
          // Process the upsteam or release part component.
          //

          // Characters '+', '-' are only valid for the full version parsing.
          //
          if (c != '.' && pr != parse::full)
            bad_arg (string ("unexpected '") + c + "' character");

          // Check if the component ending is valid for the current parsing
          // state.
          //
          if (m == mode::revision || (c == '-' && m == mode::release) ||
              p == cb)
            bad_arg (string ("unexpected '") + c + "' character position");

          // Append the component to the current canonical part.
          //
          canon_part->add (cb, p, lnn < cb);

          // Update the parsing state.
          //
          cb = p + 1;

          if (m == mode::upstream || m == mode::epoch)
            ue = p;
          else if (m == mode::release)
            re = p;
          else
            assert (false);

          if (c == '+')
            m = mode::revision;
          else if (c == '-')
          {
            m = mode::release;
            canon_part = &canon_release;
            rb = cb;
            re = cb;
          }
          else if (m == mode::epoch)
            m = mode::upstream;

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

    assert (p >= cb); // 'p' denotes the end of the last component.

    // An empty component is valid for the release part, and for the upstream
    // part when constructing empty or max limit version.
    //
    if (p == cb && m != mode::release && pr != parse::upstream)
      bad_arg ("unexpected end");

    // Parse the last component.
    //
    if (m == mode::revision)
    {
      if (lnn >= cb) // Contains non-digits.
        bad_arg ("revision should be 2-byte unsigned integer");

      revision = uint16 (cb, "revision");
    }
    else if (cb != p)
    {
      canon_part->add (cb, p, lnn < cb);

      if (m == mode::epoch || m == mode::upstream)
        ue = p;
      else if (m == mode::release)
        re = p;
    }

    // Upstream and release pointer ranges are valid at the end of the day.
    //
    assert (ub <= ue && rb <= re);

    if (pr != parse::release)
    {
      // Fill upstream original and canonical parts.
      //
      if (!canon_upstream.empty ())
      {
        assert (ub != ue); // Can't happen if through all previous checks.
        canonical_upstream = canon_upstream.final ();

        if (pr == parse::full)
          upstream.assign (ub, ue);
      }
    }

    if (pr != parse::upstream)
    {
      // Fill release original and canonical parts.
      //
      if (!canon_release.empty ())
      {
        assert (rb != re); // Can't happen if through all previous checks.
        canonical_release = canon_release.final ();

        if (pr == parse::full)
          release = string (rb, re);
      }
      else
      {
        if (m == mode::release)
        {
          // Empty release part signifies the earliest possible version
          // release. Make original, and keep canonical representations empty.
          //
          if (pr == parse::full)
            release = "";
        }
        else
        {
          // Absent release part signifies the final (max) version release.
          // Assign the special value to the canonical representation, keep
          // the original one nullopt.
          //
          canonical_release = "~";
        }
      }
    }

    if (pr == parse::full && epoch == 0 && canonical_upstream.empty () &&
        canonical_release.empty ())
    {
      assert (revision == 0); // Can't happen if through all previous checks.
      bad_arg ("empty version");
    }
  }

  version& version::
  operator= (const version& v)
  {
    if (this != &v)
      *this = version (v); // Reduce to move-assignment.
    return *this;
  }

  version& version::
  operator= (version&& v)
  {
    if (this != &v)
    {
      this->~version ();
      new (this) version (move (v)); // Assume noexcept move-construction.
    }
    return *this;
  }

  string version::
  string (bool ignore_revision) const
  {
    if (empty ())
      throw logic_error ("empty version");

    std::string v (epoch != 0 ? to_string (epoch) + "~" + upstream : upstream);

    if (release)
    {
      v += '-';
      v += *release;
    }

    if (!ignore_revision && revision != 0)
    {
      v += '+';
      v += to_string (revision);
    }

    return v;
  }

  // text_file
  //
  text_file::
  ~text_file ()
  {
    if (file)
      path.~path_type ();
    else
      text.~string ();
  }

  text_file::
  text_file (text_file&& f): file (f.file), comment (move (f.comment))
  {
    if (file)
      new (&path) path_type (move (f.path));
    else
      new (&text) string (move (f.text));
  }

  text_file::
  text_file (const text_file& f): file (f.file), comment (f.comment)
  {
    if (file)
      new (&path) path_type (f.path);
    else
      new (&text) string (f.text);
  }

  text_file& text_file::
  operator= (text_file&& f)
  {
    if (this != &f)
    {
      this->~text_file ();
      new (this) text_file (move (f)); // Assume noexcept move-construction.
    }
    return *this;
  }

  text_file& text_file::
  operator= (const text_file& f)
  {
    if (this != &f)
      *this = text_file (f); // Reduce to move-assignment.
    return *this;
  }

  // depends
  //

  dependency_constraint::
  dependency_constraint (optional<version> mnv, bool mno,
                         optional<version> mxv, bool mxo)
      : min_version (move (mnv)),
        max_version (move (mxv)),
        min_open (mno),
        max_open (mxo)
  {
    assert (
      // Min and max versions can't both be absent.
      //
      (min_version || max_version) &&

      // Version should be non-empty.
      //
      (!min_version || !min_version->empty ()) &&
      (!max_version || !max_version->empty ()) &&

      // Absent version endpoint (infinity) should be open.
      //
      (min_version || min_open) && (max_version || max_open));

    if (min_version && max_version)
    {
      if (*min_version > *max_version)
        throw invalid_argument ("min version is greater than max version");

      if (*min_version == *max_version && (min_open || max_open))
        throw invalid_argument ("equal version endpoints not closed");
    }
  }

  ostream&
  operator<< (ostream& o, const dependency_constraint& c)
  {
    assert (!c.empty ());

    if (!c.min_version)
      return o << (c.max_open ? "< " : "<= ") << *c.max_version;

    if (!c.max_version)
      return o << (c.min_open ? "> " : ">= ") << *c.min_version;

    if (*c.min_version == *c.max_version)
        return o << "== " << *c.min_version;

    return o << (c.min_open ? '(' : '[') << *c.min_version << " "
             << *c.max_version << (c.max_open ? ')' : ']');
  }

  ostream&
  operator<< (ostream& o, const dependency& d)
  {
    o << d.name;

    if (d.constraint)
      o << ' ' << *d.constraint;

    return o;
  }

  ostream&
  operator<< (ostream& o, const dependency_alternatives& as)
  {
    if (as.conditional)
      o << "? ";

    bool f (true);
    for (const dependency& a: as)
      o << (f ? (f = false, "") : " | ") << a;

    if (!as.comment.empty ())
      o << "; " << as.comment;

    return o;
  }

  // package_manifest
  //
  package_manifest::
  package_manifest (parser& p, bool iu)
      : package_manifest (p, p.next (), false, iu) // Delegate
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");
  }

  package_manifest::
  package_manifest (parser& p, name_value nv, bool iu)
      : package_manifest (p, nv, true, iu) // Delegate
  {
  }

  package_manifest::
  package_manifest (parser& p, name_value nv, bool il, bool iu)
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

        // Versions like 1.2.3- are forbidden in manifest as intended to be
        // used for version constrains rather than actual releases.
        //
        if (version.release && version.release->empty ())
          bad_name ("invalid package version release");
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

        description = text_file (move (v));
      }
      else if (n == "description-file")
      {
        if (il)
          bad_name ("package description-file not allowed");

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

        path p (v);

        if (p.absolute ())
          bad_value ("package description-file path is absolute");

        description = text_file (move (p), move (c));
      }
      else if (n == "changes")
      {
        if (v.empty ())
          bad_value ("empty package changes specification");

        changes.emplace_back (move (v));
      }
      else if (n == "changes-file")
      {
        if (il)
          bad_name ("package changes-file not allowed");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("no path in package changes-file");

        path p (v);

        if (p.absolute ())
          bad_value ("package changes-file path is absolute");

        changes.emplace_back (move (p), move (c));
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
          static const string cb ("=<>([");
          for (char c; i != e && cb.find (c = *i) == string::npos; ++i)
          {
            if (!space (c))
              ne = i + 1;
          }

          if (i == e)
            da.push_back (dependency {lv, nullopt});
          else
          {
            string nm (b, ne);

            if (nm.empty ())
              bad_value ("prerequisite package name not specified");

            // Got to version range.
            //
            dependency_constraint dc;
            const char* op (&*i);
            char mnv (*op);
            if (mnv == '(' || mnv == '[')
            {
              bool min_open (mnv == '(');

              string::size_type pos (lv.find_first_not_of (spaces, ++i - b));
              if (pos == string::npos)
                bad_value ("no prerequisite package min version specified");

              i = b + pos;
              pos = lv.find_first_of (spaces, pos);

              static const char* no_max_version (
                "no prerequisite package max version specified");

              if (pos == string::npos)
                bad_value (no_max_version);

              version_type min_version;

              try
              {
                min_version = version_type (string (i, b + pos));
              }
              catch (const invalid_argument& e)
              {
                bad_value (
                  string ("invalid prerequisite package min version: ") +
                  e.what ());
              }

              pos = lv.find_first_not_of (spaces, pos);
              if (pos == string::npos)
                bad_value (no_max_version);

              i = b + pos;
              static const string mve (spaces + "])");
              pos = lv.find_first_of (mve, pos);

              static const char* invalid_range (
                "invalid prerequisite package version range");

              if (pos == string::npos)
                bad_value (invalid_range);

              version_type max_version;

              try
              {
                max_version = version_type (string (i, b + pos));
              }
              catch (const invalid_argument& e)
              {
                bad_value (
                  string ("invalid prerequisite package max version: ") +
                  e.what ());
              }

              pos = lv.find_first_of ("])", pos); // Might be a space.
              if (pos == string::npos)
                bad_value (invalid_range);

              try
              {
                dc = dependency_constraint (move (min_version),
                                            min_open,
                                            move (max_version),
                                            lv[pos] == ')');
              }
              catch (const invalid_argument& e)
              {
                bad_value (
                  string ("invalid dependency constraint: ") + e.what ());
              }

              if (lv[pos + 1] != '\0')
                bad_value (
                  "unexpected text after prerequisite package version range");
            }
            else
            {
              // Version comparison notation.
              //
              enum comparison {eq, lt, gt, le, ge};
              comparison operation (eq); // Uninitialized warning.

              if (strncmp (op, "==", 2) == 0)
              {
                operation = eq;
                i += 2;
              }
              else if (strncmp (op, ">=", 2) == 0)
              {
                operation = ge;
                i += 2;
              }
              else if (strncmp (op, "<=", 2) == 0)
              {
                operation = le;
                i += 2;
              }
              else if (*op == '>')
              {
                operation = gt;
                ++i;
              }
              else if (*op == '<')
              {
                operation = lt;
                ++i;
              }
              else
                bad_value ("invalid prerequisite package version comparison");

              string::size_type pos (lv.find_first_not_of (spaces, i - b));

              if (pos == string::npos)
                bad_value ("no prerequisite package version specified");

              version_type v;

              try
              {
                v = version_type (lv.c_str () + pos);
              }
              catch (const invalid_argument& e)
              {
                bad_value (string ("invalid prerequisite package version: ") +
                           e.what ());
              }

              switch (operation)
              {
              case comparison::eq:
                dc = dependency_constraint (v);
                break;
              case comparison::lt:
                dc = dependency_constraint (nullopt, true, move (v), true);
                break;
              case comparison::le:
                dc = dependency_constraint (nullopt, true, move (v), false);
                break;
              case comparison::gt:
                dc = dependency_constraint (move (v), true, nullopt, true);
                break;
              case comparison::ge:
                dc = dependency_constraint (move (v), false, nullopt, true);
                break;
              }
            }

            dependency d {move (nm), move (dc)};
            da.push_back (move (d));
          }
        }

        if (da.empty ())
          bad_value ("empty package dependency specification");

        dependencies.push_back (da);
      }
      else if (n == "location")
      {
        if (!il)
          bad_name ("package location not allowed");

        if (location)
          bad_name ("package location redefinition");

        try
        {
          path l (v);

          if (l.empty ())
            bad_value ("empty package location");

          if (l.absolute ())
            bad_value ("absolute package location");

          location = move (l);
        }
        catch (const invalid_path&)
        {
          bad_value ("invalid package location");
        }
      }
      else if (n == "sha256sum")
      {
        if (!il)
          bad_name ("package sha256sum not allowed");

        if (sha256sum)
          bad_name ("package sha256sum redefinition");

        if (!valid_sha256 (v))
          bad_value ("invalid package sha256sum");

        sha256sum = move (v);
      }
      else if (!iu)
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

    if (il)
    {
      if (!location)
        bad_name ("no package location specified");

      if (!sha256sum)
        bad_name ("no package sha256sum specified");
    }
  }

  void package_manifest::
  serialize (serializer& s) const
  {
    // @@ Should we check that all non-optional values are specified ?
    // @@ Should we check that values are valid: name is not empty, version
    //    release is not empty, sha256sum is a proper string, ...?
    // @@ Currently we don't know if we are serializing the individual package
    //    manifest or the package list manifest, so can't ensure all values
    //    allowed in the current context (location, sha256sum, *-file values).
    //

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
                add_comment (
                  description->path.string (), description->comment));
      else
        s.next ("description", description->text);
    }

    for (const auto& c: changes)
    {
      if (c.file)
        s.next ("changes-file", add_comment (c.path.string (), c.comment));
      else
        s.next ("changes", c.text);
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

    if (location)
      s.next ("location", location->posix_string ());

    if (sha256sum)
      s.next ("sha256sum", *sha256sum);

    s.next ("", ""); // End of manifest.
  }

  // package_manifests
  //
  package_manifests::
  package_manifests (parser& p, bool iu)
  {
    name_value nv (p.next ());

    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      bad_name ("start of package list manifest expected");

    if (nv.value != "1")
      bad_value ("unsupported format version");

    // Parse the package list manifest.
    //
    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "sha256sum")
      {
        if (!sha256sum.empty ())
          bad_name ("sha256sum redefinition");

        if (!valid_sha256 (v))
          bad_value ("invalid sha256sum");

        sha256sum = move (v);
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in package list manifest");
    }

    // Verify all non-optional values were specified.
    //
    if (sha256sum.empty ())
      bad_value ("no sha256sum specified");

    // Parse package manifests.
    //
    for (nv = p.next (); !nv.empty (); nv = p.next ())
      push_back (package_manifest (p, nv, iu));
  }

  void package_manifests::
  serialize (serializer& s) const
  {
    // Serialize the package list manifest.
    //
    // @@ Should we check that values are valid ?
    //
    s.next ("", "1"); // Start of manifest.
    s.next ("sha256sum", sha256sum);
    s.next ("", "");  // End of manifest.

    // Serialize package manifests.
    //
    for (const package_manifest& p: *this)
    {
      auto bad_value = [&p, &s](const string& d)
        {
          throw
            serialization (
              s.name (), d + " for " + p.name + "-" + p.version.string ());
        };

      if (p.description && p.description->file)
        bad_value ("forbidden description-file");

      for (const auto& c: p.changes)
        if (c.file)
          bad_value ("forbidden changes-file");

      if (!p.location)
        bad_value ("no valid location");

      if (!p.sha256sum)
        bad_value ("no valid sha256sum");

      p.serialize (s);
    }

    s.next ("", ""); // End of stream.
  }

  // url_parts
  //
  struct url_parts
  {
    using protocol = repository_location::protocol;

    protocol proto;
    string host;
    uint16_t port;
    dir_path path;

    explicit
    url_parts (const string&);
  };

  // Return the URL protocol, or nullopt if location is not a URL.
  //
  static optional<url_parts::protocol>
  is_url (const string& location)
  {
    using protocol = url_parts::protocol;

    // Check if the location starts with the specified schema. The argument
    // must be in the lower case (we don't use str[n]casecmp() since it's not
    // portable).
    //
    auto schema = [&location](const char* s) -> bool
    {
      size_t i (0);
      for (; s[i] != '\0' && s[i] == lowercase (location[i]); ++i) ;
      return s[i] == '\0';
    };

    optional<protocol> p;
    if (schema ("http://"))
      p = protocol::http;
    else if (schema ("https://"))
      p = protocol::https;

    return p;
  }

  static string
  to_string (url_parts::protocol proto,
             const string& host,
             uint16_t port,
             const dir_path& path)
  {
    string u (
      (proto == url_parts::protocol::http ? "http://" : "https://") + host);

    if (port != 0)
      u += ":" + std::to_string (port);

    if (!path.empty ())
      u += "/" + path.posix_string ();

    return u;
  }

  url_parts::
  url_parts (const string& s)
  {
    optional<protocol> pr (is_url (s));
    if (!pr)
      throw invalid_argument ("invalid protocol");

    proto = *pr;

    string::size_type host_offset (s.find ("//"));
    assert (host_offset != string::npos);
    host_offset += 2;

    string::size_type p (s.find ('/', host_offset));

    if (p != string::npos)
      // Chop the path part. Path is saved as a relative one to be of the
      // same type on different operating systems including Windows.
      //
      path = dir_path (s, p + 1, string::npos);

    // Put the lower-cased version of the host part into host.
    // Chances are good it will stay unmodified.
    //
    transform (s.cbegin () + host_offset,
               p == string::npos ? s.cend () : s.cbegin () + p,
               back_inserter (host),
               lowercase);

    // Validate host name according to "2.3.1. Preferred name syntax" and
    // "2.3.4. Size limits" of https://tools.ietf.org/html/rfc1035.
    //
    // Check that there is no empty labels and ones containing chars
    // different from alpha-numeric and hyphen. Label should start from
    // letter, do not end with hypen and be not longer than 63 chars.
    // Total host name length should be not longer than 255 chars.
    //
    auto hb (host.cbegin ());
    auto he (host.cend ());
    auto ls (hb); // Host domain name label begin.
    auto pt (he); // Port begin.

    for (auto i (hb); i != he; ++i)
    {
      char c (*i);

      if (pt == he) // Didn't reach port specification yet.
      {
        if (c == ':') // Port specification reached.
          pt = i;
        else
        {
          auto n (i + 1);

          // Validate host name.
          //

          // Is first label char.
          //
          bool flc (i == ls);

          // Is last label char.
          //
          bool llc (n == he || *n == '.' || *n == ':');

          // Validate char.
          //
          bool valid (alpha (c) ||
                      (digit (c) && !flc) ||
                      ((c == '-' || c == '.') && !flc && !llc));

          // Validate length.
          //
          if (valid)
            valid = i - ls < 64 && i - hb < 256;

          if (!valid)
            throw invalid_argument ("invalid host");

          if (c == '.')
            ls = n;
        }
      }
      else
      {
        // Validate port.
        //
        if (!digit (c))
          throw invalid_argument ("invalid port");
      }
    }

    // Chop the port, if present.
    //
    if (pt == he)
      port = 0;
    else
    {
      unsigned long long n (++pt == he ? 0 : stoull (string (pt, he)));
      if (n == 0 || n > UINT16_MAX)
        throw invalid_argument ("invalid port");

      port = static_cast<uint16_t> (n);
      host.resize (pt - hb - 1);
    }

    if (host.empty ())
      throw invalid_argument ("invalid host");
  }

  // repository_location
  //
  static string
  strip_domain (const string& host)
  {
    assert (!host.empty ()); // Should be repository location host.

    string h;
    bool bpkg (false);

    if (host.compare (0, 4, "www.") == 0 ||
        host.compare (0, 4, "pkg.") == 0 ||
        (bpkg = host.compare (0, 5, "bpkg.") == 0))
    {
      if (h.assign (host, bpkg ? 5 : 4, string::npos).empty ())
        throw invalid_argument ("invalid host");
    }
    else
      h = host;

    return h;
  }

  // The 'pkg' path component stripping mode.
  //
  enum class strip_mode {version, component, path};

  static dir_path
  strip_path (const dir_path& path, strip_mode mode)
  {
    // Should be repository location path.
    //
    assert (!path.empty () && *path.begin () != "..");

    auto rb (path.rbegin ()), i (rb), re (path.rend ());

    // Find the version component.
    //
    for (; i != re; ++i)
    {
      const string& c (*i);

      if (!c.empty () && c.find_first_not_of ("1234567890") == string::npos)
        break;
    }

    if (i == re)
      throw invalid_argument ("missing repository version");

    // Validate the version. At the moment the only valid value is 1.
    //
    if (stoul (*i) != 1)
      throw invalid_argument ("unsupported repository version");

    dir_path res (rb, i);

    // Canonical name prefix part ends with the special "pkg" component.
    //
    bool pc (++i != re && (*i == "pkg" || *i == "bpkg"));

    if (pc && mode == strip_mode::component)
      ++i; // Strip the "pkg" component.

    if (!pc || mode != strip_mode::path)
      res = dir_path (i, re) / res; // Concatenate prefix and path parts.

    return res;
  }

  // Location parameter type is fully qualified as compiler gets confused with
  // string() member.
  //
  repository_location::
  repository_location (const std::string& l)
      : repository_location (l, repository_location ()) // Delegate.
  {
    if (!empty () && relative ())
      throw invalid_argument ("relative filesystem path");
  }

  repository_location::
  repository_location (const std::string& l, const repository_location& b)
  {
    // Otherwise compiler gets confused with string() member.
    //
    using std::string;

    if (l.empty ())
    {
      if (!b.empty ())
        throw invalid_argument ("empty location");

      return;
    }

    // Base repository location can not be a relative path.
    //
    if (!b.empty () && b.relative ())
      throw invalid_argument ("base relative filesystem path");

    if (is_url (l))
    {
      url_parts u (l);
      proto_ = u.proto;
      host_ = move (u.host);
      port_ = u.port;
      path_ = move (u.path);

      canonical_name_ = strip_domain (host_);

      // For canonical name and for the HTTP protocol, treat a.com and
      // a.com:80 as the same name. The same rule applies to the HTTPS
      // protocol and port 443.
      //
      if (port_ != 0 && port_ != (proto_ == protocol::http ? 80 : 443))
        canonical_name_ += ':' + std::to_string (port_);
    }
    else
    {
      path_ = dir_path (l);

      // Complete if we are relative and have base.
      //
      if (!b.empty () && path_.relative ())
      {
        // Convert the relative path location to an absolute or remote one.
        //
        proto_ = b.proto_;
        host_ = b.host_;
        port_ = b.port_;
        path_ = b.path_ / path_;

        // Set canonical name to the base location canonical name host
        // part. The path part of the canonical name is calculated below.
        //
        if (b.remote ())
          canonical_name_ =
            b.canonical_name_.substr (0, b.canonical_name_.find ("/"));
      }
    }

    // Normalize path to avoid different representations of the same location
    // and canonical name. So a/b/../c/1/x/../y and a/c/1/y to be considered
    // as same location.
    //
    try
    {
      path_.normalize ();
    }
    catch (const invalid_path&)
    {
      throw invalid_argument ("invalid path");
    }

    // Need to check path for emptiness before proceeding further as a valid
    // non empty location can not have an empty path_ member. Note that path
    // can become empty as a result of normalize () call. Example of such a
    // path is 'a/..'.
    //
    if (path_.empty ())
      throw invalid_argument ("empty path");

    // Need to check that URL path do not go past the root directory of a WEB
    // server. We can not rely on the above normalize() function call doing
    // this check as soon as path_ member contains a relative directory for the
    // remote location.
    //
    if (remote () && *path_.begin () == "..")
      throw invalid_argument ("invalid path");

    // Finish calculating the canonical name, unless we are relative.
    //
    if (relative ())
    {
      assert (canonical_name_.empty ());
      return;
    }

    // Canonical name <prefix>/<path> part.
    //
     dir_path sp (strip_path (
       path_, remote () ? strip_mode::component : strip_mode::path));

     string cp (sp.relative () ? sp.posix_string () : sp.string ());

    // Note: allow empty paths (e.g., http://stable.cppget.org/1/).
    //
    if (!canonical_name_.empty () && !cp.empty ()) // If we have host and dir.
      canonical_name_ += '/';

    canonical_name_ += cp;

    // But don't allow empty canonical names.
    //
    if (canonical_name_.empty ())
      throw invalid_argument ("empty repository name");
  }

  string repository_location::
  string () const
  {
    if (empty ())
      return std::string (); // Also function name.

    if (local ())
      return relative () ? path_.posix_string () : path_.string ();

    return to_string (proto_, host_, port_, path_);
  }

  // repository_manifest
  //
  repository_manifest::
  repository_manifest (parser& p, bool iu)
      : repository_manifest (p, p.next (), iu) // Delegate
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single repository manifest expected");
  }

  repository_manifest::
  repository_manifest (parser& p, name_value nv, bool iu)
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
      {
        if (!location.empty ())
          bad_name ("location redefinition");

        if (v.empty ())
          bad_value ("empty location");

        try
        {
          // Call prerequisite repository location constructor, do not
          // ammend relative path.
          //
          location = repository_location (move (v), repository_location ());
        }
        catch (const invalid_argument& e)
        {
          bad_value (e.what ());
        }
      }
      else if (n == "role")
      {
        if (role)
          bad_name ("role redefinition");

        auto b (repository_role_names.cbegin ());
        auto e (repository_role_names.cend ());
        auto i (find (b, e, v));

        if (i == e)
          bad_value ("unrecognized role");

        role = static_cast<repository_role> (i - b);
      }
      else if (n == "url")
      {
        if (url)
          bad_name ("url redefinition");

        if (v.empty ())
          bad_value ("empty url");

        url = move (v);
      }
      else if (n == "email")
      {
        if (email)
          bad_name ("email redefinition");

        string c (split_comment (v));

        if (v.empty ())
          bad_value ("empty email");

        email = email_type (move (v), move (c));
      }
      else if (n == "summary")
      {
        if (summary)
          bad_name ("summary redefinition");

        if (v.empty ())
          bad_value ("empty summary");

        summary = move (v);
      }
      else if (n == "description")
      {
        if (description)
          bad_name ("description redefinition");

        if (v.empty ())
          bad_value ("empty description");

        description = move (v);
      }
      else if (n == "certificate")
      {
        if (certificate)
          bad_name ("certificate redefinition");

        if (v.empty ())
          bad_value ("empty certificate");

        certificate = move (v);
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in repository manifest");
    }

    // Verify all non-optional values were specified.
    //
    // - location can be omitted
    // - role can be omitted
    //
    if (role && location.empty () != (*role == repository_role::base))
      bad_value ("invalid role");

    if (effective_role () != repository_role::base)
    {
      if (url)
        bad_value ("url not allowed");

      if (email)
        bad_value ("email not allowed");

      if (summary)
        bad_value ("summary not allowed");

      if (description)
        bad_value ("description not allowed");

      if (certificate)
        bad_value ("certificate not allowed");
    }
  }

  void repository_manifest::
  serialize (serializer& s) const
  {
    auto bad_value ([&s](const string& d) {
        throw serialization (s.name (), d);});

    s.next ("", "1"); // Start of manifest.

    if (!location.empty ())
      s.next ("location", location.string ());

    if (role)
    {
      if (location.empty () != (*role == repository_role::base))
        bad_value ("invalid role");

      auto r (static_cast<size_t> (*role));
      assert (r < repository_role_names.size ());
      s.next ("role", repository_role_names[r]);
    }

    bool b (effective_role () == repository_role::base);

    if (url)
    {
      if (!b)
        bad_value ("url not allowed");

      s.next ("url", *url);
    }

    if (email)
    {
      if (!b)
        bad_value ("email not allowed");

      s.next ("email", add_comment (*email, email->comment));
    }

    if (summary)
    {
      if (!b)
        bad_value ("summary not allowed");

      s.next ("summary", *summary);
    }

    if (description)
    {
      if (!b)
        bad_value ("description not allowed");

      s.next ("description", *description);
    }

    if (certificate)
    {
      if (!b)
        bad_value ("certificate not allowed");

      s.next ("certificate", *certificate);
    }

    s.next ("", ""); // End of manifest.
  }

  repository_role repository_manifest::
  effective_role () const
  {
    if (role)
    {
      if (location.empty () != (*role == repository_role::base))
        throw logic_error ("invalid role");

      return *role;
    }
    else
      return location.empty ()
        ? repository_role::base
        : repository_role::prerequisite;
  }

  optional<string> repository_manifest::
  effective_url (const repository_location& l) const
  {
    if (!url || (*url)[0] != '.')
      return url;

    const dir_path rp (*url);
    auto i (rp.begin ());

    static const char* invalid_url ("invalid relative url");

    auto strip ([&i, &rp]() -> bool {
        if (i != rp.end ())
        {
          const auto& c (*i++);
          if (c == "..")
            return true;

          if (c == ".")
            return false;
        }

        throw invalid_argument (invalid_url);
      });

    bool strip_d (strip ()); // Strip domain.
    bool strip_p (strip ()); // Strip path.

    // The web interface relative path with the special first two components
    // stripped.
    //
    const dir_path rpath (i, rp.end ());
    assert (rpath.relative ());

    url_parts u (l.string ());

    // Web interface URL path part.
    //
    // It is important to call strip_path() before appending the relative
    // path. Otherwise the effective URL for the path ./../../.. and the
    // repository location http://a.com/foo/pkg/1/math will wrongly be
    // http://a.com/foo/pkg instead of http://a.com.
    //
    dir_path ipath (
      strip_path (
        u.path, strip_p ? strip_mode::component : strip_mode::version) / rpath);

    static const char* invalid_location ("invalid repository location");

    try
    {
      ipath.normalize ();
    }
    catch (const invalid_path&)
    {
      throw invalid_argument (invalid_location);
    }

    assert (ipath.relative ());

    if (!ipath.empty () && *ipath.begin () == "..")
      throw invalid_argument (invalid_location);

    return to_string (
      u.proto, strip_d ? strip_domain (u.host) : u.host, u.port, ipath);
  }

  // repository_manifests
  //
  repository_manifests::
  repository_manifests (parser& p, bool iu)
  {
    name_value nv (p.next ());
    while (!nv.empty ())
    {
      push_back (repository_manifest (p, nv, iu));
      nv = p.next ();

      // Make sure there is location in all except the last entry.
      //
      if (back ().location.empty () && !nv.empty ())
        throw parsing (p.name (), nv.name_line, nv.name_column,
                       "repository location expected");
    }

    if (empty () || !back ().location.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "base repository manifest expected");
  }

  void repository_manifests::
  serialize (serializer& s) const
  {
    if (empty () || !back ().location.empty ())
      throw serialization (s.name (), "base repository manifest expected");

    // @@ Should we check that there is location in all except the last
    // entry?
    //
    for (const repository_manifest& r: *this)
      r.serialize (s);

    s.next ("", ""); // End of stream.
  }

  // signature_manifest
  //
  signature_manifest::
  signature_manifest (parser& p, bool iu)
      : signature_manifest (p, p.next (), iu) // Delegate
  {
    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single signature manifest expected");
  }

  signature_manifest::
  signature_manifest (parser& p, name_value nv, bool iu)
  {
    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      bad_name ("start of signature manifest expected");

    if (nv.value != "1")
      bad_value ("unsupported format version");

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "sha256sum")
      {
        if (!sha256sum.empty ())
          bad_name ("sha256sum redefinition");

        if (v.empty ())
          bad_value ("empty sha256sum");

        if (!valid_sha256 (v))
          bad_value ("invalid sha256sum");

        sha256sum = move (v);
      }
      else if (n == "signature")
      {
        if (!signature.empty ())
          bad_name ("signature redefinition");

        if (v.empty ())
          bad_value ("empty signature");

        // Try to base64-decode as a sanity check.
        //
        try
        {
          signature = base64_decode (v);
        }
        catch (const invalid_argument&)
        {
          bad_value ("invalid signature");
        }
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in signature manifest");
    }

    // Verify all non-optional values were specified.
    //
    if (sha256sum.empty ())
      bad_value ("no sha256sum specified");
    else if (signature.empty ())
      bad_value ("no signature specified");
  }

  void signature_manifest::
  serialize (serializer& s) const
  {
    // @@ Should we check that values are valid ?
    //
    s.next ("", "1"); // Start of manifest.

    s.next ("sha256sum", sha256sum);
    s.next ("signature", base64_encode (signature));

    s.next ("", ""); // End of manifest.
  }
}
