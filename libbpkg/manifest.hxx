// file      : libbpkg/manifest.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBPKG_MANIFEST_HXX
#define LIBBPKG_MANIFEST_HXX

#include <string>
#include <vector>
#include <cassert>
#include <cstdint>   // uint16_t
#include <ostream>
#include <utility>   // move()
#include <stdexcept> // logic_error

#include <libbutl/url.mxx>
#include <libbutl/path.mxx>
#include <libbutl/optional.mxx>
#include <libbutl/manifest-forward.hxx>

#include <libbpkg/export.hxx>
#include <libbpkg/version.hxx>

namespace bpkg
{
  using strings = std::vector<std::string>;

  // @@ Let's create <libbpkg/types.hxx> with "basic" package types.
  //
  class LIBBPKG_EXPORT version
  {
  public:
    // Let's keep the members in the order they appear in the string
    // representation.
    //
    const std::uint16_t epoch;
    const std::string upstream;
    const butl::optional<std::string> release;
    const std::uint16_t revision;

    // Upstream part canonical representation.
    //
    const std::string canonical_upstream;

    // Release part canonical representation.
    //
    const std::string canonical_release;

    // Create a special empty version. It is less than any other valid
    // version (and is conceptually equivalent to 0-).
    //
    version (): epoch (0), release (""), revision (0) {}

    // Throw std::invalid_argument if the passed string is not a valid
    // version representation.
    //
    explicit
    version (const std::string& v): version (v.c_str ()) {}

    explicit
    version (const char* v): version (data_type (v, data_type::parse::full)) {}

    // Create the version object from separate epoch, upstream, release, and
    // revision parts.
    //
    // Note that it is possible (and legal) to create the special empty
    // version via this interface as version(0, string(), string(), 0).
    //
    version (std::uint16_t epoch,
             std::string upstream,
             butl::optional<std::string> release,
             std::uint16_t revision);

    version (version&&) = default;
    version (const version&) = default;
    version& operator= (version&&);
    version& operator= (const version&);

    std::string
    string (bool ignore_revision = false) const;

    bool
    operator< (const version& v) const noexcept {return compare (v) < 0;}

    bool
    operator> (const version& v) const noexcept {return compare (v) > 0;}

    bool
    operator== (const version& v) const noexcept {return compare (v) == 0;}

    bool
    operator<= (const version& v) const noexcept {return compare (v) <= 0;}

    bool
    operator>= (const version& v) const noexcept {return compare (v) >= 0;}

    bool
    operator!= (const version& v) const noexcept {return compare (v) != 0;}

    int
    compare (const version& v, bool ignore_revision = false) const noexcept
    {
      if (epoch != v.epoch)
        return epoch < v.epoch ? -1 : 1;

      if (int c = canonical_upstream.compare (v.canonical_upstream))
        return c;

      if (int c = canonical_release.compare (v.canonical_release))
        return c;

      if (!ignore_revision && revision != v.revision)
        return revision < v.revision ? -1 : 1;

      return 0;
    }

    bool
    empty () const noexcept
    {
      bool e (upstream.empty ());
      assert (!e ||
              (epoch == 0 && release && release->empty () && revision == 0));
      return e;
    }

  private:
    struct LIBBPKG_EXPORT data_type
    {
      enum class parse {full, upstream, release};

      data_type (const char*, parse);

      std::uint16_t epoch;
      std::string upstream;
      butl::optional<std::string> release;
      std::uint16_t revision;
      std::string canonical_upstream;
      std::string canonical_release;
    };

    explicit
    version (data_type&& d)
        : epoch (d.epoch),
          upstream (std::move (d.upstream)),
          release (std::move (d.release)),
          revision (d.revision),
          canonical_upstream (std::move (d.canonical_upstream)),
          canonical_release (std::move (d.canonical_release)) {}
  };

  inline std::ostream&
  operator<< (std::ostream& os, const version& v)
  {
    return os << (v.empty () ? "<empty-version>" : v.string ());
  }

  // priority
  //
  class priority
  {
  public:
    enum value_type {low, medium, high, security};

    value_type value; // Shouldn't be necessary to access directly.
    std::string comment;

    priority (value_type v = low, std::string c = "")
        : value (v), comment (std::move (c)) {}

    operator value_type () const {return value;}
  };

  // description
  // description-file
  // change
  // change-file
  //
  class LIBBPKG_EXPORT text_file
  {
  public:
    using path_type = butl::path;

