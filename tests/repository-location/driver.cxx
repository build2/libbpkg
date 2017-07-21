// file      : tests/repository-location/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <cassert>
#include <sstream>
#include <iostream>
#include <exception>
#include <stdexcept> // invalid_argument

#include <libbutl/utility.hxx>         // operator<<(ostream, exception)
#include <libbutl/optional.hxx>
#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

using namespace std;
using namespace butl;
using namespace bpkg;

using butl::optional;

static bool
bad_location (const string& l)
{
  try
  {
    repository_location bl (l);
    return false;
  }
  catch (const invalid_argument&)
  {
    return true;
  }
}

static bool
bad_location (const string& l, const repository_location& b)
{
  try
  {
    repository_location bl (l, b);
    return false;
  }
  catch (const invalid_argument&)
  {
    return true;
  }
}

static string
effective_url (const string& l, const repository_location& r)
{
  istringstream is (":1\nurl: " + l);
  manifest_parser mp (is, "");
  repository_manifest m (mp);

  optional<string> u (m.effective_url (r));
  assert (u);
  return *u;
}

static bool
bad_url (const string& l, const repository_location& r)
{
  try
  {
    effective_url (l, r);
    return false;
  }
  catch (const invalid_argument&)
  {
  }
  catch (const logic_error&)
  {
  }
  return true;
}

