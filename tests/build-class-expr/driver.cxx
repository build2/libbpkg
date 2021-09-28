// file      : tests/build-class-expr/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <ios>
#include <string>
#include <iostream>

#include <libbutl/utility.hxx>  // eof(), operator<<(ostream, exception)
#include <libbutl/optional.hxx>

#include <libbpkg/manifest.hxx>

#undef NDEBUG
#include <cassert>

// Usages:
//
// argv[0] -p
// argv[0] [<class>[:<base>]]*
//
// Parse stdin lines as build configuration class expressions and print them
// or evaluate.
//
// In the first form print expressions to stdout, one per line.
//
// In the second form sequentially match the configuration classes passed as
// arguments against the expressions, updating the match result. If the first
// expression has an underlying class set specified, then transform the
// combined expression, making the underlying class set a starting set for the
// original expression and a restricting set, simultaneously.
//
// On error print the exception description to stderr and exit with the two
// status. Otherwise, if the combined expression doesn't match then exit with
// the one status. Otherwise, exit with zero status.
//
int
main (int argc, char* argv[])
{
  using namespace std;
  using namespace butl;
  using namespace bpkg;

  using butl::optional;

  bool print (argc != 1 && argv[1] == string ("-p"));

  assert (!print || argc == 2);

  cin.exceptions (ios::badbit);

  strings cs;
  build_class_inheritance_map im;

  if (print)
    cout.exceptions (ios::failbit | ios::badbit);
  else
  {
    for (int i (1); i != argc; ++i)
    {
      string c (argv[i]);

      string base;
      size_t p (c.find (':'));

      if (p != string::npos)
      {
        base = string (c, p + 1);
        c.resize (p);
      }

      im[c] = move (base);
      cs.emplace_back (move (c));
    }
  }

  try
  {
    string s;
    bool r (false);
    optional<strings> underlying_cls;

    while (!eof (getline (cin, s)))
    {
      build_class_expr expr (s, "" /* comment */);

      if (print)
        cout << expr << endl;
      else
      {
        if (!underlying_cls)
        {
          underlying_cls = move (expr.underlying_classes);

          if (!underlying_cls->empty ())
          {
            build_class_expr expr (*underlying_cls, '+', "" /* comment */);
            expr.match (cs, im, r);
          }
        }

        expr.match (cs, im, r);
      }
    }

    if (underlying_cls && !underlying_cls->empty ())
    {
      build_class_expr expr (*underlying_cls, '&', "" /* comment */);
      expr.match (cs, im, r);
    }

    return print || r ? 0 : 1;
  }
  catch (const exception& e)
  {
    cerr << e << endl;
    return 2;
  }
}
