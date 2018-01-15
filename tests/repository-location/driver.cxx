// file      : tests/repository-location/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <cassert>
#include <sstream>
#include <iostream>
#include <stdexcept> // invalid_argument, logic_error

#include <libbutl/optional.mxx>
#include <libbutl/manifest-parser.mxx>

#include <libbpkg/manifest.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  using butl::optional;

  inline static repository_location
  loc ()
  {
    return repository_location ();
  }

  inline static repository_location
  loc (const string& l, repository_type t = repository_type::bpkg)
  {
    return repository_location (repository_url (l), t);
  }

  inline static repository_location
  loc (const string& l, const repository_location& b)
  {
    return repository_location (repository_url (l), b);
  }

  inline static bool
  bad_loc (const string& l, repository_type t = repository_type::bpkg)
  {
    try
    {
      repository_location bl (repository_url (l), t);
      return false;
    }
    catch (const invalid_argument&)
    {
      return true;
    }
  }

  inline static bool
  bad_loc (const string& l, const repository_location& b)
  {
    try
    {
      repository_location bl (repository_url (l), b);
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

  inline static bool
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
    using proto = repository_protocol;

    if (argc != 1)
    {
      cerr << "usage: " << argv[0] << endl;
      return 1;
    }

    // Test invalid locations.
    //
    // Invalid host.
    //
    assert (bad_loc ("http:///aa/1/bb"));
    assert (bad_loc ("http:///1/aa/bb"));
    assert (bad_loc ("http://www./aa/1/bb"));
    assert (bad_loc ("http://b|2.org/aa/1/bb"));
    assert (bad_loc ("file://abc/"));

    // Invalid port.
    //
    assert (bad_loc ("http://a:/aa/bb"));
    assert (bad_loc ("http://a:1b/aa/bb"));
    assert (bad_loc ("http://c.ru:8a80/1/b"));
    assert (bad_loc ("http://c.ru:8:80/1/b"));
    assert (bad_loc ("http://a:0/aa/bb"));
    assert (bad_loc ("http://c.ru:65536/1/b"));

    // Invalid path.
    //
    assert (bad_loc ("", loc ("http://stable.cppget.org/1/misc")));
    assert (bad_loc ("1"));
    assert (bad_loc ("1/"));
    assert (bad_loc ("1/.."));
    assert (bad_loc ("bbb"));
    assert (bad_loc ("aaa/bbb"));
    assert (bad_loc ("http://"));
    assert (bad_loc ("http://aa"));
    assert (bad_loc ("https://aa"));
    assert (bad_loc ("http://aa/"));
    assert (bad_loc ("http://aa/b/.."));
    assert (bad_loc ("http://aa/."));
    assert (bad_loc ("http://aa/bb"));
    assert (bad_loc ("http://a.com/../c/1/aa"));
    assert (bad_loc ("http://a.com/a/b/../../../c/1/aa"));
    assert (bad_loc ("file://"));

#ifndef _WIN32
    assert (bad_loc ("/aaa/bbb"));
#else
    assert (bad_loc ("c:\\aaa\\bbb"));
#endif

    // No URL fragment.
    //
#ifndef _WIN32
    assert (bad_loc ("file://localhost/", repository_type::git));
#else
    assert (bad_loc ("file://localhost/c:/", repository_type::git));
#endif

    // Invalid version.
    //
    assert (bad_loc ("3/aaa/bbb"));

    // Invalid prerequisite repository location.
    //
    assert (bad_loc ("a/c/1/bb"));

    assert (bad_loc ("a/c/1/bb", loc ("./var/1/stable", loc ())));

    assert (bad_loc ("../../../1/math",
                     loc ("http://stable.cppget.org/1/misc")));

    assert (bad_loc ("../..", loc ("http://stable.cppget.org/1/misc")));

    assert (bad_loc ("http:/abc"));
    assert (bad_loc ("http:///abc"));
    assert (bad_loc ("http://1.1.1.1"));
    assert (bad_loc ("http://[123]"));

#ifndef _WIN32
    assert (bad_loc ("file:////abc", repository_type::git));
    assert (bad_loc ("zzz:/abc", repository_type::git));
#else
    assert (bad_loc ("file:/abc", repository_type::git));
#endif

    // Invalid web interface URL.
    //
    assert (bad_url (".a/..", loc ("http://stable.cppget.org/1/misc")));
    assert (bad_url ("../a/..", loc ("http://stable.cppget.org/1/misc")));
    assert (bad_url ("../.a", loc ("http://stable.cppget.org/1/misc")));
#ifndef _WIN32
    assert (bad_url ("../../..", loc ("/var/1/misc")));
    assert (bad_url ("../../..", loc ("/var/pkg/1/misc")));
#else
    assert (bad_url ("..\\..\\..", loc ("c:\\var\\1\\misc")));
    assert (bad_url ("..\\..\\..", loc ("c:\\var\\pkg\\1\\misc")));
#endif

    assert (bad_url ("../../..", loc ()));

    assert (bad_url ("../../../../..",
                     loc ("http://pkg.stable.cppget.org/foo/pkg/1/misc")));

    assert (bad_url ("../../../../../abc",
                     loc ("http://stable.cppget.org/foo/1/misc")));

    // Test valid locations.
    //
    {
      repository_location l (loc (""));
      assert (l.string ().empty ());
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l (loc ("1/aa/bb", loc ()));
      assert (l.string () == "1/aa/bb");

      // Relative locations have no canonical name.
      //
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l (loc ("bpkg/1/aa/bb", loc ()));
      assert (l.string () == "bpkg/1/aa/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l (loc ("b/pkg/1/aa/bb", loc ()));
      assert (l.string () == "b/pkg/1/aa/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l (loc ("aa/..", loc ()));
      assert (l.string () == ".");
      assert (l.canonical_name ().empty ());
    }

#ifndef _WIN32
    {
      repository_location l (loc ("/1/aa/bb", loc ()));
      assert (l.string () == "/1/aa/bb");
      assert (l.canonical_name () == "bpkg:/aa/bb");
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("/pkg/1/aa/bb", loc ()));
      assert (l.string () == "/pkg/1/aa/bb");
      assert (l.canonical_name () == "bpkg:aa/bb");
    }
    {
      repository_location l (loc ("/var/bpkg/1", loc ()));
      assert (l.string () == "/var/bpkg/1");
      assert (l.canonical_name () == "bpkg:/var/bpkg");
    }
    {
      repository_location l (loc ("/1", loc ()));
      assert (l.string () == "/1");
      assert (l.canonical_name () == "bpkg:/");
    }
    {
      repository_location l (loc ("/var/pkg/1/example.org/math/testing",
                                  loc ()));
      assert (l.string () == "/var/pkg/1/example.org/math/testing");
      assert (l.canonical_name () == "bpkg:example.org/math/testing");
    }
    {
      repository_location l (loc ("/var/pkg/example.org/1/math/testing",
                                  loc ()));
      assert (l.string () == "/var/pkg/example.org/1/math/testing");
      assert (l.canonical_name () == "bpkg:/var/pkg/example.org/math/testing");
    }
    {
      repository_location l (loc ("/a/b/../c/1/aa/../bb", loc ()));
      assert (l.string () == "/a/c/1/bb");
      assert (l.canonical_name () == "bpkg:/a/c/bb");
    }
    {
      repository_location l (loc ("/a/b/../c/pkg/1/aa/../bb", loc ()));
      assert (l.string () == "/a/c/pkg/1/bb");
      assert (l.canonical_name () == "bpkg:bb");
    }
    {
      repository_location l (loc ("file:///repo/1/path"));
      assert (l.url () == loc ("file:/repo/1/path").url ());
      assert (l.url () == loc ("/repo/1/path").url ());
      assert (l.string () == "/repo/1/path");
      assert (l.canonical_name () == "bpkg:/repo/path");
    }
    {
      repository_location l (loc ("file:/git/repo#branch",
                                  repository_type::git));
      assert (l.string () == "file:/git/repo#branch");
      assert (l.canonical_name () == "git:/git/repo");
    }
    {
      repository_location l (loc ("file://localhost/#master",
                                  repository_type::git));
      assert (l.string () == "file:/#master");
      assert (l.canonical_name () == "git:/");
    }
#else
    {
      repository_location l (loc ("c:\\1\\aa\\bb", loc ()));
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "bpkg:c:\\aa\\bb");
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("c:/1/aa/bb", loc ()));
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "bpkg:c:\\aa\\bb");
    }
    {
      repository_location l (loc ("c:\\pkg\\1\\aa\\bb", loc ()));
      assert (l.string () == "c:\\pkg\\1\\aa\\bb");
      assert (l.canonical_name () == "bpkg:aa/bb");
    }
    {
      repository_location l (loc ("c:\\var\\pkg\\1\\example.org\\math\\tst",
                                  loc ()));
      assert (l.string () == "c:\\var\\pkg\\1\\example.org\\math\\tst");
      assert (l.canonical_name () == "bpkg:example.org/math/tst");
    }
    {
      repository_location l (loc ("c:\\var\\pkg\\example.org\\1\\math\\tst",
                                  loc ()));
      assert (l.string () == "c:\\var\\pkg\\example.org\\1\\math\\tst");

      assert (l.canonical_name () ==
              "bpkg:c:\\var\\pkg\\example.org\\math\\tst");
    }
    {
      repository_location l (loc ("c:/a/b/../c/1/aa/../bb", loc ()));

      assert (l.string () == "c:\\a\\c\\1\\bb");
      assert (l.canonical_name () == "bpkg:c:\\a\\c\\bb");
    }
    {
      repository_location l (loc ("c:/a/b/../c/pkg/1/aa/../bb", loc ()));
      assert (l.string () == "c:\\a\\c\\pkg\\1\\bb");
      assert (l.canonical_name () == "bpkg:bb");
    }
    {
      repository_location l (loc ("file:///c:/repo/1/path"));
      assert (l.url () == loc ("file:/c:/repo/1/path").url ());
      assert (l.url () == loc ("c:/repo/1/path").url ());
      assert (l.string () == "c:\\repo\\1\\path");
      assert (l.canonical_name () == "bpkg:c:\\repo\\path");
    }
    {
      repository_location l (loc ("file:/c:/git/repo#branch",
                                  repository_type::git));
      assert (l.string () == "file:/c:/git/repo#branch");
      assert (l.canonical_name () == "git:c:\\git\\repo");
    }
    {
      repository_location l (loc ("file://localhost/c:/#master",
                                  repository_type::git));
      assert (l.string () == "file:/c:#master");
      assert (l.canonical_name () == "git:c:");
    }