int
main (int argc, char* argv[])
{
  using protocol = repository_location::protocol;

  if (argc != 1)
  {
    cerr << "usage: " << argv[0] << endl;
    return 1;
  }

  try
  {
    // Test invalid locations.
    //
    // Invalid host.
    //
    assert (bad_location ("http:///aa/bb"));
    assert (bad_location ("http://1/aa/bb"));
    assert (bad_location ("http:///1/aa/bb"));
    assert (bad_location ("http://1a/aa/bb"));
    assert (bad_location ("http://a..a/aa/bb"));
    assert (bad_location ("http://.a.a/aa/bb"));
    assert (bad_location ("http://a.a./aa/bb"));
    assert (bad_location ("http://a.1a/aa/bb"));
    assert (bad_location ("http://a.1a.a/aa/bb"));
    assert (bad_location ("http://a.-ab/aa/bb"));
    assert (bad_location ("http://a.-ab.a/aa/bb"));
    assert (bad_location ("http://a.ab-/aa/bb"));
    assert (bad_location ("http://a.ab-.a/aa/bb"));
    assert (bad_location ("http://a.ab-:80/aa/bb"));
    assert (bad_location ("http://a.ab.:80/aa/bb"));
    assert (bad_location ("http://1.1.1.1.r/1/b"));
    assert (bad_location ("http://www./aa/1/bb"));

    assert (bad_location ("http://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                          "aaaaaaaaaaaaaaaaaaaa.org/1/b/"));

    // Invalid port.
    //
    assert (bad_location ("http://a:/aa/bb"));
    assert (bad_location ("http://a:1b/aa/bb"));
    assert (bad_location ("http://c.ru:8a80/1/b"));
    assert (bad_location ("http://c.ru:8:80/1/b"));
    assert (bad_location ("http://a:0/aa/bb"));
    assert (bad_location ("http://c.ru:65536/1/b"));

    // Invalid path.
    //
    assert (
      bad_location ("",
                    repository_location ("http://stable.cppget.org/1/misc")));

    assert (bad_location ("1"));
    assert (bad_location ("1/"));
    assert (bad_location ("1/.."));
    assert (bad_location ("bbb"));
    assert (bad_location ("aaa/bbb"));
    assert (bad_location ("http://aa"));
    assert (bad_location ("https://aa"));
    assert (bad_location ("http://aa/"));
    assert (bad_location ("http://aa/b/.."));
    assert (bad_location ("http://aa/."));
    assert (bad_location ("http://aa/bb"));
    assert (bad_location ("http://a.com/../c/1/aa"));
    assert (bad_location ("http://a.com/a/b/../../../c/1/aa"));

#ifndef _WIN32
    assert (bad_location ("/aaa/bbb"));
#else
    assert (bad_location ("c:\\aaa\\bbb"));
#endif

    // Invalid version.
    //
    assert (bad_location ("3/aaa/bbb"));

    // Invalid prerequisite repository location.
    //
    assert (bad_location ("a/c/1/bb"));

    assert (bad_location ("a/c/1/bb",
                          repository_location ("./var/1/stable",
                                               repository_location ())));

    assert (bad_location ("../../../1/math",
                          repository_location (
                            "http://stable.cppget.org/1/misc")));


    assert (bad_location ("../..",
                          repository_location (
                            "http://stable.cppget.org/1/misc")));

    // Invalid web interface URL.
    //
    assert (bad_url (".a/..",
                     repository_location (
                       "http://stable.cppget.org/1/misc")));

    assert (bad_url ("../a/..",
                     repository_location (
                       "http://stable.cppget.org/1/misc")));

    assert (bad_url ("../.a",
                     repository_location (
                       "http://stable.cppget.org/1/misc")));
#ifndef _WIN32
    assert (bad_url ("../../..",
                     repository_location (
                       "/var/1/misc")));

    assert (bad_url ("../../..",
                     repository_location (
                       "/var/pkg/1/misc")));
#else
    assert (bad_url ("..\\..\\..",
                     repository_location (
                       "c:\\var\\1\\misc")));

    assert (bad_url ("..\\..\\..",
                     repository_location (
                       "c:\\var\\pkg\\1\\misc")));
#endif

    assert (bad_url ("../../..", repository_location ()));

    assert (bad_url ("../../../../..",
                     repository_location (
                       "http://pkg.stable.cppget.org/foo/pkg/1/misc")));

    assert (bad_url ("../../../../../abc",
                     repository_location (
                       "http://stable.cppget.org/foo/1/misc")));

    // Test valid locations.
    //
    {
      repository_location l ("");
      assert (l.string ().empty ());
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("1/aa/bb", repository_location ());
      assert (l.string () == "1/aa/bb");

      // Relative locations have no canonical name.
      //
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("bpkg/1/aa/bb", repository_location ());
      assert (l.string () == "bpkg/1/aa/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("b/pkg/1/aa/bb", repository_location ());
      assert (l.string () == "b/pkg/1/aa/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("aa/..", repository_location ());
      assert (l.string () == ".");
      assert (l.canonical_name ().empty ());
    }
#ifndef _WIN32
    {
      repository_location l ("/1/aa/bb", repository_location ());
      assert (l.string () == "/1/aa/bb");
      assert (l.canonical_name () == "/aa/bb");
    }
    {
      repository_location l ("/pkg/1/aa/bb", repository_location ());
      assert (l.string () == "/pkg/1/aa/bb");
      assert (l.canonical_name () == "aa/bb");
    }
    {
      repository_location l ("/var/bpkg/1", repository_location ());
      assert (l.string () == "/var/bpkg/1");
      assert (l.canonical_name () == "/var/bpkg");
    }
    {
      repository_location l ("/1", repository_location ());
      assert (l.string () == "/1");
      assert (l.canonical_name () == "/");
    }
    {
      repository_location l ("/var/pkg/1/example.org/math/testing",
                             repository_location ());
      assert (l.string () == "/var/pkg/1/example.org/math/testing");
      assert (l.canonical_name () == "example.org/math/testing");
    }
    {
      repository_location l ("/var/pkg/example.org/1/math/testing",
                             repository_location ());
      assert (l.string () == "/var/pkg/example.org/1/math/testing");
      assert (l.canonical_name () == "/var/pkg/example.org/math/testing");
    }
    {
      repository_location l ("/a/b/../c/1/aa/../bb", repository_location ());
      assert (l.string () == "/a/c/1/bb");
      assert (l.canonical_name () == "/a/c/bb");
    }
    {
      repository_location l ("/a/b/../c/pkg/1/aa/../bb",
                             repository_location ());
      assert (l.string () == "/a/c/pkg/1/bb");
      assert (l.canonical_name () == "bb");
    }
#else
    {
      repository_location l ("c:\\1\\aa\\bb", repository_location ());
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "c:\\aa\\bb");
    }
    {
      repository_location l ("c:/1/aa/bb", repository_location ());
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "c:\\aa\\bb");
    }
    {
      repository_location l ("c:\\pkg\\1\\aa\\bb", repository_location ());
      assert (l.string () == "c:\\pkg\\1\\aa\\bb");
      assert (l.canonical_name () == "aa/bb");
    }
    {
      repository_location l ("c:\\var\\pkg\\1\\example.org\\math\\testing",
                             repository_location ());
      assert (l.string () == "c:\\var\\pkg\\1\\example.org\\math\\testing");
      assert (l.canonical_name () == "example.org/math/testing");
    }
    {
      repository_location l ("c:\\var\\pkg\\example.org\\1\\math\\testing",
                             repository_location ());
      assert (l.string () == "c:\\var\\pkg\\example.org\\1\\math\\testing");

      assert (l.canonical_name () ==
              "c:\\var\\pkg\\example.org\\math\\testing");
    }
    {
      repository_location l ("c:/a/b/../c/1/aa/../bb", repository_location ());
      assert (l.string () == "c:\\a\\c\\1\\bb");
      assert (l.canonical_name () == "c:\\a\\c\\bb");
    }
    {
      repository_location l ("c:/a/b/../c/pkg/1/aa/../bb",
                             repository_location ());
      assert (l.string () == "c:\\a\\c\\pkg\\1\\bb");
      assert (l.canonical_name () == "bb");
    }
#endif
    {
      repository_location l ("../c/../c/./1/aa/../bb",
                             repository_location ());
      assert (l.string () == "../c/1/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("http://www.a.com:80/1/aa/bb");
      assert (l.string () == "http://www.a.com:80/1/aa/bb");
      assert (l.canonical_name () == "a.com/aa/bb");
      assert (l.proto () == protocol::http);
    }
    {
      repository_location l ("https://www.a.com:443/1/aa/bb");
      assert (l.string () == "https://www.a.com:443/1/aa/bb");
      assert (l.canonical_name () == "a.com/aa/bb");
      assert (l.proto () == protocol::https);
    }
    {
      repository_location l ("http://www.a.com:8080/dd/1/aa/bb");
      assert (l.string () == "http://www.a.com:8080/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:8080/dd/aa/bb");
      assert (l.proto () == protocol::http);
    }
    {
      repository_location l ("http://www.a.com:8080/dd/pkg/1/aa/bb");
      assert (l.string () == "http://www.a.com:8080/dd/pkg/1/aa/bb");
      assert (l.canonical_name () == "a.com:8080/dd/aa/bb");
      assert (l.proto () == protocol::http);
    }
    {
      repository_location l ("http://www.a.com:8080/bpkg/dd/1/aa/bb");
      assert (l.string () == "http://www.a.com:8080/bpkg/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:8080/bpkg/dd/aa/bb");
      assert (l.proto () == protocol::http);
    }
    {
      repository_location l ("https://www.a.com:444/dd/1/aa/bb");
      assert (l.string () == "https://www.a.com:444/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:444/dd/aa/bb");
      assert (l.proto () == protocol::https);
    }
    {
      repository_location l ("http://a.com/a/b/../c/1/aa/../bb");
      assert (l.string () == "http://a.com/a/c/1/bb");
      assert (l.canonical_name () == "a.com/a/c/bb");
    }
    {
      repository_location l ("https://a.com/a/b/../c/1/aa/../bb");
      assert (l.string () == "https://a.com/a/c/1/bb");
      assert (l.canonical_name () == "a.com/a/c/bb");
    }
    {
      repository_location l ("http://www.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://pkg.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://pkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://bpkg.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://bpkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://abc.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://abc.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "abc.cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://pkg.www.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://pkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "www.cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://bpkg.www.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://bpkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "www.cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://cppget.org/qw//1/a//b/");
      assert (l.string () == "http://cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/qw/a/b");
    }
    {
      repository_location l ("http://stable.cppget.org/1/");
      assert (l.canonical_name () == "stable.cppget.org");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("../../1/math", l1);
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "stable.cppget.org/math");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("../../pkg/1/math", l1);
      assert (l2.string () == "http://stable.cppget.org/pkg/1/math");
      assert (l2.canonical_name () == "stable.cppget.org/math");
    }
    {
      repository_location l1 ("https://stable.cppget.org/1/misc");
      repository_location l2 ("../../1/math", l1);
      assert (l2.string () == "https://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "stable.cppget.org/math");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("../math", l1);
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "stable.cppget.org/math");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("math/..", l1);
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "stable.cppget.org/misc");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 (".", l1);
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "stable.cppget.org/misc");
    }
    {
      repository_location l1 ("http://www.stable.cppget.org:8080/1");
      repository_location l2 ("../1/math", l1);
      assert (l2.string () == "http://www.stable.cppget.org:8080/1/math");
      assert (l2.canonical_name () == "stable.cppget.org:8080/math");
      assert (l2.proto () == protocol::http);
    }
    {
      repository_location l1 ("https://www.stable.cppget.org:444/1");
      repository_location l2 ("../1/math", l1);
      assert (l2.string () == "https://www.stable.cppget.org:444/1/math");
      assert (l2.canonical_name () == "stable.cppget.org:444/math");
      assert (l2.proto () == protocol::https);
    }
#ifndef _WIN32
    {
      repository_location l1 ("/var/r1/1/misc");
      repository_location l2 ("../../../r2/1/math", l1);
      assert (l2.string () == "/var/r2/1/math");
      assert (l2.canonical_name () == "/var/r2/math");
    }
    {
      repository_location l1 ("/var/1/misc");
      repository_location l2 ("../math", l1);
      assert (l2.string () == "/var/1/math");
      assert (l2.canonical_name () == "/var/math");
    }
    {
      repository_location l1 ("/var/1/stable");
      repository_location l2 ("/var/1/test", l1);
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "/var/test");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("/var/1/test", l1);
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "/var/test");
    }
    {
      repository_location l1 ("/var/1/stable");
      repository_location l2 ("/var/1/stable", repository_location ());
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
#else
    {
      repository_location l1 ("c:/var/r1/1/misc");
      repository_location l2 ("../../../r2/1/math", l1);
      assert (l2.string () == "c:\\var\\r2\\1\\math");
      assert (l2.canonical_name () == "c:\\var\\r2\\math");
    }
    {
      repository_location l1 ("c:/var/1/misc");
      repository_location l2 ("../math", l1);
      assert (l2.string () == "c:\\var\\1\\math");
      assert (l2.canonical_name () == "c:\\var\\math");
    }
    {
      repository_location l1 ("c:/var/1/stable");
      repository_location l2 ("c:\\var\\1\\test", l1);
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "c:\\var\\test");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("c:/var/1/test", l1);
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "c:\\var\\test");
    }
    {
      repository_location l1 ("c:/var/1/stable");
      repository_location l2 ("c:/var/1/stable", repository_location ());
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
#endif
    {
      repository_location l1 ("http://www.cppget.org/1/stable");
      repository_location l2 ("http://abc.com/1/test", l1);
      assert (l2.string () == "http://abc.com/1/test");
      assert (l2.canonical_name () == "abc.com/test");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/");
      repository_location l2 ("http://stable.cppget.org/1/",
                             repository_location ());
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }

    // Test valid web interface locations.
    //
    {
      repository_location l ("http://cppget.org/1/misc");
      assert (effective_url ("http://cppget.org/pkg", l) ==
              "http://cppget.org/pkg");
    }
    {
      repository_location l ("http://cppget.org/1/misc");
      assert (effective_url ("https://cppget.org/pkg", l) ==
              "https://cppget.org/pkg");
    }
    {
      repository_location l ("http://pkg.cppget.org/foo/pkg/1/misc/stable");
      assert (effective_url ("./.", l) ==
              "http://pkg.cppget.org/foo/pkg/misc/stable");
    }
    {
      repository_location l ("http://cppget.org/foo/1/misc/stable");
      assert (effective_url ("./.", l) ==
              "http://cppget.org/foo/misc/stable");
    }
    {
      repository_location l ("http://pkg.cppget.org/foo/pkg/1/misc/stable");
      assert (effective_url ("././..", l) ==
              "http://pkg.cppget.org/foo/pkg/misc");
    }
    {
      repository_location l ("http://pkg.cppget.org/foo/pkg/1/misc");
      assert (effective_url ("././../../..", l) == "http://pkg.cppget.org");
    }
    {
      repository_location l ("https://pkg.cppget.org/foo/pkg/1/misc/stable");
      assert (effective_url ("../.", l) ==
              "https://cppget.org/foo/pkg/misc/stable");
    }
    {
      repository_location l ("https://pkg.cppget.org/foo/pkg/1/misc/stable");
      assert (effective_url (".././..", l) ==
              "https://cppget.org/foo/pkg/misc");
    }
    {
      repository_location l ("https://bpkg.cppget.org/foo/bpkg/1/misc/stable");
      assert (effective_url ("./..", l) ==
              "https://bpkg.cppget.org/foo/misc/stable");
    }
    {
      repository_location l ("https://bpkg.cppget.org/foo/bpkg/1/misc/stable");
      assert (effective_url ("./../..", l) ==
              "https://bpkg.cppget.org/foo/misc");
    }
    {
      repository_location l ("http://www.cppget.org/foo/bpkg/1/misc/stable");
      assert (effective_url ("../..", l) ==
              "http://cppget.org/foo/misc/stable");
    }
    {
      repository_location l ("http://cppget.org/pkg/foo/1/misc/stable");
      assert (effective_url ("../..", l) ==
              "http://cppget.org/pkg/foo/misc/stable");
    }
    {
      repository_location l ("http://www.cppget.org/foo/bpkg/1/misc/stable");
      assert (effective_url ("../../..", l) ==
              "http://cppget.org/foo/misc");
    }
    {
      repository_location l ("http://pkg.cppget.org/foo/pkg/1/misc");
      assert (effective_url ("../../../..", l) == "http://cppget.org");
    }
    {
      repository_location l ("http://www.cppget.org/foo/bpkg/1/misc/stable");
      assert (effective_url ("../../../abc", l) ==
              "http://cppget.org/foo/misc/abc");
    }
  }
  catch (const exception& e)
  {
    cerr << e << endl;
    return 1;
  }
}
