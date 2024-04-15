// file      : libbpkg/manifest.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbpkg/manifest.hxx>

#include <string>
#include <limits>
#include <ostream>
#include <sstream>
#include <cassert>
#include <cstdlib>     // strtoull()
#include <cstring>     // strncmp(), strcmp(), strchr(), strcspn()
#include <utility>     // move()
#include <cstdint>     // uint*_t
#include <algorithm>   // find(), find_if(), find_first_of(), replace()
#include <stdexcept>   // invalid_argument
#include <type_traits> // remove_reference

#include <libbutl/url.hxx>
#include <libbutl/path.hxx>
#include <libbutl/utf8.hxx>
#include <libbutl/base64.hxx>
#include <libbutl/utility.hxx>             // icasecmp(), lcase(), alnum(),
                                           // digit(), xdigit(), next_word()
#include <libbutl/filesystem.hxx>          // dir_exist()
#include <libbutl/small-vector.hxx>
#include <libbutl/char-scanner.hxx>
#include <libbutl/manifest-parser.hxx>
#include <libbutl/manifest-serializer.hxx>
#include <libbutl/standard-version.hxx>

#include <libbpkg/buildfile-scanner.hxx>

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

    for (char c: s)
    {
      if ((c < 'a' || c > 'f' ) && !digit (c))
        return false;
    }

    return true;
  }

  inline static bool
  valid_fingerprint (const string& s) noexcept
  {
    size_t n (s.size ());
    if (n != 32 * 3 - 1)
      return false;

    for (size_t i (0); i != n; ++i)
    {
      char c (s[i]);
      if ((i + 1) % 3 == 0) // Must be the colon,
      {
        if (c != ':')
          return false;
      }
      else if (!xdigit (c)) // Must be a hex digit.
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
           optional<uint16_t> r,
           uint32_t i)
      : epoch (e),
        upstream (move (u)),
        release (move (l)),
        revision (r),
        iteration (i),
        canonical_upstream (
          data_type (upstream.c_str (),
                     data_type::parse::upstream,
                     none).
            canonical_upstream),
        canonical_release (
          data_type (release ? release->c_str () : nullptr,
                     data_type::parse::release,
                     none).
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

      if (revision)
        throw invalid_argument ("revision for empty version");

      if (iteration != 0)
        throw invalid_argument ("iteration for empty version");
    }
    // Empty release signifies the earliest possible release. Revision and/or
    // iteration are meaningless in such a context.
    //
    else if (release && release->empty () && (revision || iteration != 0))
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

  // Return zero for versions having the '0[+<revision>]' form (stubs) and one
  // otherwise. Used for both version and version::data_type types.
  //
  template <typename T>
  inline static uint16_t
  default_epoch (const T& v)
  {
    return v.canonical_upstream.empty () && !v.release ? 0 : 1;
  }

  version::data_type::
  data_type (const char* v, parse pr, version::flags fl)
  {
    if ((fl & version::fold_zero_revision) != 0)
      assert (pr == parse::full);

    if ((fl & version::allow_iteration) != 0)
      assert (pr == parse::full);

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

    auto bad_arg = [](const string& d) {throw invalid_argument (d);};

    auto parse_uint = [&bad_arg](const string& s, auto& r, const char* what)
    {
      using type = typename remove_reference<decltype (r)>::type;

      if (!s.empty () && s[0] != '-' && s[0] != '+') // strtoull() allows these.
      {
        const char* b (s.c_str ());
        char* e (nullptr);
        errno = 0; // We must clear it according to POSIX.
        uint64_t v (strtoull (b, &e, 10)); // Can't throw.

        if (errno != ERANGE    &&
            e == b + s.size () &&
            v <= numeric_limits<type>::max ())
        {
          r = static_cast<type> (v);
          return;
        }
      }

      bad_arg (string (what) + " should be " +
               std::to_string (sizeof (type)) + "-byte unsigned integer");
    };

    auto parse_uint16 = [&parse_uint](const string& s, const char* what)
    {
      uint16_t r;
      parse_uint (s, r, what);
      return r;
    };

    auto parse_uint32 = [&parse_uint](const string& s, const char* what)
    {
      uint32_t r;
      parse_uint (s, r, what);
      return r;
    };

    assert (v != nullptr);

    // Parse the iteration, if allowed.
    //
    // Note that allowing iteration is not very common, so let's handle it in
    // an ad hoc way not to complicate the subsequent parsing.
    //
    string storage;
    if (pr == parse::full)
    {
      iteration = 0;

      // Note that if not allowed but the iteration is present, then the below
      // version parsing code will fail with appropriate diagnostics.
      //
      if ((fl & version::allow_iteration) != 0)
      {
        if (const char* p = strchr (v, '#'))
        {
          iteration = parse_uint32 (p + 1, "iteration");

          storage.assign (v, p - v);
          v = storage.c_str ();
        }
      }
    }

    optional<uint16_t> ep;

    enum class mode {epoch, upstream, release, revision};
    mode m (pr == parse::full
            ? (v[0] == '+'
               ? mode::epoch
               : mode::upstream)
            : (pr == parse::upstream
               ? mode::upstream
               : mode::release));

    canonical_part canon_upstream;
    canonical_part canon_release;

    canonical_part* canon_part (
      pr == parse::release ? &canon_release : &canon_upstream);

    const char* cb (m != mode::epoch ? v : v + 1); // Begin of a component.
    const char* ub (cb);                           // Begin of upstream part.
    const char* ue (cb);                           // End of upstream part.
    const char* rb (cb);                           // Begin of release part.
    const char* re (cb);                           // End of release part.
    const char* lnn (cb - 1);                      // Last non numeric char.

    const char* p (cb);
    for (char c; (c = *p) != '\0'; ++p)
    {
      switch (c)
      {
      case '+':
      case '-':
      case '.':
        {
          // Process the epoch part or the upstream/release part component.
          //

          // Characters '+', '-' are only valid for the full version parsing.
          //
          if (c != '.' && pr != parse::full)
            bad_arg (string ("unexpected '") + c + "' character");

          // Check if the component ending is valid for the current parsing
          // state.
          //
          if (m == mode::revision || (c == '-' && m == mode::release) ||
              (c != '-' && m == mode::epoch) || p == cb)
            bad_arg (string ("unexpected '") + c + "' character position");

          // Depending on the mode, parse epoch or append the component to the
          // current canonical part.
          //
          if (m == mode::epoch)
          {
            if (lnn >= cb) // Contains non-digits.
              bad_arg ("epoch should be 2-byte unsigned integer");

            ep = parse_uint16 (string (cb, p), "epoch");
          }
          else
            canon_part->add (cb, p, lnn < cb);

          // Update the parsing state.
          //
          // Advance begin of a component.
          //
          cb = p + 1;

          // Advance end of the upstream/release parts.
          //
          if (m == mode::upstream)
            ue = p;
          else if (m == mode::release)
            re = p;
          else
            assert (m == mode::epoch);

          // Switch the mode if the component is terminated with '+' or '-'.
          //
          if (c == '+')
            m = mode::revision;
          else if (c == '-')
          {
            if (m == mode::epoch)
            {
              m = mode::upstream;
              ub = cb;
              ue = cb;
            }
            else
            {
              m = mode::release;
              rb = cb;
              re = cb;

              canon_part = &canon_release;
            }
          }

          break;
        }
      default:
        {
          if (!alnum (c))
            bad_arg ("alpha-numeric characters expected in a component");
        }
      }

      if (!digit (c))
        lnn = p;
    }

    assert (p >= cb); // 'p' denotes the end of the last component.

    // The epoch must always be followed by the upstream.
    //
    // An empty component is valid for the release part, and for the
    // upstream part when constructing empty or max limit version.
    //
    if (m == mode::epoch ||
        (p == cb && m != mode::release && pr != parse::upstream))
      bad_arg ("unexpected end");

    // Parse the last component.
    //
    if (m == mode::revision)
    {
      if (lnn >= cb) // Contains non-digits.
        bad_arg ("revision should be 2-byte unsigned integer");

      uint16_t rev (parse_uint16 (cb, "revision"));

      if (rev != 0 || (fl & fold_zero_revision) == 0)
        revision = rev;
    }
    else if (cb != p)
    {
      canon_part->add (cb, p, lnn < cb);

      if (m == mode::upstream)
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

    if (pr == parse::full)
    {
      epoch = ep ? *ep : default_epoch (*this);

      if (epoch == 0                  &&
          canonical_upstream.empty () &&
          canonical_release.empty ())
      {
        assert (!revision); // Can't happen if through all previous checks.
        bad_arg ("empty version");
      }
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
  operator= (version&& v) noexcept
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

    std::string v (epoch != default_epoch (*this)
                   ? '+' + to_string (epoch) + '-' + upstream
                   : upstream);

    if (release)
    {
      v += '-';
      v += *release;
    }

    if (!ignore_revision)
    {
      if (revision)
      {
        v += '+';
        v += to_string (*revision);
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
  text_file (text_file&& f) noexcept
      : file (f.file), comment (move (f.comment))
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
  operator= (text_file&& f) noexcept
  {
    if (this != &f)
    {
      this->~text_file ();
      new (this) text_file (move (f)); // Rely on noexcept move-construction.
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

  // text_type
  //
  string
  to_string (text_type t)
  {
    switch (t)
    {
    case text_type::plain:       return "text/plain";
    case text_type::github_mark: return "text/markdown;variant=GFM";
    case text_type::common_mark: return "text/markdown;variant=CommonMark";
    }

    assert (false); // Can't be here.
    return string ();
  }

  optional<text_type>
  to_text_type (const string& t)
  {
    auto bad_type = [] (const string& d) {throw invalid_argument (d);};

    // Parse the media type representation (see RFC2045 for details) into the
    // type/subtype value and the parameter list. Note: we don't support
    // parameter quoting and comments for simplicity.
    //
    size_t p (t.find (';'));
    const string& tp (p != string::npos ? trim (string (t, 0, p)) : t);

    small_vector<pair<string, string>, 1> ps;

    while (p != string::npos)
    {
      // Extract parameter name.
      //
      size_t b (p + 1);
      p = t.find ('=', b);

      if (p == string::npos)
        bad_type ("missing '='");

      string n (trim (string (t, b, p - b)));

      // Extract parameter value.
      //
      b = p + 1;
      p = t.find (';', b);

      string v (trim (string (t,
                              b,
                              p != string::npos ? p - b : string::npos)));

      ps.emplace_back (move (n), move (v));
    }

    // Calculate the resulting text type, failing on unrecognized media type,
    // unexpected parameter name or value.
    //
    // Note that type, subtype, and parameter names are matched
    // case-insensitively.
    //
    optional<text_type> r;

    // Currently only the plain and markdown text types are allowed. Later we
    // can potentially introduce some other text types.
    //
    if (icasecmp (tp, "text/plain") == 0)
    {
      // Currently, we don't expect parameters for plain text. Later we can
      // potentially introduce some plain text variants.
      //
      if (ps.empty ())
        r = text_type::plain;
    }
    else if (icasecmp (tp, "text/markdown") == 0)
    {
      // Currently, a single optional variant parameter with the two possible
      // values is allowed for markdown. Later we can potentially introduce
      // some other markdown variants.
      //
      if (ps.empty () ||
          (ps.size () == 1 && icasecmp (ps[0].first, "variant") == 0))
      {
        // Note that markdown variants are matched case-insensitively (see
        // RFC7763 for details).
        //
        string v;
        if (ps.empty () || icasecmp (v = move (ps[0].second), "GFM") == 0)
          r = text_type::github_mark;
        else if (icasecmp (v, "CommonMark") == 0)
          r = text_type::common_mark;
      }
    }
    else if (icasecmp (tp, "text/", 5) != 0)
      bad_type ("text type expected");

    return r;
  }

  // typed_text_file
  //
  optional<text_type> typed_text_file::
  effective_type (bool iu) const
  {
    optional<text_type> r;

    if (type)
    {
      r = to_text_type (*type);
    }
    else if (file)
    {
      string ext (path.extension ());
      if (ext.empty () || icasecmp (ext, "txt") == 0)
        r = text_type::plain;
      else if (icasecmp (ext, "md") == 0 || icasecmp (ext, "markdown") == 0)
        r = text_type::github_mark;
    }
    else
      r = text_type::plain;

    if (!r && !iu)
      throw invalid_argument ("unknown text type");

    return r;
  }

  // manifest_url
  //
  manifest_url::
  manifest_url (const std::string& u, std::string c)
      : url (u),
        comment (move (c))
  {
    if (rootless)
      throw invalid_argument ("rootless URL");

    if (icasecmp (scheme, "file") == 0)
      throw invalid_argument ("local URL");

    if (!authority || authority->empty ())
      throw invalid_argument ("no authority");
  }

  // version_constraint
  //
  version_constraint::
  version_constraint (const std::string& s)
  {
    using std::string;

    auto bail = [] (const string& d) {throw invalid_argument (d);};

    char c (s[0]);
    if (c == '(' || c == '[') // The version range.
    {
      bool min_open (c == '(');

      size_t p (s.find_first_not_of (spaces, 1));
      if (p == string::npos)
        bail ("no min version specified");

      size_t e (s.find_first_of (spaces, p));

      const char* no_max_version ("no max version specified");

      if (e == string::npos)
        bail (no_max_version);

      version min_version;
      string mnv (s, p, e - p);

      // Leave the min version empty if it refers to the dependent package
      // version.
      //
      if (mnv != "$")
      try
      {
        min_version = version (mnv, version::none);
      }
      catch (const invalid_argument& e)
      {
        bail (string ("invalid min version: ") + e.what ());
      }

      p = s.find_first_not_of (spaces, e);
      if (p == string::npos)
        bail (no_max_version);

      e = s.find_first_of (" \t])", p);

      const char* invalid_range ("invalid version range");

      if (e == string::npos)
        bail (invalid_range);

      version max_version;
      string mxv (s, p, e - p);

      // Leave the max version empty if it refers to the dependent package
      // version.
      //
      if (mxv != "$")
      try
      {
        max_version = version (mxv, version::none);
      }
      catch (const invalid_argument& e)
      {
        bail (string ("invalid max version: ") + e.what ());
      }

      e = s.find_first_of ("])", e); // Might be a space.
      if (e == string::npos)
        bail (invalid_range);

      if (e + 1 != s.size ())
        bail ("unexpected text after version range");

      // Can throw invalid_argument that we don't need to intercept.
      //
      *this = version_constraint (move (min_version), min_open,
                                  move (max_version), s[e] == ')');
    }
    else if (c == '~' || c == '^') // The shortcut operator.
    {
      // If the shortcut operator refers to the dependent package version (the
      // '$' character), then create an incomplete constraint. Otherwise,
      // assume the version is standard and parse the operator representation
      // as the standard version constraint.
      //
      size_t p (s.find_first_not_of (spaces, 1));

      if (p != string::npos && s[p] == '$' && p + 1 == s.size ())
      {
        *this = version_constraint (version (), c == '~',
                                    version (), c == '^');
      }
      else
      {
        // To be used in the shortcut operator the package version must be
        // standard.
        //
        // Can throw invalid_argument that we don't need to intercept.
        //
        standard_version_constraint vc (s);

        try
        {
          assert (vc.min_version && vc.max_version);

          // Note that standard_version::string() folds the zero revision.
          // However, that's ok since the shortcut operator ~X.Y.Z+0
          // translates into [X.Y.Z+0 X.Y+1.0-) which covers the same versions
          // set as [X.Y.Z X.Y+1.0-).
          //
          *this = version_constraint (version (vc.min_version->string ()),
                                      vc.min_open,
                                      version (vc.max_version->string ()),
                                      vc.max_open);
        }
        catch (const invalid_argument&)
        {
          // Any standard version is a valid package version, so the
          // conversion should never fail.
          //
          assert (false);
        }
      }
    }
    else // The version comparison notation.
    {
      enum comparison {eq, lt, gt, le, ge};
      comparison operation (eq); // Uninitialized warning.
      size_t p (2);

      if (s.compare (0, 2, "==") == 0)
        operation = eq;
      else if (s.compare (0, 2, ">=") == 0)
        operation = ge;
      else if (s.compare (0, 2, "<=") == 0)
        operation = le;
      else if (c == '>')
      {
        operation = gt;
        p = 1;
      }
      else if (c == '<')
      {
        operation = lt;
        p = 1;
      }
      else
        bail ("invalid version comparison");

      p = s.find_first_not_of (spaces, p);

      if (p == string::npos)
        bail ("no version specified");

      try
      {
        version v;
        string vs (s, p);

        // Leave the version empty if it refers to the dependent package
        // version.
        //
        if (vs != "$")
          v = version (vs, version::none);

        switch (operation)
        {
        case comparison::eq:
          *this = version_constraint (v);
          break;
        case comparison::lt:
          *this = version_constraint (nullopt, true, move (v), true);
          break;
        case comparison::le:
          *this = version_constraint (nullopt, true, move (v), false);
          break;
        case comparison::gt:
          *this = version_constraint (move (v), true, nullopt, true);
          break;
        case comparison::ge:
          *this = version_constraint (move (v), false, nullopt, true);
          break;
        }
      }
      catch (const invalid_argument& e)
      {
        bail (string ("invalid version: ") + e.what ());
      }
    }
  }

  version_constraint::
  version_constraint (optional<version> mnv, bool mno,
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

      // Absent version endpoint (infinity) should be open.
      //
      (min_version || min_open) && (max_version || max_open));

    if (min_version && max_version)
    {
      bool mxe (max_version->empty ());

      // If endpoint versions do not refer to the dependent package version
      // then the min version must (naturally) be lower than or equal to the
      // max version.
      //
      if (*min_version > *max_version && !mxe)
      {
        // Handle the (X+Y X] and [X+Y X] corner cases (any revision of
        // version X greater (or equal) than X+Y). Note that technically
        // X+Y > X (see version::compare() for details).
        //
        // Also note that we reasonably fail for (X+Y X) and [X+Y X).
        //
        if (!(!max_open              &&
              !max_version->revision &&
              max_version->compare (*min_version,
                                    true /* ignore_revision */) == 0))
          throw invalid_argument ("min version is greater than max version");
      }

      if (*min_version == *max_version)
      {
        // For non-empty versions, both endpoints must be closed, representing
        // the `== <version>` constraint. For empty versions no greater than
        // one endpoint can be open, representing the `== $`, `~$`, or `^$`
        // constraints.
        //
        if ((!mxe && (min_open || max_open)) || (mxe && min_open && max_open))
          throw invalid_argument ("equal version endpoints not closed");

        // If endpoint versions do not refer to the dependent package version
        // then they can't be earliest. Note: an empty version is earliest.
        //
        if (!mxe && max_version->release && max_version->release->empty ())
          throw invalid_argument ("equal version endpoints are earliest");
      }
    }
  }

  version_constraint version_constraint::
  effective (version v) const
  {
    using std::string;

    // The dependent package version can't be empty or earliest.
    //
    if (v.empty ())
      throw invalid_argument ("dependent version is empty");

    if (v.release && v.release->empty ())
      throw invalid_argument ("dependent version is earliest");

    // For the more detailed description of the following semantics refer to
    // the depends value documentation.

    // Strip the revision and iteration.
    //
    v = version (v.epoch,
                 move (v.upstream),
                 move (v.release),
                 nullopt /* revision */,
                 0 /* iteration */);

    // Calculate effective constraint for a shortcut operator.
    //
    if (min_version                &&
        min_version->empty ()      &&
        max_version == min_version &&
        (min_open || max_open))
    {
      assert (!min_open || !max_open); // Endpoints cannot be both open.

      string vs (v.string ());

      // Note that a stub dependent is not allowed for the shortcut operator.
      // However, we allow it here to fail later (in
      // standard_version_constraint()) with a more precise description.
      //
      optional<standard_version> sv (
        parse_standard_version (vs, standard_version::allow_stub));

      if (!sv)
        throw invalid_argument ("dependent version is not standard");

      standard_version_constraint vc (min_open ? "~$" : "^$", *sv);

      try
      {
        assert (vc.min_version && vc.max_version);

        return version_constraint (version (vc.min_version->string ()),
                                   vc.min_open,
                                   version (vc.max_version->string ()),
                                   vc.max_open);
      }
      catch (const invalid_argument&)
      {
        // There shouldn't be a reason for version_constraint() to throw.
        //
        assert (false);
      }
    }

    // Calculate effective constraint for a range.
    //
    return version_constraint (
      min_version && min_version->empty () ? v : min_version, min_open,
      max_version && max_version->empty () ? v : max_version, max_open);
  }

  std::string version_constraint::
  string () const
  {
    assert (!empty ());

    auto ver = [] (const version& v) {return v.empty () ? "$" : v.string ();};

    if (!min_version)
      return (max_open ? "< " : "<= ") + ver (*max_version);

    if (!max_version)
      return (min_open ? "> " : ">= ") + ver (*min_version);

    if (*min_version == *max_version)
    {
      const version& v (*min_version);

      if (!min_open && !max_open)
        return "== " + ver (v);

      assert (v.empty () && (!min_open || !max_open));
      return min_open ? "~$" : "^$";
    }

    // If the range can potentially be represented as a range shortcut
    // operator (^ or ~), having the [<standard-version> <standard-version>)
    // form, then produce the resulting string using the standard version
    // constraint code.
    //
    if (!min_open              &&
        max_open               &&
        !min_version->empty () &&
        !max_version->empty ())
    {
      if (optional<standard_version> mnv =
          parse_standard_version (min_version->string (),
                                  standard_version::allow_earliest))
      {
        if (optional<standard_version> mxv =
            parse_standard_version (max_version->string (),
                                    standard_version::allow_earliest))
        try
        {
          return standard_version_constraint (
            move (*mnv), min_open, move (*mxv), max_open).string ();
        }
        catch (const invalid_argument&)
        {
          // Invariants for both types of constraints are the same, so the
          // conversion should never fail.
          //
          assert (false);
        }
      }
    }

    // Represent as a range.
    //
    std::string r (min_open ? "(" : "[");
    r += ver (*min_version);
    r += ' ';
    r += ver (*max_version);
    r += max_open ? ')' : ']';
    return r;
  }

  // dependency
  //
  dependency::
  dependency (std::string d)
  {
    using std::string;
    using iterator = string::const_iterator;

    iterator b (d.begin ());
    iterator i (b);
    iterator ne (b); // End of name.
    iterator e (d.end ());

    // Find end of name (ne).
    //
    // Grep for '=<>([~^' in the bpkg source code and update, if changed.
    //
    const string cb ("=<>([~^");
    for (char c; i != e && cb.find (c = *i) == string::npos; ++i)
    {
      if (!space (c))
        ne = i + 1;
    }

    try
    {
      name = package_name (i == e ? move (d) : string (b, ne));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid package name: ") + e.what ());
    }

    if (i != e)
    try
    {
      constraint = version_constraint (string (i, e));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid package constraint: ") +
                              e.what ());
    }
  }

  // dependency_alternative
  //
  string dependency_alternative::
  string () const
  {
    std::string r (size () > 1 ? "{" : "");

    bool first (true);
    for (const dependency& d: *this)
    {
      if (!first)
        r += ' ';
      else
        first = false;

      r += d.string ();
    }

    if (size () > 1)
      r += '}';

    if (single_line ())
    {
      if (enable)
      {
        r += " ? (";
        r += *enable;
        r += ')';
      }

      if (reflect)
      {
        r += ' ';
        r += *reflect;
      }
    }
    else
    {
      // Add an extra newline between the clauses.
      //
      bool first (true);

      r += "\n{";

      if (enable)
      {
        first = false;

        r += "\n  enable (";
        r += *enable;
        r += ')';
      }

      if (prefer)
      {
        if (!first)
          r += '\n';
        else
          first = false;

        r += "\n  prefer\n  {\n";
        r += *prefer;
        r += "  }";

        assert (accept);

        r += "\n\n  accept (";
        r += *accept;
        r += ')';
      }
      else if (require)
      {
        if (!first)
          r += '\n';
        else
          first = false;

        r += "\n  require\n  {\n";
        r += *require;
        r += "  }";
      }

      if (reflect)
      {
        if (!first)
          r += '\n';
        else
          first = false;

        r += "\n  reflect\n  {\n";
        r += *reflect;
        r += "  }";
      }

      r += "\n}";
    }

    return r;
  }

  // dependency_alternatives
  //
  class dependency_alternatives_lexer: public char_scanner<utf8_validator>
  {
  public:
    enum class token_type
    {
      eos,
      newline,
      word,
      buildfile,

      question,        // ?

      lcbrace,         // {
      rcbrace,         // }

      lparen,          // (
      rparen,          // )

      lsbrace,         // [
      rsbrace,         // ]

      equal,           // ==
      less,            // <
      greater,         // >
      less_equal,      // <=
      greater_equal,   // >=

      tilde,           // ~
      caret,           // ^

      bit_or           // |
    };

    struct token
    {
      token_type type;
      std::string value;

      uint64_t line;
      uint64_t column;

      std::string
      string (bool diag = true) const;
    };

    // If true, then comments are allowed and are treated as whitespace
    // characters.
    //
    bool comments = false;

  public:
    // Note that name is stored by shallow reference.
    //
    dependency_alternatives_lexer (istream& is,
                                   const string& name,
                                   uint64_t line,
                                   uint64_t column)
        : char_scanner (is,
                        utf8_validator (codepoint_types::graphic, U"\n\r\t"),
                        true /* crlf */,
                        line,
                        column),
          name_ (name),
          buildfile_scan_ (*this, name_) {}

    // The following functions throw manifest_parsing on invalid UTF-8
    // sequence.
    //

    // Peek the next non-whitespace character.
    //
    xchar
    peek_char ();

    // Extract next token (other than of the buildfile type) from the stream.
    //
    // Note that it is ok to call next() again after getting eos.
    //
    token
    next ();

    // The next_*() functions extract the buildfile token from the stream.
    // Throw manifest_parsing on error (invalid buildfile fragment, etc).
    //
    // Note that they are just thin wrappers around the scan_*() functions
    // (see buildfile-scanner.hxx for details).
    //
    token
    next_eval ();

    token
    next_line (char stop);

    token
    next_block ();

  private:
    using base = char_scanner<utf8_validator>;

    xchar
    get ()
    {
      xchar c (base::get (ebuf_));

      if (invalid (c))
        throw parsing (name_, c.line, c.column, ebuf_);

      return c;
    }

    void
    get (const xchar& peeked)
    {
      base::get (peeked);
    }

    xchar
    peek ()
    {
      xchar c (base::peek (ebuf_));

      if (invalid (c))
        throw parsing (name_, c.line, c.column, ebuf_);

      return c;
    }

    void
    skip_spaces ();

  private:
    const string& name_;

    // Buffer for a get()/peek() potential error.
    //
    string ebuf_;

    buildfile_scanner<utf8_validator, 1> buildfile_scan_;
  };

  dependency_alternatives_lexer::token dependency_alternatives_lexer::
  next ()
  {
    using type = token_type;

    skip_spaces ();

    uint64_t ln (line);
    uint64_t cl (column);

    xchar c (get ());

    auto make_token = [ln, cl] (type t, string v = string ())
    {
      return token {t, move (v), ln, cl};
    };

    if (eos (c))
      return make_token (type::eos);

    // NOTE: don't forget to also update the below separators list if changing
    // anything here.
    //
    switch (c)
    {
    case '\n': return make_token (type::newline);
    case '?':  return make_token (type::question);
    case '(':  return make_token (type::lparen);
    case ')':  return make_token (type::rparen);
    case '{':  return make_token (type::lcbrace);
    case '}':  return make_token (type::rcbrace);
    case '[':  return make_token (type::lsbrace);
    case ']':  return make_token (type::rsbrace);

    case '=':
      {
        if (peek () == '=')
        {
          get ();
          return make_token (type::equal);
        }
        break;
      }

    case '<':
      {
        if ((c = peek ()) == '=')
        {
          get (c);
          return make_token (type::less_equal);
        }
        else
          return make_token (type::less);
      }

    case '>':
      {
        if ((c = peek ()) == '=')
        {
          get (c);
          return make_token (type::greater_equal);
        }
        else
          return make_token (type::greater);
      }

    case '~':  return make_token (type::tilde);
    case '^':  return make_token (type::caret);

    case '|': return make_token (type::bit_or);
    }

    // Otherwise it is a word.
    //
    // Starts with a non-whitespace character which has not been recognized as
    // a part of some other token.
    //
    string r (1, c);

    // Add subsequent characters until eos or separator is encountered.
    //
    const char* s (" \n\t?(){}[]=<>~^|");
    for (c = peek (); !eos (c) && strchr (s, c) == nullptr; c = peek ())
    {
      r += c;
      get (c);
    }

    return make_token (type::word, move (r));
  }

  dependency_alternatives_lexer::token dependency_alternatives_lexer::
  next_eval ()
  {
    skip_spaces ();

    uint64_t ln (line);
    uint64_t cl (column);

    try
    {
      // Strip the trailing whitespaces.
      //
      return token {token_type::buildfile,
                    trim (buildfile_scan_.scan_eval ()),
                    ln,
                    cl};
    }
    catch (const buildfile_scanning& e)
    {
      throw parsing (e.name, e.line, e.column, e.description);
    }
  }

  dependency_alternatives_lexer::token dependency_alternatives_lexer::
  next_line (char stop)
  {
    skip_spaces ();

    uint64_t ln (line);
    uint64_t cl (column);

    try
    {
      // Strip the trailing whitespaces.
      //
      return token {token_type::buildfile,
                    trim (buildfile_scan_.scan_line (stop)),
                    ln,
                    cl};
    }
    catch (const buildfile_scanning& e)
    {
      throw parsing (e.name, e.line, e.column, e.description);
    }
  }

  dependency_alternatives_lexer::token dependency_alternatives_lexer::
  next_block ()
  {
    uint64_t ln (line);
    uint64_t cl (column);

    try
    {
      // Don't trim the token value not to strip the potential block indenting
      // on the first line.
      //
      return token {token_type::buildfile,
                    buildfile_scan_.scan_block (),
                    ln,
                    cl};
    }
    catch (const buildfile_scanning& e)
    {
      throw parsing (e.name, e.line, e.column, e.description);
    }
  }

  dependency_alternatives_lexer::xchar dependency_alternatives_lexer::
  peek_char ()
  {
    skip_spaces ();
    return peek ();
  }

  void dependency_alternatives_lexer::
  skip_spaces ()
  {
    xchar c (peek ());
    bool start (c.column == 1);

    for (; !eos (c); c = peek ())
    {
      switch (c)
      {
      case ' ':
      case '\t': break;

      case '#':
        {
          if (!comments)
            return;

          get (c);

          // See if this is a multi-line comment in the form:
          //
          /*
            #\
            ...
            #\
          */
          auto ml = [&c, this] () -> bool
          {
            if ((c = peek ()) == '\\')
            {
              get (c);
              if ((c = peek ()) == '\n' || eos (c))
                return true;
            }

            return false;
          };

          if (ml ())
          {
            // Scan until we see the closing one.
            //
            for (;;)
            {
              if (c == '#' && ml ())
                break;

              if (eos (c = peek ()))
                throw parsing (name_,
                               c.line, c.column,
                               "unterminated multi-line comment");

              get (c);
            }
          }
          else
          {
            // Read until newline or eos.
            //
            for (; !eos (c) && c != '\n'; c = peek ())
              get (c);
          }

          continue;
        }

      case '\n':
        {
          // Skip empty lines.
          //
          if (start)
            break;
        }
        // Fall through.
      default: return;
      }

      get (c);
    }
  }

  std::string dependency_alternatives_lexer::token::
  string (bool diag) const
  {
    std::string q (diag ? "'" : "");

    switch (type)
    {
    case token_type::eos:           return diag ? "<end of stream>" : "";
    case token_type::newline:       return diag ? "<newline>" : "\n";
    case token_type::word:          return q + value + q;
    case token_type::buildfile:     return (diag
                                            ? "<buildfile fragment>"
                                            : value);
    case token_type::question:      return q + '?' + q;
    case token_type::lparen:        return q + '(' + q;
    case token_type::rparen:        return q + ')' + q;
    case token_type::lcbrace:       return q + '{' + q;
    case token_type::rcbrace:       return q + '}' + q;
    case token_type::lsbrace:       return q + '[' + q;
    case token_type::rsbrace:       return q + ']' + q;
    case token_type::equal:         return q + "==" + q;
    case token_type::less:          return q + '<' + q;
    case token_type::greater:       return q + '>' + q;
    case token_type::less_equal:    return q + "<=" + q;
    case token_type::greater_equal: return q + ">=" + q;
    case token_type::tilde:         return q + '~' + q;
    case token_type::caret:         return q + '^' + q;
    case token_type::bit_or:        return q + '|' + q;
    }

    assert (false); // Can't be here.
    return "";
  }

  class dependency_alternatives_parser
  {
  public:

    // If the requirements flavor is specified, then only enable and reflect
    // clauses are allowed in the multi-line representation.
    //
    explicit
    dependency_alternatives_parser (bool requirements = false)
        : requirements_ (requirements) {}

    // Throw manifest_parsing if representation is invalid.
    //
    void
    parse (const package_name& dependent,
           istream&,
           const string& name,
           uint64_t line,
           uint64_t column,
           dependency_alternatives&);

  private:
    using lexer      = dependency_alternatives_lexer;
    using token      = lexer::token;
    using token_type = lexer::token_type;

    token_type
    next (token&, token_type&);

    token_type
    next_eval (token&, token_type&);

    token_type
    next_line (token&, token_type&);

    token_type
    next_block (token&, token_type&);

    // Receive the token/type from which it should start consuming and in
    // return the token/type contains the first token that has not been
    // consumed (normally eos, newline, or '|').
    //
    dependency_alternative
    parse_alternative (token&, token_type&, bool first);

    // Helpers.
    //
    // Throw manifest_parsing with the `<what> expected instead of <token>`
    // description.
    //
    [[noreturn]] void
    unexpected_token (const token&, string&& what);

    bool requirements_;

    const package_name* dependent_;
    const string* name_;
    lexer* lexer_;
    dependency_alternatives* result_;
  };

  [[noreturn]] void dependency_alternatives_parser::
  unexpected_token (const token& t, string&& w)
  {
    w += " expected";

    // Don't add the `instead of...` part, if the unexpected token is eos or
    // an empty word/buildfile.
    //
    if (t.type != token_type::eos &&
        ((t.type != token_type::word && t.type != token_type::buildfile) ||
         !t.value.empty ()))
    {
      w += " instead of ";
      w += t.string ();
    }

    throw parsing (*name_, t.line, t.column, w);
  }

  void dependency_alternatives_parser::
  parse (const package_name& dependent,
         istream& is,
         const string& name,
         uint64_t line,
         uint64_t column,
         dependency_alternatives& result)
  {
    lexer lexer (is, name, line, column);

    dependent_ = &dependent;
    name_ = &name;
    lexer_ = &lexer;
    result_ = &result;

    string what (requirements_ ? "requirement" : "dependency");

    token t;
    token_type tt;
    next (t, tt);

    // Make sure the representation is not empty, unless we are in the
    // requirements mode. In the latter case fallback to creating a simple
    // unconditional requirement. Note that it's the caller's responsibility
    // to verify that a non-empty comment is specified in this case.
    //
    if (tt == token_type::eos)
    {
      if (!requirements_)
        unexpected_token (t, what + " alternatives");

      dependency_alternative da;
      da.push_back (dependency ());

      result_->push_back (move (da));
      return;
    }

    for (bool first (true); tt != token_type::eos; )
    {
      dependency_alternative da (parse_alternative (t, tt, first));

      // Skip newline after the dependency alternative, if present.
      //
      if (tt == token_type::newline)
        next (t, tt);

      // Make sure that the simple requirement has the only alternative in the
      // representation.
      //
      if (requirements_   &&
          da.size () == 1 &&
          (da[0].name.empty () || (da.enable && da.enable->empty ())))
      {
        assert (first);

        if (tt != token_type::eos)
          throw parsing (*name_,
                         t.line,
                         t.column,
                         "end of simple requirement expected");
      }
      else
      {
        if (tt != token_type::eos && tt != token_type::bit_or)
          unexpected_token (t, "end of " + what + " alternatives or '|'");
      }

      if (tt == token_type::bit_or)
      {
        next (t, tt);

        // Skip newline after '|', if present.
        //
        if (tt == token_type::newline)
          next (t, tt);

        // Make sure '|' is not followed by eos.
        //
        if (tt == token_type::eos)
          unexpected_token (t, move (what));
      }

      result_->push_back (move (da));

      first = false;
    }
  }

  dependency_alternative dependency_alternatives_parser::
  parse_alternative (token& t, token_type& tt, bool first)
  {
    using type  = token_type;

    dependency_alternative r;

    string what (requirements_ ? "requirement" : "dependency");
    string config ("config." + dependent_->variable () + '.');

    auto bad_token = [&t, this] (string&& what)
    {
      unexpected_token (t, move (what));
    };

    // Check that the current token type matches the expected one. Throw
    // manifest_parsing if that's not the case. Use the expected token type
    // name for the error description or the custom name, if specified. For
    // the word and buildfile token types the custom name must be specified.
    //
    // Only move from the custom name argument if throwing exception.
    //
    auto expect_token = [&tt, &bad_token] (type et,
                                           string&& what = string ())
    {
      assert ((et != type::word && et != type::buildfile) || !what.empty ());

      if (tt != et)
      {
        if (what.empty ())
        {
          token e {et, "", 0, 0};
          bad_token (e.string ());
        }
        else
          bad_token (move (what));
      }
    };

    // Parse dependencies.
    //
    // If the current token starts the version constraint, then read its
    // tokens, rejoin them, and return the constraint string representation.
    // Otherwise return nullopt.
    //
    // Note that normally the caller reads the dependency package name, reads
    // the version constraint and, if present, appends it to the dependency,
    // and then creates the dependency object with a single constructor call.
    //
    // Note: doesn't read token that follows the constraint.
    //
    auto try_scan_version_constraint =
      [&t, &tt, &bad_token, &expect_token, this] () -> optional<string>
    {
      switch (t.type)
      {
      case type::lparen:
      case type::lsbrace:
        {
          string r (t.string (false /* diag */));

          next (t, tt);

          expect_token (type::word, "version");

          r += t.string (false /* diag */);
          r += ' ';

          next (t, tt);

          expect_token (type::word, "version");

          r += t.string (false /* diag */);

          next (t, tt);

          if (tt != type::rparen && tt != type::rsbrace)
            bad_token ("')' or ']'");

          r += t.string (false /* diag */);

          return optional<string> (move (r));
        }

      case type::equal:
      case type::less:
      case type::greater:
      case type::less_equal:
      case type::greater_equal:
      case type::tilde:
      case type::caret:
        {
          string r (t.string (false /* diag */));

          next (t, tt);

          expect_token (type::word, "version");

          r += t.string (false /* diag */);

          return optional<string> (move (r));
        }

      default: return nullopt;
      }
    };

    // Parse the evaluation context including the left and right parenthesis
    // and return the enclosed buildfile fragment.
    //
    // Note: no token is read after terminating ')'.
    //
    auto parse_eval = [&t, &tt, &expect_token, &bad_token, this] ()
    {
      next (t, tt);
      expect_token (type::lparen);

      next_eval (t, tt);

      if (t.value.empty ())
        bad_token ("condition");

      string r (move (t.value));

      next (t, tt);
      expect_token (type::rparen);

      return r;
    };

    const char* vccs ("([<>=!~^");

    bool group (tt == type::lcbrace); // Dependency group.

    if (group)
    {
      next (t, tt);

      if (tt == type::rcbrace)
        bad_token (move (what));

      while (tt != type::rcbrace)
      {
        expect_token (type::word, what + " or '}'");

        string   d  (move (t.value));
        uint64_t dl (t.line);
        uint64_t dc (t.column);

        next (t, tt);

        optional<string> vc (try_scan_version_constraint ());

        if (vc)
        {
          d += *vc;

          next (t, tt);
        }

        try
        {
          r.emplace_back (d);
        }
        catch (const invalid_argument& e)
        {
          throw parsing (*name_, dl, dc, e.what ());
        }
      }

      // See if a common version constraint follows the dependency group and
      // parse it if that's the case.
      //
      // Note that we need to be accurate not to consume what may end up to be
      // a part of the reflect config.
      //
      lexer::xchar c (lexer_->peek_char ());

      if (!lexer::eos (c) && strchr (vccs, c) != nullptr)
      {
        next (t, tt);

        uint64_t vcl (t.line);
        uint64_t vcc (t.column);

        optional<string> vc (try_scan_version_constraint ());

        if (!vc)
          bad_token ("version constraint");

        try
        {
          version_constraint c (*vc);

          for (dependency& d: r)
          {
            if (!d.constraint)
              d.constraint = c;
          }
        }
        catch (const invalid_argument& e)
        {
          throw parsing (*name_,
                         vcl,
                         vcc,
                         string ("invalid version constraint: ") + e.what ());
        }
      }
    }
    else        // Single dependency.
    {
      // If we see the question mark instead of a word in the requirements
      // mode, then this is a simple requirement. In this case parse the
      // evaluation context, if present, and bail out.
      //
      if (requirements_ && first && tt == type::question)
      {
        r.emplace_back (dependency ());

        bool eval (lexer_->peek_char () == '(');
        r.enable = eval ? parse_eval () : string ();

        next (t, tt);

        // @@ TMP Treat requirements similar to `? cli` as `cli ?` until
        //    toolchain 0.15.0 and libodb-mssql 2.5.0-b.22 are both released.
        //
        //    NOTE: don't forget to drop the temporary test in
        //    tests/manifest/testscript when dropping this workaround.
        //
        if (!eval && tt == type::word)
        try
        {
          r.back ().name = package_name (move (t.value));
          next (t, tt);
        }
        catch (const invalid_argument&) {}

        return r;
      }

      expect_token (type::word, move (what));

      string   d  (move (t.value));
      uint64_t dl (t.line);
      uint64_t dc (t.column);

      // See if a version constraint follows the dependency package name and
      // parse it if that's the case.
      //
      lexer::xchar c (lexer_->peek_char ());

      if (!lexer::eos (c) && strchr (vccs, c) != nullptr)
      {
        next (t, tt);

        optional<string> vc (try_scan_version_constraint ());

        if (!vc)
          bad_token ("version constraint");

        d += *vc;
      }

      try
      {
        r.emplace_back (d);
      }
      catch (const invalid_argument& e)
      {
        throw parsing (*name_, dl, dc, e.what ());
      }
    }

    // See if there is an enable condition and parse it if that's the case.
    //
    {
      lexer::xchar c (lexer_->peek_char ());

      if (c == '?')
      {
        next (t, tt);
        expect_token (type::question);

        // If we don't see the opening parenthesis in the requirements mode,
        // then this is a simple requirement. In this case set the enable
        // condition to an empty string and bail out.
        //
        c = lexer_->peek_char ();

        if (requirements_ && first && !group && c != '(')
        {
          r.enable = "";

          next (t, tt);
          return r;
        }

        r.enable = parse_eval ();
      }
    }

    // See if there is a reflect config and parse it if that's the case.
    //
    {
      lexer::xchar c (lexer_->peek_char ());

      if (!lexer::eos (c) && strchr ("|\n", c) == nullptr)
      {
        next_line (t, tt);

        string& l (t.value);
        if (l.compare (0, config.size (), config) != 0)
          bad_token (config + "* variable assignment");

        r.reflect = move (l);
      }
    }

    // If the dependencies are terminated with the newline, then check if the
    // next token is '{'. If that's the case, then this is a multi-line
    // representation.
    //
    next (t, tt);

    if (tt == type::newline)
    {
      next (t, tt);

      if (tt == type::lcbrace)
      {
        if (r.enable)
          throw parsing (
            *name_,
            t.line,
            t.column,
            "multi-line " + what + " form with inline enable clause");

        if (r.reflect)
          throw parsing (
            *name_,
            t.line,
            t.column,
            "multi-line " + what + " form with inline reflect clause");

        // Allow comments.
        //
        lexer_->comments = true;

        next (t, tt);
        expect_token (type::newline);

        // Parse the clauses.
        //
        for (next (t, tt); tt == type::word; next (t, tt))
        {
          auto fail_dup = [&t, this] ()
          {
            throw parsing (*name_, t.line, t.column, "duplicate clause");
          };

          auto fail_precede = [&t, this] (const char* what)
          {
            throw parsing (
              *name_,
              t.line,
              t.column,
              t.value + " clause should precede " + what + " clause");
          };

          auto fail_conflict = [&t, this] (const char* what)
          {
            throw parsing (
              *name_,
              t.line,
              t.column,
              t.value + " and " + what + " clauses are mutually exclusive");
          };

          auto fail_requirements = [&t, this] ()
          {
            throw parsing (
              *name_,
              t.line,
              t.column,
              t.value + " clause is not permitted for requirements");
          };

          // Parse the buildfile fragment block including the left and right
          // curly braces (expected to be on the separate lines) and return
          // the enclosed fragment.
          //
          // Note that an empty buildfile fragment is allowed.
          //
          auto parse_block = [&t, &tt, &expect_token, this] ()
          {
            next (t, tt);
            expect_token (type::newline);

            next (t, tt);
            expect_token (type::lcbrace);

            next (t, tt);
            expect_token (type::newline);

            next_block (t, tt);

            return move (t.value);
          };

          const string& v (t.value);

          if (v == "enable")
          {
            if (r.enable)
              fail_dup ();

            if (r.prefer)
              fail_precede ("prefer");

            if (r.require)
              fail_precede ("require");

            if (r.reflect)
              fail_precede ("reflect");

            r.enable = parse_eval ();

            next (t, tt);
            expect_token (type::newline);
          }
          else if (v == "prefer")
          {
            if (requirements_)
              fail_requirements ();

            if (r.prefer)
              fail_dup ();

            if (r.require)
              fail_conflict ("require");

            if (r.reflect)
              fail_precede ("reflect");

            r.prefer = parse_block ();

            // The accept clause must follow, so parse it.
            //
            next (t, tt);

            if (tt != type::word || t.value != "accept")
              bad_token ("accept clause");

            r.accept = parse_eval ();

            next (t, tt);
            expect_token (type::newline);
          }
          else if (v == "require")
          {
            if (requirements_)
              fail_requirements ();

            if (r.require)
              fail_dup ();

            if (r.prefer)
              fail_conflict ("prefer");

            if (r.reflect)
              fail_precede ("reflect");

            r.require = parse_block ();
          }
          else if (v == "reflect")
          {
            if (r.reflect)
              fail_dup ();

            r.reflect = parse_block ();
          }
          else if (v == "accept")
          {
            if (requirements_)
              fail_requirements ();

            throw parsing (*name_,
                           t.line,
                           t.column,
                           "accept clause should follow prefer clause");
          }
          else
            bad_token (what + " alternative clause");
        }

        expect_token (type::rcbrace);

        // Disallow comments.
        //
        lexer_->comments = false;

        next (t, tt);
      }
    }

    return r;
  }

  dependency_alternatives_parser::token_type dependency_alternatives_parser::
  next (token& t, token_type& tt)
  {
    t = lexer_->next ();
    tt = t.type;
    return tt;
  }

  dependency_alternatives_parser::token_type dependency_alternatives_parser::
  next_eval (token& t, token_type& tt)
  {
    t = lexer_->next_eval ();
    tt = t.type;
    return tt;
  }

  dependency_alternatives_parser::token_type dependency_alternatives_parser::
  next_line (token& t, token_type& tt)
  {
    t = lexer_->next_line ('|');
    tt = t.type;
    return tt;
  }

  dependency_alternatives_parser::token_type dependency_alternatives_parser::
  next_block (token& t, token_type& tt)
  {
    t = lexer_->next_block ();
    tt = t.type;
    return tt;
  }

  dependency_alternatives::
  dependency_alternatives (const std::string& s,
                           const package_name& dependent,
                           const std::string& name,
                           uint64_t line,
                           uint64_t column)
  {
    using std::string;

    auto vc (parser::split_comment (s));

    comment = move (vc.second);

    const string& v (vc.first);
    buildtime = (v[0] == '*');

    string::const_iterator b (v.begin ());
    string::const_iterator e (v.end ());

    if (buildtime)
    {
      string::size_type p (v.find_first_not_of (spaces, 1));
      b = p == string::npos ? e : b + p;
    }

    dependency_alternatives_parser p;
    istringstream is (b == v.begin () ? v : string (b, e));
    p.parse (dependent, is, name, line, column, *this);
  }

  string dependency_alternatives::
  string () const
  {
    std::string r (buildtime ? "* " : "");

    const dependency_alternative* prev (nullptr);
    for (const dependency_alternative& da: *this)
    {
      if (prev != nullptr)
      {
        r += prev->single_line () ? " |" : "\n|";
        r += !da.single_line () || !prev->single_line () ? '\n' : ' ';
      }

      r += da.string ();
      prev = &da;
    }

    return serializer::merge_comment (r, comment);
  }

  // requirement_alternative
  //
  string requirement_alternative::
  string () const
  {
    using std::string;

    string r (size () > 1 ? "{" : "");

    bool first (true);
    for (const string& rq: *this)
    {
      if (!first)
        r += ' ';
      else
        first = false;

      r += rq;
    }

    if (size () > 1)
      r += '}';

    if (single_line ())
    {
      if (enable)
      {
        if (!simple ())
        {
          r += " ? (";
          r += *enable;
          r += ')';
        }
        else
        {
          // Note that the (single) requirement id may or may not be empty.
          //
          if (!r.empty ())
            r += ' ';

          r += '?';

          if (!enable->empty ())
          {
            r += " (";
            r += *enable;
            r += ')';
          }
        }
      }

      if (reflect)
      {
        r += ' ';
        r += *reflect;
      }
    }
    else
    {
      r += "\n{";

      if (enable)
      {
        r += "\n  enable (";
        r += *enable;
        r += ')';
      }

      if (reflect)
      {
        if (enable)
          r += '\n';

        r += "\n  reflect\n  {\n";
        r += *reflect;
        r += "  }";
      }

      r += "\n}";
    }

    return r;
  }

  // requirement_alternatives
  //
  requirement_alternatives::
  requirement_alternatives (const std::string& s,
                            const package_name& dependent,
                            const std::string& name,
                            uint64_t line,
                            uint64_t column)
  {
    using std::string;

    auto vc (parser::split_comment (s));

    comment = move (vc.second);

    const string& v (vc.first);
    buildtime = (v[0] == '*');

    string::const_iterator b (v.begin ());
    string::const_iterator e (v.end ());

    if (buildtime)
    {
      string::size_type p (v.find_first_not_of (spaces, 1));
      b = p == string::npos ? e : b + p;
    }

    // We will use the dependency alternatives parser to parse the
    // representation into a temporary dependency alternatives in the
    // requirements mode. Then we will move the dependency alternatives into
    // the requirement alternatives using the string representation of the
    // dependencies.
    //
    dependency_alternatives_parser p (true /* requirements */);
    istringstream is (b == v.begin () ? v : string (b, e));

    dependency_alternatives das;
    p.parse (dependent, is, name, line, column, das);

    for (dependency_alternative& da: das)
    {
      requirement_alternative ra (move (da.enable), move (da.reflect));

      // Also handle the simple requirement.
      //
      for (dependency& d: da)
        ra.push_back (!d.name.empty () ? d.string () : string ());

      push_back (move (ra));
    }

    // Make sure that the simple requirement is accompanied with a non-empty
    // comment.
    //
    if (simple () && comment.empty ())
    {
      // Let's describe the following error cases differently:
      //
      // requires: ?
      // requires:
      //
      throw parsing (name,
                     line,
                     column,
                     (back ().enable
                      ? "no comment specified for simple requirement"
                      : "requirement or comment expected"));
    }
  }

  std::string requirement_alternatives::
  string () const
  {
    using std::string;

    string r (buildtime ? "* " : "");

    const requirement_alternative* prev (nullptr);
    for (const requirement_alternative& ra: *this)
    {
      if (prev != nullptr)
      {
        r += prev->single_line () ? " |" : "\n|";
        r += !ra.single_line () || !prev->single_line () ? '\n' : ' ';
      }

      r += ra.string ();
      prev = &ra;
    }

    // For better readability separate the comment from the question mark for
    // the simple requirement with an empty condition.
    //
    if (simple () && conditional () && back ().enable->empty ())
      r += ' ';

    return serializer::merge_comment (r, comment);
  }

  // build_class_term
  //
  build_class_term::
  ~build_class_term ()
  {
    if (simple)
      name.~string ();
    else
      expr.~vector<build_class_term> ();
  }

  build_class_term::
  build_class_term (build_class_term&& t) noexcept
      : operation (t.operation),
        inverted (t.inverted),
        simple (t.simple)
  {
    if (simple)
      new (&name) string (move (t.name));
    else
      new (&expr) vector<build_class_term> (move (t.expr));
  }

  build_class_term::
  build_class_term (const build_class_term& t)
      : operation (t.operation),
        inverted (t.inverted),
        simple (t.simple)
  {
    if (simple)
      new (&name) string (t.name);
    else
      new (&expr) vector<build_class_term> (t.expr);
  }

  build_class_term& build_class_term::
  operator= (build_class_term&& t) noexcept
  {
    if (this != &t)
    {
      this->~build_class_term ();

      // Rely on noexcept move-construction.
      //
      new (this) build_class_term (move (t));
    }
    return *this;
  }

  build_class_term& build_class_term::
  operator= (const build_class_term& t)
  {
    if (this != &t)
      *this = build_class_term (t); // Reduce to move-assignment.
    return *this;
  }

  bool build_class_term::
  validate_name (const string& s)
  {
    if (s.empty ())
      throw invalid_argument ("empty class name");

    size_t i (0);
    char c (s[i++]);

    if (!(alnum (c) || c == '_'))
      throw invalid_argument (
        "class name '" + s + "' starts with '" + c + '\'');

    for (; i != s.size (); ++i)
    {
      if (!(alnum (c = s[i]) || c == '+' || c == '-' || c == '_' || c == '.'))
        throw invalid_argument (
          "class name '" + s + "' contains '" + c + '\'');
    }

    return s[0] == '_';
  }

  // build_class_expr
  //
  // Parse the string representation of a space-separated, potentially empty
  // build class expression.
  //
  // Calls itself recursively when a nested expression is encountered. In this
  // case the second parameter points to the position at which the nested
  // expression starts (right after the opening parenthesis). Updates the
  // position to refer the nested expression end (right after the closing
  // parenthesis).
  //
  static vector<build_class_term>
  parse_build_class_expr (const string& s, size_t* p = nullptr)
  {
    vector<build_class_term> r;

    bool root (p == nullptr);
    size_t e (0);

    if (root)
      p = &e;

    size_t n;
    for (size_t b (0); (n = next_word (s, b, *p)); )
    {
      string t (s, b, n);

      // Check for the nested expression end.
      //
      if (t == ")")
      {
        if (root)
          throw invalid_argument ("class term expected instead of ')'");

        break;
      }

      // Parse the term.
      //
      char op (t[0]); // Can be '\0'.

      if (op != '+')
      {
        if (op != '-' && op != '&')
          throw invalid_argument (
            "class term '" + t + "' must start with '+', '-', or '&'");

        // Only the root expression may start with a term having the '-' or
        // '&' operation.
        //
        if (r.empty () && !root)
          throw invalid_argument (
            "class term '" + t + "' must start with '+'");
      }

      bool inv (t[1] == '!');     // Can be '\0'.
      string nm (t, inv ? 2 : 1);

      // Append the compound term.
      //
      if (nm == "(")
        r.emplace_back (parse_build_class_expr (s, p), op, inv);

      // Append the simple term.
      //
      else
      {
        build_class_term::validate_name (nm);
        r.emplace_back (move (nm), op, inv);
      }
    }

    // Verify that the nested expression is terminated with the closing
    // parenthesis and is not empty.
    //
    if (!root)
    {
      // The zero-length of the last term means that we escaped the loop due
      // to the eos.
      //
      if (n == 0)
        throw invalid_argument (
          "nested class expression must be closed with ')'");

      if (r.empty ())
        throw invalid_argument ("empty nested class expression");
    }

    return r;
  }

  build_class_expr::
  build_class_expr (const std::string& s, std::string c)
      : comment (move (c))
  {
    using std::string;

    size_t eb (0); // Start of expression.

    // Parse the underlying classes until the expression term, ':', or eos is
    // encountered.
    //
    for (size_t b (0); next_word (s, b, eb); )
    {
      string nm (s, b, eb - b);

      if (nm[0] == '+' || nm[0] == '-' || nm[0] == '&')
      {
        // Expression must always be separated with ':' from the underlying
        // classes.
        //
        if (!underlying_classes.empty ())
          throw invalid_argument ("class expression separator ':' expected");

        eb = b; // Reposition to the term beginning.
        break;
      }
      else if (nm == ":")
      {
        // The ':' separator must follow the underlying class set.
        //
        if (underlying_classes.empty ())
          throw invalid_argument ("underlying class set expected");

        break;
      }

      build_class_term::validate_name (nm);
      underlying_classes.emplace_back (move (nm));
    }

    expr = parse_build_class_expr (eb == 0 ? s : string (s, eb));

    // At least one of the expression or underlying class set should be
    // present in the representation.
    //
    if (expr.empty () && underlying_classes.empty ())
      throw invalid_argument ("empty class expression");
  }

  build_class_expr::
  build_class_expr (const strings& cs, char op, std::string c)
      : comment (move (c))
  {
    vector<build_class_term> r;

    for (const std::string& c: cs)
      r.emplace_back (c, op == '-' ? '-' : '+', false /* inverse */);

    if (op == '&' && !r.empty ())
    {
      build_class_term t (move (r), '&', false /* inverse */);
      r = vector<build_class_term> ({move (t)});
    }

    expr = move (r);
  }

  // Return string representation of the build class expression.
  //
  static string
  to_string (const vector<build_class_term>& expr)
  {
    string r;
    for (const build_class_term& t: expr)
    {
      if (!r.empty ())
        r += ' ';

      r += t.operation;

      if (t.inverted)
        r += '!';

      r += t.simple ? t.name : "( " + to_string (t.expr) + " )";
    }

    return r;
  }

  string build_class_expr::
  string () const
  {
    using std::string;

    string r;
    for (const string& c: underlying_classes)
    {
      if (!r.empty ())
        r += ' ';

      r += c;
    }

    if (!expr.empty ())
    {
      if (!r.empty ())
        r += " : " + to_string (expr);
      else
        r = to_string (expr);
    }

    return r;
  }

  // Match build configuration classes against an expression, updating the
  // result.
  //
  static void
  match_classes (const strings& cs,
                 const build_class_inheritance_map& im,
                 const vector<build_class_term>& expr,
                 bool& r)
  {
    for (const build_class_term& t: expr)
    {
      // Note that the '+' operation may only invert false and the '-' and '&'
      // operations may only invert true (see below). So, let's optimize it a
      // bit.
      //
      if ((t.operation == '+') == r)
        continue;

      bool m (false);

      // We don't expect the class list to be long, so the linear search should
      // be fine.
      //
      if (t.simple)
      {
        // Check if any of the classes or their bases match the term name.
        //
        for (const string& c: cs)
        {
          m = (c == t.name);

          if (!m)
          {
            // Go through base classes.
            //
            for (auto i (im.find (c)); i != im.end (); )
            {
              const string& base (i->second);

              // Bail out if the base class matches.
              //
              m = (base == t.name);

              if (m)
                break;

              i = im.find (base);
            }
          }

          if (m)
            break;
        }
      }
      else
        match_classes (cs, im, t.expr, m);

      if (t.inverted)
        m = !m;

      switch (t.operation)
      {
      case '+': if (m) r = true;  break;
      case '-': if (m) r = false; break;
      case '&': r &= m;           break;
      default:  assert (false);
      }
    }
  }

  void build_class_expr::
  match (const strings& cs,
         const build_class_inheritance_map& im,
         bool& r) const
  {
    match_classes (cs, im, expr, r);
  }

  // build_auxiliary
  //
  optional<pair<string, string>> build_auxiliary::
  parse_value_name (const string& n)
  {
    // Check if the value name matches exactly.
    //
    if (n == "build-auxiliary")
      return make_pair (string (), string ());

    // Check if this is a *-build-auxiliary name.
    //
    if (n.size () > 16 &&
        n.compare (n.size () - 16, 16, "-build-auxiliary") == 0)
    {
      return make_pair (string (n, 0, n.size () - 16), string ());
    }

    // Check if this is a build-auxiliary-* name.
    //
    if (n.size () > 16 && n.compare (0, 16, "build-auxiliary-") == 0)
      return make_pair (string (), string (n, 16));

    // Check if this is a *-build-auxiliary-* name.
    //
    size_t p (n.find ("-build-auxiliary-"));

    if (p != string::npos   &&
        p != 0              && // Not '-build-auxiliary-*'?
        p + 17 != n.size () && // Not '*-build-auxiliary-'?
        n.find ("-build-auxiliary-", p + 17) == string::npos) // Unambiguous?
    {
      return make_pair (string (n, 0, p), string (n, p + 17));
    }

    return nullopt;
  }

  // test_dependency_type
  //
  string
  to_string (test_dependency_type t)
  {
    switch (t)
    {
    case test_dependency_type::tests:      return "tests";
    case test_dependency_type::examples:   return "examples";
    case test_dependency_type::benchmarks: return "benchmarks";
    }

    assert (false); // Can't be here.
    return string ();
  }

  test_dependency_type
  to_test_dependency_type (const string& t)
  {
         if (t == "tests")      return test_dependency_type::tests;
    else if (t == "examples")   return test_dependency_type::examples;
    else if (t == "benchmarks") return test_dependency_type::benchmarks;
    else throw invalid_argument ("invalid test dependency type '" + t + '\'');
  }


  // test_dependency
  //
  test_dependency::
  test_dependency (std::string v, test_dependency_type t)
      : type (t)
  {
    using std::string;

    // We will use the dependency alternatives parser to parse the
    // `<name> [<version-constraint>] ['?' <enable-condition>] [<reflect-config>]`
    // representation into a temporary dependency alternatives object. Then we
    // will verify that the result has no multiple alternatives/dependency
    // packages and unexpected clauses and will move the required information
    // (dependency, reflection, etc) into the being created test dependency
    // object.

    // Verify that there is no newline characters to forbid the multi-line
    // dependency alternatives representation.
    //
    if (v.find ('\n') != string::npos)
      throw invalid_argument ("unexpected <newline>");

    buildtime = (v[0] == '*');

    size_t p (v.find_first_not_of (spaces, buildtime ? 1 : 0));

    if (p == string::npos)
      throw invalid_argument ("no package name specified");

    string::const_iterator b (v.begin () + p);
    string::const_iterator e (v.end ());

    // Extract the dependency package name in advance, to pass it to the
    // parser which will use it to verify the reflection variable name.
    //
    // Note that multiple packages can only be specified in {} to be accepted
    // by the parser. In our case such '{' would be interpreted as a part of
    // the package name and so would fail complaining about an invalid
    // character. Let's handle this case manually to avoid the potentially
    // confusing error description.
    //
    assert (b != e); // We would fail earlier otherwise.

    if (*b == '{')
      throw invalid_argument ("only single package allowed");

    package_name dn;

    try
    {
      p = v.find_first_of (" \t=<>[(~^", p); // End of the package name.
      dn = package_name (string (b, p == string::npos ? e : v.begin () + p));
    }
    catch (const invalid_argument& e)
    {
      throw invalid_argument (string ("invalid package name: ") + e.what ());
    }

    // Parse the value into the temporary dependency alternatives object.
    //
    dependency_alternatives das;

    try
    {
      dependency_alternatives_parser p;
      istringstream is (b == v.begin () ? v : string (b, e));
      p.parse (dn, is, "" /* name */, 1, 1, das);
    }
    catch (const manifest_parsing& e)
    {
      throw invalid_argument (e.description);
    }

    // Verify that there are no multiple dependency alternatives.
    //
    assert (!das.empty ()); // Enforced by the parser.

    if (das.size () != 1)
      throw invalid_argument ("unexpected '|'");

    dependency_alternative& da (das[0]);

    // Verify that there are no multiple dependencies in the alternative.
    //
    // The parser can never end up with no dependencies in an alternative and
    // we already verified that there can't be multiple of them (see above).
    //
    assert (da.size () == 1);

    // Verify that there are no unexpected clauses.
    //
    // Note that the require, prefer, and accept clauses can only be present
    // in the multi-line representation and we have already verified that this
    // is not the case. So there is nothing to verify here.

    // Move the dependency and the enable and reflect clauses into the being
    // created test dependency object.
    //
    static_cast<dependency&> (*this) = move (da[0]);

    enable  = move (da.enable);
    reflect = move (da.reflect);
  }

  string test_dependency::
  string () const
  {
    std::string r (buildtime
                   ? "* " + dependency::string ()
                   :        dependency::string ());

    if (enable)
    {
      r += " ? (";
      r += *enable;
      r += ')';
    }

    if (reflect)
    {
      r += ' ';
      r += *reflect;
    }

    return r;
  }

  // pkg_package_manifest
  //
  static build_class_expr
  parse_build_class_expr (const name_value& nv,
                          bool first,
                          const string& source_name)
  {
    pair<string, string> vc (parser::split_comment (nv.value));
    string& v (vc.first);
    string& c (vc.second);

    auto bad_value = [&v, &nv, &source_name] (const string& d,
                                              const invalid_argument& e)
    {
      throw !source_name.empty ()
            ? parsing (source_name,
                       nv.value_line, nv.value_column,
                       d + ": " + e.what ())
            : parsing (d + " in '" + v + "': " + e.what ());
    };

    build_class_expr r;

    try
    {
      r = build_class_expr (v, move (c));

      // Underlying build configuration class set may appear only in the
      // first builds value.
      //
      if (!r.underlying_classes.empty () && !first)
        throw invalid_argument ("unexpected underlying class set");
    }
    catch (const invalid_argument& e)
    {
      bad_value ("invalid package builds", e);
    }

    return r;
  }

  static build_constraint
  parse_build_constraint (const name_value& nv,
                          bool exclusion,
                          const string& source_name)
  {
    pair<string, string> vc (parser::split_comment (nv.value));
    string& v (vc.first);
    string& c (vc.second);

    auto bad_value = [&v, &nv, &source_name] (const string& d)
    {
      throw !source_name.empty ()
            ? parsing (source_name, nv.value_line, nv.value_column, d)
            : parsing (d + " in '" + v + '\'');
    };

    size_t p (v.find ('/'));
    string nm (p != string::npos ? v.substr (0, p) : move (v));

    optional<string> tg (p != string::npos
                         ? optional<string> (string (v, p + 1))
                         : nullopt);

    if (nm.empty ())
      bad_value ("empty build configuration name pattern");

    if (tg && tg->empty ())
      bad_value ("empty build target pattern");

    return build_constraint (exclusion, move (nm), move (tg), move (c));
  }

  static email
  parse_email (const name_value& nv,
               const char* what,
               const string& source_name,
               bool empty = false)
  {
    auto bad_value = [&nv, &source_name] (const string& d)
    {
      throw !source_name.empty ()
            ? parsing (source_name, nv.value_line, nv.value_column, d)
            : parsing (d);
    };

    pair<string, string> vc (parser::split_comment (nv.value));
    string& v (vc.first);
    string& c (vc.second);

    if (v.empty () && !empty)
      bad_value (string ("empty ") + what + " email");

    return email (move (v), move (c));
  }

  // Parse the [*-]build-auxiliary[-*] manifest value.
  //
  // Note that the environment name is expected to already be retrieved using
  // build_auxiliary::parse_value_name().
  //
  static build_auxiliary
  parse_build_auxiliary (const name_value& nv,
                         string&& env_name,
                         const string& source_name)
  {
    auto bad_value = [&nv, &source_name] (const string& d)
    {
      throw !source_name.empty ()
            ? parsing (source_name, nv.value_line, nv.value_column, d)
            : parsing (d);
    };

    pair<string, string> vc (parser::split_comment (nv.value));
    string& v (vc.first);
    string& c (vc.second);

    if (v.empty ())
      bad_value ("empty build auxiliary configuration name pattern");

    return build_auxiliary (move (env_name), move (v), move (c));
  }

  // Parse the [*-]build-bot manifest value and append it to the specified
  // custom bot public keys list. Make sure the specified key is not empty and
  // is not a duplicate and throw parsing if that's not the case.
  //
  // Note: value name is not used by this function (and so can be moved out,
  // etc before the call).
  //
  static void
  parse_build_bot (const name_value& nv, const string& source_name, strings& r)
  {
    const string& v (nv.value);

    auto bad_value = [&nv, &source_name, &v] (const string& d,
                                              bool add_key = true)
    {
      throw !source_name.empty ()
            ? parsing (source_name, nv.value_line, nv.value_column, d)
            : parsing (!add_key ? d : (d + ":\n" + v));
    };

    if (v.empty ())
      bad_value ("empty custom build bot public key", false /* add_key */);

    if (find (r.begin (), r.end (), v) != r.end ())
      bad_value ("duplicate custom build bot public key");

    r.push_back (v);
  }

  const version stub_version (0, "0", nullopt, nullopt, 0);

  // Parse until next() returns end-of-manifest value.
  //
  static void
  parse_package_manifest (
    const string& name,
    const function<name_value ()>& next,
    const function<package_manifest::translate_function>& translate,
    bool iu,
    bool cv,
    package_manifest_flags fl,
    package_manifest& m)
  {
    name_value nv;

    auto bad_name ([&name, &nv](const string& d) {
        throw parsing (name, nv.name_line, nv.name_column, d);});

    auto bad_value ([&name, &nv](const string& d) {
        throw parsing (name, nv.value_line, nv.value_column, d);});

    auto parse_email = [&bad_name, &name] (const name_value& nv,
                                           optional<email>& r,
                                           const char* what,
                                           bool empty = false)
    {
      if (r)
        bad_name (what + string (" email redefinition"));

      r = bpkg::parse_email (nv, what, name, empty);
    };

    // Parse the [*-]build-auxiliary[-*] manifest value and append it to the
    // specified build auxiliary list. Make sure that the list contains not
    // more than one entry with unspecified environment name and throw parsing
    // if that's not the case. Also make sure that there are no entry
    // redefinitions (multiple entries with the same environment name).
    //
    auto parse_build_auxiliary = [&bad_name, &name] (const name_value& nv,
                                                     string&& en,
                                                     vector<build_auxiliary>& r)
    {
      build_auxiliary a (bpkg::parse_build_auxiliary (nv, move (en), name));

      if (find_if (r.begin (), r.end (),
                   [&a] (const build_auxiliary& ba)
                   {
                     return ba.environment_name == a.environment_name;
                   }) != r.end ())
        bad_name ("build auxiliary environment redefinition");

      r.push_back (move (a));
    };

    auto parse_url = [&bad_value] (const string& v,
                                   const char* what) -> manifest_url
    {
      auto p (parser::split_comment (v));

      if (v.empty ())
        bad_value (string ("empty ") + what + " url");

      manifest_url r;

      try
      {
        r = manifest_url (p.first, move (p.second));
      }
      catch (const invalid_argument& e)
      {
        bad_value (string ("invalid ") + what + " url: " + e.what ());
      }

      return r;
    };

    // Parse a string list (topics, keywords, etc). If the list length exceeds
    // five entries, then truncate it if the truncate flag is set and throw
    // manifest_parsing otherwise.
    //
    auto parse_list = [&bad_name, &bad_value] (const string& v,
                                               small_vector<string, 5>& r,
                                               char delim,
                                               bool single_word,
                                               bool truncate,
                                               const char* what)
    {
      if (!r.empty ())
        bad_name (string ("package ") + what + " redefinition");

      // Note that we parse the whole list to validate the entries.
      //
      list_parser lp (v.begin (), v.end (), delim);
      for (string lv (lp.next ()); !lv.empty (); lv = lp.next ())
      {
        if (single_word && lv.find_first_of (spaces) != string::npos)
          bad_value (string ("only single-word ") + what + " allowed");

        r.push_back (move (lv));
      }

      if (r.empty ())
        bad_value (string ("empty package ") + what + " specification");

      // If the list length limit is exceeded then truncate it or throw.
      //
      if (r.size () > 5)
      {
        if (truncate)
          r.resize (5);
        else
          bad_value (string ("up to five ") + what + " allowed");
      }
    };

    // Note: the n argument is the distribution name length.
    //
    auto parse_distribution = [&bad_name, &bad_value] (string&& nm, size_t n,
                                                       string&& vl)
    {
      size_t p (nm.find ('-'));

      // Distribution-related manifest value name always has a dash-starting
      // suffix (-name, etc).
      //
      assert (p != string::npos);

      if (p < n)
        bad_name ("distribution name '" + string (nm, 0, n) + "' contains '-'");

      if (vl.empty ())
        bad_value ("empty package distribution value");

      return distribution_name_value (move (nm), move (vl));
    };

    auto add_distribution = [&m, &bad_name] (distribution_name_value&& nv,
                                             bool unique)
    {
      vector<distribution_name_value>& dvs (m.distribution_values);

      if (unique &&
          find_if (dvs.begin (), dvs.end (),
                   [&nv] (const distribution_name_value& dnv)
                   {return dnv.name == nv.name;}) != dvs.end ())
      {
        bad_name ("package distribution value redefinition");
      }

      dvs.push_back (move (nv));
    };

    auto flag = [fl] (package_manifest_flags f)
    {
      return (fl & f) != package_manifest_flags::none;
    };

    // Based on the buildfile path specified via the `*-build[2]` value name
    // or the `build-file` value set the manifest's alt_naming flag if absent
    // and verify that it doesn't change otherwise. If it does, then return
    // the error description and nullopt otherwise.
    //
    auto alt_naming = [&m] (const string& p) -> optional<string>
    {
      assert (!p.empty ());

      bool an (p.back () == '2');

      if (!m.alt_naming)
        m.alt_naming = an;
      else if (*m.alt_naming != an)
        return string (*m.alt_naming ? "alternative" : "standard") +
               " buildfile naming scheme is already used";

      return nullopt;
    };

    // Try to parse and verify the buildfile path specified via the
    // `*-build[2]` value name or the `build-file` value and set the
    // manifest's alt_naming flag. On success return the normalized path with
    // the suffix stripped and nullopt and the error description
    // otherwise. Expects that the prefix is not empty.
    //
    // Specifically, verify that the path doesn't contain backslashes, is
    // relative, doesn't refer outside the packages's build subdirectory, and
    // was not specified yet. Also verify that the file name is not empty.
    //
    auto parse_buildfile_path =
      [&m, &alt_naming] (string&& p, string& err) -> optional<path>
      {
        if (optional<string> e = alt_naming (p))
        {
          err = move (*e);
          return nullopt;
        }

        // Verify that the path doesn't contain backslashes which would be
        // interpreted differently on Windows and POSIX.
        //
        if (p.find ('\\') != string::npos)
        {
          err = "backslash in package buildfile path";
          return nullopt;
        }

        // Strip the '(-|.)build' suffix.
        //
        size_t n (*m.alt_naming ? 7 : 6);
        assert (p.size () > n);

        p.resize (p.size () - n);

        try
        {
          path f (move (p));

          // Fail if the value name is something like `config/-build`.
          //
          if (f.to_directory ())
          {
            err = "empty package buildfile name";
            return nullopt;
          }

          if (f.absolute ())
          {
            err = "absolute package buildfile path";
            return nullopt;
          }

          // Verify that the path refers inside the package's build/
          // subdirectory.
          //
          f.normalize (); // Note: can't throw since the path is relative.

          if (dir_path::traits_type::parent (*f.begin ()))
          {
            err = "package buildfile path refers outside build/ subdirectory";
            return nullopt;
          }

          // Check for duplicates.
          //
          const vector<buildfile>& bs (m.buildfiles);
          const vector<path>& bps (m.buildfile_paths);

          if (find_if (bs.begin (), bs.end (),
                       [&f] (const auto& v) {return v.path == f;})
              != bs.end () ||
              find (bps.begin (), bps.end (), f) != bps.end ())
          {
            err = "package buildfile redefinition";
            return nullopt;
          }

          return f;
        }
        catch (const invalid_path&)
        {
          err = "invalid package buildfile path";
          return nullopt;
        }
      };

    // Return the package build configuration with the specified name, if
    // already exists. If no configuration matches, then create one, if
    // requested, and throw manifest_parsing otherwise. If the new
    // configuration creation is not allowed, then the description for a
    // potential manifest_parsing exception needs to also be specified.
    //
    auto build_conf = [&m, &bad_name] (string&& nm,
                                       bool create = true,
                                       const string& desc = "")
      -> build_package_config&
    {
      // The error description must only be specified if the creation of the
      // package build configuration is not allowed.
      //
      assert (desc.empty () == create);

      small_vector<build_package_config, 1>& cs (m.build_configs);

      auto i (find_if (cs.begin (), cs.end (),
                       [&nm] (const build_package_config& c)
                       {return c.name == nm;}));

      if (i != cs.end ())
        return *i;

      if (!create)
        bad_name (desc + ": no build package configuration '" + nm + '\'');

      // Add the new build configuration (arguments, builds, etc will come
      // later).
      //
      cs.emplace_back (move (nm));
      return cs.back ();
    };

    // Cache the upstream version manifest value and validate whether it's
    // allowed later, after the version value is parsed.
    //
    optional<name_value> upstream_version;

    // We will cache the depends and the test dependency manifest values to
    // parse and, if requested, complete the version constraints later, after
    // the version value is parsed. We will also cache the requires values to
    // parse them later, after the package name is parsed.
    //
    vector<name_value> dependencies;
    vector<name_value> requirements;
    small_vector<name_value, 1> tests;

    // We will cache the descriptions and changes and their type values to
    // validate them later, after all are parsed.
    //
    optional<name_value> description;
    optional<name_value> description_type;
    optional<name_value> package_description;
    optional<name_value> package_description_type;
    vector<name_value>   changes;
    optional<name_value> changes_type;

    // It doesn't make sense for only emails to be specified for a package
    // build configuration. Thus, we will cache the build configuration email
    // manifest values to parse them later, after all other build
    // configuration values are parsed, and to make sure that the build
    // configurations they refer to are also specified.
    //
    vector<name_value> build_config_emails;
    vector<name_value> build_config_warning_emails;
    vector<name_value> build_config_error_emails;

    m.build_configs.emplace_back ("default");

    for (nv = next (); !nv.empty (); nv = next ())
    {
      string& n (nv.name);
      string& v (nv.value);

      if (n == "name")
      {
        if (!m.name.empty ())
          bad_name ("package name redefinition");

        try
        {
          m.name = package_name (move (v));
        }
        catch (const invalid_argument& e)
        {
          bad_value (string ("invalid package name: ") + e.what ());
        }
      }
      else if (n == "version")
      {
        if (!m.version.empty ())
          bad_name ("package version redefinition");

        try
        {
          m.version = version (v);
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

        if (translate)
        {
          translate (m.version);

          // Re-validate the version after the translation.
          //
          // The following description will be confusing for the end user.
          // However, they shouldn't ever see it unless the translation
          // function is broken.
          //
          if (m.version.empty ())
            bad_value ("empty translated package version");

          if (m.version.release && m.version.release->empty ())
            bad_value ("invalid translated package version " +
                       m.version.string () + ": earliest release");
        }
      }
      else if (n == "upstream-version")
      {
        if (upstream_version)
          bad_name ("upstream package version redefinition");

        if (v.empty ())
          bad_value ("empty upstream package version");

        upstream_version = move (nv);
      }
      else if (n == "type")
      {
        if (m.type)
          bad_name ("package type redefinition");

        if (v.empty () || v.find (',') == 0)
          bad_value ("empty package type");

        m.type = move (v);
      }
      else if (n == "language")
      {
        // Strip the language extra information, if present.
        //
        size_t p (v.find (','));
        if (p != string::npos)
          v.resize (p);

        // Determine the language impl flag.
        //
        bool impl (false);
        p = v.find ('=');
        if (p != string::npos)
        {
          string s (trim (string (v, p + 1)));
          if (s != "impl")
            bad_value (!s.empty ()
                       ? "unexpected '" + s + "' value after '='"
                       : "expected 'impl' after '='");

          impl = true;

          v.resize (p);
        }

        // Finally, validate and add the language.
        //
        trim_right (v);

        if (v.empty ())
          bad_value ("empty package language");

        if (find_if (m.languages.begin (), m.languages.end (),
                     [&v] (const language& l) {return l.name == v;}) !=
            m.languages.end ())
          bad_value ("duplicate package language");

        m.languages.emplace_back (move (v), impl);
      }
      else if (n == "project")
      {
        if (m.project)
          bad_name ("package project redefinition");

        try
        {
          m.project = package_name (move (v));
        }
        catch (const invalid_argument& e)
        {
          bad_value (string ("invalid project name: ") + e.what ());
        }
      }
      else if (n == "summary")
      {
        if (!m.summary.empty ())
          bad_name ("package summary redefinition");

        if (v.empty ())
          bad_value ("empty package summary");

        m.summary = move (v);
      }
      else if (n == "topics")
      {
        parse_list (v,
                    m.topics,
                    ','   /* delim */,
                    false /* single_word */,
                    false /* truncate */,
                    "topics");
      }
      else if (n == "keywords")
      {
        parse_list (v,
                    m.keywords,
                    ' '   /* delim */,
                    true  /* single_word */,
                    false /* truncate */,
                    "keywords");
      }
      else if (n == "tags")
      {
        parse_list (v,
                    m.keywords,
                    ','  /* delim */,
                    true /* single_word */,
                    true /* truncate */,
                    "tags");
      }
      else if (n == "description")
      {
        if (description)
        {
          if (description->name == "description-file")
            bad_name ("project description and description file are "
                      "mutually exclusive");
          else
            bad_name ("project description redefinition");
        }

        if (v.empty ())
          bad_value ("empty project description");

        description = move (nv);
      }
      else if (n == "description-file")
      {
        if (flag (package_manifest_flags::forbid_file))
          bad_name ("project description file not allowed");

        if (description)
        {
          if (description->name == "description-file")
            bad_name ("project description file redefinition");
          else
            bad_name ("project description file and description are "
                      "mutually exclusive");
        }

        description = move (nv);
      }
      else if (n == "description-type")
      {
        if (description_type)
          bad_name ("project description type redefinition");

        description_type = move (nv);
      }
      else if (n == "package-description")
      {
        if (package_description)
        {
          if (package_description->name == "package-description-file")
            bad_name ("package description and description file are "
                      "mutually exclusive");
          else
            bad_name ("package description redefinition");
        }

        if (v.empty ())
          bad_value ("empty package description");

        package_description = move (nv);
      }
      else if (n == "package-description-file")
      {
        if (flag (package_manifest_flags::forbid_file))
          bad_name ("package description file not allowed");

        if (package_description)
        {
          if (package_description->name == "package-description-file")
            bad_name ("package description file redefinition");
          else
            bad_name ("package description file and description are "
                      "mutually exclusive");
        }

        package_description = move (nv);
      }
      else if (n == "package-description-type")
      {
        if (package_description_type)
          bad_name ("package description type redefinition");

        package_description_type = move (nv);
      }
      else if (n == "changes")
      {
        if (v.empty ())
          bad_value ("empty package changes specification");

        changes.emplace_back (move (nv));
      }
      else if (n == "changes-file")
      {
        if (flag (package_manifest_flags::forbid_file))
          bad_name ("package changes-file not allowed");

        changes.emplace_back (move (nv));
      }
      else if (n == "changes-type")
      {
        if (changes_type)
          bad_name ("package changes type redefinition");

        changes_type = move (nv);
      }
      else if (n == "url")
      {
        if (m.url)
          bad_name ("project url redefinition");

        m.url = parse_url (v, "project");
      }
      else if (n == "email")
      {
        parse_email (nv, m.email, "project");
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
        parse_email (nv, m.package_email, "package");
      }
      else if (n == "build-email")
      {
        parse_email (nv, m.build_email, "build", true /* empty */);
      }
      else if (n == "build-warning-email")
      {
        parse_email (nv, m.build_warning_email, "build warning");
      }
      else if (n == "build-error-email")
      {
        parse_email (nv, m.build_error_email, "build error");
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
        {
          // Reserve the license schemes for the future use and only recognize
          // the 'other' scheme for now, if specified. By default, the 'spdx'
          // scheme is implied.
          //
          // Note that if the substring that precedes ':' contains the
          // 'DocumentRef-' substring, then this is not a license scheme but
          // the license is a SPDX License Expression (see SPDX user defined
          // license reference for details).
          //
          size_t p (lv.find (':'));

          if (p != string::npos            &&
              lv.find ("DocumentRef-") > p &&
              lv.compare (0, p, "other") != 0)
            bad_value ("invalid package license scheme");

          l.push_back (move (lv));
        }

        if (l.empty ())
          bad_value ("empty package license specification");

        m.license_alternatives.push_back (move (l));
      }
      else if (n == "depends")
      {
        dependencies.push_back (move (nv));
      }
      else if (n == "requires")
      {
        requirements.push_back (move (nv));
      }
      else if (n == "builds")
      {
        m.builds.push_back (
          parse_build_class_expr (nv, m.builds.empty (), name));
      }
      else if (n == "build-include")
      {
        m.build_constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, name));
      }
      else if (n == "build-exclude")
      {
        m.build_constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, name));
      }
      else if (optional<pair<string, string>> ba =
               build_auxiliary::parse_value_name (n))
      {
        if (ba->first.empty ()) // build-auxiliary*?
        {
          parse_build_auxiliary (nv, move (ba->second), m.build_auxiliaries);
        }
        else                    // *-build-auxiliary*
        {
          build_package_config& bc (build_conf (move (ba->first)));
          parse_build_auxiliary (nv, move (ba->second), bc.auxiliaries);
        }
      }
      else if (n == "build-bot")
      {
        parse_build_bot (nv, name, m.build_bot_keys);
      }
      else if (n.size () > 13 &&
               n.compare (n.size () - 13, 13, "-build-config") == 0)
      {
        auto vc (parser::split_comment (v));

        n.resize (n.size () - 13);

        build_package_config& bc (build_conf (move (n)));

        if (!bc.arguments.empty () || !bc.comment.empty ())
          bad_name ("build configuration redefinition");

        bc.arguments = move (vc.first);
        bc.comment = move (vc.second);
      }
      else if (n.size () > 7 && n.compare (n.size () - 7, 7, "-builds") == 0)
      {
        n.resize (n.size () - 7);

        build_package_config& bc (build_conf (move (n)));

        bc.builds.push_back (
          parse_build_class_expr (nv, bc.builds.empty (), name));
      }
      else if (n.size () > 14 &&
               n.compare (n.size () - 14, 14, "-build-include") == 0)
      {
        n.resize (n.size () - 14);

        build_package_config& bc (build_conf (move (n)));

        bc.constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, name));
      }
      else if (n.size () > 14 &&
               n.compare (n.size () - 14, 14, "-build-exclude") == 0)
      {
        n.resize (n.size () - 14);

        build_package_config& bc (build_conf (move (n)));

        bc.constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, name));
      }
      else if (n.size () > 10 &&
               n.compare (n.size () - 10, 10, "-build-bot") == 0)
      {
        n.resize (n.size () - 10);

        build_package_config& bc (build_conf (move (n)));
        parse_build_bot (nv, name, bc.bot_keys);
      }
      else if (n.size () > 12 &&
               n.compare (n.size () - 12, 12, "-build-email") == 0)
      {
        n.resize (n.size () - 12);
        build_config_emails.push_back (move (nv));
      }
      else if (n.size () > 20 &&
               n.compare (n.size () - 20, 20, "-build-warning-email") == 0)
      {
        n.resize (n.size () - 20);
        build_config_warning_emails.push_back (move (nv));
      }
      else if (n.size () > 18 &&
               n.compare (n.size () - 18, 18, "-build-error-email") == 0)
      {
        n.resize (n.size () - 18);
        build_config_error_emails.push_back (move (nv));
      }
      // @@ TMP time to drop *-0.14.0?
      //
      else if (n == "tests"      || n == "tests-0.14.0"    ||
               n == "examples"   || n == "examples-0.14.0" ||
               n == "benchmarks" || n == "benchmarks-0.14.0")
      {
        // Strip the '-0.14.0' suffix from the value name, if present.
        //
        size_t p (n.find ('-'));
        if (p != string::npos)
          n.resize (p);

        tests.push_back (move (nv));
      }
      else if (n == "bootstrap-build" || n == "bootstrap-build2")
      {
        if (optional<string> e = alt_naming (n))
          bad_name (*e);

        if (m.bootstrap_build)
          bad_name ("package " + n + " redefinition");

        m.bootstrap_build = move (v);
      }
      else if (n == "root-build" || n == "root-build2")
      {
        if (optional<string> e = alt_naming (n))
          bad_name (*e);

        if (m.root_build)
          bad_name ("package " + n + " redefinition");

        m.root_build = move (v);
      }
      else if ((n.size () > 6 && n.compare (n.size () - 6, 6, "-build") == 0) ||
               (n.size () > 7 && n.compare (n.size () - 7, 7, "-build2") == 0))
      {
        string err;
        if (optional<path> p = parse_buildfile_path (move (n), err))
          m.buildfiles.push_back (buildfile (move (*p), move (v)));
        else
          bad_name (err);
      }
      else if (n == "build-file")
      {
        if (flag (package_manifest_flags::forbid_file))
          bad_name ("package build-file not allowed");

        // Verify that the buildfile extension is either build or build2.
        //
        if ((v.size () > 6 && v.compare (v.size () - 6, 6, ".build") == 0) ||
            (v.size () > 7 && v.compare (v.size () - 7, 7, ".build2") == 0))
        {
          string err;
          if (optional<path> p = parse_buildfile_path (move (v), err))
          {
            // Verify that the resulting path differs from bootstrap and root.
            //
            const string& s (p->string ());
            if (s == "bootstrap" || s == "root")
              bad_value (s + " not allowed");

            m.buildfile_paths.push_back (move (*p));
          }
          else
            bad_value (err);
        }
        else
          bad_value ("path with build or build2 extension expected");

      }
      else if (n.size () > 5 && n.compare (n.size () - 5, 5, "-name") == 0)
      {
        add_distribution (
          parse_distribution (move (n), n.size () - 5, move (v)),
          false /* unique */);
      }
      // Note: must precede the check for the "-version" suffix.
      //
      else if (n.size () > 22 &&
               n.compare (n.size () - 22, 22, "-to-downstream-version") == 0)
      {
        add_distribution (
          parse_distribution (move (n), n.size () - 22, move (v)),
          false /* unique */);
      }
      // Note: must follow the check for "upstream-version".
      //
      else if (n.size () > 8 && n.compare (n.size () - 8, 8, "-version") == 0)
      {
        // If the value is forbidden then throw, but only after the name is
        // validated. Thus, check for that before we move the value from.
        //
        bool bad (v == "$" &&
                  flag (package_manifest_flags::forbid_incomplete_values));

        // Can throw.
        //
        distribution_name_value d (
          parse_distribution (move (n), n.size () - 8, move (v)));

        if (bad)
          bad_value ("$ not allowed");

        add_distribution (move (d), true /* unique */);
      }
      else if (n == "location")
      {
        if (flag (package_manifest_flags::forbid_location))
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
        if (flag (package_manifest_flags::forbid_sha256sum))
          bad_name ("package sha256sum not allowed");

        if (m.sha256sum)
          bad_name ("package sha256sum redefinition");

        if (!valid_sha256 (v))
          bad_value ("invalid package sha256sum");

        m.sha256sum = move (v);
      }
      else if (n == "fragment")
      {
        if (flag (package_manifest_flags::forbid_fragment))
          bad_name ("package repository fragment not allowed");

        if (m.fragment)
          bad_name ("package repository fragment redefinition");

        if (v.empty ())
          bad_value ("empty package repository fragment");

        m.fragment = move (v);
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
    else if (m.license_alternatives.empty ())
      bad_value ("no project license specified");

    // Verify that the upstream version is not specified for a stub.
    //
    if (upstream_version)
    {
      // Restore as bad_name() uses its line/column.
      //
      nv = move (*upstream_version);

      if (m.version.compare (stub_version, true /* ignore_revision */) == 0)
        bad_name ("upstream package version specified for a stub");

      m.upstream_version = move (nv.value);
    }

    // Parse and validate a text/file manifest value and its respective type
    // value, if present. Return a typed_text_file object.
    //
    auto parse_text_file = [iu, &nv, &bad_value] (name_value&& text_file,
                                                  optional<name_value>&& type,
                                                  const char* what)
      -> typed_text_file
    {
      typed_text_file r;

      // Restore as bad_value() uses its line/column.
      //
      nv = move (text_file);

      string& v (nv.value);
      const string& n (nv.name);

      if (n.size () > 5 && n.compare (n.size () - 5, 5, "-file") == 0)
      {
        auto vc (parser::split_comment (v));

        path p;
        try
        {
          p = path (move (vc.first));
        }
        catch (const invalid_path& e)
        {
          bad_value (string ("invalid ") + what + " file: " + e.what ());
        }

        if (p.empty ())
          bad_value (string ("no path in ") + what + " file");

        if (p.absolute ())
          bad_value (string (what) + " file path is absolute");

        r = typed_text_file (move (p), move (vc.second));
      }
      else
        r = typed_text_file (move (v));

      if (type)
        r.type = move (type->value);

      // Verify the text type.
      //
      try
      {
        r.effective_type (iu);
      }
      catch (const invalid_argument& e)
      {
        if (type)
        {
          // Restore as bad_value() uses its line/column. Note that we don't
          // need to restore the moved out type value.
          //
          nv = move (*type);

          bad_value (string ("invalid ") + what + " type: " + e.what ());
        }
        else
        {
          // Note that this can only happen due to inability to guess the
          // type from the file extension. Let's help the user here a bit.
          //
          assert (r.file);

          bad_value (string ("invalid ") + what + " file: " + e.what () +
                     " (use " + string (n, 0, n.size () - 5)            +
                     "-type manifest value to specify explicitly)");
        }
      }

      return r;
    };

    // As above but also accepts nullopt as the text_file argument, in which
    // case throws manifest_parsing if the type is specified and return
    // nullopt otherwise.
    //
    auto parse_text_file_opt = [&nv, &bad_name, &parse_text_file]
                               (optional<name_value>&& text_file,
                                optional<name_value>&& type,
                                const char* what) -> optional<typed_text_file>
    {
      // Verify that the text/file value is specified if the type value is
      // specified.
      //
      if (!text_file)
      {
        if (type)
        {
          // Restore as bad_name() uses its line/column.
          //
          nv = move (*type);

          bad_name (string ("no ") + what + " for specified type");
        }

        return nullopt;
      }

      return parse_text_file (move (*text_file), move (type), what);
    };

    // Parse the project/package descriptions/types.
    //
    m.description = parse_text_file_opt (move (description),
                                         move (description_type),
                                         "project description");

    m.package_description =
      parse_text_file_opt (move (package_description),
                           move (package_description_type),
                           "package description");

    // Parse the package changes/types.
    //
    // Note: at the end of the loop the changes_type variable may contain
    // value in unspecified state but we can still check for the value
    // presence.
    //
    for (name_value& c: changes)
    {
      // Move the changes_type value from for the last changes entry.
      //
      m.changes.push_back (
        parse_text_file (move (c),
                         (&c != &changes.back ()
                          ? optional<name_value> (changes_type)
                          : move (changes_type)),
                         "changes"));
    }

    // If there are multiple changes and the changes type is not explicitly
    // specified, then verify that all changes effective types are the same.
    // Note that in the "ignore unknown" mode there can be unresolved
    // effective types which we just skip.
    //
    if (changes.size () > 1 && !changes_type)
    {
      optional<text_type> type;

      for (size_t i (0); i != m.changes.size (); ++i)
      {
        const typed_text_file& c (m.changes[i]);

        if (optional<text_type> t = c.effective_type (iu))
        {
          if (!type)
          {
            type = *t;
          }
          else if (*t != *type)
          {
            // Restore as bad_value() uses its line/column.
            //
            nv = move (changes[i]);

            bad_value ("changes type '" + to_string (*t) + "' differs from " +
                       " previous type '" + to_string (*type) + "'");
          }
        }
      }
    }

    // Parse the build configuration emails.
    //
    // Note: the argument can only be one of the build_config_*emails
    // variables (see above) to distinguish between the email kinds.
    //
    auto parse_build_config_emails = [&nv,
                                      &build_config_emails,
                                      &build_config_warning_emails,
                                      &build_config_error_emails,
                                      &build_conf,
                                      &parse_email]
                                     (vector<name_value>&& emails)
    {
      enum email_kind {build, warning, error};

      email_kind ek (
        &emails == &build_config_emails         ? email_kind::build   :
        &emails == &build_config_warning_emails ? email_kind::warning :
        email_kind::error);

      // The argument can only be one of the build_config_*emails variables.
      //
      assert (ek != email_kind::error || &emails == &build_config_error_emails);

      for (name_value& e: emails)
      {
        // Restore as bad_name() and bad_value() use its line/column.
        //
        nv = move (e);

        build_package_config& bc (
          build_conf (move (nv.name),
                      false /* create */,
                      "stray build notification email"));

        parse_email (
          nv,
          (ek == email_kind::build   ? bc.email         :
           ek == email_kind::warning ? bc.warning_email :
           bc.error_email),
          (ek == email_kind::build   ? "build configuration"         :
           ek == email_kind::warning ? "build configuration warning" :
           "build configuration error"),
          ek == email_kind::build /* empty */);
      }
    };

    parse_build_config_emails (move (build_config_emails));
    parse_build_config_emails (move (build_config_warning_emails));
    parse_build_config_emails (move (build_config_error_emails));

    // Now, when the version manifest value is parsed, we can parse the
    // dependencies and complete their constraints, if requested.
    //
    auto complete_constraint = [&m, cv, &flag] (auto&& dep)
    {
      if (dep.constraint)
      try
      {
        version_constraint& vc (*dep.constraint);

        if (!vc.complete () &&
            flag (package_manifest_flags::forbid_incomplete_values))
          throw invalid_argument ("$ not allowed");

        // Complete the constraint.
        //
        if (cv)
          vc = vc.effective (m.version);
      }
      catch (const invalid_argument& e)
      {
        throw invalid_argument ("invalid package constraint '" +
                                dep.constraint->string () + "': " + e.what ());
      }

      return move (dep);
    };

    // Parse the regular dependencies.
    //
    for (name_value& d: dependencies)
    {
      nv = move (d); // Restore as bad_value() uses its line/column.

      // Parse dependency alternatives.
      //
      try
      {
        dependency_alternatives das (nv.value,
                                     m.name,
                                     name,
                                     nv.value_line,
                                     nv.value_column);

        for (dependency_alternative& da: das)
        {
          for (dependency& d: da)
            d = complete_constraint (move (d));
        }

        m.dependencies.push_back (move (das));
      }
      catch (const invalid_argument& e)
      {
        bad_value (e.what ());
      }
    }

    // Parse the requirements.
    //
    for (const name_value& r: requirements)
    {
      m.requirements.push_back (
        requirement_alternatives (r.value,
                                  m.name,
                                  name,
                                  r.value_line,
                                  r.value_column));
    }

    // Parse the test dependencies.
    //
    for (name_value& t: tests)
    {
      nv = move (t); // Restore as bad_value() uses its line/column.

      try
      {
        m.tests.push_back (
          complete_constraint (
            test_dependency (move (nv.value),
                             to_test_dependency_type (nv.name))));
      }
      catch (const invalid_argument& e)
      {
        bad_value (e.what ());
      }
    }

    // Now, when the version manifest value is parsed, we complete the
    // <distribution>-version values, if requested.
    //
    if (cv)
    {
      for (distribution_name_value& nv: m.distribution_values)
      {
        const string& n (nv.name);
        string& v (nv.value);

        if (v == "$"                                                         &&
            (n.size () > 8 && n.compare (n.size () - 8, 8, "-version") == 0) &&
            n.find ('-') == n.size () - 8)
        {
          v = version (default_epoch (m.version),
                       move (m.version.upstream),
                       nullopt /* release */,
                       nullopt /* revision */,
                       0 /* iteration */).string ();
        }
      }
    }

    if (!m.location && flag (package_manifest_flags::require_location))
      bad_name ("no package location specified");

    if (!m.sha256sum && flag (package_manifest_flags::require_sha256sum))
      bad_name ("no package sha256sum specified");

    if (flag (package_manifest_flags::require_text_type))
    {
      if (m.description && !m.description->type)
        bad_name ("no project description type specified");

      if (m.package_description && !m.package_description->type)
        bad_name ("no package description type specified");

      // Note that changes either all have the same explicitly specified type
      // or have no type.
      //
      if (!m.changes.empty () && !m.changes.front ().type)
      {
        // @@ TMP To support older repositories allow absent changes type
        //        until toolchain 0.16.0 is released.
        //
        //        Note that for such repositories the packages may not have
        //        changes values other than plan text. Thus, we can safely set
        //        this type, if they are absent, so that the caller can always
        //        be sure that these values are always present for package
        //        manifest lists.
        //bad_name ("no package changes type specified");
        for (typed_text_file& c: m.changes)
          c.type = "text/plain";
      }
    }

    if (!m.bootstrap_build &&
        flag (package_manifest_flags::require_bootstrap_build))
    {
      // @@ TMP To support older repositories allow absent bootstrap build
      //        and alt_naming until toolchain 0.15.0 is released.
      //
      //        Note that for such repositories the packages may not have any
      //        need for the bootstrap buildfile (may not have any dependency
      //        clauses, etc). Thus, we can safely set the bootstrap build and
      //        alt_naming values to an empty string and false, respectively,
      //        if they are absent, so that the caller can always be sure that
      //        these values are always present for package manifest lists.
      //
      //        Note: don't forget to uncomment no-bootstrap test in
      //        tests/manifest/testscript when removing this workaround.
      //
      // bad_name ("no package bootstrap build specified");
      m.bootstrap_build = "project = " + m.name.string () + '\n';
      m.alt_naming = false;
    }
  }

  static void
  parse_package_manifest (
    parser& p,
    name_value nv,
    const function<package_manifest::translate_function>& tf,
    bool iu,
    bool cv,
    package_manifest_flags fl,
    package_manifest& m)
  {
    // Make sure this is the start and we support the version.
    //
    if (!nv.name.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "start of package manifest expected");

    if (nv.value != "1")
      throw parsing (p.name (), nv.value_line, nv.value_column,
                     "unsupported format version");

    // Note that we rely on "small function object" optimization here.
    //
    parse_package_manifest (p.name (),
                            [&p] () {return p.next ();},
                            tf,
                            iu,
                            cv,
                            fl,
                            m);
  }

  package_manifest
  pkg_package_manifest (parser& p, name_value nv, bool iu)
  {
    return package_manifest (
      p,
      move (nv),
      iu,
      false /* complete_values */,
      package_manifest_flags::forbid_file              |
      package_manifest_flags::forbid_fragment          |
      package_manifest_flags::forbid_incomplete_values |
      package_manifest_flags::require_location         |
      package_manifest_flags::require_text_type        |
      package_manifest_flags::require_bootstrap_build);
  }

  // package_manifest
  //
  package_manifest::
  package_manifest (manifest_parser& p,
                    const function<translate_function>& tf,
                    bool iu,
                    bool cv,
                    package_manifest_flags fl)
  {
    parse_package_manifest (p, p.next (), tf, iu, cv, fl, *this);

    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");
  }

  package_manifest::
  package_manifest (const string& name,
                    vector<name_value>&& vs,
                    const function<translate_function>& tf,
                    bool iu,
                    bool cv,
                    package_manifest_flags fl)
  {
    auto i (vs.begin ());
    auto e (vs.end ());

    // Note that we rely on "small function object" optimization here.
    //
    parse_package_manifest (name,
                            [&i, &e] ()
                            {
                              return i != e ? move (*i++) : name_value ();
                            },
                            tf,
                            iu,
                            cv,
                            fl,
                            *this);
  }

  package_manifest::
  package_manifest (const string& name,
                    vector<name_value>&& vs,
                    bool iu,
                    bool cv,
                    package_manifest_flags fl)
      : package_manifest (name,
                          move (vs),
                          function<translate_function> (),
                          iu,
                          cv,
                          fl)
  {
  }

  package_manifest::
  package_manifest (manifest_parser& p,
                    name_value nv,
                    bool iu,
                    bool cv,
                    package_manifest_flags fl)
  {
    parse_package_manifest (
      p, move (nv), function<translate_function> (), iu, cv, fl, *this);
  }

  strings package_manifest::
  effective_type_sub_options (const optional<string>& t)
  {
    strings r;

    if (t)
    {
      for (size_t b (0), e (0); next_word (*t, b, e, ','); )
      {
        if (b != 0)
          r.push_back (trim (string (*t, b, e - b)));
      }
    }

    return r;
  }

  // If validate_only is true, then the package manifest is assumed to be
  // default constructed and is used as a storage for convenience of the
  // validation implementation.
  //
  static void
  override (const vector<manifest_name_value>& nvs,
            const string& name,
            package_manifest& m,
            bool validate_only)
  {
    // The first {builds, build-{include,exclude}} override value.
    //
    const manifest_name_value* cbc (nullptr);

    // The first builds override value.
    //
    const manifest_name_value* cb (nullptr);

    // The first {*-builds, *-build-{include,exclude}} override value.
    //
    const manifest_name_value* pbc  (nullptr);

    // The first {build-bot} override value.
    //
    const manifest_name_value* cbb (nullptr);

    // The first {*-build-bot} override value.
    //
    const manifest_name_value* pbb  (nullptr);

    // The first {build-*email} override value.
    //
    const manifest_name_value* cbe (nullptr);

    // The first {*-build-*email} override value.
    //
    const manifest_name_value* pbe  (nullptr);

    // List of indexes of the build configurations with the overridden build
    // constraints together with flags which indicate if the *-builds override
    // value was encountered for this configuration.
    //
    vector<pair<size_t, bool>> obcs;

    // List of indexes of the build configurations with the overridden bots.
    //
    vector<size_t> obbs;

    // List of indexes of the build configurations with the overridden emails.
    //
    vector<size_t> obes;

    // Return true if the specified package build configuration is newly
    // created by the *-build-config override.
    //
    auto config_created = [&m, confs_num = m.build_configs.size ()]
                          (const build_package_config& c)
    {
      return &c >= m.build_configs.data () + confs_num;
    };

    // Apply overrides.
    //
    for (const manifest_name_value& nv: nvs)
    {
      auto bad_name = [&name, &nv] (const string& d)
      {
        throw !name.empty ()
              ? parsing (name, nv.name_line, nv.name_column, d)
              : parsing (d);
      };

      // Reset the build-{include,exclude} value sub-group on the first call
      // but throw if any of the {*-builds, *-build-{include,exclude}}
      // override values are already encountered.
      //
      auto reset_build_constraints = [&cbc, &pbc, &nv, &bad_name, &m] ()
      {
        if (cbc == nullptr)
        {
          if (pbc != nullptr)
            bad_name ('\'' + nv.name + "' override specified together with '" +
                      pbc->name + "' override");

          m.build_constraints.clear ();
          cbc = &nv;
        }
      };

      // Reset the {builds, build-{include,exclude}} value group on the first
      // call.
      //
      auto reset_builds = [&cb, &nv, &reset_build_constraints, &m] ()
      {
        if (cb == nullptr)
        {
          reset_build_constraints ();

          m.builds.clear ();
          cb = &nv;
        }
      };

      // Return the reference to the package build configuration which matches
      // the build config value override, if exists. If no configuration
      // matches, then create one, if requested, and throw manifest_parsing
      // otherwise.
      //
      // The n argument specifies the length of the configuration name in
      // *-build-config, *-builds, *-build-{include,exclude}, *-build-bot, and
      // *-build-*email values.
      //
      auto build_conf =
        [&nv, &bad_name, &m] (size_t n, bool create) -> build_package_config&
      {
        const string& nm (nv.name);
        small_vector<build_package_config, 1>& cs (m.build_configs);

        // Find the build package configuration. If no configuration is found,
        // then create one, if requested, and throw otherwise.
        //
        auto i (find_if (cs.begin (), cs.end (),
                         [&nm, n] (const build_package_config& c)
                         {return nm.compare (0, n, c.name) == 0;}));

        if (i == cs.end ())
        {
          string cn (nm, 0, n);

          if (create)
          {
            cs.emplace_back (move (cn));
            return cs.back ();
          }
          else
            bad_name ("cannot override '" + nm + "' value: no build " +
                      "package configuration '" + cn + '\'');
        }

        return *i;
      };

      // Return the reference to the package build configuration which matches
      // the build config-specific builds group value override, if exists. If
      // no configuration matches, then throw manifest_parsing, except for the
      // validate-only mode in which case just add an empty configuration with
      // this name and return the reference to it. Also verify that no common
      // build constraints group value overrides are applied yet and throw if
      // that's not the case.
      //
      auto build_conf_constr =
        [&pbc, &cbc, &nv, &obcs, &bad_name, &build_conf, &m, validate_only]
        (size_t n) -> build_package_config&
      {
        const string& nm (nv.name);

        // If this is the first build config override value, then save its
        // address. But first verify that no common build constraints group
        // value overrides are applied yet and throw if that's not the case.
        //
        if (pbc == nullptr)
        {
          if (cbc != nullptr)
            bad_name ('\'' + nm + "' override specified together with '" +
                      cbc->name + "' override");

          pbc = &nv;
        }

        small_vector<build_package_config, 1>& cs (m.build_configs);

        // Find the build package configuration. If there is no such a
        // configuration then throw, except for the validate-only mode in
        // which case just add an empty configuration with this name.
        //
        // Note that we are using indexes rather then configuration addresses
        // due to potential reallocations.
        //
        build_package_config& r (build_conf (n, validate_only));
        size_t ci (&r - cs.data ());
        bool bv (nm.compare (n, nm.size () - n, "-builds") == 0);

        // If this is the first encountered
        // {*-builds, *-build-{include,exclude}} override for this build
        // config, then clear this config' constraints member and add an entry
        // to the overridden configs list.
        //
        auto i (find_if (obcs.begin (), obcs.end (),
                         [ci] (const auto& c) {return c.first == ci;}));

        bool first (i == obcs.end ());

        if (first)
        {
          r.constraints.clear ();

          obcs.push_back (make_pair (ci, bv));
        }

        // If this is the first encountered *-builds override, then also clear
        // this config' builds member.
        //
        if (bv && (first || !i->second))
        {
          r.builds.clear ();

          if (!first)
            i->second = true;
        }

        return r;
      };

      // Reset the {build-bot} value group on the first call but throw if any
      // of the {*-build-bot} override values are already encountered.
      //
      auto reset_build_bots = [&cbb, &pbb, &nv, &bad_name, &m] ()
      {
        if (cbb == nullptr)
        {
          if (pbb != nullptr)
            bad_name ('\'' + nv.name + "' override specified together with '" +
                      pbb->name + "' override");

          m.build_bot_keys.clear ();
          cbb = &nv;
        }
      };

      // Return the reference to the package build configuration which matches
      // the build config-specific build bot value override, if exists. If no
      // configuration matches, then throw manifest_parsing, except for the
      // validate-only mode in which case just add an empty configuration with
      // this name and return the reference to it. Also verify that no common
      // build bot value overrides are applied yet and throw if that's not the
      // case.
      //
      auto build_conf_bot =
        [&pbb, &cbb, &nv, &obbs, &bad_name, &build_conf, &m, validate_only]
        (size_t n) -> build_package_config&
      {
        const string& nm (nv.name);

        // If this is the first build config override value, then save its
        // address. But first verify that no common build bot value overrides
        // are applied yet and throw if that's not the case.
        //
        if (pbb == nullptr)
        {
          if (cbb != nullptr)
            bad_name ('\'' + nm + "' override specified together with '" +
                      cbb->name + "' override");

          pbb = &nv;
        }

        small_vector<build_package_config, 1>& cs (m.build_configs);

        // Find the build package configuration. If there is no such a
        // configuration then throw, except for the validate-only mode in
        // which case just add an empty configuration with this name.
        //
        // Note that we are using indexes rather then configuration addresses
        // due to potential reallocations.
        //
        build_package_config& r (build_conf (n, validate_only));
        size_t ci (&r - cs.data ());

        // If this is the first encountered {*-build-bot} override for this
        // build config, then clear this config' bot_keys members and add an
        // entry to the overridden configs list.
        //
        if (find (obbs.begin (), obbs.end (), ci) == obbs.end ())
        {
          r.bot_keys.clear ();

          obbs.push_back (ci);
        }

        return r;
      };

      // Reset the {build-*email} value group on the first call but throw if
      // any of the {*-build-*email} override values are already encountered.
      //
      auto reset_build_emails = [&cbe, &pbe, &nv, &bad_name, &m] ()
      {
        if (cbe == nullptr)
        {
          if (pbe != nullptr)
            bad_name ('\'' + nv.name + "' override specified together with '" +
                      pbe->name + "' override");

          m.build_email = nullopt;
          m.build_warning_email = nullopt;
          m.build_error_email = nullopt;
          cbe = &nv;
        }
      };

      // Return the reference to the package build configuration which matches
      // the build config-specific emails group value override, if exists. If
      // no configuration matches, then throw manifest_parsing, except for the
      // validate-only mode in which case just add an empty configuration with
      // this name and return the reference to it. Also verify that no common
      // build emails group value overrides are applied yet and throw if
      // that's not the case.
      //
      auto build_conf_email =
        [&pbe, &cbe, &nv, &obes, &bad_name, &build_conf, &m, validate_only]
        (size_t n) -> build_package_config&
      {
        const string& nm (nv.name);

        // If this is the first build config override value, then save its
        // address. But first verify that no common build emails group value
        // overrides are applied yet and throw if that's not the case.
        //
        if (pbe == nullptr)
        {
          if (cbe != nullptr)
            bad_name ('\'' + nm + "' override specified together with '" +
                      cbe->name + "' override");

          pbe = &nv;
        }

        small_vector<build_package_config, 1>& cs (m.build_configs);

        // Find the build package configuration. If there is no such a
        // configuration then throw, except for the validate-only mode in
        // which case just add an empty configuration with this name.
        //
        // Note that we are using indexes rather then configuration addresses
        // due to potential reallocations.
        //
        build_package_config& r (build_conf (n, validate_only));
        size_t ci (&r - cs.data ());

        // If this is the first encountered {*-build-*email} override for this
        // build config, then clear this config' email members and add an
        // entry to the overridden configs list.
        //
        if (find (obes.begin (), obes.end (), ci) == obes.end ())
        {
          r.email = nullopt;
          r.warning_email = nullopt;
          r.error_email = nullopt;

          obes.push_back (ci);
        }

        return r;
      };

      // Parse the [*-]build-auxiliary[-*] value override. If the mode is not
      // validate-only, then override the matching value and throw
      // manifest_parsing if no match. But throw only unless this is a
      // configuration-specific override (build_config is not NULL) for a
      // newly created configuration, in which case add the value instead.
      //
      auto override_build_auxiliary =
        [&bad_name,
         &name,
         &config_created,
         validate_only] (const name_value& nv,
                         string&& en,
                         vector<build_auxiliary>& r,
                         build_package_config* build_config = nullptr)
      {
        build_auxiliary a (bpkg::parse_build_auxiliary (nv, move (en), name));

        if (!validate_only)
        {
          auto i (find_if (r.begin (), r.end (),
                           [&a] (const build_auxiliary& ba)
                           {
                             return ba.environment_name == a.environment_name;
                           }));

          if (i != r.end ())
          {
            *i = move (a);
          }
          else
          {
            if (build_config != nullptr && config_created (*build_config))
              r.emplace_back (move (a));
            else
              bad_name ("no match for '" + nv.name + "' value override");
          }
        }
      };

      const string& n (nv.name);

      if (n == "builds")
      {
        reset_builds ();

        m.builds.push_back (
          parse_build_class_expr (nv, m.builds.empty (), name));
      }
      else if (n == "build-include")
      {
        reset_build_constraints ();

        m.build_constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, name));
      }
      else if (n == "build-exclude")
      {
        reset_build_constraints ();

        m.build_constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, name));
      }
      else if (n == "build-bot")
      {
        reset_build_bots ();

        parse_build_bot (nv, name, m.build_bot_keys);
      }
      else if ((n.size () > 13 &&
                n.compare (n.size () - 13, 13, "-build-config") == 0))
      {
        build_package_config& bc (
          build_conf (n.size () - 13, true /* create */));

        auto vc (parser::split_comment (nv.value));

        bc.arguments = move (vc.first);
        bc.comment = move (vc.second);
      }
      else if (n.size () > 7 && n.compare (n.size () - 7, 7, "-builds") == 0)
      {
        build_package_config& bc (build_conf_constr (n.size () - 7));

        bc.builds.push_back (
          parse_build_class_expr (nv, bc.builds.empty (), name));
      }
      else if (n.size () > 14 &&
               n.compare (n.size () - 14, 14, "-build-include") == 0)
      {
        build_package_config& bc (build_conf_constr (n.size () - 14));

        bc.constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, name));
      }
      else if (n.size () > 14 &&
               n.compare (n.size () - 14, 14, "-build-exclude") == 0)
      {
        build_package_config& bc (build_conf_constr (n.size () - 14));

        bc.constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, name));
      }
      else if (n.size () > 10 &&
               n.compare (n.size () - 10, 10, "-build-bot") == 0)
      {
        build_package_config& bc (build_conf_bot (n.size () - 10));
        parse_build_bot (nv, name, bc.bot_keys);
      }
      else if (n == "build-email")
      {
        reset_build_emails ();
        m.build_email = parse_email (nv, "build", name, true /* empty */);
      }
      else if (n == "build-warning-email")
      {
        reset_build_emails ();
        m.build_warning_email = parse_email (nv, "build warning", name);
      }
      else if (n == "build-error-email")
      {
        reset_build_emails ();
        m.build_error_email = parse_email (nv, "build error", name);
      }
      else if (n.size () > 12 &&
               n.compare (n.size () - 12, 12, "-build-email") == 0)
      {
        build_package_config& bc (build_conf_email (n.size () - 12));

        bc.email = parse_email (
          nv, "build configuration", name, true /* empty */);
      }
      else if (n.size () > 20 &&
               n.compare (n.size () - 20, 20, "-build-warning-email") == 0)
      {
        build_package_config& bc (build_conf_email (n.size () - 20));

        bc.warning_email = parse_email (
          nv, "build configuration warning", name);
      }
      else if (n.size () > 18 &&
               n.compare (n.size () - 18, 18, "-build-error-email") == 0)
      {
        build_package_config& bc (build_conf_email (n.size () - 18));

        bc.error_email = parse_email (nv, "build configuration error", name);
      }
      else if (optional<pair<string, string>> ba =
               build_auxiliary::parse_value_name (n))
      {
        if (ba->first.empty ()) // build-auxiliary*?
        {
          override_build_auxiliary (nv, move (ba->second), m.build_auxiliaries);
        }
        else                    // *-build-auxiliary*
        {
          build_package_config& bc (
            build_conf (ba->first.size (), validate_only));

          override_build_auxiliary (nv, move (ba->second), bc.auxiliaries, &bc);
        }
      }
      else
        bad_name ("cannot override '" + n + "' value");
    }

    // Common build constraints and build config overrides are mutually
    // exclusive.
    //
    assert (cbc == nullptr || pbc == nullptr);

    // Now, if not in the validate-only mode, as all the potential build
    // constraint, bot keys, and email overrides are applied, perform the
    // final adjustments to the build config constraints, bot keys, and
    // emails.
    //
    if (!validate_only)
    {
      if (cbc != nullptr)      // Common build constraints are overridden?
      {
        for (build_package_config& c: m.build_configs)
        {
          c.builds.clear ();
          c.constraints.clear ();
        }
      }
      else if (pbc != nullptr) // Build config constraints are overridden?
      {
        for (size_t i (0); i != m.build_configs.size (); ++i)
        {
          if (find_if (obcs.begin (), obcs.end (),
                       [i] (const auto& pc) {return pc.first == i;}) ==
              obcs.end ())
          {
            build_package_config& c (m.build_configs[i]);

            c.builds.clear ();
            c.constraints.clear ();
            c.builds.emplace_back ("none", "" /* comment */);
          }
        }
      }

      if (cbb != nullptr)      // Common build bots are overridden?
      {
        for (build_package_config& c: m.build_configs)
          c.bot_keys.clear ();
      }

      if (cbe != nullptr)      // Common build emails are overridden?
      {
        for (build_package_config& c: m.build_configs)
        {
          c.email = nullopt;
          c.warning_email = nullopt;
          c.error_email = nullopt;
        }
      }
      else if (pbe != nullptr) // Build config emails are overridden?
      {
        for (size_t i (0); i != m.build_configs.size (); ++i)
        {
          if (find (obes.begin (), obes.end (), i) == obes.end ())
          {
            build_package_config& c (m.build_configs[i]);

            c.email = email ();
            c.warning_email = nullopt;
            c.error_email = nullopt;
          }
        }
      }
    }
  }

  void package_manifest::
  override (const vector<manifest_name_value>& nvs, const string& name)
  {
    bpkg::override (nvs, name, *this, false /* validate_only */);
  }

  void package_manifest::
  validate_overrides (const vector<manifest_name_value>& nvs,
                      const string& name)
  {
    package_manifest p;
    bpkg::override (nvs, name, p, true /* validate_only */);
  }

  static const string description_file         ("description-file");
  static const string package_description_file ("package-description-file");
  static const string changes_file             ("changes-file");
  static const string build_file               ("build-file");

  void package_manifest::
  load_files (const function<load_function>& loader, bool iu)
  {
    // If required, load a file and verify that its content is not empty, if
    // the loader returns the content. Make the text type explicit.
    //
    auto load = [iu, &loader] (typed_text_file& text,
                               const string& file_value_name)
    {
      // Make the type explicit.
      //
      optional<text_type> t;

      // Convert the potential invalid_argument exception to the
      // manifest_parsing exception similar to what we do in the manifest
      // parser.
      //
      try
      {
        t = text.effective_type (iu);
      }
      catch (const invalid_argument& e)
      {
        if (text.type)
        {
          // Strip trailing "-file".
          //
          string prefix (file_value_name, 0, file_value_name.size () - 5);

          throw parsing ("invalid " + prefix + "-type package manifest " +
                         "value: " + e.what ());
        }
        else
        {
          throw parsing ("invalid " + file_value_name + " package " +
                         "manifest value: " + e.what ());
        }
      }


      assert (t || iu); // Can only be absent if we ignore unknown.

      if (!text.type && t)
        text.type = to_string (*t);

      // At this point the type can only be absent if the text comes from a
      // file. Otherwise, we would end up with the plain text.
      //
      assert (text.type || text.file);

      if (text.file)
      {
        if (!text.type)
          text.type = "text/unknown; extension=" + text.path.extension ();

        if (optional<string> fc = loader (file_value_name, text.path))
        {
          if (fc->empty ())
            throw parsing ("package manifest value " + file_value_name +
                           " references empty file");

          text = typed_text_file (move (*fc), move (text.type));
        }
      }
    };

    // Load the descriptions and changes, if present.
    //
    if (description)
      load (*description, description_file);

    if (package_description)
      load (*package_description, package_description_file);

    for (typed_text_file& c: changes)
      load (c, changes_file);

    // Load the build-file manifest values.
    //
    if (!buildfile_paths.empty ())
    {
      // Must already be set if the build-file value is parsed.
      //
      assert (alt_naming);

      dir_path d (*alt_naming ? "build2" : "build");

      for (auto i (buildfile_paths.begin ()); i != buildfile_paths.end (); )
      {
        path& p (*i);
        path f (d / p);
        f += *alt_naming ? ".build2" : ".build";

        if (optional<string> fc = loader (build_file, f))
        {
          buildfiles.emplace_back (move (p), move (*fc));
          i = buildfile_paths.erase (i); // Moved to buildfiles.
        }
        else
          ++i;
      }
    }
  }

  static void
  serialize_package_manifest (
    manifest_serializer& s,
    const package_manifest& m,
    bool header_only,
    const optional<standard_version>& min_ver = nullopt)
  {
    // @@ Should we check that all non-optional values are specified ?
    // @@ Should we check that values are valid: version release is not empty,
    //    sha256sum is a proper string, ...?
    // @@ Currently we don't know if we are serializing the individual package
    //    manifest or the package list manifest, so can't ensure all values
    //    allowed in the current context (*-file values).
    //

    s.next ("", "1"); // Start of manifest.

    auto bad_value ([&s](const string& d) {
        throw serialization (s.name (), d);});

    if (m.name.empty ())
      bad_value ("empty package name");

    s.next ("name", m.name.string ());
    s.next ("version", m.version.string ());

    if (m.upstream_version)
      s.next ("upstream-version", *m.upstream_version);

    if (m.type)
      s.next ("type", *m.type);

    for (const language& l: m.languages)
      s.next ("language", !l.impl ? l.name : l.name + "=impl");

    if (m.project)
      s.next ("project", m.project->string ());

    if (m.priority)
    {
      size_t v (*m.priority);
      assert (v < priority_names.size ());

      s.next ("priority",
              serializer::merge_comment (priority_names[v],
                                         m.priority->comment));
    }

    s.next ("summary", m.summary);

    for (const auto& la: m.license_alternatives)
      s.next ("license",
              serializer::merge_comment (concatenate (la), la.comment));

    if (!header_only)
    {
      if (!m.topics.empty ())
        s.next ("topics", concatenate (m.topics));

      if (!m.keywords.empty ())
        s.next ("keywords", concatenate (m.keywords, " "));

      auto serialize_text_file = [&s] (const text_file& v, const string& n)
      {
        if (v.file)
          s.next (n + "-file",
                  serializer::merge_comment (v.path.string (), v.comment));
        else
          s.next (n, v.text);
      };

      auto serialize_description = [&s, &serialize_text_file]
                                   (const optional<typed_text_file>& desc,
                                    const char* prefix)
      {
        if (desc)
        {
          string p (prefix);
          serialize_text_file (*desc, p + "description");

          if (desc->type)
            s.next (p + "description-type", *desc->type);
        }
      };

      serialize_description (m.description, "" /* prefix */);
      serialize_description (m.package_description, "package-");

      for (const auto& c: m.changes)
        serialize_text_file (c, "changes");

      // If there are any changes, then serialize the type of the first
      // changes entry, if present. Note that if it is present, then we assume
      // that the type was specified explicitly and so it is the same for all
      // entries.
      //
      if (!m.changes.empty ())
      {
        const typed_text_file& c (m.changes.front ());

        if (c.type)
          s.next ("changes-type", *c.type);
      }

      if (m.url)
        s.next ("url",
                serializer::merge_comment (m.url->string (), m.url->comment));

      if (m.doc_url)
        s.next ("doc-url",
                serializer::merge_comment (m.doc_url->string (),
                                           m.doc_url->comment));

      if (m.src_url)
        s.next ("src-url",
                serializer::merge_comment (m.src_url->string (),
                                           m.src_url->comment));

      if (m.package_url)
        s.next ("package-url",
                serializer::merge_comment (m.package_url->string (),
                                           m.package_url->comment));

      if (m.email)
        s.next ("email",
                serializer::merge_comment (*m.email, m.email->comment));

      if (m.package_email)
        s.next ("package-email",
                serializer::merge_comment (*m.package_email,
                                           m.package_email->comment));

      if (m.build_email)
        s.next ("build-email",
                serializer::merge_comment (*m.build_email,
                                           m.build_email->comment));

      if (m.build_warning_email)
        s.next ("build-warning-email",
                serializer::merge_comment (*m.build_warning_email,
                                           m.build_warning_email->comment));

      if (m.build_error_email)
        s.next ("build-error-email",
                serializer::merge_comment (*m.build_error_email,
                                           m.build_error_email->comment));

      for (const dependency_alternatives& d: m.dependencies)
        s.next ("depends", d.string ());

      for (const requirement_alternatives& r: m.requirements)
        s.next ("requires", r.string ());

      for (const test_dependency& t: m.tests)
      {
        string n (to_string (t.type));

        // If we generate the manifest for parsing by clients of libbpkg
        // versions less than 0.14.0-, then replace the introduced in 0.14.0
        // build-time tests, examples, and benchmarks values with
        // tests-0.14.0, examples-0.14.0, and benchmarks-0.14.0,
        // respectively. This way such clients will still be able to parse it,
        // ignoring unknown values.
        //
        // @@ TMP time to drop?
        //                                               0.14.0-
        if (t.buildtime && min_ver && min_ver->version < 13999990001ULL)
          n += "-0.14.0";

        s.next (n, t.string ());
      }

      for (const build_class_expr& e: m.builds)
        s.next ("builds", serializer::merge_comment (e.string (), e.comment));

      for (const build_constraint& c: m.build_constraints)
        s.next (c.exclusion ? "build-exclude" : "build-include",
                serializer::merge_comment (!c.target
                                           ? c.config
                                           : c.config + '/' + *c.target,
                                           c.comment));

      for (const build_auxiliary& ba: m.build_auxiliaries)
        s.next ((!ba.environment_name.empty ()
                 ? "build-auxiliary-" + ba.environment_name
                 : "build-auxiliary"),
                serializer::merge_comment (ba.config, ba.comment));

      for (const string& k: m.build_bot_keys)
        s.next ("build-bot", k);

      for (const build_package_config& bc: m.build_configs)
      {
        if (!bc.builds.empty ())
        {
          string n (bc.name + "-builds");
          for (const build_class_expr& e: bc.builds)
            s.next (n, serializer::merge_comment (e.string (), e.comment));
        }

        if (!bc.constraints.empty ())
        {
          string in (bc.name + "-build-include");
          string en (bc.name + "-build-exclude");

          for (const build_constraint& c: bc.constraints)
            s.next (c.exclusion ? en : in,
                    serializer::merge_comment (!c.target
                                               ? c.config
                                               : c.config + '/' + *c.target,
                                               c.comment));
        }

        if (!bc.auxiliaries.empty ())
        {
          string n (bc.name + "-build-auxiliary");

          for (const build_auxiliary& ba: bc.auxiliaries)
            s.next ((!ba.environment_name.empty ()
                     ? n + '-' + ba.environment_name
                     : n),
                    serializer::merge_comment (ba.config, ba.comment));
        }

        if (!bc.bot_keys.empty ())
        {
          string n (bc.name + "-build-bot");

          for (const string& k: bc.bot_keys)
            s.next (n, k);
        }

        if (!bc.arguments.empty () || !bc.comment.empty ())
          s.next (bc.name + "-build-config",
                  serializer::merge_comment (bc.arguments, bc.comment));

        if (bc.email)
          s.next (bc.name + "-build-email",
                  serializer::merge_comment (*bc.email, bc.email->comment));

        if (bc.warning_email)
          s.next (bc.name + "-build-warning-email",
                  serializer::merge_comment (*bc.warning_email,
                                             bc.warning_email->comment));

        if (bc.error_email)
          s.next (bc.name + "-build-error-email",
                  serializer::merge_comment (*bc.error_email,
                                             bc.error_email->comment));
      }

      bool an (m.alt_naming && *m.alt_naming);

      if (m.bootstrap_build)
        s.next (an ? "bootstrap-build2" : "bootstrap-build",
                *m.bootstrap_build);

      if (m.root_build)
        s.next (an ? "root-build2" : "root-build", *m.root_build);

      for (const auto& bf: m.buildfiles)
        s.next (bf.path.posix_string () + (an ? "-build2" : "-build"),
                bf.content);

      for (const path& f: m.buildfile_paths)
        s.next ("build-file", f.posix_string () + (an ? ".build2" : ".build"));

      for (const distribution_name_value& nv: m.distribution_values)
        s.next (nv.name, nv.value);

      if (m.location)
        s.next ("location", m.location->posix_string ());

      if (m.sha256sum)
        s.next ("sha256sum", *m.sha256sum);

      if (m.fragment)
        s.next ("fragment", *m.fragment);
    }

    s.next ("", ""); // End of manifest.
  }

  void package_manifest::
  serialize (serializer& s, const optional<standard_version>& min_ver) const
  {
    serialize_package_manifest (s, *this, false, min_ver);
  }

  void package_manifest::
  serialize_header (serializer& s) const
  {
    serialize_package_manifest (s, *this, true);
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
      else if (n == "fragment")
      {
        if (r.fragment)
          bad_name ("package repository fragment redefinition");

        if (v.empty ())
          bad_value ("empty package repository fragment");

        r.fragment = move (v);
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

    if (m.fragment)
      s.next ("fragment", *m.fragment);

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
  serialize (serializer& s, const optional<standard_version>& min_ver) const
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
          s.name (),
          d + " for " + p.name.string () + '-' + p.version.string ());
      };

      // Throw manifest_serialization if the text is in a file or untyped.
      //
      auto verify_text_file = [&bad_value] (const typed_text_file& v,
                                            const string& n)
      {
        if (v.file)
          bad_value ("forbidden " + n + "-file");

        if (!v.type)
          bad_value ("no valid " + n + "-type");
      };

      if (p.description)
        verify_text_file (*p.description, "description");

      if (p.package_description)
        verify_text_file (*p.package_description, "package-description");

      for (const auto& c: p.changes)
        verify_text_file (c, "changes");

      if (!p.buildfile_paths.empty ())
        bad_value ("forbidden build-file");

      if (!p.location)
        bad_value ("no valid location");

      if (!p.sha256sum)
        bad_value ("no valid sha256sum");

      pkg_package_manifest (s, p, min_ver);
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
  optional<repository_url_traits::scheme_type> repository_url_traits::
  translate_scheme (const string_type&         url,
                    string_type&&              scheme,
                    optional<authority_type>&  authority,
                    optional<path_type>&       path,
                    optional<string_type>&     query,
                    optional<string_type>&     fragment,
                    bool&                      rootless)
  {
    auto bad_url = [] (const char* d = "invalid URL")
    {
      throw invalid_argument (d);
    };

    // Consider non-empty URL as a path if the URL parsing failed. If the URL
    // is empty then leave the basic_url ctor to throw.
    //
    if (scheme.empty ())
    {
      if (!url.empty ())
      try
      {
        size_t p (url.find ('#'));

        if (p != string::npos)
        {
          path = path_type (url.substr (0, p)).normalize ();
          fragment = url.substr (p + 1);
        }
        else
          path = path_type (url).normalize ();

        rootless = false;
        return scheme_type::file;
      }
      catch (const invalid_path&)
      {
        // If this is not a valid path either, then let's consider the
        // argument a broken URL, and leave the basic_url ctor to throw.
        //
      }

      return nullopt;
    }

    if (!authority && !path && !query)
      bad_url ("empty URL");

    if (rootless)
      bad_url ("rootless path");

    auto translate_remote = [&authority, &path, &bad_url] ()
    {
      if (!authority || authority->host.empty ())
        bad_url ("invalid host");

      // Normalize the host name/address.
      //
      authority->host.normalize ();

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

      // URL shouldn't go past the root directory of a server.
      //
      if (!path->empty () && *path->begin () == "..")
        bad_url ("invalid path");
    };

    if (icasecmp (scheme, "http") == 0)
    {
      translate_remote ();
      return scheme_type::http;
    }
    else if (icasecmp (scheme, "https") == 0)
    {
      translate_remote ();
      return scheme_type::https;
    }
    else if (icasecmp (scheme, "git") == 0)
    {
      translate_remote ();
      return scheme_type::git;
    }
    else if (icasecmp (scheme, "ssh") == 0)
    {
      translate_remote ();

      // Should we also support the scp-like syntax (see git-clone(1) man page
      // for details)? On the one hand, it would complicate things quite a bit
      // and also clashes with Windows path representation (maybe we could
      // forbid one character long hostnames for this syntax). On the other,
      // it seems to be widespread (used by GitHub). Let's postpone it until
      // requested by users.
      //
      return scheme_type::ssh;
    }
    else if (icasecmp (scheme, "file") == 0)
    {
      if (authority)
      {
        if (!authority->empty () &&
            (icasecmp (authority->host, "localhost") != 0 ||
             authority->port != 0                         ||
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
    else
      throw invalid_argument ("unknown scheme");
  }

  repository_url_traits::string_type repository_url_traits::
  translate_scheme (string_type&                     url,
                    const scheme_type&               scheme,
                    const optional<authority_type>&  authority,
                    const optional<path_type>&       path,
                    const optional<string_type>&     /*query*/,
                    const optional<string_type>&     fragment,
                    bool                             /*rootless*/)
  {
    switch (scheme)
    {
    case scheme_type::http:  return "http";
    case scheme_type::https: return "https";
    case scheme_type::git:   return "git";
    case scheme_type::ssh:   return "ssh";
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
      return path_type (url::decode (path));
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
     string r;
     if (path.absolute ())
     {
#ifndef _WIN32
       r = path.leaf (dir_path ("/")).string ();
#else
       r = path.string ();
       replace (r.begin (), r.end (), '\\', '/');
#endif
     }
     else
       r = path.posix_string ();

     return url::encode (r, [] (char& c) {return !url::path_char (c);});
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

  inline static optional<repository_type>
  parse_repository_type (const string& t)
  {
         if (t == "pkg") return repository_type::pkg;
    else if (t == "dir") return repository_type::dir;
    else if (t == "git") return repository_type::git;
    else                 return nullopt;
  }

  repository_type
  to_repository_type (const string& t)
  {
    if (optional<repository_type> r = parse_repository_type (t))
      return *r;

    throw invalid_argument ("invalid repository type '" + t + '\'');
  }

  repository_type
  guess_type (const repository_url& url, bool local)
  {
    assert (!url.empty ());

    switch (url.scheme)
    {
    case repository_protocol::git:
      {
        return repository_type::git;
      }
    case repository_protocol::http:
    case repository_protocol::https:
    case repository_protocol::ssh:
    case repository_protocol::file:
      {
        if (url.path->extension () == "git")
          return repository_type::git;

        if (url.scheme != repository_protocol::file) // HTTP(S) or SSH?
          return repository_type::pkg;

        return local &&
          dir_exists (path_cast<dir_path> (*url.path) / dir_path (".git"))
          ? repository_type::git
          : repository_type::pkg;
      }
    }

    assert (false); // Can't be here.
    return repository_type::pkg;
  }

  // typed_repository_url
  //
  typed_repository_url::
  typed_repository_url (const string& s)
  {
    using traits = url::traits_type;

    if (traits::find (s) == 0) // Looks like a non-rootless URL?
    {
      size_t p (s.find_first_of ("+:"));

      assert (p != string::npos); // At least the colon is present.

      if (s[p] == '+')
      {
        string r (s, p + 1);
        optional<repository_type> t;

        if (traits::find (r) == 0 &&                        // URL notation?
            (t = parse_repository_type (string (s, 0, p)))) // Valid type?
        {
          repository_url u (r);

          // Only consider the URL to be typed if it is not a relative
          // path. And yes, we may end up with a relative path for the URL
          // string (e.g. ftp://example.com).
          //
          if (!(u.scheme == repository_protocol::file && u.path->relative ()))
          {
            type = move (t);
            url  = move (u);
          }
        }
      }
    }

    // Parse the whole string as a repository URL if we failed to extract the
    // type.
    //
    if (url.empty ())
      url = repository_url (s); // Throws if empty.
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
  repository_location (const std::string& s,
                       const optional<repository_type>& t,
                       bool local)
  {
    typed_repository_url tu (s);

    if (t && tu.type && t != tu.type)
      throw invalid_argument (
        "mismatching repository types: " + to_string (*t) + " specified, " +
        to_string (*tu.type) + " in URL scheme");

    repository_type et (tu.type ? *tu.type :
                        t       ? *t       :
                        guess_type (tu.url, local));

    *this = repository_location (move (tu.url), et);
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
        if (url_.scheme == repository_protocol::git ||
            url_.scheme == repository_protocol::ssh)
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
          parse_git_ref_filters (*url_.fragment);

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
      // 443), GIT (port 9418), and SSH (port 22) protocols.
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
        case repository_protocol::ssh:   def_port =   22; break;
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

    // Convert the local repository location path to lower case on Windows.
    //
    // Note that we need to do that prior to stripping the special path
    // components to match them case-insensitively, so, for example, the
    // c:\pkg\1\stable and c:\Pkg\1\stable (or c:\repo.git and c:\repo.Git)
    // repository locations end up with the same canonical name.
    //
    #ifdef _WIN32
    const path& p (local () ? path (lcase (up.string ())) : up);
    #else
    const path& p (up);
    #endif

    switch (type_)
    {
    case repository_type::pkg:
      {
        // Produce the pkg repository canonical name <prefix>/<path> part (see
        // the Repository Chaining documentation for more details).
        //
        sp = strip_path (p,
                         remote ()
                         ? strip_mode::component
                         : strip_mode::path);

        // If for an absolute path location the stripping result is empty
        // (which also means <path> part is empty as well) then fallback to
        // stripping just the version component.
        //
        if (absolute () && sp.empty ())
          sp = strip_path (p, strip_mode::version);

        break;
      }
    case repository_type::dir:
      {
        // For dir repository we use the absolute (normalized) path.
        //
        sp = p;
        break;
      }
    case repository_type::git:
      {
        // For git repository we use the absolute (normalized) path, stripping
        // the .git extension if present.
        //
        sp = strip_path (p, strip_mode::extension);
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

  string repository_location::
  string () const
  {
    if (empty ()    ||
        relative () ||
        guess_type (url_, false /* local */) == type_)
      return url_.string ();

    std::string r (to_string (type_) + '+');

    // Enforce the 'file://' notation for local URLs, adding the empty
    // authority (see manifest.hxx for details).
    //
    if (url_.scheme == repository_protocol::file &&
        !url_.authority                          &&
        !url_.fragment)
    {
      repository_url u (url_.scheme,
                        repository_url::authority_type (),
                        url_.path);

      r += u.string ();
    }
    else
      r += url_.string ();

    return r;
  }

  // git_ref_filter
  //
  git_ref_filter::
  git_ref_filter (const string& rf)
  {
    exclusion = rf[0] == '-';

    // Strip the leading +/-.
    //
    const string& s (exclusion || rf[0] == '+' ? string (rf, 1) : rf);

    size_t p (s.find ('@'));

    if (p != string::npos)
    {
      if (p != 0)
        name = string (s, 0, p);

      if (p + 1 != s.size ())
        commit = string (s, p + 1);
    }
    else if (!s.empty ())
    {
      // A 40-characters fragment that consists of only hexadecimal digits is
      // assumed to be a commit id.
      //
      if (s.size () == 40 &&
          find_if_not (s.begin (), s.end (),

                       // Resolve the required overload.
                       //
                       static_cast<bool (*)(char)> (xdigit)) == s.end ())
        commit = s;
      else
        name = s;
    }

    if (!name && !commit)
      throw invalid_argument (
        "missing refname or commit id for git repository");

    if (commit && commit->size () != 40)
      throw invalid_argument (
        "git repository commit id must be 40 characters long");
  }

  git_ref_filters
  parse_git_ref_filters (const optional<string>& fs)
  {
    if (!fs)
      return git_ref_filters ({git_ref_filter ()});

    const string& s (*fs);

    git_ref_filters r;
    bool def (s[0] == '#');

    if (def)
      r.push_back (git_ref_filter ());

    for (size_t p (def ? 1 : 0); p != string::npos; )
    {
      size_t e (s.find (',', p));
      r.emplace_back (string (s, p, e != string::npos ? e - p : e));
      p = e != string::npos ? e + 1 : e;
    }
    return r;
  }

  // repository_manifest
  //
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
                             bool iu,
                             bool verify_version = true)
  {
    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.name_line, nv.name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv.value_line, nv.value_column, d);});

    // Make sure this is the start and we support the version.
    //
    if (verify_version)
    {
      if (!nv.name.empty ())
        bad_name ("start of repository manifest expected");

      if (nv.value != "1")
        bad_value ("unsupported format version");

      nv = p.next ();
    }

    repository_manifest r;

    // The repository type value can go after the location value. So we need to
    // postpone the location value parsing until we went though all other
    // values.
    //
    optional<repository_type> type;
    optional<name_value> location;

    for (; !nv.empty (); nv = p.next ())
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
      else if (n == "trust")
      {
        if (r.trust)
          bad_name ("trust redefinition");

        if (!valid_fingerprint (v))
          bad_value ("invalid fingerprint");

        r.trust = move (v);
      }
      else if (n == "fragment")
      {
        if (r.fragment)
          bad_name ("fragment redefinition");

        if (v.empty ())
          bad_value ("empty fragment");

        r.fragment = move (v);
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

      if (!type)
        type = guess_type (u, false); // Can't throw.

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

    // Verify that all non-optional values were specified and the optional ones
    // are allowed.
    //
    // - location can be omitted
    // - role can be omitted
    // - trust, url, email, summary, description and certificate are allowed
    //
    bool base (r.effective_role () == repository_role::base);

    if (r.location.empty () != base)
      bad_value (r.location.empty ()
                 ? "no location specified"
                 : "location not allowed");

    if (r.trust && (base || r.location.type () != repository_type::pkg))
      bad_value ("trust not allowed");

    if (!base)
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

    bool b (effective_role () == repository_role::base);

    if (location.empty () != b)
      bad_value (
        location.empty () ? "no location specified" : "location not allowed");

    s.next ("", "1"); // Start of manifest.

    // Note that the location can be relative, so we also serialize the
    // repository type.
    //
    if (!location.empty ())
    {
      s.next ("location", location.string ());
      s.next ("type", to_string (location.type ()));
    }

    if (role)
    {
      auto r (static_cast<size_t> (*role));
      assert (r < repository_role_names.size ());
      s.next ("role", repository_role_names[r]);
    }

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

    if (trust)
    {
      assert (b || !location.empty ());

      if (b || location.type () != repository_type::pkg)
        bad_value ("trust not allowed");

      s.next ("trust", *trust);
    }

    if (fragment)
      s.next ("fragment", *fragment);

    s.next ("", ""); // End of manifest.
  }

  static repository_manifest empty_base;

  const repository_manifest&
  find_base_repository (const vector<repository_manifest>& ms) noexcept
  {
    for (const repository_manifest& m: ms)
    {
      if (m.effective_role () == repository_role::base)
        return m;
    }

    return empty_base;
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
                              optional<repositories_manifest_header>& header,
                              vector<repository_manifest>& ms)
  {
    // Return nullopt on eos. Otherwise, parse and verify the
    // manifest-starting format version value and return the subsequent
    // manifest value, that can potentially be empty (for an empty manifest).
    //
    // Also save the manifest-starting position (start_nv) for the
    // diagnostics.
    //
    name_value start_nv;
    auto next_manifest = [&p, &start_nv] () -> optional<name_value>
    {
      start_nv = p.next ();

      if (start_nv.empty ())
        return nullopt;

      // Make sure this is the start and we support the version.
      //
      if (!start_nv.name.empty ())
        throw parsing (p.name (), start_nv.name_line, start_nv.name_column,
                       "start of repository manifest expected");

      if (start_nv.value != "1")
        throw parsing (p.name (), start_nv.value_line, start_nv.value_column,
                       "unsupported format version");

      return p.next ();
    };

    optional<name_value> nv (next_manifest ());

    if (!nv)
      throw parsing (p.name (), start_nv.name_line, start_nv.name_column,
                     "start of repository manifest expected");

    auto bad_name ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv->name_line, nv->name_column, d);});

    auto bad_value ([&p, &nv](const string& d) {
        throw parsing (p.name (), nv->value_line, nv->value_column, d);});

    // First check if this a header manifest, if any manifest is present.
    //
    // Note that if this is none of the known header values, then we assume
    // this is a repository manifest (rather than a header that starts with an
    // unknown value; so use one of the original names to make sure it's
    // recognized as such, for example `compression:none`).
    //
    if (nv->name == "min-bpkg-version" ||
        nv->name == "compression")
    {
      header = repositories_manifest_header ();

      // First verify the version, if any.
      //
      if (nv->name == "min-bpkg-version")
      try
      {
        const string& v (nv->value);
        standard_version mbv (v, standard_version::allow_earliest);

        if (mbv > standard_version (LIBBPKG_VERSION_STR))
          bad_value (
            "incompatible repositories manifest: minimum bpkg version is " + v);

        header->min_bpkg_version = move (mbv);

        nv = p.next ();
      }
      catch (const invalid_argument& e)
      {
        bad_value (string ("invalid minimum bpkg version: ") + e.what ());
      }

      // Parse the remaining header values, failing if min-bpkg-version is
      // encountered (should be first).
      //
      for (; !nv->empty (); nv = p.next ())
      {
        const string& n (nv->name);
        string& v (nv->value);

        if (n == "min-bpkg-version")
        {
          bad_name ("minimum bpkg version must be first in repositories "
                    "manifest header");
        }
        else if (n == "compression")
        {
          header->compression = move (v);
        }
        else if (!iu)
          bad_name ("unknown name '" + n + "' in repositories manifest header");
      }

      nv = next_manifest ();
    }

    // Parse the manifest list.
    //
    // Note that if nv is present, then it contains the manifest's first
    // value, which can potentially be empty (for an empty manifest, which is
    // recognized as a base manifest).
    //
    // Also note that if the header is present but is not followed by
    // repository manifests (there is no ':' line after the header values),
    // then the empty manifest list is returned (no base manifest is
    // automatically added).
    //
    bool base (false);

    while (nv)
    {
      ms.push_back (parse_repository_manifest (p,
                                               *nv,
                                               base_type,
                                               iu,
                                               false /* verify_version */));

      // Make sure that there is a single base repository manifest in the
      // list.
      //
      if (ms.back ().effective_role () == repository_role::base)
      {
        if (base)
          throw parsing (p.name (), start_nv.name_line, start_nv.name_column,
                         "base repository manifest redefinition");
        base = true;
      }

      nv = next_manifest ();
    }
  }

  // Serialize the repository manifest list.
  //
  static void
  serialize_repository_manifests (
    serializer& s,
    const optional<repositories_manifest_header>& header,
    const vector<repository_manifest>& ms)
  {
    if (header)
    {
      s.next ("", "1"); // Start of manifest.

      const repositories_manifest_header& h (*header);

      if (h.min_bpkg_version)
        s.next ("min-bpkg-version", h.min_bpkg_version->string ());

      if (h.compression)
        s.next ("compression", *h.compression);

      s.next ("", ""); // End of manifest.
    }

    for (const repository_manifest& r: ms)
      r.serialize (s);

    s.next ("", ""); // End of stream.
  }

  // pkg_repository_manifests
  //
  pkg_repository_manifests::
  pkg_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::pkg, iu, header, *this);
  }

  void pkg_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, header, *this);
  }

  // dir_repository_manifests
  //
  dir_repository_manifests::
  dir_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::dir, iu, header, *this);
  }

  void dir_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, header, *this);
  }

  // git_repository_manifests
  //
  git_repository_manifests::
  git_repository_manifests (parser& p, bool iu)
  {
    parse_repository_manifests (p, repository_type::git, iu, header, *this);
  }

  void git_repository_manifests::
  serialize (serializer& s) const
  {
    serialize_repository_manifests (s, header, *this);
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

  // extract_package_*()
  //
  package_name
  extract_package_name (const char* s, bool allow_version)
  {
    if (!allow_version)
      return package_name (s);

    // Calculate the package name length as a length of the prefix that
    // doesn't contain spaces, slashes and the version constraint starting
    // characters. Note that none of them are valid package name characters.
    //
    size_t n (strcspn (s, " /=<>([~^"));
    return package_name (string (s, n));
  }

  version
  extract_package_version (const char* s, version::flags fl)
  {
    using traits = string::traits_type;

    if (const char* p = traits::find (s, traits::length (s), '/'))
    {
      version r (p + 1, fl);

      if (r.release && r.release->empty ())
        throw invalid_argument ("earliest version");

      if (r.compare (stub_version, true /* ignore_revision */) == 0)
        throw invalid_argument ("stub version");

      return r;
    }

    return version ();
  }
}