    bool file;

    union
    {
      std::string text;
      path_type path;
    };

    std::string comment;

    // File text constructor.
    //
    explicit
    text_file (std::string t = ""): file (false), text (std::move (t)) {}

    // File reference constructor.
    //
    text_file (path_type p, std::string c)
        : file (true), path (std::move (p)), comment (std::move (c)) {}

    text_file (text_file&&);
    text_file (const text_file&);
    text_file& operator= (text_file&&);
    text_file& operator= (const text_file&);

    ~text_file ();
  };

  // license
  //
  class licenses: public strings
  {
  public:
    std::string comment;

    explicit
    licenses (std::string c = ""): comment (std::move (c)) {}
  };

  // url
  // package-url
  //
  class url: public std::string
  {
  public:
    std::string comment;

    explicit
    url (std::string u = "", std::string c = "")
        : std::string (std::move (u)), comment (std::move (c)) {}
  };

  // email
  // package-email
  // build-email
  //
  class email: public std::string
  {
  public:
    std::string comment;

    explicit
    email (std::string e = "", std::string c = "")
        : std::string (std::move (e)), comment (std::move (c)) {}
  };

  // depends
  //
  struct LIBBPKG_EXPORT dependency_constraint
  {
    butl::optional<version> min_version;
    butl::optional<version> max_version;
    bool min_open;
    bool max_open;

    dependency_constraint (butl::optional<version> min_version, bool min_open,
                           butl::optional<version> max_version, bool max_open);

    dependency_constraint (const version& v)
        : dependency_constraint (v, false, v, false) {}

    dependency_constraint () = default;

    bool
    empty () const noexcept {return !min_version && !max_version;}
  };

  LIBBPKG_EXPORT std::ostream&
  operator<< (std::ostream&, const dependency_constraint&);

  inline bool
  operator== (const dependency_constraint& x, const dependency_constraint& y)
  {
    return x.min_version == y.min_version && x.max_version == y.max_version &&
      x.min_open == y.min_open && x.max_open == y.max_open;
  }

  inline bool
  operator!= (const dependency_constraint& x, const dependency_constraint& y)
  {
    return !(x == y);
  }

  struct dependency
  {
    std::string name;
    butl::optional<dependency_constraint> constraint;
  };

  LIBBPKG_EXPORT std::ostream&
  operator<< (std::ostream&, const dependency&);

  class dependency_alternatives: public std::vector<dependency>
  {
  public:
    bool conditional;
    bool buildtime;
    std::string comment;

    dependency_alternatives () = default;
    dependency_alternatives (bool d, bool b, std::string c)
        : conditional (d), buildtime (b), comment (std::move (c)) {}
  };

  LIBBPKG_EXPORT std::ostream&
  operator<< (std::ostream&, const dependency_alternatives&);

  // requires
  //
  class requirement_alternatives: public strings
  {
  public:
    bool conditional;
    bool buildtime;
    std::string comment;

    requirement_alternatives () = default;
    requirement_alternatives (bool d, bool b, std::string c)
        : conditional (d), buildtime (b), comment (std::move (c)) {}
  };

  class build_constraint
  {
  public:
    // If true, then the package should not be built for matching
    // configurations by automated build bots.
    //
    bool exclusion;

    // Filesystem wildcard patterns for the build configuration name and
    // target.
    //
    std::string config;
    butl::optional<std::string> target;

    std::string comment;

    build_constraint () = default;
    build_constraint (bool e,
                      std::string n,
                      butl::optional<std::string> t,
                      std::string c)
        : exclusion (e),
          config (std::move (n)),
          target (std::move (t)),
          comment (std::move (c)) {}
  };

  class LIBBPKG_EXPORT package_manifest
  {
  public:
    using version_type = bpkg::version;
    using priority_type = bpkg::priority;
    using url_type = bpkg::url;
    using email_type = bpkg::email;

    std::string name;
    version_type version;
    butl::optional<priority_type> priority;
    std::string summary;
    std::vector<licenses> license_alternatives;
    strings tags;
    butl::optional<text_file> description;
    std::vector<text_file> changes;
    url_type url;
    butl::optional<url_type> doc_url;
    butl::optional<url_type> src_url;
    butl::optional<url_type> package_url;
    email_type email;
    butl::optional<email_type> package_email;
    butl::optional<email_type> build_email;
    std::vector<dependency_alternatives> dependencies;
    std::vector<requirement_alternatives> requirements;
    std::vector<build_constraint> build_constraints;

