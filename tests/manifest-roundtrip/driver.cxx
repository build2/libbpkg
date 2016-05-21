// file      : tests/manifest-roundtrip/driver.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <cassert>
#include <fstream>
#include <iostream>

#include <butl/fdstream> // stdout_mode()

#include <bpkg/manifest-parser>
#include <bpkg/manifest-serializer>

using namespace std;
using namespace butl;
using namespace bpkg;

int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    cerr << "usage: " << argv[0] << " <file>" << endl;
    return 1;
  }

  try
  {
    ifstream ifs;
    ifs.exceptions (ifstream::badbit | ifstream::failbit);
    ifs.open (argv[1]);

    manifest_parser p (ifs, argv[1]);

    stdout_fdmode (fdtranslate::binary); // Write in binary mode.
    manifest_serializer s (cout, "stdout");

    for (bool eom (true), eos (false); !eos; )
    {
      manifest_name_value nv (p.next ());

      if (nv.empty ()) // End pair.
      {
        eos = eom;
        eom = true;
      }
      else
        eom = false;

      s.next (nv.name, nv.value);
    }
  }
  catch (const ios_base::failure&)
  {
    cerr << "io failure" << endl;
    return 1;
  }
  catch (const exception& e)
  {
    cerr << e.what () << endl;
    return 1;
  }
}
