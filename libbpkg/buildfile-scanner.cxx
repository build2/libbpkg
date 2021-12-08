// file      : libbpkg/buildfile-scanner.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbpkg/buildfile-scanner.hxx>

#include <string>

using namespace std;

namespace bpkg
{
  // buildfile_scanning
  //
  static inline string
  format (const string& n, uint64_t l, uint64_t c, const string& d)
  {
    string r;
    if (!n.empty ())
    {
      r += n;
      r += ':';
    }

    r += to_string (l);
    r += ':';
    r += to_string (c);
    r += ": error: ";
    r += d;
    return r;
  }

  buildfile_scanning::
  buildfile_scanning (const string& n, uint64_t l, uint64_t c, const string& d)
      : runtime_error (format (n, l, c, d)),
        name (n), line (l), column (c), description (d)
  {
  }
}
