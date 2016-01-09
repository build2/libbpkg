// file      : tests/repository-location/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
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
  catch (const invalid_argument)
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
    assert (bad_location ("/aaa/bbb"));
    assert (bad_location ("http://aa"));
    assert (bad_location ("https://aa"));
    assert (bad_location ("http://aa/"));
    assert (bad_location ("http://aa/b/.."));
    assert (bad_location ("http://aa/."));
    assert (bad_location ("http://aa/bb"));
    assert (bad_location ("http://a.com/../c/1/aa"));
    assert (bad_location ("http://a.com/a/b/../../../c/1/aa"));

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
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("/1/aa/bb", repository_location ());
      assert (l.string () == "/1/aa/bb");
      assert (l.canonical_name () == "aa/bb");
    }
    {
      repository_location l ("/a/b/../c/1/aa/../bb", repository_location ());
      assert (l.string () == "/a/c/1/bb");
      assert (l.canonical_name () == "bb");
    }
    {
      repository_location l ("../c/../c/./1/aa/../bb", repository_location ());
      assert (l.string () == "../c/1/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l ("http://www.a.com:80/1/aa/bb");
      assert (l.string () == "http://www.a.com:80/1/aa/bb");
      assert (l.canonical_name () == "a.com/aa/bb");
      assert (!l.secure ());
    }
    {
      repository_location l ("https://www.a.com:443/1/aa/bb");
      assert (l.string () == "https://www.a.com:443/1/aa/bb");
      assert (l.canonical_name () == "a.com/aa/bb");
      assert (l.secure ());
    }
    {
      repository_location l ("http://www.a.com:8080/dd/1/aa/bb");
      assert (l.string () == "http://www.a.com:8080/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:8080/aa/bb");
      assert (!l.secure ());
    }
    {
      repository_location l ("https://www.a.com:444/dd/1/aa/bb");
      assert (l.string () == "https://www.a.com:444/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:444/aa/bb");
      assert (l.secure ());
    }
    {
      repository_location l ("http://a.com/a/b/../c/1/aa/../bb");
      assert (l.string () == "http://a.com/a/c/1/bb");
      assert (l.canonical_name () == "a.com/bb");
    }
    {
      repository_location l ("https://a.com/a/b/../c/1/aa/../bb");
      assert (l.string () == "https://a.com/a/c/1/bb");
      assert (l.canonical_name () == "a.com/bb");
    }
    {
      repository_location l ("http://www.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/a/b");
    }
    {
      repository_location l ("http://pkg.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://pkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/a/b");
    }
    {
      repository_location l ("http://bpkg.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://bpkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/a/b");
    }
    {
      repository_location l ("http://abc.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://abc.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "abc.cppget.org/a/b");
    }
    {
      repository_location l ("http://pkg.www.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://pkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "www.cppget.org/a/b");
    }
    {
      repository_location l ("http://bpkg.www.cppget.org/qw/1/a/b/");
      assert (l.string () == "http://bpkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "www.cppget.org/a/b");
    }
    {
      repository_location l ("http://cppget.org/qw//1/a//b/");
      assert (l.string () == "http://cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/a/b");
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
      repository_location l1 ("http://www.stable.cppget.org:8080/1");
      repository_location l2 ("../1/math", l1);
      assert (l2.string () == "http://www.stable.cppget.org:8080/1/math");
      assert (l2.canonical_name () == "stable.cppget.org:8080/math");
      assert (!l2.secure ());
    }
    {
      repository_location l1 ("https://www.stable.cppget.org:444/1");
      repository_location l2 ("../1/math", l1);
      assert (l2.string () == "https://www.stable.cppget.org:444/1/math");
      assert (l2.canonical_name () == "stable.cppget.org:444/math");
      assert (l2.secure ());
    }
    {
      repository_location l1 ("/var/r1/1/misc");
      repository_location l2 ("../../../r2/1/math", l1);
      assert (l2.string () == "/var/r2/1/math");
      assert (l2.canonical_name () == "math");
    }
    {
      repository_location l1 ("/var/1/misc");
      repository_location l2 ("../math", l1);
      assert (l2.string () == "/var/1/math");
      assert (l2.canonical_name () == "math");
    }
    {
      repository_location l1 ("/var/1/stable");
      repository_location l2 ("/var/1/test", l1);
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "test");
    }
    {
      repository_location l1 ("http://stable.cppget.org/1/misc");
      repository_location l2 ("/var/1/test", l1);
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "test");
    }
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
    {
      repository_location l1 ("/var/1/stable");
      repository_location l2 ("/var/1/stable", repository_location ());
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
