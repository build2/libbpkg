// file      : tests/repository-location/driver.cxx -*- C++ -*-
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
    assert (bad_location (""));
    assert (bad_location ("1"));
    assert (bad_location ("1/"));
    assert (bad_location ("bbb"));
    assert (bad_location ("aaa/bbb"));
    assert (bad_location ("/aaa/bbb"));
    assert (bad_location ("http://aa/bb"));
    assert (bad_location ("http://a.com/../c/1/aa"));

    // Invalid version.
    //
    assert (bad_location ("3/aaa/bbb"));

    // Test valid locations.
    //
    {
      repository_location l ("1/aa/bb");
      assert (l.string () == "1/aa/bb");
      assert (l.canonical_name () == "aa/bb");
    }
    {
      repository_location l ("/a/b/../c/1/aa/../bb");
      assert (l.string () == "/a/c/1/bb");
      assert (l.canonical_name () == "bb");
    }
    {
      repository_location l ("../c/../c/./1/aa/../bb");
      assert (l.string () == "../c/1/bb");
      assert (l.canonical_name () == "bb");
    }
    {
      repository_location l ("http://www.a.com:80/1/aa/bb");
      assert (l.string () == "http://www.a.com:80/1/aa/bb");
      assert (l.canonical_name () == "a.com/aa/bb");
    }
    {
      repository_location l ("http://www.a.com:8080/dd/1/aa/bb");
      assert (l.string () == "http://www.a.com:8080/dd/1/aa/bb");
      assert (l.canonical_name () == "a.com:8080/aa/bb");
    }
    {
      repository_location l ("http://a.com/a/b/../c/1/aa/../bb");
      assert (l.string () == "http://a.com/a/c/1/bb");
      assert (l.canonical_name () == "a.com/bb");
    }
    {
      repository_location l ("http://www.CPPget.org/qw/1/a/b/");
      assert (l.string () == "http://www.cppget.org/qw/1/a/b");
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
      repository_location l ("http://cppget.org/qw//1/a//b/");
      assert (l.string () == "http://cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "cppget.org/a/b");
    }
    {
      repository_location l ("http://stable.cppget.org/1/");
      assert (l.canonical_name () == "stable.cppget.org");
    }
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
