// file      : tests/repository-location/driver.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <string>
#include <sstream>
#include <iostream>
#include <stdexcept> // invalid_argument, logic_error

#include <libbutl/optional.hxx>
#include <libbutl/manifest-parser.hxx>

#include <libbpkg/manifest.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace butl;

namespace bpkg
{
  using butl::optional;
  using butl::nullopt;

  inline static repository_location
  loc ()
  {
    return repository_location ();
  }

  inline static repository_location
  loc (const string& l, repository_type t = repository_type::pkg)
  {
    return repository_location (repository_url (l), t);
  }

  inline static repository_location
  loc (const string& l,
       const repository_location& b,
       repository_type t = repository_type::pkg)
  {
    return repository_location (repository_url (l), t, b);
  }

  inline static bool
  bad_loc (const string& l, repository_type t = repository_type::pkg)
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

  inline static repository_location
  typed_loc (const string& u, optional<repository_type> t = nullopt)
  {
    return repository_location (u, t);
  }

  inline static bool
  bad_typed_loc (const string& u, optional<repository_type> t = nullopt)
  {
    try
    {
      repository_location bl (u, t);
      return false;
    }
    catch (const invalid_argument&)
    {
      return true;
    }
  }

  inline static bool
  bad_loc (const string& l,
           const repository_location& b,
           repository_type t = repository_type::pkg)
  {
    try
    {
      repository_location bl (repository_url (l), t, b);
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
    repository_manifest m (pkg_repository_manifest (mp));

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

  inline static bool
  operator== (const git_ref_filter& x, const git_ref_filter& y)
  {
    return x.commit == y.commit && x.name == y.name &&
           x.exclusion == y.exclusion;
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

    // Invalid URL fragment.
    //
    assert (bad_loc ("https://www.example.com/test.git#",
                     repository_type::git));

    assert (bad_loc ("https://www.example.com/test.git#,",
                     repository_type::git));

    assert (bad_loc ("https://www.example.com/test.git#@",
                     repository_type::git));

    assert (bad_loc ("https://www.example.com/test.git#@123",
                     repository_type::git));

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

    // Can't be remote.
    //
    assert (bad_loc ("http://example.com/dir", repository_type::dir));

    // Invalid typed repository location.
    //
    assert (bad_typed_loc (""));                            // Empty.
    assert (bad_typed_loc ("abc+http://example.com/repo")); // Relative.

    assert (bad_typed_loc ("git+http://example.com/repo",   // Types mismatch.
                           repository_type::pkg));

    assert (bad_typed_loc ("http://example.com/repo"));     // Invalid for pkg.

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
      repository_location l (repository_url (), repository_type::pkg);
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
      assert (l.canonical_name () == "pkg:/aa/bb");
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("/pkg/1/aa/bb", loc ()));
      assert (l.string () == "/pkg/1/aa/bb");
      assert (l.canonical_name () == "pkg:aa/bb");
    }
    {
      repository_location l (loc ("/var/bpkg/1", loc ()));
      assert (l.string () == "/var/bpkg/1");
      assert (l.canonical_name () == "pkg:/var/bpkg");
    }
    {
      repository_location l (loc ("/1", loc ()));
      assert (l.string () == "/1");
      assert (l.canonical_name () == "pkg:/");
    }
    {
      repository_location l (loc ("/var/pkg/1/example.org/math/testing",
                                  loc ()));
      assert (l.string () == "/var/pkg/1/example.org/math/testing");
      assert (l.canonical_name () == "pkg:example.org/math/testing");
    }
    {
      repository_location l (loc ("/var/pkg/example.org/1/math/testing",
                                  loc ()));
      assert (l.string () == "/var/pkg/example.org/1/math/testing");
      assert (l.canonical_name () == "pkg:/var/pkg/example.org/math/testing");
    }
    {
      repository_location l (loc ("/a/b/../c/1/aa/../bb", loc ()));
      assert (l.string () == "/a/c/1/bb");
      assert (l.canonical_name () == "pkg:/a/c/bb");
    }
    {
      repository_location l (loc ("/a/b/../c/pkg/1/aa/../bb", loc ()));
      assert (l.string () == "/a/c/pkg/1/bb");
      assert (l.canonical_name () == "pkg:bb");
    }
    {
      repository_location l (loc ("file:///repo/1/path"));
      assert (l.url () == loc ("file:/repo/1/path").url ());
      assert (l.url () == loc ("/repo/1/path").url ());
      assert (l.string () == "/repo/1/path");
      assert (l.canonical_name () == "pkg:/repo/path");
    }
    {
      repository_location l (loc ("file:/git/repo#branch",
                                  repository_type::git));
      assert (l.string () == "git+file:/git/repo#branch");
      assert (l.canonical_name () == "git:/git/repo#branch");
    }
    {
      repository_location l (loc ("/git/repo#branch", repository_type::git));
      assert (l.string () == "git+file:/git/repo#branch");
      assert (l.canonical_name () == "git:/git/repo#branch");
    }
    {
      repository_location l (loc ("file://localhost/", repository_type::git));
      assert (l.string () == "git+file:///");
      assert (l.canonical_name () == "git:/");
    }
    {
      repository_location l (loc ("file://localhost/#master",
                                  repository_type::git));
      assert (l.string () == "git+file:/#master");
      assert (l.canonical_name () == "git:/#master");
    }
    {
      repository_location l (loc ("/home/user/repo", repository_type::dir));
      assert (l.string () == "dir+file:///home/user/repo");
      assert (l.canonical_name () == "dir:/home/user/repo");
    }
#else
    {
      repository_location l (loc ("c:\\1\\aa\\bb", loc ()));
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "pkg:c:\\aa\\bb");
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("c:/1/aa/bb", loc ()));
      assert (l.string () == "c:\\1\\aa\\bb");
      assert (l.canonical_name () == "pkg:c:\\aa\\bb");
    }
    {
      repository_location l (loc ("c:\\pkg\\1\\aa\\bb", loc ()));
      assert (l.string () == "c:\\pkg\\1\\aa\\bb");
      assert (l.canonical_name () == "pkg:aa/bb");
    }
    {
      repository_location l (loc ("c:\\var\\pkg\\1\\example.org\\math\\tst",
                                  loc ()));
      assert (l.string () == "c:\\var\\pkg\\1\\example.org\\math\\tst");
      assert (l.canonical_name () == "pkg:example.org/math/tst");
    }
    {
      repository_location l (loc ("c:\\var\\pkg\\example.org\\1\\math\\tst",
                                  loc ()));
      assert (l.string () == "c:\\var\\pkg\\example.org\\1\\math\\tst");

      assert (l.canonical_name () ==
              "pkg:c:\\var\\pkg\\example.org\\math\\tst");
    }
    {
      repository_location l (loc ("c:/a/b/../c/1/aa/../bb", loc ()));

      assert (l.string () == "c:\\a\\c\\1\\bb");
      assert (l.canonical_name () == "pkg:c:\\a\\c\\bb");
    }
    {
      repository_location l (loc ("c:/a/b/../c/pkg/1/aa/../bb", loc ()));
      assert (l.string () == "c:\\a\\c\\pkg\\1\\bb");
      assert (l.canonical_name () == "pkg:bb");
    }
    {
      repository_location l (loc ("file:///c:/repo/1/path"));
      assert (l.url () == loc ("file:/c:/repo/1/path").url ());
      assert (l.url () == loc ("c:/repo/1/path").url ());
      assert (l.string () == "c:\\repo\\1\\path");
      assert (l.canonical_name () == "pkg:c:\\repo\\path");
    }
    {
      repository_location l (loc ("file:/c:/git/repo#branch",
                                  repository_type::git));
      assert (l.string () == "git+file:/c:/git/repo#branch");
      assert (l.canonical_name () == "git:c:\\git\\repo#branch");
    }
    {
      repository_location l (loc ("c:\\git\\repo#branch",
                                  repository_type::git));
      assert (l.string () == "git+file:/c:/git/repo#branch");
      assert (l.canonical_name () == "git:c:\\git\\repo#branch");
    }
    {
      repository_location l (loc ("file://localhost/c:/",
                                  repository_type::git));
      assert (l.string () == "git+file:///c:");
      assert (l.canonical_name () == "git:c:");
    }
    {
      repository_location l (loc ("file://localhost/c:/#master",
                                  repository_type::git));
      assert (l.string () == "git+file:/c:#master");
      assert (l.canonical_name () == "git:c:#master");
    }
    {
      repository_location l (loc ("c:\\user\\repo", repository_type::dir));
      assert (l.string () == "dir+file:///c:/user/repo");
      assert (l.canonical_name () == "dir:c:\\user\\repo");
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
      assert (l.canonical_name () == "pkg:a.com/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("https://www.a.com:443/1/aa/bb"));
      assert (l.string () == "https://www.a.com:443/1/aa/bb");
      assert (l.canonical_name () == "pkg:a.com/aa/bb");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/dd/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/dd/1/aa/bb");
      assert (l.canonical_name () == "pkg:a.com:8080/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/dd/pkg/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/dd/pkg/1/aa/bb");
      assert (l.canonical_name () == "pkg:a.com:8080/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("http://www.a.com:8080/bpkg/dd/1/aa/bb"));
      assert (l.string () == "http://www.a.com:8080/bpkg/dd/1/aa/bb");
      assert (l.canonical_name () == "pkg:a.com:8080/bpkg/dd/aa/bb");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("https://www.a.com:444/dd/1/aa/bb"));
      assert (l.string () == "https://www.a.com:444/dd/1/aa/bb");
      assert (l.canonical_name () == "pkg:a.com:444/dd/aa/bb");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::pkg);
    }
    {
      repository_location l (loc ("https://www.example.com/test.git",
                                  repository_type::git));
      assert (l.string () == "https://www.example.com/test.git");
      assert (l.canonical_name () == "git:example.com/test");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("git://example.com/test#master",
                                  repository_type::git));
      assert (l.string () == "git://example.com/test#master");
      assert (l.canonical_name () == "git:example.com/test#master");
      assert (l.proto () == proto::git);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("ssh://example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "ssh://example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
      assert (l.proto () == proto::ssh);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("http://example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "http://example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
      assert (l.proto () == proto::http);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("https://example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
      assert (l.proto () == proto::https);
      assert (l.type () == repository_type::git);
    }
    {
      repository_location l (loc ("http://git.example.com#master",
                                  repository_type::git));
      assert (l.string () == "git+http://git.example.com/#master");
      assert (l.canonical_name () == "git:example.com#master");
    }
    {
      repository_url u ("http://git.example.com/a/#master");
      *u.path /= path ("..");

      repository_location l (u, repository_type::git);
      assert (l.string () == "git+http://git.example.com/#master");
      assert (l.canonical_name () == "git:example.com#master");
    }
    {
      repository_location l (loc ("http://a.com/a/b/../c/1/aa/../bb"));
      assert (l.string () == "http://a.com/a/c/1/bb");
      assert (l.canonical_name () == "pkg:a.com/a/c/bb");
    }
    {
      repository_location l (loc ("https://a.com/a/b/../c/1/aa/../bb"));
      assert (l.string () == "https://a.com/a/c/1/bb");
      assert (l.canonical_name () == "pkg:a.com/a/c/bb");
    }
    {
      repository_location l (loc ("http://www.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://00.00.010.0/qw/1/a/b/"));
      assert (l.string () == "http://0.0.10.0/qw/1/a/b");
      assert (l.canonical_name () == "pkg:0.0.10.0/qw/a/b");
    }
    {
      repository_location l (loc ("http://pkg.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://pkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://bpkg.CPPget.org/qw/1/a/b/"));
      assert (l.string () == "http://bpkg.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://abc.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://abc.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:abc.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://pkg.www.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://pkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:www.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://bpkg.www.cppget.org/qw/1/a/b/"));
      assert (l.string () == "http://bpkg.www.cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:www.cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("https://git.example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://git.example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
    }
    {
      repository_location l (loc ("https://scm.example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://scm.example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
    }
    {
      repository_location l (loc ("https://www.example.com/test.git#master",
                                  repository_type::git));
      assert (l.string () == "https://www.example.com/test.git#master");
      assert (l.canonical_name () == "git:example.com/test#master");
    }
    {
      repository_location l (loc ("http://cppget.org/qw//1/a//b/"));
      assert (l.string () == "http://cppget.org/qw/1/a/b");
      assert (l.canonical_name () == "pkg:cppget.org/qw/a/b");
    }
    {
      repository_location l (loc ("http://stable.cppget.org/1/"));
      assert (l.canonical_name () == "pkg:stable.cppget.org");
    }
    {
      repository_location l (typed_loc ("git+http://example.com/repo"));
      assert (l.string () == "git+http://example.com/repo");
    }
    {
      repository_location l (typed_loc ("http://example.com/repo.git"));
      assert (l.string () == "http://example.com/repo.git");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../1/math", l1));
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../pkg/1/math", l1));
      assert (l2.string () == "http://stable.cppget.org/pkg/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("https://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../../1/math", l1));
      assert (l2.string () == "https://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "http://stable.cppget.org/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/math");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("math/..", l1));
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/misc");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc (".", l1));
      assert (l2.string () == "http://stable.cppget.org/1/misc");
      assert (l2.canonical_name () == "pkg:stable.cppget.org/misc");
    }
    {
      repository_location l1 (loc ("http://www.stable.cppget.org:8080/1"));

      repository_location l2 (loc ("../1/math", l1));
      assert (l2.string () == "http://www.stable.cppget.org:8080/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org:8080/math");
      assert (l2.proto () == proto::http);
    }
    {
      repository_location l1 (loc ("https://www.stable.cppget.org:444/1"));
      repository_location l2 (loc ("../1/math", l1));
      assert (l2.string () == "https://www.stable.cppget.org:444/1/math");
      assert (l2.canonical_name () == "pkg:stable.cppget.org:444/math");
      assert (l2.proto () == proto::https);
    }
    {
      repository_location l (loc ("../test.git#master",
                                  repository_location (),
                                  repository_type::git));
      assert (l.string () == "../test.git#master");
      assert (l.canonical_name ().empty ());
      assert (l.proto () == proto::file);
    }
    {
      repository_location l1 (loc ("https://example.com/stable.git#stable",
                                   repository_type::git));
      repository_location l2 (loc ("../test.git#master",
                                   l1,
                                   repository_type::git));
      assert (l2.string () == "https://example.com/test.git#master");
      assert (l2.canonical_name () == "git:example.com/test#master");
      assert (l2.proto () == proto::https);
    }

    {
      repository_location l (loc ("http:repo/1/path", loc ()));
      assert (l.string () == "http:repo/1/path");
      assert (l.canonical_name ().empty ());
      assert (l.proto () == proto::file);
    }

#ifndef _WIN32
    {
      repository_location l1 (loc ("/var/r1/1/misc"));
      repository_location l2 (loc ("../../../r2/1/math", l1));
      assert (l2.string () == "/var/r2/1/math");
      assert (l2.canonical_name () == "pkg:/var/r2/math");
    }
    {
      repository_location l1 (loc ("/var/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "/var/1/math");
      assert (l2.canonical_name () == "pkg:/var/math");
    }
    {
      repository_location l1 (loc ("/var/1/stable"));
      repository_location l2 (loc ("/var/1/test", l1));
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "pkg:/var/test");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("/var/1/test", l1));
      assert (l2.string () == "/var/1/test");
      assert (l2.canonical_name () == "pkg:/var/test");
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
      assert (l2.canonical_name () == "pkg:c:\\var\\r2\\math");
    }
    {
      repository_location l1 (loc ("c:/var/1/misc"));
      repository_location l2 (loc ("../math", l1));
      assert (l2.string () == "c:\\var\\1\\math");
      assert (l2.canonical_name () == "pkg:c:\\var\\math");
    }
    {
      repository_location l1 (loc ("c:/var/1/stable"));
      repository_location l2 (loc ("c:\\var\\1\\test", l1));
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "pkg:c:\\var\\test");
    }
    {
      repository_location l1 (loc ("http://stable.cppget.org/1/misc"));
      repository_location l2 (loc ("c:/var/1/test", l1));
      assert (l2.string () == "c:\\var\\1\\test");
      assert (l2.canonical_name () == "pkg:c:\\var\\test");
    }
    {
      repository_location l1 (loc ("c:/var/1/stable"));
      repository_location l2 (loc ("c:/var/1/stable", loc ()));
      assert (l1.string () == l2.string ());
      assert (l1.canonical_name () == l2.canonical_name ());
    }
    {
      repository_location l1 (loc ("c:/var/pkg/1/misc"));
      repository_location l2 (loc ("c:/var/Pkg/1/Misc"));
      assert (l1.canonical_name () == "pkg:misc");
      assert (l2.canonical_name () == l1.canonical_name ());
    }
    {
      repository_location l1 (loc ("c:\\repo.git", repository_type::git));
      repository_location l2 (loc ("C:/Repo.Git", repository_type::git));
      assert (l1.canonical_name () == "git:c:\\repo");
      assert (l2.canonical_name () == l1.canonical_name ());
    }
#endif
    {
      repository_location l1 (loc ("http://www.cppget.org/1/stable"));
      repository_location l2 (loc ("http://abc.com/1/test", l1));
      assert (l2.string () == "http://abc.com/1/test");
      assert (l2.canonical_name () == "pkg:abc.com/test");
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
      repository_location l (loc ("http://cppget.org/pkg/foo/1/misc/stable"));

      assert (effective_url ("../..", l) ==
              "http://cppget.org/pkg/foo/misc/stable");
    }
    {
      repository_location l (loc ("https://git.example.com/test.git#master",
                                  repository_type::git));
      assert (effective_url ("../..", l) == "../..");
    }
    {
      repository_location l (
        loc ("http://www.cppget.org/foo/bpkg/1/misc/stable"));

      assert (effective_url ("../../..", l) == "http://cppget.org/foo/misc");
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

    // Repository URL fragments.
    //
    {
      string n ("master");
      string c ("0a53e9ddeaddad63ad106860237bbf53411d11a7");

      assert (git_ref_filter () == git_ref_filter (nullopt, nullopt, false));
      assert (git_ref_filter (n) == git_ref_filter (n, nullopt, false));
      assert (git_ref_filter ('+' + n) == git_ref_filter (n, nullopt, false));
      assert (git_ref_filter ('-' + n) == git_ref_filter (n, nullopt, true));
      assert (git_ref_filter (c + '@') == git_ref_filter (c, nullopt, false));
      assert (git_ref_filter (c) == git_ref_filter (nullopt, c, false));
      assert (git_ref_filter ('@' + c) == git_ref_filter (nullopt, c, false));
      assert (git_ref_filter (n + '@' + c) == git_ref_filter (n, c, false));

      assert (parse_git_ref_filters (nullopt) ==
              git_ref_filters {git_ref_filter ()});

      assert (parse_git_ref_filters (string ("tag")) ==
              git_ref_filters ({git_ref_filter ("tag")}));

      assert (parse_git_ref_filters (string ("#tag")) ==
              git_ref_filters ({git_ref_filter (), git_ref_filter ("tag")}));

      assert (parse_git_ref_filters (string ("a,b")) ==
              git_ref_filters ({git_ref_filter ("a"), git_ref_filter ("b")}));
    }

    // repository_url
    //
    assert (repository_url ("git://example.com/test.git") ==
            repository_url (proto::git,
                            repository_url::host_type ("example.com"),
                            dir_path ("test.git")));

    // For an empty URL object all components are absent.
    //
    {
      repository_url u;
      assert (u.empty () &&
              !u.authority && !u.path && !u.query && !u.fragment);
    }

    // Absent path.
    //
    assert (repository_url ("git://example.com").string () ==
            "git://example.com/");

    // Empty path.
    //
    assert (repository_url ("git://example.com/").string () ==
            "git://example.com/");

    // Normalized path.
    //
    assert (repository_url ("git://example.com/a/..").string () ==
            "git://example.com/");

    // No trailing slash.
    //
    assert (repository_url ("git://example.com/a/").string () ==
            "git://example.com/a");

    assert (repository_url ("a/").string () == "a");

#ifndef _WIN32
    assert (repository_url ("/a/").string () == "/a");
#else
    assert (repository_url ("c:/a/").string () == "c:\\a");
#endif

    return 0;
  }
}

int
main (int argc, char* argv[])
{
  return bpkg::main (argc, argv);
}
