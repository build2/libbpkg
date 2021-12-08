// file      : libbpkg/buildfile-scanner.txx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <cassert>

#include <libbutl/optional.hxx>

namespace bpkg
{
  template <typename V, std::size_t N>
  typename buildfile_scanner<V, N>::xchar buildfile_scanner<V, N>::
  peek ()
  {
    xchar c (scan_.peek (ebuf_));

    if (scanner::invalid (c))
      throw buildfile_scanning (name_, scan_.line, scan_.column, ebuf_);

    return c;
  }

  template <typename V, std::size_t N>
  char buildfile_scanner<V, N>::
  scan_line (std::string& l, char stop)
  {
    using namespace std;

    auto fail = [this] (const string& d)
    {
      throw buildfile_scanning (name_, scan_.line, scan_.column, d);
    };

    xchar c (peek ());

    auto next = [&l, &c, this] ()
    {
      l += c;
      scan_.get (c);
    };

    butl::optional<char> r;
    bool double_quoted (false);

    for (;
         !scanner::eos (c) && (double_quoted || (c != '\n' && c != stop));
         c = peek ())
    {
      switch (c)
      {
      case '\"':
        {
          // Start or finish scanning the double-quoted sequence.
          //
          double_quoted = !double_quoted;

          r = '\0';
          break;
        }
      case '\\':
        {
          next ();

          c = peek ();

          if (scanner::eos (c))
            fail (double_quoted
                  ? "unterminated double-quoted sequence"
                  : "unterminated escape sequence");

          r = '\0';
          break;
        }
      case '(':
        {
          next ();

          scan_line (l, ')');

          c = peek ();

          if (c != ')')
            fail ("unterminated evaluation context");

          next ();

          r = '\0';
          continue;
        }
      case '\'':
        {
          if (!double_quoted)
          {
            next ();

            for (;;)
            {
              c = peek ();

              if (scanner::eos (c))
                fail ("unterminated single-quoted sequence");

              next ();

              if (c == '\'')
                break;
            }

            r = '\0';
            continue;
          }

          break;
        }
      case '#':
        {
          if (!double_quoted)
          {
            next ();

            // See if this is a multi-line comment in the form:
            //
            /*
              #\
              ...
              #\
            */
            auto ml = [&c, &next, this] () -> bool
            {
              if ((c = peek ()) == '\\')
              {
                next ();

                if ((c = peek ()) == '\n' || scanner::eos (c))
                  return true;
              }

              return false;
            };

            if (ml ())
            {
              // Scan until we see the closing one.
              //
              for (;;)
              {
                if (c == '#' && ml ())
                  break;

                if (scanner::eos (c = peek ()))
                  fail ("unterminated multi-line comment");

                next ();
              }
            }
            else
            {
              // Read until newline or eos.
              //
              for (; !scanner::eos (c) && c != '\n'; c = peek ())
                next ();
            }

            continue;
          }

          break;
        }
      case '{':
      case '}':
        {
          if (!double_quoted)
            r = !r ? static_cast<char> (c) : '\0';

          break;
        }
      default:
        {
          if (!double_quoted && c != ' ' && c != '\t')
            r = '\0';

          break;
        }
      }

      next ();
    }

    if (double_quoted)
      fail ("unterminated double-quoted sequence");

    return r ? *r : '\0';
  }

  template <typename V, std::size_t N>
  std::string buildfile_scanner<V, N>::
  scan_line (char stop)
  {
    std::string r;
    scan_line (r, stop);
    return r;
  }

  template <typename V, std::size_t N>
  std::string buildfile_scanner<V, N>::
  scan_eval ()
  {
    std::string r;
    scan_line (r, ')');

    if (peek () != ')')
      throw buildfile_scanning (name_,
                                scan_.line,
                                scan_.column,
                                "unterminated evaluation context");

    return r;
  }

  template <typename V, std::size_t N>
  std::string buildfile_scanner<V, N>::
  scan_block ()
  {
    using namespace std;

    auto fail = [this] (const string& d)
    {
      throw buildfile_scanning (name_, scan_.line, scan_.column, d);
    };

    string r;
    for (size_t level (0);; )
    {
      if (scanner::eos (peek ()))
        fail ("unterminated buildfile block");

      size_t n (r.size ());
      char bc (scan_line (r));

      xchar c (peek ());

      // Append the newline unless this is eos.
      //
      if (c == '\n')
      {
        r += c;
        scan_.get (c);
      }
      else
        assert (scanner::eos (c));

      if (bc == '{')
      {
        ++level;
      }
      else if (bc == '}')
      {
        // If this is the fragment terminating line, then strip it from the
        // fragment and bail out.
        //
        if (level == 0)
        {
          r.resize (n);
          break;
        }
        else
          --level;
      }
    }

    return r;
  }
}
