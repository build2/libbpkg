// file      : tests/package-version/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <cassert>
#include <cstdint>   // uint16
#include <iostream>
#include <exception>
#include <stdexcept> // invalid_argument

#include <libbutl/utility.hxx>  // operator<<(ostream, exception)
#include <libbutl/optional.hxx>

#include <libbpkg/manifest.hxx>

using namespace std;
using namespace butl;
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

static bool
bad_version (uint16_t e, const string& u, const optional<string>& l, uint16_t r)
{
  try
  {
    version bv (e, u, l, r);
    return false;
  }
  catch (const invalid_argument&)
  {
    return true;
  }
}

static bool
bad_version (uint16_t e, const string& u, const char* l, uint16_t r)
{
  return bad_version (e, u, string (l), r);
}

static bool
test_constructor (const version& v)
{
  return v == version (v.epoch, v.upstream, v.release, v.revision);
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
    assert (bad_version ("1~"));            // Same.
    assert (bad_version ("1~+3"));          // Same.
    assert (bad_version ("+3"));            // Same.
    assert (bad_version ("1~-a"));          // Same.
    assert (bad_version ("1~-a+3"));        // Same.
    assert (bad_version ("-a+3"));          // Same.
    assert (bad_version ("~3.5"));          // Empty epoch.
    assert (bad_version ("a+"));            // Empty revision.
    assert (bad_version ("1~2~4.1+3"));     // Extra epoch.
    assert (bad_version ("3.5+1+4"));       // Extra revision.
    assert (bad_version ("1~~2+3"));        // Duplicated epoch separator.
    assert (bad_version ("1~2++3"));        // Duplicated revision separator.
    assert (bad_version ("a.39485739122323231.3")); // Too long component.
    assert (bad_version ("a.00000000000000000.3")); // Too long zero component.
    assert (bad_version ("1-a.00000000000000000")); // Same.
    assert (bad_version ("65536~q.3"));     // Too big epoch.
    assert (bad_version ("1+q+65536"));     // Too big revision.
    assert (bad_version ("3.5~1.4"));       // Components in epoch.
    assert (bad_version ("3.5+1.4"));       // Components in revision.
    assert (bad_version ("3 5+1"));         // Non alpha-numeric in upstream.
    assert (bad_version ("1~ +3"));         // Same.
    assert (bad_version ("1-3 5+1"));       // Non alpha-numeric in release.
    assert (bad_version ("1~1- +3"));       // Same.
    assert (bad_version ("3 5~4+1"));       // Non alpha-numeric in epoch.
    assert (bad_version ("2b~a"));          // Same.
    assert (bad_version ("1~34.1+3 5"));    // Non numeric in revision.
    assert (bad_version ("a+3s"));          // Same.
    assert (bad_version ("a."));            // Not completed upstream.
    assert (bad_version ("a..b"));          // Empty upstream component.
    assert (bad_version ("a.b-+1"));        // Revision for empty release.
    assert (bad_version ("0.0-+3"));        // Same.
    assert (bad_version ("1.2.3-~"));       // Invalid release.
    assert (bad_version ("0-"));            // Illegal version.
    assert (bad_version ("0.0-"));          // Same.

    assert (bad_version (0, "1", "", 1));      // Revision for empty release.
    assert (bad_version (1, "1~1.1", "", 2));  // Epoch in upstream.
    assert (bad_version (1, "1.1-1", "", 2));  // Release in upstream.
    assert (bad_version (1, "1.1+1", "", 2));  // Revision in upstream.
    assert (bad_version (1, "1", "1~1.1", 2)); // Epoch in release.
    assert (bad_version (1, "1", "1.1-1", 2)); // Release in release.
    assert (bad_version (1, "1", "1.1+1", 2)); // Revision in release.

    assert (bad_version (1, "", "", 0));  // Unexpected epoch.
    assert (bad_version (0, "", "1", 0)); // Unexpected release.
    assert (bad_version (0, "", "", 1));  // Unexpected revision.

    {
      version v1;
      assert (v1.empty ());
      assert (v1.canonical_upstream.empty ());
      assert (v1.canonical_release.empty ());

      version v2 ("0.0.0");
      assert (!v2.empty ());
      assert (v1.canonical_upstream.empty ());
      assert (v2.canonical_release == "~");

      assert (v1 != v2);
    }

    {
      version v ("1~0.0-");
      assert (!v.empty ());
      assert (v.string () == "1~0.0-");
      assert (v.canonical_upstream.empty ());
      assert (v.canonical_release.empty ());
      assert (test_constructor (v));
    }

    {
      version v ("a");
      assert (v.string () == "a");
      assert (v.canonical_upstream == "a");
      assert (test_constructor (v));
    }

    {
      version v ("65534~ab+65535");
      assert (v.string () == "65534~ab+65535");
      assert (v.canonical_upstream == "ab");
      assert (test_constructor (v));
    }

    {
      version v ("1");
      assert (v.string () == "1");
      assert (v.canonical_upstream == "0000000000000001");
      assert (test_constructor (v));
    }

    {
      version v ("0");
      assert (v.string () == "0");
      assert (v.canonical_upstream.empty ());
      assert (test_constructor (v));
    }

    {
      version v ("0.0.0");
      assert (v.string () == "0.0.0");
      assert (v.canonical_upstream.empty ());
      assert (test_constructor (v));
    }

    {
      version v ("1.0.0");
      assert (v.string () == "1.0.0");
      assert (v.canonical_upstream == "0000000000000001");
      assert (test_constructor (v));
    }

    {
      version v ("0.1.00");
      assert (v.string () == "0.1.00");
      assert (v.canonical_upstream == "0000000000000000.0000000000000001");
      assert (test_constructor (v));
    }

    {
      version v ("0.0a.00");
      assert (v.string () == "0.0a.00");
      assert (v.canonical_upstream == "0000000000000000.0a");
      assert (test_constructor (v));
    }

    {
      version v ("0.a00.00");
      assert (v.string () == "0.a00.00");
      assert (v.canonical_upstream == "0000000000000000.a00");
      assert (test_constructor (v));
    }

    {
      version v ("1~0");
      assert (v.string () == "1~0");
      assert (v.canonical_upstream.empty ());
      assert (test_constructor (v));
    }

    {
      version v ("0~A+1");
      assert (v.string () == "A+1");
      assert (v.canonical_upstream == "a");
      assert (test_constructor (v));
    }

    {
      version v ("10~B+0");
      assert (v.string () == "10~B");
      assert (v.canonical_upstream == "b");
      assert (test_constructor (v));
    }

    {
      version v ("3~1A.31.0.4.0+7");
      assert (v.string () == "3~1A.31.0.4.0+7");
      assert (v.canonical_upstream ==
              "1a.0000000000000031.0000000000000000.0000000000000004");
      assert (test_constructor (v));
    }

    {
      version v ("1.2.3");
      assert (v.string () == "1.2.3");
      assert (!v.release);
      assert (v.canonical_release == "~");
      assert (test_constructor (v));
    }

    {
      version v ("1.2.3+1");
      assert (v.string () == "1.2.3+1");
      assert (!v.release);
      assert (v.canonical_release == "~");
      assert (test_constructor (v));
    }

    {
      version v ("1.2.3-");
      assert (v.string () == "1.2.3-");
      assert (v.release && v.release->empty ());
      assert (v.canonical_release.empty ());
      assert (test_constructor (v));
    }

    {
      version v ("1~A-1.2.3B.00+0");
      assert (v.string () == "1~A-1.2.3B.00");
      assert (v.release && *v.release == "1.2.3B.00");
      assert (v.canonical_release == "0000000000000001.0000000000000002.3b");
      assert (test_constructor (v));
    }

    {
      version v ("65535~q.3+65535");
      assert (v.string () == "65535~q.3+65535");
      assert (!v.release);
      assert (v.canonical_release == "~");
      assert (test_constructor (v));
    }

    {
      version v (1, "1", nullopt, 2);
      assert (v.string () == "1~1+2");
      assert (!v.release);
      assert (v.canonical_release == "~");
      assert (test_constructor (v));
    }

    {
      version v (1, "1", string (), 0);
      assert (v.string () == "1~1-");
      assert (v.release && v.release->empty ());
      assert (v.canonical_release.empty ());
      assert (test_constructor (v));
    }

    assert (version ("a") == version ("a"));
    assert (version ("a") < version ("b"));
    assert (version ("a") < version ("aa"));
    assert (version ("a.a") < version ("aaa"));
    assert (version ("a") < version ("a.a"));
    assert (version ("ab") == version ("ab"));
    assert (version ("ac") < version ("bc"));
    assert (version ("ab+0") == version ("ab"));
    assert (version ("a.1+1") > version ("a.1"));
    assert (version ("0~ab") == version ("ab"));
    assert (version ("1.2") > version ("1.1"));
    assert (version ("1~1.0") > version ("2.0"));
    assert (version ("0~ab+1") == version ("ab+1"));
    assert (version ("0~ab+1").compare (version ("0~ab+2"), true) == 0);
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
    assert (version ("1.0-alpha") < version ("1.0"));
    assert (version ("1.0-") < version ("1.0"));
    assert (version ("1.0-") < version ("1.0-alpha"));
    assert (version ("1.0-alpha") < version ("1.1"));
    assert (version ("1.0-alpha+1") < version ("1.0"));
    assert (version ("1.0-alpha+1") < version ("1.1"));
    assert (version ("1.0-alpha") > version ("1.0-1"));
    assert (version ("1.0-alpha") == version ("1.0-alpha.0"));

    assert (version (1, "2.0", nullopt, 3) == version ("1~2+3"));
    assert (version (1, "2.0", string (), 0) == version ("1~2-"));
    assert (version (0, "", string (), 0) == version ());
  }
  catch (const exception& e)
  {
    cerr << e << endl;
    return 1;
  }
}
