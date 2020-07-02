// file      : libbpkg/manifest.cxx -*- C++ -*-
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

#include <libbutl/url.mxx>
#include <libbutl/path.mxx>
#include <libbutl/base64.mxx>
#include <libbutl/utility.mxx>             // icasecmp(), lcase(), alnum(),
                                           // digit(), xdigit(), next_word()
#include <libbutl/small-vector.mxx>
#include <libbutl/manifest-parser.mxx>
#include <libbutl/manifest-serializer.mxx>
#include <libbutl/standard-version.mxx>

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
                     false /* fold_zero_revision */).
            canonical_upstream),
        canonical_release (
          data_type (release ? release->c_str () : nullptr,
                     data_type::parse::release,
                     false /* fold_zero_revision */).
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
  data_type (const char* v, parse pr, bool fold_zero_rev)
  {
    if (fold_zero_rev)
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

    assert (v != nullptr);

    optional<uint16_t> ep;

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

            ep = uint16 (string (cb, p), "epoch");
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

      std::uint16_t rev (uint16 (cb, "revision"));

      if (rev != 0 || !fold_zero_rev)
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
        min_version = version (mnv, false /* fold_zero_revision */);
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
        max_version = version (mxv, false /* fold_zero_revision */);
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
          v = version (vs, false /* fold_zero_revision */);

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

  std::string dependency::
  string () const
  {
    std::string r (name.string ());

    if (constraint)
    {
      r += ' ';
      r += constraint->string ();
    }

    return r;
  }

  // dependency_alternatives
  //
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
  build_class_term (build_class_term&& t)
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
  operator= (build_class_term&& t)
  {
    if (this != &t)
    {
      this->~build_class_term ();

      // Assume noexcept move-construction.
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
        "class name '" + s + "' starts with '" + c + "'");

    for (; i != s.size (); ++i)
    {
      if (!(alnum (c = s[i]) || c == '+' || c == '-' || c == '_' || c == '.'))
        throw invalid_argument (
          "class name '" + s + "' contains '" + c + "'");
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
    else throw invalid_argument ("invalid test dependency type '" + t + "'");
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
            : parsing (d + " in '" + v + "'");
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

  const version stub_version (0, "0", nullopt, nullopt, 0);

  static void
  parse_package_manifest (
    parser& p,
    name_value nv,
    const function<package_manifest::translate_function>& tf,
    bool iu,
    bool cd,
    package_manifest_flags fl,
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

    auto parse_email = [&bad_name] (const name_value& nv,
                                    optional<email>& r,
                                    const char* what,
                                    const string& source_name,
                                    bool empty = false)
    {
      if (r)
        bad_name (what + string (" email redefinition"));

      r = bpkg::parse_email (nv, what, source_name, empty);
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

    auto flag = [fl] (package_manifest_flags f)
    {
      return (fl & f) != package_manifest_flags::none;
    };

    // Cache the upstream version manifest value and validate whether it's
    // allowed later, after the version value is parsed.
    //
    optional<name_value> upstream_version;

    // We will cache the depends and the test dependency manifest values to
    // parse and, if requested, complete the version constraints later, after
    // the version value is parsed.
    //
    vector<name_value> dependencies;
    small_vector<name_value, 1> tests;

    // We will cache the description and its type values to validate them
    // later, after both are parsed.
    //
    optional<name_value> description;
    optional<name_value> description_type;

    for (nv = p.next (); !nv.empty (); nv = p.next ())
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

        if (tf)
        {
          tf (m.version);

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
            bad_name ("package description and description-file are "
                      "mutually exclusive");
          else
            bad_name ("package description redefinition");
        }

        if (v.empty ())
          bad_value ("empty package description");

        description = move (nv);
      }
      else if (n == "description-file")
      {
        if (flag (package_manifest_flags::forbid_file))
          bad_name ("package description-file not allowed");

        if (description)
        {
          if (description->name == "description-file")
            bad_name ("package description-file redefinition");
          else
            bad_name ("package description-file and description are "
                      "mutually exclusive");
        }

        description = move (nv);
      }
      else if (n == "description-type")
      {
        if (description_type)
          bad_name ("package description-type redefinition");

        description_type = move (nv);
      }
      else if (n == "changes")
      {
        if (v.empty ())
          bad_value ("empty package changes specification");

        m.changes.emplace_back (move (v));
      }
      else if (n == "changes-file")
      {
        if (flag (package_manifest_flags::forbid_file))
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
        if (m.url)
          bad_name ("project url redefinition");

        m.url = parse_url (v, "project");
      }
      else if (n == "email")
      {
        parse_email (nv, m.email, "project", p.name ());
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
        parse_email (nv, m.package_email, "package", p.name ());
      }
      else if (n == "build-email")
      {
        parse_email (nv, m.build_email, "build", p.name (), true /* empty */);
      }
      else if (n == "build-warning-email")
      {
        parse_email (nv, m.build_warning_email, "build warning", p.name ());
      }
      else if (n == "build-error-email")
      {
        parse_email (nv, m.build_error_email, "build error", p.name ());
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
      else if (n == "builds")
      {
        m.builds.push_back (
          parse_build_class_expr (nv, m.builds.empty (), p.name ()));
      }
      else if (n == "build-include")
      {
        m.build_constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, p.name ()));
      }
      else if (n == "build-exclude")
      {
        m.build_constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, p.name ()));
      }
      else if (n == "depends")
      {
        dependencies.push_back (move (nv));
      }
      else if (n == "tests" || n == "examples" || n == "benchmarks")
      {
        tests.push_back (move (nv));
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

    // Verify that description is specified if the description type is
    // specified.
    //
    if (description_type && !description)
      bad_value ("no package description for specified description type");

    // Validate (and set) description and its type.
    //
    if (description)
    {
      // Restore as bad_value() uses its line/column.
      //
      nv = move (*description);

      string& v (nv.value);

      if (nv.name == "description-file")
      {
        auto vc (parser::split_comment (v));

        path p;
        try
        {
          p = path (move (vc.first));
        }
        catch (const invalid_path& e)
        {
          bad_value (string ("invalid package description file: ") +
                     e.what ());
        }

        if (p.empty ())
          bad_value ("no path in package description-file");

        if (p.absolute ())
          bad_value ("package description-file path is absolute");

        m.description = text_file (move (p), move (vc.second));
      }
      else
        m.description = text_file (move (v));

      if (description_type)
        m.description_type = move (description_type->value);

      // Verify the description type.
      //
      try
      {
        m.effective_description_type (iu);
      }
      catch (const invalid_argument& e)
      {
        if (description_type)
        {
          // Restore as bad_value() uses its line/column.
          //
          nv = move (*description_type);

          bad_value (string ("invalid package description type: ") +
                     e.what ());
        }
        else
          bad_value (string ("invalid package description file: ") +
                     e.what ());
      }
    }

    // Now, when the version manifest value is parsed, we can parse the
    // dependencies and complete their constraints, if requested.
    //
    auto parse_dependency = [&m, cd, &flag, &bad_value] (string&& d,
                                                         const char* what)
    {
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

      package_name nm;

      try
      {
        nm = package_name (i == e ? move (d) : string (b, ne));
      }
      catch (const invalid_argument& e)
      {
        bad_value (string ("invalid ") + what + " package name: " +
                   e.what ());
      }

      dependency r;

      if (i == e)
        r = dependency {move (nm), nullopt};
      else
      {
        try
        {
          version_constraint vc (string (i, e));

          if (!vc.complete () &&
              flag (package_manifest_flags::forbid_incomplete_dependencies))
            bad_value ("$ not allowed");

          // Complete the constraint.
          //
          if (cd)
            vc = vc.effective (m.version);

          r = dependency {move (nm), move (vc)};
        }
        catch (const invalid_argument& e)
        {
          bad_value (string ("invalid ") + what + " package constraint: " +
                     e.what ());
        }
      }

      return r;
    };

    // Parse the regular dependencies.
    //
    for (name_value& d: dependencies)
    {
      nv = move (d); // Restore as bad_value() uses its line/column.

      const string& v (nv.value);

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
        da.push_back (parse_dependency (move (lv), "prerequisite"));

      if (da.empty ())
        bad_value ("empty package dependency specification");

      m.dependencies.push_back (da);
    }

    // Parse the test dependencies.
    //
    for (name_value& v: tests)
    {
      nv = move (v); // Restore as bad_value() uses its line/column.

      dependency d (parse_dependency (move (nv.value), nv.name.c_str ()));

      try
      {
        m.tests.emplace_back (
          move (d.name),
          to_test_dependency_type (nv.name),
          move (d.constraint));
      }
      catch (const invalid_argument&)
      {
        // to_test_dependency_type() can't throw since the type string is
        // already validated.
        //
        assert (false);
      }
    }

    if (m.description       &&
        !m.description_type &&
        flag (package_manifest_flags::require_description_type))
      bad_name ("no package description type specified");

    if (!m.location && flag (package_manifest_flags::require_location))
      bad_name ("no package location specified");

    if (!m.sha256sum && flag (package_manifest_flags::require_sha256sum))
      bad_name ("no package sha256sum specified");
  }

  package_manifest
  pkg_package_manifest (parser& p, name_value nv, bool iu)
  {
    return package_manifest (
      p,
      move (nv),
      iu,
      false /* complete_depends */,
      package_manifest_flags::forbid_file              |
      package_manifest_flags::require_description_type |
      package_manifest_flags::require_location         |
      package_manifest_flags::forbid_fragment          |
      package_manifest_flags::forbid_incomplete_dependencies);
  }

  // package_manifest
  //
  package_manifest::
  package_manifest (manifest_parser& p,
                    const function<translate_function>& tf,
                    bool iu,
                    bool cd,
                    package_manifest_flags fl)
  {
    parse_package_manifest (p, p.next (), tf, iu, cd, fl, *this);

    // Make sure this is the end.
    //
    name_value nv (p.next ());
    if (!nv.empty ())
      throw parsing (p.name (), nv.name_line, nv.name_column,
                     "single package manifest expected");
  }

  package_manifest::
  package_manifest (manifest_parser& p,
                    bool iu,
                    bool cd,
                    package_manifest_flags fl)
      : package_manifest (p, function<translate_function> (), iu, cd, fl)
  {
  }

  package_manifest::
  package_manifest (manifest_parser& p,
                    name_value nv,
                    bool iu,
                    bool cd,
                    package_manifest_flags fl)
  {
    parse_package_manifest (
      p, move (nv), function<translate_function> (), iu, cd, fl, *this);
  }

  optional<text_type> package_manifest::
  effective_description_type (bool iu) const
  {
    if (!description)
      throw logic_error ("absent description");

    optional<text_type> r;

    if (description_type)
      r = to_text_type (*description_type);
    else if (description->file)
    {
      string ext (description->path.extension ());
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

  void package_manifest::
  override (const vector<manifest_name_value>& nvs, const string& name)
  {
    // Reset the build constraints value sub-group on the first call.
    //
    bool rbc (true);
    auto reset_build_constraints = [&rbc, this] ()
    {
      if (rbc)
      {
        build_constraints.clear ();
        rbc = false;
      }
    };

    // Reset the builds value group on the first call.
    //
    bool rb (true);
    auto reset_builds = [&rb, &reset_build_constraints, this] ()
    {
      if (rb)
      {
        builds.clear ();
        reset_build_constraints ();
        rb = false;
      }
    };

    // Reset the build emails value group on the first call.
    //
    bool rbe (true);
    auto reset_build_emails = [&rbe, this] ()
    {
      if (rbe)
      {
        build_email = nullopt;
        build_warning_email = nullopt;
        build_error_email = nullopt;
        rbe = false;
      }
    };

    for (const manifest_name_value& nv: nvs)
    {
      const string& n (nv.name);

      if (n == "builds")
      {
        reset_builds ();
        builds.push_back (parse_build_class_expr (nv, builds.empty (), name));
      }
      else if (n == "build-include")
      {
        reset_build_constraints ();

        build_constraints.push_back (
          parse_build_constraint (nv, false /* exclusion */, name));
      }
      else if (n == "build-exclude")
      {
        reset_build_constraints ();

        build_constraints.push_back (
          parse_build_constraint (nv, true /* exclusion */, name));
      }
      else if (n == "build-email")
      {
        reset_build_emails ();
        build_email = parse_email (nv, "build", name, true /* empty */);
      }
      else if (n == "build-warning-email")
      {
        reset_build_emails ();
        build_warning_email = parse_email (nv, "build warning", name);
      }
      else if (n == "build-error-email")
      {
        reset_build_emails ();
        build_error_email = parse_email (nv, "build error", name);
      }
      else
      {
        string d ("cannot override '" + n + "' value");

        throw !name.empty ()
              ? parsing (name, nv.name_line, nv.name_column, d)
              : parsing (d);
      }
    }
  }

  void package_manifest::
  validate_overrides (const vector<manifest_name_value>& nvs,
                      const string& name)
  {
    package_manifest p;
    p.override (nvs, name);
  }

  static const string description_file ("description-file");
  static const string changes_file     ("changes-file");

  void package_manifest::
  load_files (const function<load_function>& loader, bool iu)
  {
    auto load = [&loader] (const string& n, const path& p)
    {
      string r (loader (n, p));

      if (r.empty ())
        throw parsing ("package " + n + " references empty file");

      return r;
    };

    // Load the description-file manifest value.
    //
    if (description)
    {
      // Make the description type explicit.
      //
      optional<text_type> t (effective_description_type (iu)); // Can throw.

      assert (t || iu); // Can only be absent if we ignore unknown.

      if (!description_type && t)
        description_type = to_string (*t);

      // At this point the description type can only be absent if the
      // description comes from a file. Otherwise, we would end up with the
      // plain text.
      //
      assert (description_type || description->file);

      if (description->file)
      {
        if (!description_type)
          description_type = "text/unknown; extension=" +
                             description->path.extension ();

        description = text_file (load (description_file, description->path));
      }
    }

    // Load the changes-file manifest values.
    //
    for (text_file& c: changes)
    {
      if (c.file)
        c = text_file (load (changes_file, c.path));
    }
  }

  static void
  serialize_package_manifest (manifest_serializer& s,
                              const package_manifest& m,
                              bool header_only)
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

      if (m.description)
      {
        if (m.description->file)
          s.next ("description-file",
                  serializer::merge_comment (m.description->path.string (),
                                             m.description->comment));
        else
          s.next ("description", m.description->text);

        if (m.description_type)
          s.next ("description-type", *m.description_type);
      }

      for (const auto& c: m.changes)
      {
        if (c.file)
          s.next ("changes-file",
                  serializer::merge_comment (c.path.string (), c.comment));
        else
          s.next ("changes", c.text);
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
        s.next ("depends",
                (d.conditional
                 ? (d.buildtime ? "?* " : "? ")
                 : (d.buildtime ? "* " : "")) +
                serializer::merge_comment (concatenate (d, " | "), d.comment));

      for (const requirement_alternatives& r: m.requirements)
        s.next ("requires",
                (r.conditional
                 ? (r.buildtime ? "?* " : "? ")
                 : (r.buildtime ? "* " : "")) +
                serializer::merge_comment (concatenate (r, " | "), r.comment));

      for (const test_dependency& p: m.tests)
        s.next (to_string (p.type), p.string ());

      for (const build_class_expr& e: m.builds)
        s.next ("builds", serializer::merge_comment (e.string (), e.comment));

      for (const build_constraint& c: m.build_constraints)
        s.next (c.exclusion ? "build-exclude" : "build-include",
                serializer::merge_comment (!c.target
                                           ? c.config
                                           : c.config + "/" + *c.target,
                                           c.comment));

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
  serialize (serializer& s) const
  {
    serialize_package_manifest (s, *this, false);
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
          s.name (),
          d + " for " + p.name.string () + "-" + p.version.string ());
      };

      if (p.description)
      {
        if (p.description->file)
          bad_value ("forbidden description-file");

        if (!p.description_type)
          bad_value ("no valid description-type");
      }

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

    throw invalid_argument ("invalid repository type '" + t + "'");
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
                              vector<repository_manifest>& ms)
  {
    bool base (false);

    for (name_value nv (p.next ()); !nv.empty (); nv = p.next ())
    {
      ms.push_back (parse_repository_manifest (p, nv, base_type, iu));

      // Make sure that there is a single base repository manifest in the
      // list.
      //
      if (ms.back ().effective_role () == repository_role::base)
      {
        if (base)
          throw parsing (p.name (), nv.name_line, nv.name_column,
                         "base repository manifest redefinition");
        base = true;
      }
    }
  }

  // Serialize the repository manifest list.
  //
  static void
  serialize_repository_manifests (serializer& s,
                                  const vector<repository_manifest>& ms)
  {
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