#endif
    {
      repository_location l (loc ("../c/../c/./1/aa/../bb", loc ()));
      assert (l.string () == "../c/1/bb");
      assert (l.canonical_name ().empty ());
    }
    {
      repository_location l (loc ("http://www.a.com:80/1/aa/bb"));
      assert (l.string () == "http://www.a.com:80/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("https://www.a.com:443/1/aa/bb"));
      assert (l.string () == "https://www.a.com:443/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com/aa/bb");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/dd/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/dd/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com:8080/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/dd/pkg/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/dd/pkg/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com:8080/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/bpkg/dd/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/bpkg/dd/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com:8080/bpkg/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("https://www.a.com:444/dd/1/aa/bb"));
      assert (l.string () == "https://www.a.com:444/dd/1/aa/bb");
      assert (l.canonical_name () == "bpkg:a.com:444/dd/aa/bb");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::bpkg);
    }
    {
      repository_location l (loc ("git://github.com/test#master",
                                  repository_type::git));
      assert (l.string () == "git://github.com/test#master");
      assert (l.canonical_name () == "git:github.com/test");
      assert (l.proto () == proto::git);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("http://github.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "http://github.com/test.git#master");
      assert (l.canonical_name () == "git:github.com/test");
      assert (l.proto () == proto::http);
    }
    {
      repository_location l (loc ("https://github.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://github.com/test.git#master");
      assert (l.canonical_name () == "git:github.com/test");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("http://git.example.com#master",
                                  repository_type::git));
      assert (l.string () == "http://git.example.com/#master");
      assert (l.canonical_name () == "git:example.com");
    }
    {
      repository_location l (loc ("http://a.com/a/b/../c/1/aa/../bb"));
      assert (l.string () == "http://a.com/a/c/1/bb");
      assert (l.canonical_name () == "bpkg:a.com/a/c/bb");
    }
    {
      repository_location l (loc ("https://a.com/a/b/../c/1/aa/../bb"));
      assert (l.string () == "https://a.com/a/c/1/bb");
      assert (l.canonical_name () == "bpkg:a.com/a/c/bb");
    }
    {
      repository_location l (loc ("http://www.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://pkg.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://pkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://bpkg.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://bpkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://abc.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://abc.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:abc.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://pkg.www.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://pkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:www.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://bpkg.www.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://bpkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:www.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("https://git.github.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://git.github.com/test.git#master");
      assert (l.canonical_name () == "git:github.com/test");
    }
    {
      repository_location l (loc ("https://scm.github.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://scm.github.com/test.git#master");
      assert (l.canonical_name () == "git:github.com/test");
    }
    {
      repository_location l (loc ("https://www.github.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://www.github.com/test.git#master");
      assert (l.canonical_name () == "git:github.com/test");
    }
    {
      repository_location l (loc ("http://cppget.org/qw//1/a//b/"));
      assert (l.string () == "http://cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "bpkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://stable.cppget.org/1/"));
      assert (l.canonical_name () == "bpkg:stable.cppget.org");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../1/math", l1));
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../pkg/1/math", l1));
      assert (l2.string () == "http://stable.cppget.org/pkg/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("https://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../1/math", l1));
      assert (l2.string () == "https://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("math/..", l1));
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/misc");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc (".", l1));
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org/misc");
    }
    {
      repository_location l1 (loc ("http://www.stable.cppget.org:8080/1"));

      repository_location l2 (loc ("../1/math", l1));
      assert (l2.string () == "http://www.stable.cppget.org:8080/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org:8080/math");
      assert (l2.proto () == proto::http);
    }
    {
      repository_location l1 (loc ("https://www.stable.cppget.org:444/1"));
      repository_location l2 (loc ("../1/math", l1));
      assert (l2.string () == "https://www.stable.cppget.org:444/1/math");
      assert (l2.canonical_name () == "bpkg:stable.cppget.org:444/math");
      assert (l2.proto () == proto::https);
    }
#ifndef _WIN32
    {
      repository_location l1 (loc ("/var/r1/1/misc"));
      repository_location l2 (loc ("../../../r2/1/math", l1));
      assert (l2.string () == "/var/r2/1/math");
      assert (l2.canonical_name () == "bpkg:/var/r2/math");
    }
    {
      repository_location l1 (loc ("/var/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "/var/1/math");
      assert (l2.canonical_name () == "bpkg:/var/math");
    }
    {
      repository_location l1 (loc ("/var/1/stable"));
      repository_location l2 (loc ("/var/1/test", l1));
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "bpkg:/var/test");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("/var/1/test", l1));
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "bpkg:/var/test");
    }
    {
      repository_location l1 (loc ("/var/1/stable"));
      repository_location l2 (loc ("/var/1/stable", loc ()));
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
#else
    {
      repository_location l1 (loc ("c:/var/r1/1/misc"));
      repository_location l2 (loc ("../../../r2/1/math", l1));
      assert (l2.string () == "c:\\var\\r2\\1\\math");
      assert (l2.canonical_name () == "bpkg:c:\\var\\r2\\math");
    }
    {
      repository_location l1 (loc ("c:/var/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "c:\\var\\1\\math");
      assert (l2.canonical_name () == "bpkg:c:\\var\\math");
    }
    {
      repository_location l1 (loc ("c:/var/1/stable"));
      repository_location l2 (loc ("c:\\var\\1\\test", l1));
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "bpkg:c:\\var\\test");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("c:/var/1/test", l1));
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "bpkg:c:\\var\\test");
    }
    {
      repository_location l1 (loc ("c:/var/1/stable"));
      repository_location l2 (loc ("c:/var/1/stable", loc ()));
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
#endif
    {
      repository_location l1 (loc ("http://www.cppget.org/1/stable"));
      repository_location l2 (loc ("http://abc.com/1/test", l1));
      assert (l2.string () == "http://abc.com/1/test");
      assert (l2.canonical_name () == "bpkg:abc.com/test");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/"));
      repository_location l2 (loc ("http://stable.cppget.org/1/", loc ()));
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }

    // Test valid web interface locations.
    //
    {
      repository_location l (loc ("http://cppget.org/1/misc"));
      assert (effective_url ("http://cppget.org/pkg", l) ==
              "http://cppget.org/pkg");
    }
    {
      repository_location l (loc ("http://cppget.org/1/misc"));
      assert (effective_url ("https://cppget.org/pkg", l) ==
              "https://cppget.org/pkg");
    }
    {
      repository_location l (
        loc ("http://pkg.cppget.org/foo/pkg/1/misc/stable"));

      assert (effective_url ("./.", l) ==
              "http://pkg.cppget.org/foo/pkg/misc/stable");
    }
    {
      repository_location l (loc ("http://cppget.org/foo/1/misc/stable"));
      assert (effective_url ("./.", l) ==
              "http://cppget.org/foo/misc/stable");
    }
    {
      repository_location l (
        loc ("http://pkg.cppget.org/foo/pkg/1/misc/stable"));

      assert (effective_url ("././..", l) ==
              "http://pkg.cppget.org/foo/pkg/misc");
    }
    {
      repository_location l (loc ("http://pkg.cppget.org/foo/pkg/1/misc"));
      assert (effective_url ("././../../..", l) == "http://pkg.cppget.org");
    }
    {
      repository_location l (
        loc ("https://pkg.cppget.org/foo/pkg/1/misc/stable"));

      assert (effective_url ("../.", l) ==
              "https://cppget.org/foo/pkg/misc/stable");
    }
    {
      repository_location l (
        loc ("https://pkg.cppget.org/foo/pkg/1/misc/stable"));

      assert (effective_url (".././..", l) ==
              "https://cppget.org/foo/pkg/misc");
    }
    {
      repository_location l (
        loc ("https://bpkg.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("./..", l) ==
              "https://bpkg.cppget.org/foo/misc/stable");
    }
    {
      repository_location l (
        loc ("https://bpkg.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("./../..", l) ==
              "https://bpkg.cppget.org/foo/misc");
    }
    {
      repository_location l (
        loc ("http://www.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("../..", l) ==
              "http://cppget.org/foo/misc/stable");
    }
    {
      repository_location l (
        loc ("http://cppget.org/pkg/foo/1/misc/stable"));

      assert (effective_url ("../..", l) ==
              "http://cppget.org/pkg/foo/misc/stable");
    }
    {
      repository_location l (
        loc ("http://www.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("../../..", l) ==
              "http://cppget.org/foo/misc");
    }
    {
      repository_location l (loc ("http://pkg.cppget.org/foo/pkg/1/misc"));
      assert (effective_url ("../../../..", l) == "http://cppget.org");
    }
    {
      repository_location l (
        loc ("http://www.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("../../../abc", l) ==
              "http://cppget.org/foo/misc/abc");
    }

    // repository_url
    //
    assert (repository_url ("git://example.com/test.git") ==
            repository_url (proto::git,
                            repository_url::host_type ("example.com"),
                            dir_path ("test.git")));

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
