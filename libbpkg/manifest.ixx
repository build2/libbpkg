// file      : libbpkg/manifest.ixx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <stdexcept> // logic_error

namespace bpkg
{
  // version
  //
  inline int version::
  compare (const version& v, bool ir, bool ii) const noexcept
  {
    if (epoch != v.epoch)
      return epoch < v.epoch ? -1 : 1;

    if (int c = canonical_upstream.compare (v.canonical_upstream))
      return c;

    if (int c = canonical_release.compare (v.canonical_release))
      return c;

    if (!ir)
    {
      if (revision != v.revision)
        return revision < v.revision ? -1 : 1;

      if (!ii && iteration != v.iteration)
        return iteration < v.iteration ? -1 : 1;
    }

    return 0;
  }

  inline bool version::
  operator< (const version& v) const noexcept
  {
    return compare (v) < 0;
  }

  inline bool version::
  operator> (const version& v) const noexcept
  {
    return compare (v) > 0;
  }

  inline bool version::
  operator== (const version& v) const noexcept
  {
    return compare (v) == 0;
  }

  inline bool version::
  operator<= (const version& v) const noexcept
  {
    return compare (v) <= 0;
  }

  inline bool version::
  operator>= (const version& v) const noexcept
  {
    return compare (v) >= 0;
  }

  inline bool version::
  operator!= (const version& v) const noexcept
  {
    return compare (v) != 0;
  }

  inline version::flags
  operator&= (version::flags& x, version::flags y)
  {
    return x = static_cast<version::flags> (
      static_cast<std::uint16_t> (x) &
      static_cast<std::uint16_t> (y));
  }

  inline version::flags
  operator|= (version::flags& x, version::flags y)
  {
    return x = static_cast<version::flags> (
      static_cast<std::uint16_t> (x) |
      static_cast<std::uint16_t> (y));
  }

  inline version::flags
  operator& (version::flags x, version::flags y)
  {
    return x &= y;
  }

  inline version::flags
  operator| (version::flags x, version::flags y)
  {
    return x |= y;
  }

  // version_constraint
  //
  inline bool
  operator== (const version_constraint& x, const version_constraint& y)
  {
    return x.min_version == y.min_version && x.max_version == y.max_version &&
           x.min_open == y.min_open && x.max_open == y.max_open;
  }

  inline bool
  operator!= (const version_constraint& x, const version_constraint& y)
  {
    return !(x == y);
  }

  // dependency
  //
  inline std::string dependency::
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

  inline std::ostream&
  operator<< (std::ostream& os, const dependency& d)
  {
    return os << d.string ();
  }

  // dependency_alternatives
  //
  inline bool dependency_alternatives::
  conditional () const
  {
    for (const dependency_alternative& da: *this)
    {
      if (da.enable)
        return true;
    }

    return false;
  }

  // requirement_alternatives
  //
  inline bool requirement_alternatives::
  conditional () const
  {
    for (const requirement_alternative& ra: *this)
    {
      if (ra.enable)
        return true;
    }

    return false;
  }

  // distribution_name_value
  //
  inline butl::optional<std::string> distribution_name_value::
  distribution (const std::string& s) const
  {
    using namespace std;

    size_t sn (s.size ());
    size_t nn (name.size ());

    if (nn > sn && name.compare (nn - sn, sn, s) == 0)
    {
      size_t p (name.find ('-'));

      if (p == nn - sn)
        return string (name, 0, p);
    }

    return butl::nullopt;
  }

  // package_manifest_flags
  //
  inline package_manifest_flags
  operator&= (package_manifest_flags& x, package_manifest_flags y)
  {
    return x = static_cast<package_manifest_flags> (
      static_cast<std::uint16_t> (x) &
      static_cast<std::uint16_t> (y));
  }

  inline package_manifest_flags
  operator|= (package_manifest_flags& x, package_manifest_flags y)
  {
    return x = static_cast<package_manifest_flags> (
      static_cast<std::uint16_t> (x) |
      static_cast<std::uint16_t> (y));
  }

  inline package_manifest_flags
  operator& (package_manifest_flags x, package_manifest_flags y)
  {
    return x &= y;
  }

