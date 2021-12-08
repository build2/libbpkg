// file      : tests/buildfile-scanner/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>      // ios_base::failbit, ios_base::badbit
#include <string>
#include <iostream>

#include <libbutl/utf8.hxx>
#include <libbutl/utility.hxx>      // operator<<(ostream,exception)
#include <libbutl/char-scanner.hxx>

#include <libbpkg/buildfile-scanner.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;
using namespace bpkg;

// Usages:
//
// argv[0] (-e|-l [<char>]|-b)
//
// Read and scan the buildfile from stdin and print the scan result to stdout.
//
// -e          scan evaluation context
// -l [<char>] scan single line, optionally terminated with the stop character
// -b          scan buildfile block
//
int
main (int argc, char* argv[])
{
  assert (argc >= 2);

  string mode (argv[1]);

  cin.exceptions  (ios_base::failbit | ios_base::badbit);
  cout.exceptions (ios_base::failbit | ios_base::badbit);

  using scanner = char_scanner<utf8_validator>;

  scanner s (cin);

  string bsn ("stdin");
  buildfile_scanner<utf8_validator, 1> bs (s, bsn);

  try
  {
    string r;

    if (mode == "-e")
    {
      scanner::xchar c (s.get ());
      assert (c == '(');

      r += c;
      r += bs.scan_eval ();

      c = s.get ();
      assert (c == ')');

      r += c;
    }
    else if (mode == "-l")
    {
      char stop ('\0');

      if (argc == 3)
      {
        const char* chr (argv[2]);
        assert (chr[0] != '\0' && chr[1] == '\0');

        stop = chr[0];
      }

      r += bs.scan_line (stop);

      scanner::xchar c (s.get ());
      assert (scanner::eos (c) || c == '\n' || (stop != '\0' && c == stop));
    }
    else if (mode == "-b")
    {
      scanner::xchar c (s.get ());
      assert (c == '{');

      r += c;
      r += bs.scan_block ();

      assert (scanner::eos (s.peek ()));

      r += "}\n";
    }
    else
      assert (false);

    cout << r;
  }
  catch (const buildfile_scanning& e)
  {
    cerr << e << endl;
    return 1;
  }

  return 0;
}
