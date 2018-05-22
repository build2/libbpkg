// file      : libbpkg/package-name.hxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBPKG_PACKAGE_NAME_HXX
#define LIBBPKG_PACKAGE_NAME_HXX

#include <string>
#include <utility> // move()
#include <ostream>

#include <libbutl/utility.mxx> // casecmp()

#include <libbpkg/export.hxx>
#include <libbpkg/version.hxx>

namespace bpkg
{
  class LIBBPKG_EXPORT package_name
  {
  public:
    // Create package name from string verifying that it complied with the
    // specification and throwing std::invalid_argument if that's not the
    // case. Note that in this case the passed value is guaranteed to be
    // unchanged.
    //
    explicit
    package_name (const std::string& s): package_name (std::string (s)) {}

    explicit
    package_name (std::string&&);

    // Create a special empty package name.
    //
    package_name () = default;

    // Create an arbitrary string that can be used in contexts that expect
    // a package name. For example, a package name pattern for use in ODB query
    // expressions.
    //
    enum raw_string_type {raw_string};
    package_name (std::string s, raw_string_type): value_ (std::move (s)) {}

    bool
    empty () const noexcept {return value_.empty ();}

    const std::string&
    string () const& noexcept {return value_;}

    // Moves the underlying package name string out of the package name object.
    // The object becomes empty. Usage: std::move (name).string ().
    //
    std::string
    string () && {std::string r; r.swap (this->value_); return r;}

    // Compare ignoring case. Note that a string is not checked to be a valid
    // package name.
    //
    int compare (const package_name& n) const {return compare (n.value_);}
    int compare (const std::string& n) const {return compare (n.c_str ());}
    int compare (const char* n) const {return butl::casecmp (value_, n);}

  private:
    std::string value_;
  };

  inline bool
  operator< (const package_name& x, const package_name& y)
  {
    return x.compare (y) < 0;
  }

  inline bool
  operator> (const package_name& x, const package_name& y)
  {
    return x.compare (y) > 0;
  }

  inline bool
  operator== (const package_name& x, const package_name& y)
  {
    return x.compare (y) == 0;
  }

  inline bool
  operator<= (const package_name& x, const package_name& y)
  {
    return x.compare (y) <= 0;
  }

  inline bool
  operator>= (const package_name& x, const package_name& y)
  {
    return x.compare (y) >= 0;
  }

  inline bool
  operator!= (const package_name& x, const package_name& y)
  {
    return x.compare (y) != 0;
  }

  template <typename T>
  inline auto
  operator< (const package_name& x, const T& y)
  {
    return x.compare (y) < 0;
  }

  template <typename T>
  inline auto
  operator> (const package_name& x, const T& y)
  {
    return x.compare (y) > 0;
  }

  template <typename T>
  inline auto
  operator== (const package_name& x, const T& y)
  {
    return x.compare (y) == 0;
  }

  template <typename T>
  inline auto
  operator<= (const package_name& x, const T& y)
  {
    return x.compare (y) <= 0;
  }

  template <typename T>
  inline auto
  operator>= (const package_name& x, const T& y)
  {
    return x.compare (y) >= 0;
  }

  template <typename T>
  inline auto
  operator!= (const package_name& x, const T& y)
  {
    return x.compare (y) != 0;
  }

  template <typename T>
  inline auto
  operator< (const T& x, const package_name& y)
  {
    return y > x;
  }

  template <typename T>
  inline auto
  operator> (const T& x, const package_name& y)
  {
    return y < x;
  }

  template <typename T>
  inline auto
  operator== (const T& x, const package_name& y)
  {
    return y == x;
  }

  template <typename T>
  inline auto
  operator<= (const T& x, const package_name& y)
  {
    return y >= x;
  }

  template <typename T>
  inline auto
  operator>= (const T& x, const package_name& y)
  {
    return y <= x;
  }

  template <typename T>
  inline auto
  operator!= (const T& x, const package_name& y)
  {
    return y != x;
  }

  inline std::ostream&
  operator<< (std::ostream& os, const package_name& v)
  {
    return os << v.string ();
  }
}

#endif // LIBBPKG_PACKAGE_NAME_HXX
