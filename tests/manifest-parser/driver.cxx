// file      : tests/manifest-parser/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <vector>
#include <string>
#include <utility> // pair
#include <cassert>
#include <sstream>
#include <iostream>

#include <bpkg/manifest-parser>

using namespace std;
using namespace bpkg;

using pairs = vector<pair<string, string>>;

static bool
parse (const char* manifest, const pairs& expected);

static bool
fail (const char* manifest);

int
main ()
{
  // Whitespaces and comments.
  //
  assert (parse (" \t", {{"",""}}));
  assert (parse (" \t\n \n\n", {{"",""}}));
  assert (parse ("# one\n  #two", {{"",""}}));

  // Test encountering eos at various points.
  //
  assert (parse ("", {{"",""}}));
  assert (parse (" ", {{"",""}}));
  assert (parse ("\n", {{"",""}}));
  assert (fail ("a"));
  assert (parse (":1\na:", {{"","1"},{"a", ""},{"",""},{"",""}}));

  // Invalid manifests.
  //
  assert (fail ("a:"));          // format version pair expected
  assert (fail (":"));           // format version value expected
  assert (fail (":9"));          // unsupported format version
  assert (fail ("a"));           // ':' expected after name
  assert (fail ("a b"));         // ':' expected after name
  assert (fail ("a\tb"));        // ':' expected after name
  assert (fail ("a\nb"));        // ':' expected after name
  assert (fail (":1\na:b\n:9")); // unsupported format version

  // Empty manifest.
  //
  assert (parse (":1", {{"","1"},{"",""},{"",""}}));
  assert (parse (" \t :1", {{"","1"},{"",""},{"",""}}));
  assert (parse (" \t : 1", {{"","1"},{"",""},{"",""}}));
  assert (parse (" \t : 1 ", {{"","1"},{"",""},{"",""}}));
  assert (parse (":1\n", {{"","1"},{"",""},{"",""}}));
  assert (parse (":1 \n", {{"","1"},{"",""},{"",""}}));

  // Single manifest.
  //
  assert (parse (":1\na:x", {{"","1"},{"a", "x"},{"",""},{"",""}}));
  assert (parse (":1\na:x\n", {{"","1"},{"a","x"},{"",""},{"",""}}));
  assert (parse (":1\na:x\nb:y",
                 {{"","1"},{"a","x"},{"b","y"},{"",""},{"",""}}));
  assert (parse (":1\na:x\n\tb : y\n  #comment",
                 {{"","1"},{"a","x"},{"b","y"},{"",""},{"",""}}));

  // Multiple manifests.
  //
  assert (parse (":1\na:x\n:\nb:y",
                 {{"","1"},{"a", "x"},{"",""},
                  {"","1"},{"b", "y"},{"",""},{"",""}}));
  assert (parse (":1\na:x\n:1\nb:y",
                 {{"","1"},{"a", "x"},{"",""},
                  {"","1"},{"b", "y"},{"",""},{"",""}}));
  assert (parse (":1\na:x\n:\nb:y\n:\nc:z\n",
                 {{"","1"},{"a", "x"},{"",""},
                  {"","1"},{"b", "y"},{"",""},
                  {"","1"},{"c", "z"},{"",""},{"",""}}));

  // Name parsing.
  //
  assert (parse (":1\nabc:", {{"","1"},{"abc",""},{"",""},{"",""}}));
  assert (parse (":1\nabc :", {{"","1"},{"abc",""},{"",""},{"",""}}));
  assert (parse (":1\nabc\t:", {{"","1"},{"abc",""},{"",""},{"",""}}));

  // Simple value parsing.
  //
  assert (parse (":1\na: \t xyz \t ", {{"","1"},{"a","xyz"},{"",""},{"",""}}));

  // Simple value escaping.
  //
  assert (parse (":1\na:x\\", {{"","1"},{"a","x"},{"",""},{"",""}}));
  assert (parse (":1\na:x\\\ny", {{"","1"},{"a","xy"},{"",""},{"",""}}));
  assert (parse (":1\na:x\\\\\nb:",
                 {{"","1"},{"a","x\\"},{"b",""},{"",""},{"",""}}));
  assert (parse (":1\na:x\\\\\\\nb:",
                 {{"","1"},{"a","x\\\\"},{"b",""},{"",""},{"",""}}));

  // Simple value literal newline.
  //
  assert (parse (":1\na:x\\\n\\",
                 {{"","1"},{"a","x\n"},{"",""},{"",""}}));
  assert (parse (":1\na:x\\\n\\\ny",
                 {{"","1"},{"a","x\ny"},{"",""},{"",""}}));
  assert (parse (":1\na:x\\\n\\\ny\\\n\\\nz",
                 {{"","1"},{"a","x\ny\nz"},{"",""},{"",""}}));

  // Multi-line value parsing.
  //
  assert (parse (":1\na:\\", {{"","1"},{"a", ""},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n", {{"","1"},{"a", ""},{"",""},{"",""}}));
  assert (parse (":1\na:\\x", {{"","1"},{"a", "\\x"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n\\", {{"","1"},{"a", ""},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n\\\n", {{"","1"},{"a", ""},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n\\x\n\\",
                 {{"","1"},{"a", "\\x"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\nx\ny", {{"","1"},{"a", "x\ny"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n \n#\t\n\\",
                 {{"","1"},{"a", " \n#\t"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\n\n\n\\", {{"","1"},{"a", "\n"},{"",""},{"",""}}));

  // Multi-line value escaping.
  //
  assert (parse (":1\na:\\\nx\\",
                 {{"","1"},{"a","x"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\nx\\\ny\n\\",
                 {{"","1"},{"a","xy"},{"",""},{"",""}}));
  assert (parse (":1\na:\\\nx\\\\\n\\\nb:",
                 {{"","1"},{"a","x\\"},{"b",""},{"",""},{"",""}}));
  assert (parse (":1\na:\\\nx\\\\\\\n\\\nb:",
                 {{"","1"},{"a","x\\\\"},{"b",""},{"",""},{"",""}}));
}

static std::ostream&
operator<< (std::ostream& os, const pairs& ps)
{
  os << '{';

  bool f (true);
  for (const auto& p: ps)
    os << (f ? (f = false, "") : ",")
       << '{' << p.first << ',' << p.second << '}';

  os << '}';
  return os;
}

static pairs
parse (const char* m)
{
  istringstream is (m);
  is.exceptions (istream::failbit | istream::badbit);
  manifest_parser p (is, "");

  pairs r;

  for (bool eom (true), eos (false); !eos; )
  {
    auto nv (p.next ());

    if (nv.name.empty () && nv.value.empty ()) // End pair.
    {
      eos = eom;
      eom = true;
    }
    else
      eom = false;

    r.emplace_back (nv.name, nv.value); // move
  }

  return r;
}

static bool
parse (const char* m, const pairs& e)
{
  pairs r (parse (m));

  if (r != e)
  {
    cerr << "actual: " << r << endl
         << "expect: " << e << endl;

    return false;
  }

  return true;
}

static bool
fail (const char* m)
{
  try
  {
    pairs r (parse (m));
    cerr << "nofail: " << r << endl;
    return false;
  }
  catch (const manifest_parsing& e)
  {
    cerr << e.what () << endl;
  }

  return true;
}
