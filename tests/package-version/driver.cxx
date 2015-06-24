// file      : tests/package-version/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2015 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <cassert>
#include <iostream>
#include <exception>
#include <stdexcept> // invalid_argument

#include <bpkg/manifest>

using namespace std;
using namespace bpkg;

static bool
bad_version (const string& v)
{
  try
  {
    version bv (v);
    return false;
  }
  catch (const invalid_argument&)
  {
    return true;
  }
}

int
main (int argc, char* argv[])
{
  if (argc != 1)
  {
    cerr << "usage: " << argv[0] << endl;
    return 1;
  }

  try
  {
    assert (bad_version (""));              // Empty upstream.
    assert (bad_version ("1+"));            // Same.
    assert (bad_version ("1+-3"));          // Same.
    assert (bad_version ("-3"));            // Same.
    assert (bad_version ("+3.5"));          // Empty epoch.
    assert (bad_version ("a-"));            // Empty revision.
    assert (bad_version ("1+2+4.1-3"));     // Extra epoch.
    assert (bad_version ("3.5-1-4"));       // Extra revision.
    assert (bad_version ("1++2-3"));        // Duplicated epoch separator.
    assert (bad_version ("1+2--3"));        // Duplicated revision separator.
    assert (bad_version ("a.394857391.3")); // Too long numeric component.
    assert (bad_version ("a.000000000.3")); // Too long numeric zero component.
    assert (bad_version ("65536+q.3"));     // Too big epoch.
    assert (bad_version ("1+q-65536"));     // Too big revision.
    assert (bad_version ("3.5+1.4"));       // Components in epoch.
    assert (bad_version ("3.5-1.4"));       // Components in revision.
    assert (bad_version ("3 5-1"));         // Non alpha-numeric in upstream.
    assert (bad_version ("1+ -3"));         // Same.
    assert (bad_version ("3 5+4-1"));       // Non alpha-numeric in epoch.
    assert (bad_version ("2b+a"));          // Same.
    assert (bad_version ("1+34.1-3 5"));    // Non alpha-numeric in revision.
    assert (bad_version ("a-3s"));          // Same.
    assert (bad_version ("a."));            // Not completed upstream.
    assert (bad_version ("a..b"));          // Empty upstream component.

    {
      version v ("a");
      assert (v.string () == "a");
      assert (v.canonical_upstream () == "a");
    }

    {
      version v ("65535+ab-65535");
      assert (v.string () == "65535+ab-65535");
      assert (v.canonical_upstream () == "ab");
    }

    {
      version v ("1");
      assert (v.string () == "1");
      assert (v.canonical_upstream () == "00000001");
    }

    {
      version v ("0");
      assert (v.string () == "0");
      assert (v.canonical_upstream ().empty ());
    }

    {
      version v ("0.0.0");
      assert (v.string () == "0.0.0");
      assert (v.canonical_upstream ().empty ());
    }

    {
      version v ("1.0.0");
      assert (v.string () == "1.0.0");
      assert (v.canonical_upstream () == "00000001");
    }

    {
      version v ("0.1.00");
      assert (v.string () == "0.1.00");
      assert (v.canonical_upstream () == "00000000.00000001");
    }

    {
      version v ("0.0a.00");
      assert (v.string () == "0.0a.00");
      assert (v.canonical_upstream () == "00000000.0a");
    }

    {
      version v ("0.a00.00");
      assert (v.string () == "0.a00.00");
      assert (v.canonical_upstream () == "00000000.a00");
    }

    {
      version v ("1+0");
      assert (v.string () == "1+0");
      assert (v.canonical_upstream ().empty ());
    }

    {
      version v ("0+A-1");
      assert (v.string () == "A-1");
      assert (v.canonical_upstream () == "a");
    }

    {
      version v ("10+B-0");
      assert (v.string () == "10+B");
      assert (v.canonical_upstream () == "b");
    }

    {
      version v ("3+1A.31.0.4.0-7");
      assert (v.string () == "3+1A.31.0.4.0-7");
      assert (v.canonical_upstream () == "1a.00000031.00000000.00000004");
    }

    assert (version ("a") == version ("a"));
    assert (version ("a") < version ("b"));
    assert (version ("a") < version ("aa"));
    assert (version ("a.a") < version ("aaa"));
    assert (version ("a") < version ("a.a"));
    assert (version ("ab") == version ("ab"));
    assert (version ("ac") < version ("bc"));
    assert (version ("ab-0") == version ("ab"));
    assert (version ("a.1-1") > version ("a.1"));
    assert (version ("0+ab") == version ("ab"));
    assert (version ("1.2") > version ("1.1"));
    assert (version ("1+1.0") > version ("2.0"));
    assert (version ("0+ab-1") == version ("ab-1"));
    assert (version ("0+ab-1").compare (version ("0+ab-2"), true) == 0);
    assert (version ("12") > version ("2"));
    assert (version ("2") < version ("12"));
    assert (version ("1") == version ("01"));
    assert (version ("1") == version ("1.0"));
    assert (version ("1.3") == version ("1.3.0"));
    assert (version ("1.3") == version ("1.3.0.0"));
    assert (version ("1.3.1") > version ("1.3"));
    assert (version ("1.30") > version ("1.5"));
    assert (version ("1.alpha.1") < version ("1.Beta.1"));
    assert (version ("1.Alpha.1") < version ("1.beta.1"));
    assert (version ("1.Alpha.1") == version ("1.ALPHA.1"));
    assert (version ("a.1") < version ("ab1"));
    assert (version ("a.2") < version ("a.1b"));
    assert (version ("0.0.0") == version ("0"));
    assert (version ("1.0.0") == version ("01"));
    assert (version ("0.1.00") == version ("00.1"));
    assert (version ("0.0a.00") == version ("00.0a"));
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