  inline package_manifest_flags
  operator| (package_manifest_flags x, package_manifest_flags y)
  {
    return x |= y;
  }

  // build_class_expr
  //
  inline bool build_class_expr::
  match (const strings& cs, const build_class_inheritance_map& bs) const
  {
    bool r (false);
    match (cs, bs, r);
    return r;
  }

  // package_manifest
  //
  inline package_manifest::
  package_manifest (butl::manifest_parser& p,
                    bool iu,
                    bool cv,
                    package_manifest_flags fl)
      : package_manifest (p, std::function<translate_function> (), iu, cv, fl)
  {
  }

  inline package_manifest
  pkg_package_manifest (butl::manifest_parser& p, bool iu, bool cvs)
  {
    return package_manifest (p, iu, cvs);
  }

  inline std::string package_manifest::
  effective_type (const butl::optional<std::string>& t, const package_name& n)
  {
    if (t)
    {
      std::string tp (*t, 0, t->find (','));
      butl::trim (tp);
      return tp == "exe" || tp == "lib" ? tp : "other";
    }

    const std::string& s (n.string ());
    return s.size () > 3 && s.compare (0, 3, "lib") == 0 ? "lib" : "exe";
  }

  inline std::string package_manifest::
  effective_type () const
  {
    return effective_type (type, name);
  }

  inline strings package_manifest::
  effective_type_sub_options () const
  {
    return effective_type_sub_options (type);
  }

  inline butl::small_vector<language, 1> package_manifest::
  effective_languages (const butl::small_vector<language, 1>& ls,
                       const package_name& n)
  {
    if (!ls.empty ())
      return ls;

    std::string ext (n.extension ());
    return butl::small_vector<language, 1> (
      1,
      language (!ext.empty () ? move (ext) : "cc", false /* impl */));
  }

  inline butl::small_vector<language, 1> package_manifest::
  effective_languages () const
  {
    return effective_languages (languages, name);
  }

  // repository_location
  //
  inline repository_type repository_location::
  type () const
  {
    if (empty ())
      throw std::logic_error ("empty location");

    return type_;
  }

  inline repository_location::
  repository_location (const repository_location& l,
                       const repository_location& base)
      : repository_location (l.url (), l.type (), base)
  {
  }

  inline bool repository_location::
  local () const
  {
    if (empty ())
      throw std::logic_error ("empty location");

    return url_.scheme == repository_protocol::file;
  }

  inline bool repository_location::
  remote () const
  {
    return !local ();
  }

  inline bool repository_location::
  absolute () const
  {
    if (empty ())
      throw std::logic_error ("empty location");

    // Note that in remote locations path is always relative.
    //
    return url_.path->absolute ();
  }

  inline bool repository_location::
  relative () const
  {
    return local () && url_.path->relative ();
  }

  inline repository_basis repository_location::
  basis () const
  {
    switch (type ())
    {
    case repository_type::pkg: return repository_basis::archive;
    case repository_type::dir: return repository_basis::directory;
    case repository_type::git: return repository_basis::version_control;
    }

    assert (false); // Can't be here.
    return repository_basis::archive;
  }

  inline bool repository_location::
  archive_based () const
  {
    return basis () == repository_basis::archive;
  }

  inline bool repository_location::
  directory_based () const
  {
    return basis () == repository_basis::directory;
  }

  inline bool repository_location::
  version_control_based () const
  {
    return basis () == repository_basis::version_control;
  }

  inline const butl::path& repository_location::
  path () const
  {
    if (empty ())
      throw std::logic_error ("empty location");

    return *url_.path;
  }

  inline const std::string& repository_location::
  host () const
  {
    if (local ())
      throw std::logic_error ("local location");

    return url_.authority->host;
  }

  inline std::uint16_t repository_location::
  port () const
  {
    if (local ())
      throw std::logic_error ("local location");

    return url_.authority->port;
  }

  inline repository_protocol repository_location::
  proto () const
  {
    if (empty ())
      throw std::logic_error ("empty location");

    return url_.scheme;
  }

  inline const butl::optional<std::string>& repository_location::
  fragment () const
  {
    if (relative ())
      throw std::logic_error ("relative filesystem path");

    return url_.fragment;
  }
}