    // The following values are only valid in the manifest list (and only for
    // certain repository types).
    //
    butl::optional<butl::path> location;
    butl::optional<std::string> sha256sum;

  public:
    package_manifest () = default; // VC export.

    void
    serialize (butl::manifest_serializer&) const;
  };

  // Create individual package manifest.
  //
  LIBBPKG_EXPORT package_manifest
  pkg_package_manifest (butl::manifest_parser&, bool ignore_unknown = false);

  LIBBPKG_EXPORT package_manifest
  git_package_manifest (butl::manifest_parser&, bool ignore_unknown = false);

  // Create an element of the package list manifest.
  //
  LIBBPKG_EXPORT package_manifest
  pkg_package_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown = false);

  LIBBPKG_EXPORT package_manifest
  git_package_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown = false);

  // Serialize.
  //
  inline void
  pkg_package_manifest (butl::manifest_serializer& s,
                        const package_manifest& m)
  {
    m.serialize (s);
  }

  // Normally there is no need to serialize git package manifest, unless for
  // testing.
  //
  LIBBPKG_EXPORT void
  git_package_manifest (butl::manifest_serializer&, const package_manifest&);

  class LIBBPKG_EXPORT pkg_package_manifests:
    public std::vector<package_manifest>
  {
  public:
    using base_type = std::vector<package_manifest>;

    using base_type::base_type;

    // Checksum of the corresponding repository_manifests.
    //
    std::string sha256sum;

  public:
    pkg_package_manifests () = default;
    pkg_package_manifests (butl::manifest_parser&,
                           bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  class LIBBPKG_EXPORT git_package_manifests:
    public std::vector<package_manifest>
  {
  public:
    using base_type = std::vector<package_manifest>;

    using base_type::base_type;

  public:
    git_package_manifests () = default;
    git_package_manifests (butl::manifest_parser&,
                           bool ignore_unknown = false);

    // Normally there is no need to serialize git package manifests, unless for
    // testing.
    //
    void
    serialize (butl::manifest_serializer&) const;
  };

  // Traits class for the repository URL object.
  //
  enum class repository_protocol {file, http, https, git};

  struct LIBBPKG_EXPORT repository_url_traits
  {
    using string_type = std::string;
    using path_type   = butl::path;

    using scheme_type    = repository_protocol;
    using authority_type = butl::basic_url_authority<string_type>;

    static scheme_type
    translate_scheme (const string_type&,
                      string_type&&,
                      butl::optional<authority_type>&,
                      butl::optional<path_type>&,
                      butl::optional<string_type>&,
                      butl::optional<string_type>&);

    static string_type
    translate_scheme (string_type&,
                      const scheme_type&,
                      const butl::optional<authority_type>&,
                      const butl::optional<path_type>&,
                      const butl::optional<string_type>&,
                      const butl::optional<string_type>&);

    static path_type
    translate_path (string_type&&);

    static string_type
    translate_path (const path_type&);
  };

  // Repository URL. Note that it represents both the remote (http(s)://,
  // git://, etc) and local (file:// as well as plain directory path)
  // repository URLs. May also be empty.
  //
  // Notes:
  //
  // - For an empty URL object all components are absent. For a non-empty one
  //   the path is always present and normalized. The string representation of
  //   non-empty object with non-empty path never contains the trailing slash
  //   (except for the root path on POSIX system).
  //
  // - For the remote URL object the host name is in the lower case (IPv4/6 are
  //   not supported) and the path is relative.
  //
  // - For the local URL object the path can be relative or absolute. Query
  //   can not be present. Represent the object using the file:// notation if
  //   it is absolute and the authority or fragment is present. Otherwise
  //   represent it as a local path, appending the fragment if present.
  //
  using repository_url = butl::basic_url<repository_protocol,
                                         repository_url_traits>;

  // Repository type.
  //
  enum class repository_type {pkg, git};

  LIBBPKG_EXPORT std::string
  to_string (repository_type);

  LIBBPKG_EXPORT repository_type
  to_repository_type (const std::string&); // May throw std::invalid_argument.

  inline std::ostream&
  operator<< (std::ostream& os, repository_type t)
  {
    return os << to_string (t);
  }

  // Guess the repository type for the URL:
  //
  // 1. If scheme is git then git.
  //
  // 2. If scheme is http(s), then check if path has the .git extension,
  //    then git, otherwise pkg.
  //
  // 3. If local (which will normally be without the .git extension), check
  //    if directory contains the .git/ subdirectory then git, otherwise pkg.
  //
  // Can throw system_error in the later case.
  //
  LIBBPKG_EXPORT repository_type
  guess_type (const repository_url&, bool local);

  class LIBBPKG_EXPORT repository_location
  {
  public:
    // Create a special empty repository_location.
    //
    repository_location () = default;

    // Create remote, absolute or empty repository location making sure that
    // the URL matches the repository type. Throw std::invalid_argument if the
    // URL object is a relative local path.
    //
    // Note that the repository location string representation may differ from
    // the original URL in the presence of the trailing slash. This may cause
    // problems with some WEB servers that are sensitive to the trailing slash
    // presence/absence. For example:
    //
    // $ git clone http://git.sv.gnu.org/r/config.git
    //   warning: redirecting to http://git.savannah.gnu.org/r/config.git/
    //
    // Also note that we disregard the slash presence/absence on multiple
    // levels:
    //
    // - reduce absent path to an empty one in
    //   repository_url_traits::translate_scheme() (so a.com/ becomes a.com)
    //
    // - use path::*string() rather than path::*representation() functions
    //   in repository_url_traits::translate_*() functions
    //
    // - may append slash in repository_location ctor
    //
    explicit
    repository_location (repository_url, repository_type);

    // Create a potentially relative pkg repository location. If base is not
    // empty, use it to complete the relative location to the remote/absolute
    // one. Throw std::invalid_argument if base is not empty but the location
    // is empty, base itself is relative, or the resulting completed location
    // is invalid.
    //
    repository_location (repository_url,
                         repository_type,
                         const repository_location& base);

    repository_location (const repository_location& l,
                         const repository_location& base)
        : repository_location (l.url (), l.type (), base) {}

    // Note that relative locations have no canonical name. Canonical name of
    // an empty location is the empty name.
    //
    const std::string&
    canonical_name () const noexcept {return canonical_name_;}

    // There are 3 types of locations: remote, local absolute filesystem
    // path and local relative filesystem path. Plus there is the special
    // empty location. The following predicates can be used to determine
    // what kind of location it is. Note that except for empty(), all the
    // other predicates throw std::logic_error for an empty location.
    //
    bool
    empty () const noexcept {return url_.empty ();}

    bool
    local () const
    {
      if (empty ())
        throw std::logic_error ("empty location");

      return url_.scheme == repository_protocol::file;
    }

    bool
    remote () const
    {
      return !local ();
    }

    bool
    absolute () const
    {
      if (empty ())
        throw std::logic_error ("empty location");

      // Note that in remote locations path is always relative.
      //
      return url_.path->absolute ();
    }

    bool
    relative () const
    {
      return local () && url_.path->relative ();
    }

    repository_type
    type () const
    {
      if (empty ())
        throw std::logic_error ("empty location");

      return type_;
    }

    // URL of an empty location is empty.
    //
    const repository_url&
    url () const
    {
      return url_;
    }

    // Repository path. Note that for repository types that refer to
    // "directories" it always contains the trailing slash.
    //
    const butl::path&
    path () const
    {
      if (empty ())
        throw std::logic_error ("empty location");

      return *url_.path;
    }

    const std::string&
    host () const
    {
      if (local ())
        throw std::logic_error ("local location");

      return url_.authority->host;
    }

    // Value 0 indicated that no port was specified explicitly.
    //
    std::uint16_t
    port () const
    {
      if (local ())
        throw std::logic_error ("local location");

      return url_.authority->port;
    }

    repository_protocol
    proto () const
    {
      if (empty ())
        throw std::logic_error ("empty location");

      return url_.scheme;
    }

    const butl::optional<std::string>&
    fragment () const
    {
      if (relative ())
        throw std::logic_error ("relative filesystem path");

      return url_.fragment;
    }

    bool
    archive_based () const
    {
      switch (type ())
      {
      case repository_type::pkg: return true;
      case repository_type::git: return false;
      }

      assert (false); // Can't be here.
      return false;
    }

    bool
    version_control_based () const
    {
      return !archive_based ();
    }

    // String representation of an empty location is the empty string.
    //
    std::string
    string () const
    {
      return url_.string ();
    }

  private:
    std::string canonical_name_;
    repository_url url_;
    repository_type type_;
  };

  inline std::ostream&
  operator<< (std::ostream& os, const repository_location& l)
  {
    return os << l.string ();
  }

  // Branch and/or commit. At least one of them must be present. If both of
  // them are present then the commit is expected to belong to the branch
  // history. Note that the branch member can also denote a tag.
  //
  class LIBBPKG_EXPORT git_reference
  {
  public:
    butl::optional<std::string> branch;
    butl::optional<std::string> commit;

  public:
    // Parse the [<branch>][@<commit>] repository URL fragment representation.
    //
    explicit
    git_reference (const butl::optional<std::string>&);

    git_reference (butl::optional<std::string> b,
                   butl::optional<std::string> c)
        : branch (std::move (b)),
          commit (std::move (c)) {}
  };

  enum class repository_role
  {
    base,
    prerequisite,
    complement
  };

  class LIBBPKG_EXPORT repository_manifest
  {
  public:
    using email_type = bpkg::email;

    repository_location location;         // Non-empy for non-base roles.
    butl::optional<repository_role> role;

    // The following values may only be present for the base repository (and
    // only for certain repository types).
    //
    butl::optional<std::string> url;
    butl::optional<email_type>  email;
    butl::optional<std::string> summary;
    butl::optional<std::string> description;
    butl::optional<std::string> certificate;

    // Return the effective role of the repository. If the role is not
    // explicitly specified (see the role member above), then calculate
    // the role based on the location. Specifically, if the location is
    // empty, then the effective role is base. Otherwise -- prerequisite.
    // If the role is specified, then verify that it is consistent with
    // the location value (that is, base if the location is empty and
    // prerequisite or complement if not) and return that. Otherwise,
    // throw std::logic_error.
    //
    repository_role
    effective_role () const;

    // Return the effective web interface URL based on the specified remote
    // repository location. If url is not present, doesn't start with '.', or
    // the repository type differs from pkg, then return it unchanged.
    // Otherwise, process the relative format as described in the manifest
    // specification. Throw std::invalid_argument if the relative url format is
    // invalid or if the repository location is empty or local.
    //
    butl::optional<std::string>
    effective_url (const repository_location&) const;

  public:
    repository_manifest () = default; // VC export.

    void
    serialize (butl::manifest_serializer&) const;
  };

  // Create individual repository manifest.
  //
  LIBBPKG_EXPORT repository_manifest
  pkg_repository_manifest (butl::manifest_parser&,
                           bool ignore_unknown = false);

  LIBBPKG_EXPORT repository_manifest
  git_repository_manifest (butl::manifest_parser&,
                           bool ignore_unknown = false);

  // Create an element of the repository list manifest.
  //
  LIBBPKG_EXPORT repository_manifest
  pkg_repository_manifest (butl::manifest_parser&,
                           butl::manifest_name_value start,
                           bool ignore_unknown = false);

  LIBBPKG_EXPORT repository_manifest
  git_repository_manifest (butl::manifest_parser&,
                           butl::manifest_name_value start,
                           bool ignore_unknown = false);

  class LIBBPKG_EXPORT pkg_repository_manifests:
    public std::vector<repository_manifest>
  {
  public:
    using base_type = std::vector<repository_manifest>;

    using base_type::base_type;

    pkg_repository_manifests () = default;
    pkg_repository_manifests (butl::manifest_parser&,
                              bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  class LIBBPKG_EXPORT git_repository_manifests:
    public std::vector<repository_manifest>
  {
  public:
    using base_type = std::vector<repository_manifest>;

    using base_type::base_type;

    git_repository_manifests () = default;
    git_repository_manifests (butl::manifest_parser&,
                              bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  class LIBBPKG_EXPORT signature_manifest
  {
  public:
    // Checksum of the corresponding package_manifests.
    //
    std::string sha256sum;

    // Signature of the corresponding package_manifests. Calculated by
    // encrypting package_manifests checksum (stored in sha256sum) with the
    // repository certificate private key.
    //
    std::vector<char> signature;

  public:
    signature_manifest () = default;
    signature_manifest (butl::manifest_parser&, bool ignore_unknown = false);

    // Serialize sha256sum and base64-encoded representation of the signature.
    //
    void
    serialize (butl::manifest_serializer&) const;

  private:
    // Used for delegating in public constructor. Strictly speaking is not
    // required, as a signature_manifest currently never appears as a part of
    // a manifest list, but kept for the consistency with other manifests
    // implementations.
    //
    signature_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown);
  };
}

#endif // LIBBPKG_MANIFEST_HXX
