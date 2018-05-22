// file      : libbpkg/package-name.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2018 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <libbpkg/package-name.hxx>

#include <string>
#include <vector>
#include <utility>   // move()
#include <algorithm> // find()
#include <stdexcept> // invalid_argument

#include <libbutl/utility.mxx> // alpha(), alnum()

using namespace std;
using namespace butl;

namespace bpkg
{
  // package_name
  //
  static const vector<string> illegal_pkg_names ({
      "build",
      "con", "prn", "aux", "nul",
      "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
      "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"});

  static const string legal_pkg_chars ("_+-.");

  package_name::
  package_name (std::string&& nm)
  {
    if (nm.size () < 2)
      throw invalid_argument ("length is less than two characters");

    if (find (illegal_pkg_names.begin (), illegal_pkg_names.end (), nm) !=
        illegal_pkg_names.end ())
      throw invalid_argument ("illegal name");

    if (!alpha (nm.front ()))
      throw invalid_argument ("illegal first character (must be alphabetic)");

    // Here we rely on the fact that the name length >= 2.
    //
    for (auto i (nm.cbegin () + 1), e (nm.cend () - 1); i != e; ++i)
    {
      char c (*i);

      if (!(alnum(c) || legal_pkg_chars.find (c) != string::npos))
        throw invalid_argument ("illegal character");
    }

    if (!alnum (nm.back ()))
      throw invalid_argument (
        "illegal last character (must be alphabetic or digit)");

    value_ = move (nm);
  }
}
