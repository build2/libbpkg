// file      : libbpkg/manifest.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbpkg/manifest.hxx>

#include <string>
#include <ostream>
#include <sstream>
#include <cassert>
#include <cstring>   // strncmp(), strcmp()
#include <utility>   // move()
#include <cstdint>   // uint*_t, UINT16_MAX
#include <algorithm> // find(), find_if_not(), find_first_of(), replace()
#include <stdexcept> // invalid_argument

#include <libbutl/path.mxx>
#include <libbutl/base64.mxx>
#include <libbutl/utility.mxx>             // casecmp(), lcase(), alpha(),
                                           // digit()
#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  using parser = manifest_parser;
  using parsing = manifest_parsing;
  using serializer = manifest_serializer;
  using serialization = manifest_serialization;
  using name_value = manifest_name_value;

  using butl::optional;
  using butl::nullopt;

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
  version (uint16_t e,
           std::string u,
           optional<std::string> l,
           uint16_t r,
           uint32_t i)
      : epoch (e),
        upstream (move (u)),
        release (move (l)),
        revision (r),
        iteration (i),
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

      if (iteration != 0)
        throw invalid_argument ("iteration for empty version");
    }
    // Empty release signifies the earliest possible release. Revision and/or
    // iteration are meaningless in such a context.
    //
    else if (release && release->empty () && (revision != 0 || iteration != 0))
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

      bool zo (false); // Digit zero-only component.
      if (numeric)
      {
        size_t n (end - begin);

        if (n > 16)
          throw invalid_argument ("16 digits maximum allowed in a component");

        append (16 - n, '0'); // Add padding zeros.
        append (begin, n);

        // See if all zero.
        //
        for (; begin != end && *begin == '0'; ++begin) ;
        zo = (begin == end);
      }
      else
        append (lcase (begin, end - begin));

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

    auto bad_arg = [](const string& d) {throw invalid_argument (d);};

    auto uint16 = [&bad_arg](const string& s, const char* what) -> uint16_t
    {
      try
      {
        uint64_t v (stoull (s));

        if (v <= UINT16_MAX) // From <cstdint>.
          return static_cast<uint16_t> (v);
      }
      catch (const std::exception&)
      {
        // Fall through.
      }

      bad_arg (string (what) + " should be 2-byte unsigned integer");

      assert (false); // Can't be here.
      return 0;
    };

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
  string (bool ignore_revision, bool ignore_iteration) const
  {
    using std::to_string; // Hidden by to_string (repository_type).

    if (empty ())
      throw logic_error ("empty version");

    std::string v (epoch != 0 ? to_string (epoch) + "~" + upstream : upstream);

    if (release)
    {
      v += '-';
      v += *release;
    }

    if (!ignore_revision)
    {
      if (revision != 0)
      {
        v += '+';
        v += to_string (revision);
      }

      if (!ignore_iteration && iteration != 0)
      {
        v += '#';
        v += to_string (iteration);
      }
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
      o << '?';

    if (as.buildtime)
      o << '*';

    if (as.conditional || as.buildtime)
      o << ' ';

    bool f (true);
    for (const dependency& a: as)
      o << (f ? (f = false, "") : " | ") << a;

    if (!as.comment.empty ())
      o << "; " << as.comment;

    return o;
  }

  // pkg_package_manifest
  //
  static void
  parse_package_manifest (parser& p,
                          name_value nv,
                          bool il,
                          bool iu,
                          package_manifest& m)
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

    auto add_build_constraint = [&bad_value, &m] (bool e, const string& vc)
    {
      auto vcp (parser::split_comment (vc));
      string v (move (vcp.first));
      string c (move (vcp.second));

      size_t p (v.find ('/'));
      string nm (p != string::npos ? v.substr (0, p) : move (v));
      optional<string> tg (p != string::npos
                           ? optional<string> (string (v, p + 1))
                           : nullopt);

      if (nm.empty ())
        bad_value ("empty build configuration name pattern");

      if (tg && tg->empty ())
        bad_value ("empty build target pattern");

      m.build_constraints.emplace_back (e, move (nm), move (tg), move (c));
    };

    auto parse_url = [&bad_value] (const string& v, const char* what) -> url
    {
      auto p (parser::split_comment (v));

      if (v.empty ())
        bad_value (string ("empty ") + what + " url");

      return url (move (p.first), move (p.second));
    };

    auto parse_email = [&bad_value] (const string& v,
                                     const char* what,
                                     bool empty = false) -> email
    {
      auto p (parser::split_comment (v));

      if (v.empty () && !empty)
        bad_value (string ("empty ") + what + " email");

      return email (move (p.first), move (p.second));
    };

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "name")
      {
        if (!m.name.empty ())
          bad_name ("package name redefinition");

        if (v.empty ())
          bad_value ("empty package name");

        m.name = move (v);
      }
      else if (n == "version")
      {
        if (!m.version.empty ())
          bad_name ("package version redefinition");

        try
        {
          m.version = version (move (v));
        }
        catch (const invalid_argument& e)
        {
          bad_value (string ("invalid package version: ") + e.what ());
        }

        // Versions like 1.2.3- are forbidden in manifest as intended to be
        // used for version constrains rather than actual releases.
        //
        if (m.version.release && m.version.release->empty ())
          bad_value ("invalid package version release");
      }
      else if (n == "summary")
      {
        if (!m.summary.empty ())
          bad_name ("package summary redefinition");

        if (v.empty ())
          bad_value ("empty package summary");

        m.summary = move (v);
      }
      else if (n == "tags")
      {
        if (!m.tags.empty ())
          bad_name ("package tags redefinition");

        list_parser lp (v.begin (), v.end ());
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
        {
          if (lv.find_first_of (spaces) != string::npos)
            bad_value ("only single-word tags allowed");

          m.tags.push_back (move (lv));
        }

        if (m.tags.empty ())
          bad_value ("empty package tags specification");
      }
      else if (n == "description")
      {
        if (m.description)
        {
          if (m.description->file)
            bad_name ("package description and description-file are "
                      "mutually exclusive");
          else
            bad_name ("package description redefinition");
        }

        if (v.empty ())
          bad_value ("empty package description");

        m.description = text_file (move (v));
      }
      else if (n == "description-file")
      {
        if (il)
          bad_name ("package description-file not allowed");

        if (m.description)
        {
          if (m.description->file)
            bad_name ("package description-file redefinition");
          else
            bad_name ("package description-file and description are "
                      "mutually exclusive");
        }

        auto vc (parser::split_comment (v));
        path p (move (vc.first));

        if (p.empty ())
          bad_value ("no path in package description-file");

        if (p.absolute ())
          bad_value ("package description-file path is absolute");

        m.description = text_file (move (p), move (vc.second));
      }
      else if (n == "changes")
      {
        if (v.empty ())
          bad_value ("empty package changes specification");

        m.changes.emplace_back (move (v));
      }
      else if (n == "changes-file")
      {
        if (il)
          bad_name ("package changes-file not allowed");

        auto vc (parser::split_comment (v));
        path p (move (vc.first));

        if (p.empty ())
          bad_value ("no path in package changes-file");

        if (p.absolute ())
          bad_value ("package changes-file path is absolute");

        m.changes.emplace_back (move (p), move (vc.second));
      }
      else if (n == "url")
      {
        if (!m.url.empty ())
          bad_name ("project url redefinition");

        m.url = parse_url (v, "project");
      }
      else if (n == "email")
      {
        if (!m.email.empty ())
          bad_name ("project email redefinition");

        m.email = parse_email (v, "project");
      }
      else if (n == "doc-url")
      {
        if (m.doc_url)
          bad_name ("doc url redefinition");

        m.doc_url = parse_url (v, "doc");
      }
      else if (n == "src-url")
      {
        if (m.src_url)
          bad_name ("src url redefinition");

        m.src_url = parse_url (v, "src");
      }
      else if (n == "package-url")
      {
        if (m.package_url)
          bad_name ("package url redefinition");

        m.package_url = parse_url (v, "package");
      }
      else if (n == "package-email")
      {
        if (m.package_email)
          bad_name ("package email redefinition");

        m.package_email = parse_email (v, "package");
      }
      else if (n == "build-email")
      {
        if (m.build_email)
          bad_name ("build email redefinition");

        m.build_email = parse_email (v, "build", true);
      }
      else if (n == "priority")
      {
        if (m.priority)
          bad_name ("package priority redefinition");

        auto vc (parser::split_comment (v));
        strings::const_iterator b (priority_names.begin ());
        strings::const_iterator e (priority_names.end ());
        strings::const_iterator i (find (b, e, vc.first));

        if (i == e)
          bad_value ("invalid package priority");

        m.priority =
          priority (static_cast<priority::value_type> (i - b),
                    move (vc.second));
      }
      else if (n == "license")
      {
        auto vc (parser::split_comment (v));
        licenses l (move (vc.second));

        list_parser lp (vc.first.begin (), vc.first.end ());
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
          l.push_back (move (lv));

        if (l.empty ())
          bad_value ("empty package license specification");

        m.license_alternatives.push_back (move (l));
      }
      else if (n == "requires")
      {
        // Allow specifying ?* in any order.
        //
        size_t n (v.size ());
        size_t cond ((n > 0 && v[0] == '?') || (n > 1 && v[1] == '?') ? 1 : 0);
        size_t btim ((n > 0 && v[0] == '*') || (n > 1 && v[1] == '*') ? 1 : 0);

        auto vc (parser::split_comment (v));

        const string& vl (vc.first);
        requirement_alternatives ra (cond != 0, btim != 0, move (vc.second));

        string::const_iterator b (vl.begin ());
        string::const_iterator e (vl.end ());

        if (ra.conditional || ra.buildtime)
        {
          string::size_type p (vl.find_first_not_of (spaces, cond + btim));
          b = p == string::npos ? e : b + p;
        }

        list_parser lp (b, e, '|');
        for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
          ra.push_back (lv);

        if (ra.empty () && ra.comment.empty ())
          bad_value ("empty package requirement specification");

        m.requirements.push_back (move (ra));
      }
      else if (n == "build-include")
      {
        add_build_constraint (false, v);
      }
      else if (n == "build-exclude")
      {
        add_build_constraint (true, v);
      }
      else if (n == "depends")
      {
        // Allow specifying ?* in any order.
        //
        size_t n (v.size ());
        size_t cond ((n > 0 && v[0] == '?') || (n > 1 && v[1] == '?') ? 1 : 0);
        size_t btim ((n > 0 && v[0] == '*') || (n > 1 && v[1] == '*') ? 1 : 0);

        auto vc (parser::split_comment (v));

        const string& vl (vc.first);
        dependency_alternatives da (cond != 0, btim != 0, move (vc.second));

        string::const_iterator b (vl.begin ());
        string::const_iterator e (vl.end ());

        if (da.conditional || da.buildtime)
        {
          string::size_type p (vl.find_first_not_of (spaces, cond + btim));
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

              version min_version;

              try
              {
                min_version = version (string (i, b + pos));
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

              version max_version;

              try
              {
                max_version = version (string (i, b + pos));
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

              version v;

              try
              {
                v = version (lv.c_str () + pos);
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

        m.dependencies.push_back (da);
      }
      else if (n == "location")
      {
        if (!il)
          bad_name ("package location not allowed");

        if (m.location)
          bad_name ("package location redefinition");

        try
        {
          path l (v);

          if (l.empty ())
            bad_value ("empty package location");

          if (l.absolute ())
            bad_value ("absolute package location");

          m.location = move (l);
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

        if (m.sha256sum)
          bad_name ("package sha256sum redefinition");

        if (!valid_sha256 (v))
          bad_value ("invalid package sha256sum");

        m.sha256sum = move (v);
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in package manifest");
    }

    // Verify all non-optional values were specified.
    //
    if (m.name.empty ())
      bad_value ("no package name specified");
    else if (m.version.empty ())
      bad_value ("no package version specified");
    else if (m.summary.empty ())
      bad_value ("no package summary specified");
    else if (m.url.empty ())
      bad_value ("no project url specified");
    else if (m.email.empty ())
      bad_value ("no project email specified");
    else if (m.license_alternatives.empty ())
      bad_value ("no project license specified");

    if (il)
    {
      if (!m.location)
        bad_name ("no package location specified");

      if (!m.sha256sum)
        bad_name ("no package sha256sum specified");
    }
  }

  package_manifest
  pkg_package_manifest (parser& p, name_value nv, bool iu)
  {
    package_manifest r;
    parse_package_manifest (p, nv, true, iu, r);
    return r;
  }

  // package_manifest
  //
  package_manifest::
  package_manifest (manifest_parser& p, bool iu)
  {
    parse_package_manifest (p, p.next (), false, iu, *this);

    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");
  }

  void package_manifest::
  serialize (serializer& s) const
  {
    // @@ Should we check that all non-optional values are specified ?
    // @@ Should we check that values are valid: name is not empty, version
    //    release is not empty, sha256sum is a proper string, ...?
    // @@ Currently we don't know if we are serializing the individual package
    //    manifest or the package list manifest, so can't ensure all values
    //    allowed in the current context (*-file values).
    //

    s.next ("", "1"); // Start of manifest.
    s.next ("name", name);
    s.next ("version", version.string ());

    if (priority)
    {
      size_t v (*priority);
      assert (v < priority_names.size ());

      s.next ("priority",
              serializer::merge_comment (priority_names[v],
                                         priority->comment));
    }

    s.next ("summary", summary);

    for (const auto& la: license_alternatives)
      s.next ("license",
              serializer::merge_comment (concatenate (la), la.comment));

    if (!tags.empty ())
      s.next ("tags", concatenate (tags));

    if (description)
    {
      if (description->file)
        s.next ("description-file",
                serializer::merge_comment (description->path.string (),
                                           description->comment));
      else
        s.next ("description", description->text);
    }

    for (const auto& c: changes)
    {
      if (c.file)
        s.next ("changes-file",
                serializer::merge_comment (c.path.string (), c.comment));
      else
        s.next ("changes", c.text);
    }

    s.next ("url", serializer::merge_comment (url, url.comment));
    if (doc_url)
      s.next ("doc-url",
              serializer::merge_comment (*doc_url, doc_url->comment));

    if (src_url)
      s.next ("src-url",
              serializer::merge_comment (*src_url, src_url->comment));

    if (package_url)
      s.next ("package-url",
              serializer::merge_comment (*package_url,
                                         package_url->comment));

    s.next ("email", serializer::merge_comment (email, email.comment));

    if (package_email)
      s.next ("package-email",
              serializer::merge_comment (*package_email,
                                         package_email->comment));

    if (build_email)
      s.next ("build-email",
              serializer::merge_comment (*build_email,
                                         build_email->comment));

    for (const auto& d: dependencies)
      s.next ("depends",
              (d.conditional
               ? (d.buildtime ? "?* " : "? ")
               : (d.buildtime ? "* " : "")) +
              serializer::merge_comment (concatenate (d, " | "), d.comment));

    for (const auto& r: requirements)
      s.next ("requires",
              (r.conditional
               ? (r.buildtime ? "?* " : "? ")
               : (r.buildtime ? "* " : "")) +
              serializer::merge_comment (concatenate (r, " | "), r.comment));

    for (const auto& c: build_constraints)
      s.next (c.exclusion ? "build-exclude" : "build-include",
              serializer::merge_comment (!c.target
                                         ? c.config
                                         : c.config + "/" + *c.target,
                                         c.comment));

    if (location)
      s.next ("location", location->posix_string ());

    if (sha256sum)
      s.next ("sha256sum", *sha256sum);

    s.next ("", ""); // End of manifest.
  }

  // Parse the directory manifest that may contain the only (and required)
  // location name that refers to the package directory.
  //
  static package_manifest
  parse_directory_manifest (parser& p, name_value nv, bool iu)
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

    package_manifest r;

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "location")
      {
        if (r.location)
          bad_name ("package location redefinition");

        try
        {
          path l (v);

          if (l.empty ())
            bad_value ("empty package location");

          if (l.absolute ())
            bad_value ("absolute package location");

          // Make sure that the path is a directory (contains the trailing
          // slash).
          //
          if (!l.to_directory ())
            l = path_cast<dir_path> (move (l));

          r.location = move (l);
        }
        catch (const invalid_path&)
        {
          bad_value ("invalid package location");
        }
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in package manifest");
    }

    if (!r.location)
      bad_name ("no package location specified");

    return r;
  }

  static package_manifest
  parse_directory_manifest (parser& p, bool iu)
  {
    package_manifest r (parse_directory_manifest (p, p.next (), iu));

    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");

    return r;
  }

  // Serialize the directory manifest (see above).
  //
  static void
  serialize_directory_manifest (serializer& s, const package_manifest& m)
  {
    s.next ("", "1"); // Start of manifest.

    auto bad_value ([&s](const string& d) {
        throw serialization (s.name (), d);});

    if (!m.location)
      bad_value ("no valid location");

    s.next ("location", m.location->posix_representation ());

    s.next ("", ""); // End of manifest.
  }

  // dir_package_manifest
  //
  package_manifest
  dir_package_manifest (parser& p, name_value nv, bool iu)
  {
    return parse_directory_manifest (p, nv, iu);
  }

  package_manifest
  dir_package_manifest (parser& p, bool iu)
  {
    return parse_directory_manifest (p, iu);
  }

  void
  dir_package_manifest (serializer& s, const package_manifest& m)
  {
    serialize_directory_manifest (s, m);
  }

  // git_package_manifest
  //
  package_manifest
  git_package_manifest (parser& p, name_value nv, bool iu)
  {
    return parse_directory_manifest (p, nv, iu);
  }

  package_manifest
  git_package_manifest (parser& p, bool iu)
  {
    return parse_directory_manifest (p, iu);
  }

  void
  git_package_manifest (serializer& s, const package_manifest& m)
  {
    serialize_directory_manifest (s, m);
  }

  // pkg_package_manifests
  //
  pkg_package_manifests::
  pkg_package_manifests (parser& p, bool iu)
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
      push_back (pkg_package_manifest (p, move (nv), iu));
  }

  void pkg_package_manifests::
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
        throw serialization (
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

      pkg_package_manifest (s, p);
    }

    s.next ("", ""); // End of stream.
  }

  // Parse package directory manifests.
  //
  static void
  parse_directory_manifests (parser& p, bool iu, vector<package_manifest>& ms)
  {
    // Normally, such manifests are created manually, so let's check for
    // duplicates.
    //
    for (name_value nv (p.next ()); !nv.empty (); )
    {
      package_manifest pm (dir_package_manifest (p, move (nv), iu));
      nv = p.next ();

      for (const auto& m: ms)
      {
        if (m.location == pm.location)
          throw parsing (p.name (),
                         nv.name_line, nv.name_column,
                         "duplicate package manifest");
      }

      ms.push_back (move (pm));
    }
  }

  // Serialize package directory manifests.
  //
  static void
  serialize_directory_manifests (serializer& s,
                                 const vector<package_manifest>& ms)
  {
    for (const package_manifest& m: ms)
      serialize_directory_manifest (s, m);

    s.next ("", ""); // End of stream.
  }

  // dir_package_manifests
  //
  dir_package_manifests::
  dir_package_manifests (parser& p, bool iu)
  {
    parse_directory_manifests (p, iu, *this);
  }

  void dir_package_manifests::
  serialize (serializer& s) const
  {
    serialize_directory_manifests (s, *this);
  }

  // git_package_manifests
  //
  git_package_manifests::
  git_package_manifests (parser& p, bool iu)
  {
    parse_directory_manifests (p, iu, *this);
  }

  void git_package_manifests::
  serialize (serializer& s) const
  {
    serialize_directory_manifests (s, *this);
  }

  // repository_url_traits
  //
  repository_url_traits::scheme_type repository_url_traits::
  translate_scheme (const string_type&         url,
                    string_type&&              scheme,
                    optional<authority_type>&  authority,
                    optional<path_type>&       path,
                    optional<string_type>&     query,
                    optional<string_type>&     fragment)
  {
    auto bad_url = [] (const char* d = "invalid URL")
    {
      throw invalid_argument (d);
    };

    auto translate_remote = [&authority, &path, &bad_url] ()
    {
      if (!authority || authority->host.empty ())
        bad_url ("invalid host");

      if (authority->host.kind != url_host_kind::name)
        bad_url ("unsupported host type");

      // Normalize the host name.
      //
      lcase (authority->host.value);

      // We don't distinguish between the absent and empty paths for the
      // remote repository URLs.
      //
      if (!path)
        path = path_type ();

      if (path->absolute ())
        bad_url ("absolute path");

      try
      {
        path->normalize (false /* actual */, true /* cur_empty */);
      }
      catch (const invalid_path&)
      {
        assert (false); // Can't be here as the path is relative.
      }

      // URL shouldn't go past the root directory of a WEB server.
      //
      if (!path->empty () && *path->begin () == "..")
        bad_url ("invalid path");
    };

    if (casecmp (scheme, "http") == 0)
    {
      translate_remote ();
      return scheme_type::http;
    }
    else if (casecmp (scheme, "https") == 0)
    {
      translate_remote ();
      return scheme_type::https;
    }
    else if (casecmp (scheme, "git") == 0)
    {
      translate_remote ();
      return scheme_type::git;
    }
    else if (casecmp (scheme, "file") == 0)
    {
      if (authority)
      {
        if (!authority->empty () &&
            (casecmp (authority->host, "localhost") != 0 ||
             authority->port != 0 ||
             !authority->user.empty ()))
          throw invalid_argument ("invalid authority");

        // We don't distinguish between the absent, empty and localhost
        // authorities for the local repository locations.
        //
        authority = nullopt;
      }

      if (!path)
        bad_url ("absent path");

      // On POSIX make the relative (to the authority "root") path absolute. On
      // Windows it must already be an absolute path.
      //
#ifndef _WIN32
      if (path->absolute ())
        bad_url ("absolute path");

      path = path_type ("/") / *path;
#else
      if (path->relative ())
        bad_url ("relative path");
#endif

      assert (path->absolute ());

      try
      {
        path->normalize ();
      }
      catch (const invalid_path&)
      {
        bad_url ("invalid path"); // Goes past the root directory.
      }

      if (query)
        bad_url ();

      return scheme_type::file;
    }
    // Consider URL as a path if the URL parsing failed.
    //
    else if (scheme.empty ())
    {
      try
      {
        size_t p (url.find ('#'));

        if (p != string::npos)
        {
          path = path_type (url.substr (0, p)).normalize ();
          fragment = url.substr (p + 1); // Note: set after path normalization.
        }
        else
          path = path_type (url).normalize ();
      }
      catch (const invalid_path&)
      {
        // If this is not a valid path either, then let's consider the argument
        // a broken URL, and leave the basic_url ctor to throw.
        //
      }

      return scheme_type::file;
    }
    else
      throw invalid_argument ("unknown scheme");
  }

  repository_url_traits::string_type repository_url_traits::
  translate_scheme (string_type&                     url,
                    const scheme_type&               scheme,
                    const optional<authority_type>&  authority,
                    const optional<path_type>&       path,
                    const optional<string_type>&     /*query*/,
                    const optional<string_type>&     fragment)
  {
    switch (scheme)
    {
    case scheme_type::http:  return "http";
    case scheme_type::https: return "https";
    case scheme_type::git:   return "git";
    case scheme_type::file:
      {
        assert (path);

        if (path->absolute () && (fragment || authority))
          return "file";

        url = path->relative () ? path->posix_string () : path->string ();

        if (fragment)
        {
          assert (path->relative ());

          url += '#';
          url += *fragment;
        }

        return string_type ();
      }
    }

    assert (false); // Can't be here.
    return "";
  }

  repository_url_traits::path_type repository_url_traits::
  translate_path (string_type&& path)
  {
    try
    {
      return path_type (move (path));
    }
    catch (const invalid_path&)
    {
      throw invalid_argument ("invalid url");
    }
  }

   repository_url_traits::string_type repository_url_traits::
   translate_path (const path_type& path)
   {
     // If the path is absolute then this is a local URL object and the file://
     // URL notation is being produced. Thus, on POSIX we need to make the path
     // relative (to the authority "root"). On Windows the path should stay
     // absolute but the directory separators must be converted to the POSIX
     // ones.
     //
     if (path.absolute ())
     {
#ifndef _WIN32
       return path.leaf (dir_path ("/")).string ();
#else
       string r (path.string ());
       replace (r.begin (), r.end (), '\\', '/');
       return r;
#endif
     }

     return path.posix_string ();
   }

  // repository_type
  //
  string
  to_string (repository_type t)
  {
    switch (t)
    {
    case repository_type::pkg: return "pkg";
    case repository_type::dir: return "dir";
    case repository_type::git: return "git";
    }

    assert (false); // Can't be here.
    return string ();
  }

  repository_type
  to_repository_type (const string& t)
  {
         if (t == "pkg") return repository_type::pkg;
    else if (t == "dir") return repository_type::dir;
    else if (t == "git") return repository_type::git;
    else throw invalid_argument ("invalid repository type '" + t + "'");
  }

  repository_type
  guess_type (const repository_url& url, bool local)
  {
    switch (url.scheme)
    {
    case repository_protocol::git:
      {
        return repository_type::git;
      }
    case repository_protocol::http:
    case repository_protocol::https:
      {
        return url.path->extension () == "git"
          ? repository_type::git
          : repository_type::pkg;
      }
    case repository_protocol::file:
      {
        return local &&
          dir_exists (path_cast<dir_path> (*url.path) / dir_path (".git"),
                      false)
          ? repository_type::git
          : repository_type::pkg;
      }
    }

    assert (false); // Can't be here.
    return repository_type::pkg;
  }

  // repository_location
  //
  static string
  strip_domain (const string& host, repository_type type)
  {
    assert (!host.empty ()); // Should be repository location host.

    optional<string> h;

    switch (type)
    {
    case repository_type::pkg:
      {
        bool bpkg (false);
        if (host.compare (0, 4, "www.") == 0 ||
            host.compare (0, 4, "pkg.") == 0 ||
            (bpkg = host.compare (0, 5, "bpkg.") == 0))
          h = string (host, bpkg ? 5 : 4);

        break;
      }
    case repository_type::git:
      {
        if (host.compare (0, 4, "www.") == 0 ||
            host.compare (0, 4, "git.") == 0 ||
            host.compare (0, 4, "scm.") == 0)
          h = string (host, 4);

        break;
      }
    case repository_type::dir:
      {
        // Can't be here as repository location for the dir type can only be
        // local.
        //
        assert (false);
        break;
      }
    }

    if (h && h->empty ())
      throw invalid_argument ("invalid host");

    return h ? *h : host;
  }

  // The 'pkg' path component and '.git' extension stripping mode.
  //
  enum class strip_mode {version, component, path, extension};

  static path
  strip_path (const path& p, strip_mode mode)
  {
    if (mode == strip_mode::extension)
    {
      const char* e (p.extension_cstring ());
      return e != nullptr && strcmp (e, "git") == 0 ? p.base () : p;
    }

    // Should be pkg repository location path.
    //
    assert (!p.empty () && *p.begin () != "..");

    auto rb (p.rbegin ()), i (rb), re (p.rend ());

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
    try
    {
      if (stoul (*i) != 1)
        throw invalid_argument ("unsupported repository version");
    }
    catch (const std::exception&)
    {
      throw invalid_argument ("invalid repository version");
    }

    path res (rb, i);

    // Canonical name prefix part ends with the special "pkg" component.
    //
    bool pc (++i != re && (*i == "pkg" || *i == "bpkg"));

    if (pc && mode == strip_mode::component)
      ++i; // Strip the "pkg" component.

    if (!pc || mode != strip_mode::path)
      res = path (i, re) / res; // Concatenate prefix and path parts.

    return res;
  }

  repository_location::
  repository_location (repository_url u, repository_type t)
      : repository_location (move (u), t, repository_location ()) // Delegate.
  {
    if (!empty () && relative ())
      throw invalid_argument ("relative filesystem path");
  }

  repository_location::
  repository_location (repository_url u,
                       repository_type t,
                       const repository_location& b)
      : url_ (move (u)),
        type_ (t)
  {
    using std::string;    // Hidden by string().
    using std::to_string; // Hidden by to_string(repository_type).
    using butl::path;     // Hidden by path().

    if (url_.empty ())
    {
      if (!b.empty ())
        throw invalid_argument ("empty location");

      return;
    }

    // Make sure that the URL object is properly constructed (see notes for the
    // repository_url class in the header).
    //
    assert (url_.path &&
            remote () == (url_.authority && !url_.authority->empty ()));

    // Verify that the URL object matches the repository type.
    //
    switch (t)
    {
    case repository_type::pkg:
      {
        if (url_.scheme == repository_protocol::git)
          throw invalid_argument ("unsupported scheme for pkg repository");

        if (url_.fragment)
          throw invalid_argument ("unexpected fragment for pkg repository");

        break;
      }
    case repository_type::dir:
      {
        if (url_.scheme != repository_protocol::file)
          throw invalid_argument (
            "unsupported scheme for dir repository");

        if (url_.fragment)
          throw invalid_argument ("unexpected fragment for dir repository");

        break;
      }
    case repository_type::git:
      {
        // Verify the URL fragment.
        //
        if (url_.fragment)
          git_ref_filter r (*url_.fragment);

        break;
      }
    }

    // Base repository location can not be a relative path.
    //
    if (!b.empty () && b.relative ())
      throw invalid_argument ("base location is relative filesystem path");

    path& up (*url_.path);

    // For the repositories that are "directories", make sure that the URL
    // object's path is a directory (contains the trailing slash).
    //
    switch (t)
    {
    case repository_type::pkg:
    case repository_type::dir:
    case repository_type::git:
      {
        if (!up.to_directory ())
          up = path_cast<dir_path> (move (up));
        break;
      }
    }

    if (remote ())
    {
      canonical_name_ = to_string (type_);
      canonical_name_ += ':';
      canonical_name_ += strip_domain (url_.authority->host, type_);

      // For canonical name and for the HTTP protocol, treat a.com and
      // a.com:80 as the same name. The same rule applies to the HTTPS (port
      // 443) and GIT (port 9418) protocols.
      //
      uint16_t port (url_.authority->port);
      if (port != 0)
      {
        uint16_t def_port (0);

        switch (url_.scheme)
        {
        case repository_protocol::http:  def_port =   80; break;
        case repository_protocol::https: def_port =  443; break;
        case repository_protocol::git:   def_port = 9418; break;
        case repository_protocol::file:  assert (false); // Can't be here.
        }

        if (port != def_port)
          canonical_name_ += ':' + to_string (port);
      }
    }
    else
    {
      // Complete if we are relative and have base.
      //
      if (!b.empty () && up.relative ())
      {
        // Convert the relative path location to an absolute or remote one.
        //
        repository_url u (b.url ());
        *u.path /= up;

        // Override the base repository fragment.
        //
        u.fragment = move (url_.fragment);

        url_ = move (u);

        // Set canonical name to the base location canonical name 'pkg:<host>'
        // part. The '<path>[#<fragment>]' part of the canonical name is
        // calculated below.
        //
        if (b.remote ())
          canonical_name_ =
            b.canonical_name_.substr (0,
                                      b.canonical_name_.find_first_of ("/#"));
      }
    }

    // Normalize path to avoid different representations of the same location
    // and canonical name. So a/b/../c/1/x/../y and a/c/1/y to be considered
    // as same location.
    //
    // Note that we need to collapse 'example.com/a/..' to 'example.com/',
    // rather than to 'example.com/.'.
    //
    try
    {
      up.normalize (false /* actual */, remote () /* cur_empty */);
    }
    catch (const invalid_path&)
    {
      throw invalid_argument ("invalid path");
    }

    // For pkg repository we need to check path for emptiness before
    // proceeding further as a valid non empty location can not have an empty
    // URL object path member (which can be the case for the remote location,
    // but not for the relative or absolute).
    //
    if (type_ == repository_type::pkg && up.empty ())
      throw invalid_argument ("empty path");

    // Need to check that URL path do not go past the root directory of a WEB
    // server. We can not rely on the above normalize() function call doing
    // this check as soon as URL object path member contains a relative
    // directory for the remote location.
    //
    if (remote () && !up.empty () && *up.begin () == "..")
      throw invalid_argument ("invalid path");

    // Finish calculating the canonical name, unless we are relative.
    //
    if (relative ())
    {
      assert (canonical_name_.empty ());
      return;
    }

    // Canonical name part that is produced from the repository location path
    // part. The algorithm depends on the repository type.
    //
    path sp;

    switch (type_)
    {
    case repository_type::pkg:
      {
        // Produce the pkg repository canonical name <prefix>/<path> part (see
        // the Repository Chaining documentation for more details).
        //
        sp = strip_path (up,
                         remote ()
                         ? strip_mode::component
                         : strip_mode::path);

        // If for an absolute path location the stripping result is empty
        // (which also means <path> part is empty as well) then fallback to
        // stripping just the version component.
        //
        if (absolute () && sp.empty ())
          sp = strip_path (up, strip_mode::version);

        break;
      }
    case repository_type::dir:
      {
        // For dir repository we use the absolute (normalized) path.
        //
        sp = up;
        break;
      }
    case repository_type::git:
      {
        // For git repository we use the absolute (normalized) path, stripping
        // the .git extension if present.
        //
        sp = strip_path (up, strip_mode::extension);
        break;
      }
    }

    string cp (sp.relative () ? sp.posix_string () : sp.string ());

    // Don't allow canonical names without both host and path parts.
    //
    if (canonical_name_.empty () && cp.empty ())
      throw invalid_argument ("empty repository name");

    // Note: allow empty paths (e.g., http://stable.cppget.org/1/).
    //
    if (!cp.empty ())
    {
      if (!canonical_name_.empty ()) // If we have host and dir.
        canonical_name_ += '/';
      else                           // If we have just dir.
      {
        canonical_name_ = to_string (type_);
        canonical_name_ += ':';
      }
    }

    canonical_name_ += cp;

    if (url_.fragment)
    {
      canonical_name_ += '#';
      canonical_name_ += *url_.fragment;
    }
  }

  // git_ref_filter
  //
  git_ref_filter::
  git_ref_filter (const string& frag)
  {
    size_t p (frag.find ('@'));
    if (p != string::npos)
    {
      if (p != 0)
        name = string (frag, 0, p);

      if (p + 1 != frag.size ())
        commit = string (frag, p + 1);
    }
    else if (!frag.empty ())
    {
      // A 40-characters fragment that consists of only hexadecimal digits is
      // assumed to be a commit id.
      //
      if (frag.size () == 40 &&
          find_if_not (frag.begin (), frag.end (),

                       // Resolve the required overload.
                       //
                       static_cast<bool (*)(char)> (xdigit)) == frag.end ())
        commit = frag;
      else
        name = frag;
    }

    if (!name && !commit)
      throw invalid_argument (
        "missing reference name or commit id for git repository");

    if (commit && commit->size () != 40)
      throw invalid_argument (
        "git repository commit id must be 40 characters long");
  }

  // repository_manifest
  //
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
    static const char* invalid_location ("invalid repository location");

    if (l.local ())
      throw invalid_argument (invalid_location);

    if (l.type () != repository_type::pkg || !url || (*url)[0] != '.')
      return url;

    const path rp (*url);
    auto i (rp.begin ());

    static const char* invalid_url ("invalid relative url");

    auto strip = [&i, &rp]() -> bool
    {
      if (i != rp.end ())
      {
        const auto& c (*i++);
        if (c == "..")
          return true;

        if (c == ".")
          return false;
      }

      throw invalid_argument (invalid_url);
    };

    bool strip_d (strip ()); // Strip domain.
    bool strip_p (strip ()); // Strip path.

    // The web interface relative path with the special first two components
    // stripped.
    //
    const path rpath (i, rp.end ());
    assert (rpath.relative ());

    repository_url u (l.url ());

    if (strip_d)
      u.authority->host.value = strip_domain (u.authority->host,
                                              repository_type::pkg);

    // Web interface URL path part.
    //
    // It is important to call strip_path() before appending the relative
    // path. Otherwise the effective URL for the path ./../../.. and the
    // repository location http://a.com/foo/pkg/1/math will wrongly be
    // http://a.com/foo/pkg instead of http://a.com.
    //
    path ipath (strip_path (*u.path,
                            strip_p
                            ? strip_mode::component
                            : strip_mode::version) /
                rpath);

    try
    {
      ipath.normalize (false /* actual */, true /* cur_empty */);
    }
    catch (const invalid_path&)
    {
      throw invalid_argument (invalid_location);
    }

    assert (ipath.relative ());

    if (!ipath.empty () && *ipath.begin () == "..")
      throw invalid_argument (invalid_location);

    // Strip the trailing slash for an empty path.
    //
    u.path = !ipath.empty () ? move (ipath) : optional<path> ();
    return u.string ();
  }

  static repository_manifest
  parse_repository_manifest (parser& p,
                             name_value nv,
                             repository_type base_type,
                             bool iu)
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

    repository_manifest r;

    // The repository type value can go after the location value. So we need to
    // postpone the location value parsing until we went though all other
    // values.
    //
    optional<repository_type> type;
    optional<name_value> location;

    for (nv = p.next (); !nv.empty (); nv = p.next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "location")
      {
        if (location)
          bad_name ("location redefinition");

        if (v.empty ())
          bad_value ("empty location");

        location = move (nv);
      }
      else if (n == "type")
      {
        if (type)
          bad_name ("type redefinition");

        try
        {
          type = to_repository_type (v);
        }
        catch (const invalid_argument& e)
        {
          bad_value (e.what ());
        }
      }
      else if (n == "role")
      {
        if (r.role)
          bad_name ("role redefinition");

        auto b (repository_role_names.cbegin ());
        auto e (repository_role_names.cend ());
        auto i (find (b, e, v));

        if (i == e)
          bad_value ("unrecognized role");

        r.role = static_cast<repository_role> (i - b);
      }
      else if (n == "url")
      {
        if (r.url)
          bad_name ("url redefinition");

        if (v.empty ())
          bad_value ("empty url");

        r.url = move (v);
      }
      else if (n == "email")
      {
        if (r.email)
          bad_name ("email redefinition");

        auto vc (parser::split_comment (v));

        if (vc.first.empty ())
          bad_value ("empty email");

        r.email = email (move (vc.first), move (vc.second));
      }
      else if (n == "summary")
      {
        if (r.summary)
          bad_name ("summary redefinition");

        if (v.empty ())
          bad_value ("empty summary");

        r.summary = move (v);
      }
      else if (n == "description")
      {
        if (r.description)
          bad_name ("description redefinition");

        if (v.empty ())
          bad_value ("empty description");

        r.description = move (v);
      }
      else if (n == "certificate")
      {
        if (base_type != repository_type::pkg)
          bad_name ("certificate not allowed");

        if (r.certificate)
          bad_name ("certificate redefinition");

        if (v.empty ())
          bad_value ("empty certificate");

        r.certificate = move (v);
      }
      else if (!iu)
        bad_name ("unknown name '" + n + "' in repository manifest");
    }

    // Parse location.
    //
    if (location)
    try
    {
      repository_url u (move (location->value));

      // If the prerequisite repository type is not specified explicitly then
      // we consider it to be the base repository type for the relative
      // location or guess it otherwise.
      //
      if (!type)
      {
        if (u.scheme == repository_protocol::file && u.path->relative ())
        {
          type = base_type;

          // Strip the URL fragment if the base repository type is dir (see
          // the Repository Manifest documentation for the gory details).
          //
          if (base_type == repository_type::dir)
            u.fragment = nullopt;
        }
        else
          type = guess_type (u, false); // Can't throw.
      }

      // Call prerequisite repository location constructor, do not amend
      // relative path.
      //
      r.location = repository_location (u, *type, repository_location ());
    }
    catch (const invalid_argument& e)
    {
      nv = move (*location); // Restore as bad_value() uses its line/column.
      bad_value (e.what ());
    }

    // Verify that all non-optional values were specified.
    //
    // - location can be omitted
    // - role can be omitted
    //
    if (r.role && r.location.empty () != (*r.role == repository_role::base))
      bad_value ("invalid role");

    if (r.effective_role () != repository_role::base)
    {
      if (r.url)
        bad_value ("url not allowed");

      if (r.email)
        bad_value ("email not allowed");

      if (r.summary)
        bad_value ("summary not allowed");

      if (r.description)
        bad_value ("description not allowed");

      if (r.certificate)
        bad_value ("certificate not allowed");
    }

    return r;
  }

  static repository_manifest
  parse_repository_manifest (parser& p, repository_type base_type, bool iu)
  {
    repository_manifest r (
      parse_repository_manifest (p, p.next (), base_type, iu));

    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single repository manifest expected");

    return r;
  }

  void repository_manifest::
  serialize (serializer& s) const
  {
    auto bad_value ([&s](const string& d) {
        throw serialization (s.name (), d);});

    s.next ("", "1"); // Start of manifest.

    if (!location.empty ())
    {
      s.next ("location", location.string ());
      s.next ("type", to_string (location.type ()));
    }

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

      s.next ("email", serializer::merge_comment (*email, email->comment));
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

  // pkg_repository_manifest
  //
  repository_manifest
  pkg_repository_manifest (parser& p, bool iu)
  {
    return parse_repository_manifest (p, repository_type::pkg, iu);
  }

  repository_manifest
  pkg_repository_manifest (parser& p, name_value nv, bool iu)
  {
    return parse_repository_manifest (p, nv, repository_type::pkg, iu);
  }

  // dir_repository_manifest
  //
  repository_manifest
  dir_repository_manifest (parser& p, bool iu)
  {
    return parse_repository_manifest (p, repository_type::dir, iu);
  }

  repository_manifest
  dir_repository_manifest (parser& p, name_value nv, bool iu)
  {
    return parse_repository_manifest (p, nv, repository_type::dir, iu);
  }

  // git_repository_manifest
  //
  repository_manifest
  git_repository_manifest (parser& p, bool iu)
  {
    return parse_repository_manifest (p, repository_type::git, iu);
  }

  repository_manifest
  git_repository_manifest (parser& p, name_value nv, bool iu)
  {
    return parse_repository_manifest (p, nv, repository_type::git, iu);
  }

  // Parse the repository manifest list.
  //
  static void
  parse_repository_manifests (parser& p,
                              repository_type base_type,
                              bool iu,
                              vector<repository_manifest>& ms)
  {
    name_value nv (p.next ());
    while (!nv.empty ())
    {
      ms.push_back (parse_repository_manifest (p, move (nv), base_type, iu));
      nv = p.next ();

      // Make sure there is location in all except the last entry.
      //
      if (ms.back ().location.empty () && !nv.empty ())
        throw parsing (p.name (), nv.name_line, nv.name_column,
                       "repository location expected");
    }

    if (ms.empty () || !ms.back ().location.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "base repository manifest expected");
  }

  // Serialize the repository manifest list.
  //
  static void
  serialize_repository_manifests (serializer& s,
                                  const vector<repository_manifest>& ms)
  {
    if (ms.empty () || !ms.back ().location.empty ())
      throw serialization (s.name (), "base repository manifest expected");

    // @@ Should we check that there is location in all except the last
    //    entry?
    //
    for (const repository_manifest& r: ms)
      r.serialize (s);

    s.next ("", ""); // End of stream.
  }

  // pkg_repository_manifests
  //
  pkg_repository_manifests::
  pkg_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::pkg, iu, *this);
  }

  void pkg_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, *this);
  }

  // dir_repository_manifests
  //
  dir_repository_manifests::
  dir_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::dir, iu, *this);
  }

  void dir_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, *this);
  }

  // git_repository_manifests
  //
  git_repository_manifests::
  git_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::git, iu, *this);
  }

  void git_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, *this);
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

    // Make sure this is the end.
    //
    nv = p.next ();
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single signature manifest expected");
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
