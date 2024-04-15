// file      : libbpkg/manifest.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBPKG_MANIFEST_HXX
#define LIBBPKG_MANIFEST_HXX

#include <map>
#include <string>
#include <vector>
#include <cassert>
#include <cstdint>    // uint*_t
#include <ostream>
#include <utility>    // move(), pair
#include <functional>

#include <libbutl/url.hxx>
#include <libbutl/path.hxx>
#include <libbutl/optional.hxx>
#include <libbutl/small-vector.hxx>
#include <libbutl/standard-version.hxx>
#include <libbutl/manifest-forward.hxx>

#include <libbpkg/package-name.hxx>

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
    // representation. We also make them const to make sure things stay
    // consistent.
    //
    const std::uint16_t epoch;
    const std::string upstream;
    const butl::optional<std::string> release;

    // The absent revision semantics depends on the context the version object
    // is used in. Normally, it is equivalent to zero revision but may have a
    // special meaning, for example, denoting any package revision.
    //
    const butl::optional<std::uint16_t> revision;

    const std::uint32_t iteration;

    // Upstream part canonical representation.
    //
    const std::string canonical_upstream;

    // Release part canonical representation.
    //
    const std::string canonical_release;

    // Create a special empty version. It is less than any other valid
    // version (and is conceptually equivalent to +0-0-).
    //
    version (): epoch (0), release (""), iteration (0) {}

    // By default, treat the zero revision as no revision. Throw
    // std::invalid_argument if the passed string is not a valid version
    // representation.
    //
    enum flags
    {
      none               = 0,
      fold_zero_revision = 0x01,
      allow_iteration    = 0x02
    };

    explicit
    version (const std::string& v, flags fl = fold_zero_revision)
        : version (v.c_str (), fl) {}

    explicit
    version (const char* v, flags fl = fold_zero_revision)
        : version (data_type (v, data_type::parse::full, fl))
    {
    }

    // Create the version object from separate epoch, upstream, release,
    // revision, and iteration parts.
    //
    // Note that it is possible (and legal) to create the special empty
    // version via this interface as version(0, string(), string(), nullopt, 0).
    //
    version (std::uint16_t epoch,
             std::string upstream,
             butl::optional<std::string> release,
             butl::optional<std::uint16_t> revision,
             std::uint32_t iteration);

    std::uint16_t
    effective_revision () const noexcept {return revision ? *revision : 0;}

    version (version&&) = default;
    version (const version&) = default;
    version& operator= (version&&) noexcept;
    version& operator= (const version&);

    // If the revision is ignored, then the iteration (that semantically
    // extends the revision) is also ignored, regardless of the argument.
    //
    std::string
    string (bool ignore_revision = false, bool ignore_iteration = false) const;

    bool operator<  (const version& v) const noexcept;
    bool operator>  (const version& v) const noexcept;
    bool operator== (const version& v) const noexcept;
    bool operator<= (const version& v) const noexcept;
    bool operator>= (const version& v) const noexcept;
    bool operator!= (const version& v) const noexcept;

    // If the revision is ignored, then the iteration is also ignored,
    // regardless of the argument (see above for details).
    //
    int
    compare (const version& v,
             bool ignore_revision = false,
             bool ignore_iteration = false) const noexcept;

    bool
    empty () const noexcept
    {
      bool e (upstream.empty ());

      assert (!e ||
              (epoch == 0 &&
               release && release->empty () &&
               !revision && iteration == 0));

      return e;
    }

  private:
    struct LIBBPKG_EXPORT data_type
    {
      enum class parse {full, upstream, release};

      data_type (const char*, parse, flags);

      // Note that there is no iteration component as it can't be present in
      // the string representation passed to the ctor.
      //
      std::uint16_t epoch;
      std::string upstream;
      butl::optional<std::string> release;
      butl::optional<std::uint16_t> revision;
      std::uint32_t iteration;
      std::string canonical_upstream;
      std::string canonical_release;
    };

    explicit
    version (data_type&& d)
        : epoch (d.epoch),
          upstream (std::move (d.upstream)),
          release (std::move (d.release)),
          revision (d.revision),
          iteration (d.iteration),
          canonical_upstream (std::move (d.canonical_upstream)),
          canonical_release (std::move (d.canonical_release)) {}
  };

  inline std::ostream&
  operator<< (std::ostream& os, const version& v)
  {
    return os << (v.empty () ? "<empty-version>" : v.string ());
  }

  version::flags operator&  (version::flags,  version::flags);
  version::flags operator|  (version::flags,  version::flags);
  version::flags operator&= (version::flags&, version::flags);
  version::flags operator|= (version::flags&, version::flags);

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

  // language
  //
  struct language
  {
    std::string name;
    bool        impl; // True if implementation-only.

    language (): impl (false) {}
    language (std::string n, bool i): name (std::move (n)), impl (i) {}
  };

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

    text_file (text_file&&) noexcept;
    text_file (const text_file&);
    text_file& operator= (text_file&&) noexcept;
    text_file& operator= (const text_file&);

    ~text_file ();
  };

  enum class text_type
  {
    plain,
    common_mark,
    github_mark
  };

  LIBBPKG_EXPORT std::string
  to_string (text_type);

  // Throw std::invalid_argument if the argument is not a well-formed text
  // type. Otherwise, return nullopt for an unknown text variant.
  //
  LIBBPKG_EXPORT butl::optional<text_type>
  to_text_type (const std::string&);

  inline std::ostream&
  operator<< (std::ostream& os, text_type t)
  {
    return os << to_string (t);
  }

  // description
  // description-file
  // description-type
  // package-description
  // package-description-file
  // package-description-type
  // change
  // change-file
  // change-type
  //
  class LIBBPKG_EXPORT typed_text_file: public text_file
  {
  public:
    butl::optional<std::string> type;

    // File text constructor.
    //
    explicit
    typed_text_file (std::string s = "",
                     butl::optional<std::string> t = butl::nullopt)
        : text_file (std::move (s)), type (std::move (t)) {}

    // File reference constructor.
    //
    typed_text_file (path_type p,
                     std::string c,
                     butl::optional<std::string> t = butl::nullopt)
        : text_file (std::move (p), std::move (c)), type (std::move (t)) {}

    // Return the type value if present, text_type::github_mark if it refers
    // to a file with the .md or .markdown extension and text_type::plain if
    // it refers to a file with the .txt extension or no extension or the text
    // does not come from a file. Depending on the ignore_unknown value either
    // throw std::invalid_argument or return nullopt if the type value or the
    // file extension is unknown.
    //
    // Note: also throws std::invalid_argument if the type is not well-formed.
    // This, however, may not happen for an object created by the package
    // manifest parser since it has already verified that.
    //
    butl::optional<text_type>
    effective_type (bool ignore_unknown = false) const;
  };

  // license
  //
  class licenses: public butl::small_vector<std::string, 1>
  {
  public:
    std::string comment;

    explicit
    licenses (std::string c = ""): comment (std::move (c)) {}
  };

  // url
  // doc-url
  // src-url
  // package-url
  //
  // URL that has the following constraints:
  //
  // - is not rootless
  // - is not local (the scheme is not `file`)
  // - authority is present and is not empty
  //
  // See libbutl/url.hxx for details.
  //
  // NOTE: this class must not be DLL-exported wholesale (non-exported base).
  //
  class manifest_url: public butl::url
  {
  public:
    std::string comment;

    // Throw invalid_argument on parsing or constraints checking error.
    //
    explicit LIBBPKG_EXPORT
    manifest_url (const std::string& u, std::string c = "");

    manifest_url () = default;
  };

  // email
  // package-email
  // build-email
  // build-warning-email
  // build-error-email
  //
  class email: public std::string
  {
  public:
    std::string comment;

    explicit
    email (std::string e = "", std::string c = "")
        : std::string (std::move (e)), comment (std::move (c)) {}
  };

  // Represented as a version range. Note that the versions may refer to the
  // dependent package version and can be completed with the actual versions
  // using the effective() function. Such versions are represented as an empty
  // version object and have the dollar character string representation.
  //
  // If the version endpoints are equal and both are closed, then this is the
  // `== <version>` constraint (in particular, `== $` if empty endpoints). If
  // both endpoints are empty and one of them is open, then this is either
  // `~$` (min endpoint is open) or `^$` (max endpoint is open). Note that
  // equal endpoints can never be both open.
  //
  // An absent endpoint version revision has the 'any revision' meaning
  // (except for an earliest release for which the revision is meaningless) and
  // so translates into the effective revision differently, depending on the
  // range endpoint side and openness:
  //
  // [X Y)  ==  [X+0   Y+0)
  // (X Y]  ==  (X+max Y+max]
  //
  class LIBBPKG_EXPORT version_constraint
  {
  public:
    butl::optional<version> min_version;
    butl::optional<version> max_version;
    bool min_open;
    bool max_open;

    // Preserve the zero endpoint version revisions (see above for details).
    //
    explicit
    version_constraint (const std::string&);

    version_constraint (butl::optional<version> min_version, bool min_open,
                        butl::optional<version> max_version, bool max_open);

    explicit
    version_constraint (const version& v)
        : version_constraint (v, false, v, false) {}

    version_constraint () = default;

    bool
    empty () const noexcept {return !min_version && !max_version;}

    bool
    complete () const noexcept
    {
      return (!min_version || !min_version->empty ()) &&
             (!max_version || !max_version->empty ());
    }

    // Return the completed constraint if it refers to the dependent package
    // version and copy of itself otherwise. Throw std::invalid_argument if
    // the resulting constraint is invalid (max version is less than min
    // version in range, non-standard or latest snapshot version for a
    // shortcut operator, etc.).
    //
    version_constraint
    effective (version) const;

    std::string
    string () const;
  };

  inline std::ostream&
  operator<< (std::ostream& os, const version_constraint& vc)
  {
    return os << vc.string ();
  }

  inline bool
  operator== (const version_constraint&, const version_constraint&);

  inline bool
  operator!= (const version_constraint&, const version_constraint&);

  struct LIBBPKG_EXPORT dependency
  {
    package_name name;
    butl::optional<version_constraint> constraint;

    dependency () = default;
    dependency (package_name n, butl::optional<version_constraint> c)
        : name (std::move (n)), constraint (std::move (c)) {}

    // Parse the dependency string representation in the
    // `<name> [<version-constraint>]` form. Throw std::invalid_argument if
    // the value is invalid.
    //
    explicit
    dependency (std::string);

    std::string
    string () const;
  };

  std::ostream&
  operator<< (std::ostream&, const dependency&);

  // depends
  //
  // The dependency alternative can be represented in one of the following
  // forms.
  //
  // Single-line form:
  //
  //   <dependencies> ['?' <enable-condition>] [<reflect-config>]
  //
  //   <dependencies> = <dependency> |
  //                    ({ <dependency> [ <dependency>]* } [<version-constraint>])
  //
  //   <enable-condition> - buildfile evaluation context
  //   <reflect-config>   - dependent package configuration variable assignment
  //
  //   If the version constraint is specified after the dependency group, it
  //   only applies to dependencies without a version constraint.
  //
  // Multi-line forms:
  //
  //   <dependencies>
  //   {
  //     enable <enable-condition>
  //
  //     prefer
  //     {
  //       <prefer-config>
  //     }
  //
  //     accept <accept-condition>
  //
  //     reflect
  //     {
  //       <reflect-config>
  //     }
  //   }
  //   |
  //   <dependencies>
  //   {
  //     enable <enable-condition>
  //
  //     require
  //     {
  //       <require-config>
  //     }
  //
  //     reflect
  //     {
  //       <reflect-config>
  //     }
  //   }
  //
  //   <prefer-config>    - buildfile fragment containing dependency packages
  //                        configuration variables assignments
  //
  //   <accept-condition> - buildfile evaluation context
  //
  //   <require-config>   - buildfile fragment containing dependency packages
  //                        configuration variables assignments
  //
  //   <reflect-config>   - buildfile fragment containing dependent package
  //                        configuration variables assignments
  //
  //   In the multi-line form the block may contain comments besides the
  //   clauses. The '#' character starts a single-line comment which spans
  //   until the end of the line. Unless it is followed with '\' followed by
  //   the newline in which case this is a multi-line comment which spans
  //   until the closing '#\' is encountered.
  //
  // The dependency alternative is only considered by bpkg if the enable
  // condition evaluates to true. If the enable clause is not specified, then
  // it is always considered.
  //
  // The prefer clause specifies the preferred dependency package
  // configuration that may potentially differ from the resulting
  // configuration after the preferred/required configurations from all the
  // selected dependency alternatives of all the dependent packages are
  // "negotiated" by bpkg. The accept clause is used to verify that the
  // resulting configuration is still acceptable for the dependent
  // package. The accept clause must always be specified if the prefer clause
  // is specified.
  //
  // The require clause specifies the only acceptable dependency packages
  // configuration. It is a shortcut for specifying the prefer/accept clauses,
  // where the accept condition verifies all the variable values assigned in
  // the prefer clause. The require clause and the prefer/accept clause pair
  // are optional and are mutually exclusive.
  //
  // The reflect clause specifies the dependent package configuration that
  // should be used if the alternative is selected.
  //
  // All clauses are optional but at least one of them must be specified.
  //
  class dependency_alternative: public butl::small_vector<dependency, 1>
  {
  public:
    butl::optional<std::string> enable;
    butl::optional<std::string> reflect;
    butl::optional<std::string> prefer;
    butl::optional<std::string> accept;
    butl::optional<std::string> require;

    dependency_alternative () = default;
    dependency_alternative (butl::optional<std::string> e,
                            butl::optional<std::string> r,
                            butl::optional<std::string> p,
                            butl::optional<std::string> a,
                            butl::optional<std::string> q)
        : enable (std::move (e)),
          reflect (std::move (r)),
          prefer (std::move (p)),
          accept (std::move (a)),
          require (std::move (q)) {}

    // Can be used to copy a dependency alternative object, while omitting
    // some clauses which are no longer needed.
    //
    dependency_alternative (butl::optional<std::string> e,
                            butl::optional<std::string> r,
                            butl::optional<std::string> p,
                            butl::optional<std::string> a,
                            butl::optional<std::string> q,
                            butl::small_vector<dependency, 1> ds)
        : small_vector<dependency, 1> (move (ds)),
          enable (std::move (e)),
          reflect (std::move (r)),
          prefer (std::move (p)),
          accept (std::move (a)),
          require (std::move (q)) {}

    // Return the single-line representation if possible (the prefer and
    // require clauses are absent and the reflect clause either absent or
    // contains no newlines).
    //
    LIBBPKG_EXPORT std::string
    string () const;

    // Return true if the string() function would return the single-line
    // representation.
    //
    bool
    single_line () const
    {
      return !prefer  &&
             !require &&
             (!reflect || reflect->find ('\n') == std::string::npos);
    }
  };

  inline std::ostream&
  operator<< (std::ostream& os, const dependency_alternative& da)
  {
    return os << da.string ();
  }

  class dependency_alternatives:
    public butl::small_vector<dependency_alternative, 1>
  {
  public:
    bool buildtime;
    std::string comment;

    dependency_alternatives () = default;
    dependency_alternatives (bool b, std::string c)
        : buildtime (b), comment (std::move (c)) {}

    // Parse the dependency alternatives string representation in the form:
    //
    // [*] <alternative> [ '|' <alternative>]* [; <comment>]
    //
    // Where <alternative> can be single or multi-line (see above). Note also
    // that leading `*` and trailing comment can be on separate lines. Throw
    // manifest_parsing if the value is invalid.
    //
    // Use the dependent package name to verify that the reflect clauses in
    // the dependency alternative representations refer to the dependent
    // package configuration variable.
    //
    // Optionally, specify the stream name to use when creating the
    // manifest_parsing exception. The start line and column arguments can be
    // used to align the exception information with a containing stream. This
    // is useful when the alternatives representation is a part of some larger
    // text (manifest, etc).
    //
    // Note that semicolons inside alternatives must be escaped with the
    // backslash (not to be treated as the start of a comment). Backslashes at
    // the end of buildfile fragment lines need to also be escaped, if
    // dependency alternatives representation comes from the manifest file
    // (since trailing backslashes in manifest lines has special semantics).
    //
    explicit LIBBPKG_EXPORT
    dependency_alternatives (const std::string&,
                             const package_name& dependent,
                             const std::string& name = std::string (),
                             std::uint64_t line = 1,
                             std::uint64_t column = 1);

    LIBBPKG_EXPORT std::string
    string () const;

    // Return true if there is a conditional alternative in the list.
    //
    bool
    conditional () const;
  };

  inline std::ostream&
  operator<< (std::ostream& os, const dependency_alternatives& das)
  {
    return os << das.string ();
  }

  // requires
  //
  // The requirement alternative string representation is similar to that of
  // the dependency alternative with the following differences:
  //
  // - The requirement id (with or without version) can mean anything (but
  //   must still be a valid package name).
  //
  // - Only the enable and reflect clauses are permitted (reflect is allowed
  //   for potential future support of recognized requirement alternatives,
  //   for example, C++ standard).
  //
  // - The simplified representation syntax, where the comment carries the
  //   main information and thus is mandatory, is also supported (see
  //   requirement_alternatives for details). For example:
  //
  //   requires: ; X11 libs.
  //   requires: ? ($windows) ; Only 64-bit.
  //   requires: ? ; Only 64-bit if on Windows.
  //   requires: x86_64 ? ; Only if on Windows.
  //
  class requirement_alternative: public butl::small_vector<std::string, 1>
  {
  public:
    butl::optional<std::string> enable;
    butl::optional<std::string> reflect;

    requirement_alternative () = default;
    requirement_alternative (butl::optional<std::string> e,
                             butl::optional<std::string> r)
        : enable (std::move (e)), reflect (std::move (r)) {}

    // Return the single-line representation if possible (the reflect clause
    // either absent or contains no newlines).
    //
    LIBBPKG_EXPORT std::string
    string () const;

    // Return true if the string() function would return the single-line
    // representation.
    //
    bool
    single_line () const
    {
      return !reflect || reflect->find ('\n') == std::string::npos;
    }

    // Return true if this is a single requirement with an empty id or an
    // empty enable condition.
    //
    bool
    simple () const
    {
      return size () == 1 && (back ().empty () || (enable && enable->empty ()));
    }
  };

  class requirement_alternatives:
    public butl::small_vector<requirement_alternative, 1>
  {
  public:
    bool buildtime;
    std::string comment;

    requirement_alternatives () = default;
    requirement_alternatives (bool b, std::string c)
        : buildtime (b), comment (std::move (c)) {}

    // Parse the requirement alternatives string representation in the
    // following forms:
    //
    // [*] <alternative> [ '|' <alternative>]* [; <comment>]
    // [*] [<requirement-id>] [? [<enable-condition>]] ; <comment>
    //
    // Parsing the second form ends up with a single alternative with a single
    // potentially empty requirement id, potentially with an enable condition
    // with potentially empty value (see examples above).
    //
    // Throw manifest_parsing if the value is invalid.
    //
    // Optionally, specify the stream name to use when creating the
    // manifest_parsing exception. The start line and column arguments can be
    // used to align the exception information with a containing stream. This
    // is useful when the alternatives representation is a part of some larger
    // text (manifest, etc).
    //
    explicit LIBBPKG_EXPORT
    requirement_alternatives (const std::string&,
                              const package_name& dependent,
                              const std::string& name = std::string (),
                              std::uint64_t line = 1,
                              std::uint64_t column = 1);

    LIBBPKG_EXPORT std::string
    string () const;

    // Return true if there is a conditional alternative in the list.
    //
    bool
    conditional () const;

    // Return true if this is a single simple requirement alternative.
    //
    bool
    simple () const
    {
      return size () == 1 && back ().simple ();
    }
  };

  inline std::ostream&
  operator<< (std::ostream& os, const requirement_alternatives& ra)
  {
    return os << ra.string ();
  }

  class build_constraint
  {
  public:
    // If true, then the package should not be built for matching target
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

  // Package manifest value validation forbid/require flags.
  //
  // Some package manifest values can be forbidden or required for certain
  // repository types and in specific contexts (for example, when parsing an
  // individual manifest, a manifest list, etc).
  //
  // Also note that, naturally, the forbid_* and require_* flags are mutually
  // exclusive for the same value.
  //
  enum class package_manifest_flags: std::uint16_t
  {
    none                     = 0x000,

    forbid_file              = 0x001, // Forbid *-file manifest values.
    forbid_location          = 0x002,
    forbid_sha256sum         = 0x004,
    forbid_fragment          = 0x008,
    forbid_incomplete_values = 0x010, // depends, <distribution>-version, etc.

    require_location         = 0x020,
    require_sha256sum        = 0x040,
    require_text_type        = 0x080, // description-type, changes-type, etc.
    require_bootstrap_build  = 0x100
  };

  package_manifest_flags
  operator& (package_manifest_flags, package_manifest_flags);

  package_manifest_flags
  operator| (package_manifest_flags, package_manifest_flags);

  package_manifest_flags
  operator&= (package_manifest_flags&, package_manifest_flags);

  package_manifest_flags
  operator|= (package_manifest_flags&, package_manifest_flags);

  // Target build configuration class term.
  //
  class LIBBPKG_EXPORT build_class_term
  {
  public:
    char operation; // '+', '-' or '&'
    bool inverted;  // Operation is followed by '!'.
    bool simple;    // Name if true, expr otherwise.
    union
    {
      std::string                   name; // Class name.
      std::vector<build_class_term> expr; // Parenthesized expression.
    };

    // Create the simple term object (class name).
    //
    build_class_term (std::string n, char o, bool i)
        : operation (o), inverted (i), simple (true), name (std::move (n)) {}

    // Create the compound term object (parenthesized expression).
    //
    build_class_term (std::vector<build_class_term> e, char o, bool i)
        : operation (o), inverted (i), simple (false), expr (std::move (e)) {}

    // Required by VC for some reason.
    //
    build_class_term ()
        : operation ('\0'), inverted (false), simple (true), name () {}

    build_class_term (build_class_term&&) noexcept;
    build_class_term (const build_class_term&);
    build_class_term& operator= (build_class_term&&) noexcept;
    build_class_term& operator= (const build_class_term&);

    ~build_class_term ();

    // Check that the specified string is a valid class name, that is
    // non-empty, containing only alpha-numeric characters, '_', '+', '-', '.'
    // (except as the first character for the last three). Return true if the
    // name is reserved (starts with '_'). Throw std::invalid_argument if
    // invalid.
    //
    static bool
    validate_name (const std::string&);
  };

  // Map of derived build classes to their bases.
  //
  using build_class_inheritance_map = std::map<std::string, std::string>;

  // Target build configuration class expression. Includes comment and
  // optional underlying set.
  //
  class LIBBPKG_EXPORT build_class_expr
  {
  public:
    std::string comment;
    strings underlying_classes;
    std::vector<build_class_term> expr;

  public:
    build_class_expr () = default;

    // Parse the string representation of a space-separated build class
    // expression, potentially prepended with a space-separated underlying
    // build class set, in which case the expression can be empty. If both,
    // underlying class set and expression are present, then they should be
    // separated with the semicolon. Throw std::invalid_argument if the
    // representation is invalid. Some expression examples:
    //
    // +gcc
    // -msvc -clang
    // default leagacy
    // default leagacy :
    // default leagacy : -msvc
    // default leagacy : &gcc
    //
    build_class_expr (const std::string&, std::string comment);

    // Create the expression object from a class list (c1, c2, ...) using the
    // specified operation (+/-/&) according to the following rules:
    //
    // +  ->  +c1 +c2 ...
    // -  ->  -c1 -c2 ...
    // &  ->  &( +c1 +c2 ... )
    //
    // An empty class list results in an empty expression.
    //
    // Note: it is assumed that the class names are valid.
    //
    build_class_expr (const strings& classes,
                      char operation,
                      std::string comment);

    // Return the string representation of the build class expression,
    // potentially prepended with the underlying class set.
    //
    std::string
    string () const;

    // Match a target build configuration that belongs to the specified list
    // of classes (and recursively to their bases) against the expression.
    // Either return or update the result (the latter allows to sequentially
    // matching against a list of expressions).
    //
    // Notes:
    //
    // - The derived-to-base map is not verified (that there are no
    //   inheritance cycles, etc.).
    //
    // - The underlying class set doesn't affect the match in any way (it
    //   should have been used to pre-filter the set of target build
    //   configurations).
    //
    void
    match (const strings&,
           const build_class_inheritance_map&,
           bool& result) const;

    bool
    match (const strings&, const build_class_inheritance_map&) const;
  };

  inline std::ostream&
  operator<< (std::ostream& os, const build_class_expr& bce)
  {
    return os << bce.string ();
  }

  // Build auxiliary configuration name-matching wildcard. Includes optional
  // environment name (specified as a suffix in the [*-]build-auxiliary[-*]
  // value name) and comment.
  //
  class LIBBPKG_EXPORT build_auxiliary
  {
  public:
    std::string environment_name;

    // Filesystem wildcard pattern for the build auxiliary configuration name.
    //
    std::string config;

    std::string comment;

    build_auxiliary () = default;
    build_auxiliary (std::string en,
                     std::string cf,
                     std::string cm)
        : environment_name (std::move (en)),
          config (std::move (cf)),
          comment (std::move (cm)) {}

    // Parse a package manifest value name in the [*-]build-auxiliary[-*] form
    // into the pair of the build package configuration name (first) and the
    // build auxiliary environment name (second), with an unspecified name
    // represented as an empty string. Return nullopt if the value name
    // doesn't match this form.
    //
    static butl::optional<std::pair<std::string, std::string>>
    parse_value_name (const std::string&);
  };

  // Package build configuration. Includes comment and optional overrides for
  // target build configuration class expressions/constraints, auxiliaries,
  // custom bot public keys, and notification emails.
  //
  // Note that in the package manifest the build bot keys list contains the
  // public keys data (std::string type). However, for other use cases it may
  // be convenient to store some other key representations (public key object
  // pointers represented as key fingerprints, etc; see brep for such a use
  // case).
  //
  template <typename K>
  class build_package_config_template
  {
  public:
    using email_type = bpkg::email;
    using key_type = K;

    std::string name;

    // Whitespace separated list of potentially double/single-quoted package
    // configuration arguments for bpkg-pkg-build command executed by
    // automated build bots.
    //
    std::string arguments;

    std::string comment;

    butl::small_vector<build_class_expr, 1> builds;
    std::vector<build_constraint> constraints;

    // Note that all entries in this list must have distinct environment names
    // (with empty name being one of the possibilities).
    //
    std::vector<build_auxiliary> auxiliaries;

    std::vector<key_type> bot_keys;

    butl::optional<email_type> email;
    butl::optional<email_type> warning_email;
    butl::optional<email_type> error_email;

    build_package_config_template () = default;

    build_package_config_template (std::string n,
                                   std::string a,
                                   std::string c,
                                   butl::small_vector<build_class_expr, 1> bs,
                                   std::vector<build_constraint> cs,
                                   std::vector<build_auxiliary> as,
                                   std::vector<key_type> bks,
                                   butl::optional<email_type> e,
                                   butl::optional<email_type> we,
                                   butl::optional<email_type> ee)
        : name (move (n)),
          arguments (move (a)),
          comment (move (c)),
          builds (move (bs)),
          constraints (move (cs)),
          auxiliaries (move (as)),
          bot_keys (move (bks)),
          email (move (e)),
          warning_email (move (we)),
          error_email (move (ee)) {}

    // Built incrementally.
    //
    explicit
    build_package_config_template (std::string n): name (move (n)) {}

    // Return the configuration's build class expressions/constraints if they
    // override the specified common expressions/constraints and return the
    // latter otherwise (see package_manifest::override() for the override
    // semantics details).
    //
    const butl::small_vector<build_class_expr, 1>&
    effective_builds (const butl::small_vector<build_class_expr, 1>& common)
      const noexcept
    {
      return !builds.empty () ? builds : common;
    }

    const std::vector<build_constraint>&
    effective_constraints (const std::vector<build_constraint>& common) const
      noexcept
    {
      return !builds.empty () || !constraints.empty () ? constraints : common;
    }

    // Return the configuration's auxiliaries, if specified, and the common
    // ones otherwise.
    //
    const std::vector<build_auxiliary>&
    effective_auxiliaries (const std::vector<build_auxiliary>& common) const
      noexcept
    {
      return !auxiliaries.empty () ? auxiliaries : common;
    }

    // Return the configuration's custom bot public keys, if specified, and
    // the common ones otherwise.
    //
    const std::vector<key_type>&
    effective_bot_keys (const std::vector<key_type>& common) const noexcept
    {
      return !bot_keys.empty () ? bot_keys : common;
    }

    // Return the configuration's build notification emails if they override
    // the specified common build notification emails and return the latter
    // otherwise (see package_manifest::override() for the override semantics
    // details).
    //
    const butl::optional<email_type>&
    effective_email (const butl::optional<email_type>& common) const noexcept
    {
      return email || warning_email || error_email ? email : common;
    }

    const butl::optional<email_type>&
    effective_warning_email (const butl::optional<email_type>& common) const
      noexcept
    {
      return email || warning_email || error_email ? warning_email : common;
    }

    const butl::optional<email_type>&
    effective_error_email (const butl::optional<email_type>& common) const
      noexcept
    {
      return email || warning_email || error_email ? error_email : common;
    }
  };

  using build_package_config = build_package_config_template<std::string>;

  enum class test_dependency_type
  {
    tests,
    examples,
    benchmarks
  };

  LIBBPKG_EXPORT std::string
  to_string (test_dependency_type);

  // May throw std::invalid_argument.
  //
  LIBBPKG_EXPORT test_dependency_type
  to_test_dependency_type (const std::string&);

  inline std::ostream&
  operator<< (std::ostream& os, test_dependency_type t)
  {
    return os << to_string (t);
  }

  struct LIBBPKG_EXPORT test_dependency: dependency
  {
    test_dependency_type type;
    bool buildtime;
    butl::optional<std::string> enable;
    butl::optional<std::string> reflect;

    test_dependency () = default;
    test_dependency (package_name n,
                     test_dependency_type t,
                     bool b,
                     butl::optional<version_constraint> c,
                     butl::optional<std::string> e,
                     butl::optional<std::string> r)
        : dependency {std::move (n), std::move (c)},
          type (t),
          buildtime (b),
          enable (std::move (e)),
          reflect (std::move (r)) {}

    // Parse the test dependency string representation in the
    // `[*] <name> [<version-constraint>] ['?' <enable-condition>] [<reflect-config>]`
    // form. Throw std::invalid_argument if the value is invalid.
    //
    // Verify that the reflect clause, if present, refers to the test
    // dependency package configuration variable. Note that such variable
    // value normally signals the dependent package being tested.
    //
    test_dependency (std::string, test_dependency_type);

    std::string
    string () const;
  };

  // Package's buildfile path and content.
  //
  struct buildfile
  {
    // The path is relative to the package's build/ subdirectory with the
    // extension stripped.
    //
    // For example, for the build/config/common.build file the path will be
    // config/common.
    //
    // Note that the actual file path depends on the project's buildfile
    // naming scheme and for the config/common example above the actual path
    // can also be build2/config/common.build2.
    //
    butl::path path;
    std::string content;

    buildfile () = default;
    buildfile (butl::path p, std::string c)
        : path (std::move (p)),
          content (std::move (c)) {}
  };

  // Binary distribution package information.
  //
  // The name is prefixed with the <distribution> id, typically name/version
  // pair in the <name>[_<version>] form. For example:
  //
  // debian-name
  // debian_10-name
  // ubuntu_20.04-name
  //
  // Currently recognized names:
  //
  // <distribution>-name
  // <distribution>-version
  // <distribution>-to-downstream-version
  //
  // Note that the value format/semantics can be distribution-specific.
  //
  struct distribution_name_value
  {
    std::string name;
    std::string value;

    distribution_name_value () = default;
    distribution_name_value (std::string n, std::string v)
        : name (std::move (n)),
          value (std::move (v)) {}

    // Return the name's <distribution> component if the name has the
    // specified suffix, which is assumed to be valid (-name, etc).
    //
    butl::optional<std::string>
    distribution (const std::string& suffix) const;
  };

  class LIBBPKG_EXPORT package_manifest
  {
  public:
    using version_type = bpkg::version;
    using priority_type = bpkg::priority;
    using email_type = bpkg::email;

    package_name name;
    version_type version;
    butl::optional<std::string> upstream_version;
    butl::optional<std::string> type;             // <name>[, ...]
    butl::small_vector<language, 1> languages;    // <name>[=impl][, ...]
    butl::optional<package_name> project;
    butl::optional<priority_type> priority;
    std::string summary;
    butl::small_vector<licenses, 1> license_alternatives;

    butl::small_vector<std::string, 5> topics;
    butl::small_vector<std::string, 5> keywords;
    butl::optional<typed_text_file> description;
    butl::optional<typed_text_file> package_description;
    butl::small_vector<typed_text_file, 1> changes;
    butl::optional<manifest_url> url;
    butl::optional<manifest_url> doc_url;
    butl::optional<manifest_url> src_url;
    butl::optional<manifest_url> package_url;
    butl::optional<email_type> email;
    butl::optional<email_type> package_email;
    butl::optional<email_type> build_email;
    butl::optional<email_type> build_warning_email;
    butl::optional<email_type> build_error_email;
    std::vector<dependency_alternatives> dependencies;
    std::vector<requirement_alternatives> requirements;
    butl::small_vector<test_dependency, 1> tests;

    // Common build classes, constraints, auxiliaries, and custom bot public
    // keys that apply to all configurations unless overridden.
    //
    // Note that all entries in build_auxiliaries must have distinct
    // environment names (with empty name being one of the possibilities).
    //
    butl::small_vector<build_class_expr, 1> builds;
    std::vector<build_constraint> build_constraints;
    std::vector<build_auxiliary> build_auxiliaries;
    strings build_bot_keys;

    // Note that the parsing constructor adds the implied (empty) default
    // configuration at the beginning of the list. Also note that serialize()
    // writes no values for such a configuration.
    //
    butl::small_vector<build_package_config, 1> build_configs; // 1 for default.

    // If true, then this package use the alternative buildfile naming scheme
    // (build2/, .build2). In the manifest serialization this is encoded as
    // either *-build or *-build2 value names.
    //
    butl::optional<bool> alt_naming;

    butl::optional<std::string> bootstrap_build;
    butl::optional<std::string> root_build;

    // Additional buildfiles which are potentially included by root.build.
    //
    std::vector<buildfile>  buildfiles;      // Buildfiles content.
    std::vector<butl::path> buildfile_paths;

    // The binary distributions package information.
    //
    std::vector<distribution_name_value> distribution_values;

    // The following values are only valid in the manifest list (and only for
    // certain repository types).
    //
    butl::optional<butl::path> location;
    butl::optional<std::string> sha256sum;
    butl::optional<std::string> fragment;

    // Extract the name from optional type, returning either `exe`, `lib`, or
    // `other`.
    //
    // Specifically, if type is present but the name is not recognized, then
    // return `other`. If type is absent and the package name starts with the
    // `lib` prefix, then return `lib`. Otherwise, return `exe`.
    //
    std::string
    effective_type () const;

    static std::string
    effective_type (const butl::optional<std::string>&, const package_name&);

    // Extract sub-options from optional type.
    //
    strings
    effective_type_sub_options () const;

    static strings
    effective_type_sub_options (const butl::optional<std::string>&);

    // Translate the potentially empty list of languages to a non-empty one.
    //
    // Specifically, if the list of languages is not empty, then return it as
    // is. Otherwise, if the package name has an extension (as in, say,
    // libbutl.bash), then return it as the language. Otherwise, return `cc`
    // (unspecified c-common language).
    //
    butl::small_vector<language, 1>
    effective_languages () const;

    static butl::small_vector<language, 1>
    effective_languages (const butl::small_vector<language, 1>&,
                         const package_name&);

    // Return effective project name.
    //
    const package_name&
    effective_project () const noexcept {return project ? *project : name;}

  public:
    package_manifest () = default;

    // Create individual manifest.
    //
    // The default package_manifest_flags value corresponds to a valid
    // individual package manifest.
    //
    package_manifest (butl::manifest_parser&,
                      bool ignore_unknown = false,
                      bool complete_values = true,
                      package_manifest_flags =
                        package_manifest_flags::forbid_location  |
                        package_manifest_flags::forbid_sha256sum |
                        package_manifest_flags::forbid_fragment);

    // As above but also call the translate function for the version value
    // passing through any exception it may throw. Throw std::invalid_argument
    // if the resulting version isn't a valid package version (empty, earliest
    // release, etc).
    //
    // In particular, the translation function may "patch" the version with
    // the snapshot information (see <libbutl/standard-version.hxx> for
    // details). This translation is normally required for manifests of
    // packages that are accessed as directories (as opposed to package
    // archives that should have their version already patched).
    //
    using translate_function = void (version_type&);

    package_manifest (butl::manifest_parser&,
                      const std::function<translate_function>&,
                      bool ignore_unknown = false,
                      bool complete_values = true,
                      package_manifest_flags =
                        package_manifest_flags::forbid_location  |
                        package_manifest_flags::forbid_sha256sum |
                        package_manifest_flags::forbid_fragment);

    // As above but construct the package manifest from the pre-parsed
    // manifest values list.
    //
    // Note that the list is expected not to contain the format version nor
    // the end-of-manifest/stream pairs.
    //
    package_manifest (const std::string& name,
                      std::vector<butl::manifest_name_value>&&,
                      bool ignore_unknown = false,
                      bool complete_values = true,
                      package_manifest_flags =
                        package_manifest_flags::forbid_location  |
                        package_manifest_flags::forbid_sha256sum |
                        package_manifest_flags::forbid_fragment);

    package_manifest (const std::string& name,
                      std::vector<butl::manifest_name_value>&&,
                      const std::function<translate_function>&,
                      bool ignore_unknown = false,
                      bool complete_values = true,
                      package_manifest_flags =
                        package_manifest_flags::forbid_location  |
                        package_manifest_flags::forbid_sha256sum |
                        package_manifest_flags::forbid_fragment);

    // Create an element of the list manifest.
    //
    package_manifest (butl::manifest_parser&,
                      butl::manifest_name_value start,
                      bool ignore_unknown,
                      bool complete_values,
                      package_manifest_flags);

    // Override manifest values with the specified. Throw manifest_parsing if
    // any value is invalid, cannot be overridden, or its name is not
    // recognized.
    //
    // The specified values other than [*-]build-auxiliary[-*] override the
    // whole groups they belong to, resetting all the group values prior to
    // being applied. The [*-]build-auxiliary[-*] values only override the
    // matching values, which are expected to already be present in the
    // manifest. Currently, only the following value groups/values can be
    // overridden:
    //
    //   {build-*email}
    //   {builds, build-{include,exclude}}
    //   {build-bot}
    //   {*-builds, *-build-{include,exclude}}
    //   {*-build-bot}
    //   {*-build-config}
    //   {*-build-*email}
    //
    //   [*-]build-auxiliary[-*]
    //
    // Throw manifest_parsing if the configuration specified by the build
    // package configuration-specific build constraint, email, auxiliary, or
    // custom bot public key value override doesn't exists. In contrast, for
    // the build config override add a new configuration if it doesn't exist
    // and update the arguments of the existing configuration otherwise. In
    // the former case, all the potential build constraint, email, auxiliary,
    // and bot key overrides for such a newly added configuration must follow
    // the respective *-build-config override.
    //
    // Note that the build constraints group values (both common and build
    // config-specific) are overridden hierarchically so that the
    // [*-]build-{include,exclude} overrides don't affect the respective
    // [*-]builds values.
    //
    // Also note that the common and build config-specific build constraints
    // group value overrides are mutually exclusive. If the common build
    // constraints are overridden, then all the build config-specific
    // constraints are removed. Otherwise, if some build config-specific
    // constraints are overridden, then for the remaining configs the build
    // constraints are reset to `builds: none`.
    //
    // Similar to the build constraints groups, the common and build
    // config-specific custom bot key value overrides are mutually
    // exclusive. If the common custom bot keys are overridden, then all the
    // build config-specific custom bot keys are removed. Otherwise, if some
    // build config-specific custom bot keys are overridden, then for the
    // remaining configs the custom bot keys are left unchanged.
    //
    // Similar to the above, the common and build config-specific build emails
    // group value overrides are mutually exclusive. If the common build
    // emails are overridden, then all the build config-specific emails are
    // reset to nullopt. Otherwise, if some build config-specific emails are
    // overridden, then for the remaining configs the email is reset to the
    // empty value and the warning and error emails are reset to nullopt
    // (which effectively disables email notifications for such
    // configurations).
    //
    // If a non-empty source name is specified, then the specified values are
    // assumed to also include the line/column information and the possibly
    // thrown manifest_parsing exception will contain the invalid value's
    // location information. Otherwise, the exception description will refer
    // to the invalid value instead.
    //
    void
    override (const std::vector<butl::manifest_name_value>&,
              const std::string& source_name);

    // Validate the overrides without applying them to any manifest.
    //
    // Specifically, validate that the override values can be parsed according
    // to their name semantics and that the value sequence makes sense (no
    // mutually exclusive values, etc). Note, however, that the subsequent
    // applying of the successfully validated overrides to a specific package
    // manifest may still fail (no build config exists for specified *-builds,
    // etc).
    //
    static void
    validate_overrides (const std::vector<butl::manifest_name_value>&,
                        const std::string& source_name);

    // If the minimum libbpkg version is specified, then also apply the
    // required backward compatibility workarounds to the serialized manifest
    // so that clients of all libbpkg versions greater or equal to the
    // specified version can parse it, ignoring unknown values.
    //
    // Note that clients of the latest major libbpkg version can fully
    // recognize the produced manifest and thus can parse it without ignoring
    // unknown values.
    //
    void
    serialize (
      butl::manifest_serializer&,
      const butl::optional<butl::standard_version>& = butl::nullopt) const;

    // Serialize only package manifest header values.
    //
    void
    serialize_header (butl::manifest_serializer&) const;

    // Load the *-file manifest values using the specified load function that
    // returns the file contents passing through any exception it may throw.
    // If nullopt is returned, then the respective *-file value is left
    // unexpanded. Set the potentially absent project description, package
    // description, and changes type values to their effective types. If an
    // effective type is nullopt then assign a synthetic unknown type if the
    // ignore_unknown argument is true and throw manifest_parsing otherwise.
    //
    // Note that if the returned file contents is empty, load_files() makes
    // sure that this is allowed by the value's semantics throwing
    // manifest_parsing otherwise. However, the load function may want to
    // recognize such cases itself in order to issue more precise diagnostics.
    //
    using load_function =
      butl::optional<std::string> (const std::string& name,
                                   const butl::path& value);

    void
    load_files (const std::function<load_function>&,
                bool ignore_unknown = false);
  };

  // Create individual package manifest.
  //
  package_manifest
  pkg_package_manifest (butl::manifest_parser& p,
                        bool ignore_unknown = false,
                        bool complete_values = true);

  LIBBPKG_EXPORT package_manifest
  dir_package_manifest (butl::manifest_parser&, bool ignore_unknown = false);

  LIBBPKG_EXPORT package_manifest
  git_package_manifest (butl::manifest_parser&, bool ignore_unknown = false);

  // Create an element of the package list manifest.
  //
  LIBBPKG_EXPORT package_manifest
  pkg_package_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown = false);

  LIBBPKG_EXPORT package_manifest
  dir_package_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown = false);

  LIBBPKG_EXPORT package_manifest
  git_package_manifest (butl::manifest_parser&,
                        butl::manifest_name_value start,
                        bool ignore_unknown = false);

  // Serialize.
  //
  inline void
  pkg_package_manifest (
    butl::manifest_serializer& s,
    const package_manifest& m,
    const butl::optional<butl::standard_version>& min_ver = butl::nullopt)
  {
    m.serialize (s, min_ver);
  }

  // Normally there is no need to serialize dir and git package manifests,
  // unless for testing.
  //
  LIBBPKG_EXPORT void
  dir_package_manifest (butl::manifest_serializer&, const package_manifest&);

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

    // If the minimum libbpkg version is specified, then also apply the
    // required backward compatibility workarounds to the serialized package
    // manifests list (see package_manifest::serialize() for details).
    //
    void
    serialize (
      butl::manifest_serializer&,
      const butl::optional<butl::standard_version>& = butl::nullopt) const;
  };

  class LIBBPKG_EXPORT dir_package_manifests:
    public std::vector<package_manifest>
  {
  public:
    using base_type = std::vector<package_manifest>;

    using base_type::base_type;

  public:
    dir_package_manifests () = default;
    dir_package_manifests (butl::manifest_parser&,
                           bool ignore_unknown = false);

    // Normally there is no need to serialize dir package manifests, unless for
    // testing.
    //
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
  enum class repository_protocol {file, http, https, git, ssh};

  struct LIBBPKG_EXPORT repository_url_traits
  {
    using string_type = std::string;
    using path_type   = butl::path;

    using scheme_type    = repository_protocol;
    using authority_type = butl::basic_url_authority<string_type>;

    static butl::optional<scheme_type>
    translate_scheme (const string_type&,
                      string_type&&,
                      butl::optional<authority_type>&,
                      butl::optional<path_type>&,
                      butl::optional<string_type>&,
                      butl::optional<string_type>&,
                      bool&);

    static string_type
    translate_scheme (string_type&,
                      const scheme_type&,
                      const butl::optional<authority_type>&,
                      const butl::optional<path_type>&,
                      const butl::optional<string_type>&,
                      const butl::optional<string_type>&,
                      bool);

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
  // - For the remote URL object the host component is normalized (see
  //   butl::basic_url_host for details) and the path is relative.
  //
  // - For the local URL object the path can be relative or absolute. Query
  //   can not be present. Represent the object using the file:// notation if
  //   it is absolute and the authority or fragment is present. Otherwise
  //   represent it as a local path, appending the fragment if present.
  //
  // - repository_url(string) ctor throws std::invalid_argument exception for
  //   an empty string.
  //
  using repository_url = butl::basic_url<repository_protocol,
                                         repository_url_traits>;

  // Repository type.
  //
  enum class repository_type {pkg, dir, git};

  LIBBPKG_EXPORT std::string
  to_string (repository_type);

  LIBBPKG_EXPORT repository_type
  to_repository_type (const std::string&); // May throw std::invalid_argument.

  inline std::ostream&
  operator<< (std::ostream& os, repository_type t)
  {
    return os << to_string (t);
  }

  // Repository basis.
  //
  enum class repository_basis
  {
    archive,
    directory,
    version_control
  };

  // Guess the repository type from the URL:
  //
  // 1. If scheme is git then git.
  //
  // 2. If path has the .git extension then git.
  //
  // 3. If scheme is http(s) or ssh then pkg.
  //
  // 4. If local, check if directory contains the .git/ subdirectory then
  //    git, otherwise pkg.
  //
  // Can throw system_error in the later case.
  //
  LIBBPKG_EXPORT repository_type
  guess_type (const repository_url&, bool local);

  // Repository URL that may have a repository type specified as part of its
  // scheme in the [<type>'+']<protocol> form. For example:
  //
  // git+http://example.com/repo  (repository type + protocol)
  // git://example.com/repo       (protocol only)
  //
  // If the substring preceding the '+' character is not a valid repository
  // type or the part that follows doesn't conform to the repository URL
  // notation, then the whole string is considered to be a repository URL.
  // For example, for all of the following strings the repository URL is
  // untyped (local) and relative:
  //
  // foo+http://example.com/repo  (invalid repository type)
  // git+ftp://example.com/repo   (invalid repository protocol)
  // git+file://example.com/repo  (invalid authority)
  // git+c:/repo                  (not a URL notation)
  //
  // Note also that in quite a few manifests where we specify the location we
  // also allow specifying the type as a separate value. While this may seem
  // redundant (and it now is in a few cases, at least for the time being),
  // keep in mind that for the local relative path the type cannot be
  // specified as part of the URL (since its representation is a non-URL).
  //
  struct LIBBPKG_EXPORT typed_repository_url
  {
    repository_url url;
    butl::optional<repository_type> type;

    explicit
    typed_repository_url (const std::string&);
  };

  class LIBBPKG_EXPORT repository_location
  {
  public:
    // Create a special empty repository_location.
    //
    repository_location () = default;

    // Create a remote or absolute repository location from a potentially
    // typed repository URL (see above).
    //
    // If the type is not specified in the URL scheme then use the one passed
    // as an argument or, if not present, guess it according to the specified
    // local flag (see above). Throw std::invalid_argument if the argument
    // doesn't represent a valid remote or absolute repository location or
    // mismatching types are specified in the URL scheme and in the argument.
    // Underlying OS errors (which may happen when guessing the type when the
    // local flag is set) are reported by throwing std::system_error.
    //
    explicit
    repository_location (
      const std::string&,
      const butl::optional<repository_type>& = butl::nullopt,
      bool local = false);

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
    repository_location (repository_url, repository_type);

    // Create a potentially relative repository location. If base is not
    // empty, use it to complete the relative location to the remote/absolute
    // one. Throw std::invalid_argument if base is not empty but the location
    // is empty, base itself is relative, or the resulting completed location
    // is invalid.
    //
    repository_location (repository_url,
                         repository_type,
                         const repository_location& base);

    repository_location (const repository_location&,
                         const repository_location& base);

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
    local () const;

    bool
    remote () const;

    bool
    absolute () const;

    bool
    relative () const;

    repository_type
    type () const;

    repository_basis
    basis () const;

    // Note that the URL of an empty location is empty.
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
    path () const;

    const std::string&
    host () const;

    // Value 0 indicated that no port was specified explicitly.
    //
    std::uint16_t
    port () const;

    repository_protocol
    proto () const;

    const butl::optional<std::string>&
    fragment () const;

    bool
    archive_based () const;

    bool
    directory_based () const;

    bool
    version_control_based () const;

    // Return an untyped URL if the correct type can be guessed just from
    // the URL. Otherwise, return the typed URL.
    //
    // String representation is empty for an empty location and is always
    // untyped for the relative location (which is a non-URL).
    //
    std::string
    string () const;

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

  // Git refname/pattern and/or commit. If none of them is present then the
  // default reference set is assumed. If both are present then the commit is
  // expected to belong to the history of the specified refs (e.g., tag or
  // branch). Note that the name member can also be an abbreviated commit id
  // (full, 40-character commit ids should always be stored in the commit
  // member since they may refer to an unadvertised commit).
  //
  class LIBBPKG_EXPORT git_ref_filter
  {
  public:
    butl::optional<std::string> name;
    butl::optional<std::string> commit;
    bool exclusion = false;

  public:
    git_ref_filter () = default; // Default reference set.

    // Parse the [+|-][<name>][@<commit>] reference filter representation.
    // Throw std::invalid_argument if the string is empty or the filter
    // representation format is invalid.
    //
    explicit
    git_ref_filter (const std::string&);

    git_ref_filter (butl::optional<std::string> n,
                    butl::optional<std::string> c,
                    bool e)
        : name (std::move (n)),
          commit (std::move (c)),
          exclusion (e) {}

    bool
    default_refs () const {return !name && !commit;}
  };

  using git_ref_filters = butl::small_vector<git_ref_filter, 2>;

  // Parse a comma-separated list of git reference filters. If the argument
  // starts with the '#' character then prepend the resulting list with the
  // default reference set filter (see above). If the argument is absent then
  // return the list containing a single default reference set filter. Throw
  // std::invalid_argument if the filter list format is invalid.
  //
  LIBBPKG_EXPORT git_ref_filters
  parse_git_ref_filters (const butl::optional<std::string>&);

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

    // The repository fingerprint to trust. May only be present for the
    // prerequisite or complement repository and only for repository types
    // that support authentication (currently only pkg).
    //
    butl::optional<std::string> trust;

    // The repository fragment id this repository belongs to (may only be
    // present for multi-fragment repositories).
    //
    butl::optional<std::string> fragment;

    // Return the effective role of the repository. If the role is not
    // explicitly specified, then the base role is assumed.
    //
    repository_role
    effective_role () const noexcept
    {
      return role ? *role : repository_role::base;
    }

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
  dir_repository_manifest (butl::manifest_parser&,
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
  dir_repository_manifest (butl::manifest_parser&,
                           butl::manifest_name_value start,
                           bool ignore_unknown = false);

  LIBBPKG_EXPORT repository_manifest
  git_repository_manifest (butl::manifest_parser&,
                           butl::manifest_name_value start,
                           bool ignore_unknown = false);

  struct repositories_manifest_header
  {
  public:
    butl::optional<butl::standard_version> min_bpkg_version;
    butl::optional<std::string>            compression;
  };

  class LIBBPKG_EXPORT pkg_repository_manifests:
    public std::vector<repository_manifest>
  {
  public:
    using base_type = std::vector<repository_manifest>;

    using base_type::base_type;

    butl::optional<repositories_manifest_header> header;

  public:
    pkg_repository_manifests () = default;
    pkg_repository_manifests (butl::manifest_parser&,
                              bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  class LIBBPKG_EXPORT dir_repository_manifests:
    public std::vector<repository_manifest>
  {
  public:
    using base_type = std::vector<repository_manifest>;

    using base_type::base_type;

    butl::optional<repositories_manifest_header> header;

  public:
    dir_repository_manifests () = default;
    dir_repository_manifests (butl::manifest_parser&,
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

    butl::optional<repositories_manifest_header> header;

  public:
    git_repository_manifests () = default;
    git_repository_manifests (butl::manifest_parser&,
                              bool ignore_unknown = false);

    void
    serialize (butl::manifest_serializer&) const;
  };

  // Search a repository manifest list for the base repository and return its
  // reference, if found. Otherwise, return a reference to an empty manifest
  // instance (which is the representation of the default base).
  //
  LIBBPKG_EXPORT const repository_manifest&
  find_base_repository (const std::vector<repository_manifest>&) noexcept;

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

  // Extract the package name component from <name>[/<version>] or
  // <name><version-constraint>. Throw invalid_argument on parsing error.
  //
  // Note: the version and version constraint are not verified.
  //
  LIBBPKG_EXPORT package_name
  extract_package_name (const char*, bool allow_version = true);

  inline package_name
  extract_package_name (const std::string& s, bool allow_version = true)
  {
    return extract_package_name (s.c_str (), allow_version);
  }

  // Extract the package version component from <name>[/<version>]. Return
  // empty version if none is specified. Throw invalid_argument on parsing
  // error and for the earliest and stub versions.
  //
  // Note: the package name is not verified.
  //
  LIBBPKG_EXPORT version
  extract_package_version (const char*,
                           version::flags fl = version::fold_zero_revision);

  inline version
  extract_package_version (const std::string& s,
                           version::flags fl = version::fold_zero_revision)
  {
    return extract_package_version (s.c_str (), fl);
  }
}

#include <libbpkg/manifest.ixx>

#endif // LIBBPKG_MANIFEST_HXX
