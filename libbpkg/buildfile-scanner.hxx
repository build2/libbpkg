// file      : libbpkg/buildfile-scanner.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef LIBBPKG_BUILDFILE_SCANNER_HXX
#define LIBBPKG_BUILDFILE_SCANNER_HXX

#include <string>
#include <cstdint>   // uint64_t
#include <cstddef>   // size_t
#include <stdexcept> // runtime_error

#include <libbutl/char-scanner.hxx>

#include <libbpkg/export.hxx>

namespace bpkg
{
  // Scan buildfile fragments, respecting the single- and double-quoted
  // character sequences, backslash-escaping, comments, evaluation
  // contexts, and nested blocks.
  //
  class LIBBPKG_EXPORT buildfile_scanning: public std::runtime_error
  {
  public:
    buildfile_scanning (const std::string& name,
                        std::uint64_t line,
                        std::uint64_t column,
                        const std::string& description);

    std::string name;
    std::uint64_t line;
    std::uint64_t column;
    std::string description;
  };

  template <typename V, std::size_t N>
  class buildfile_scanner
  {
  public:
    // Note that name is stored by shallow reference.
    //
    buildfile_scanner (butl::char_scanner<V, N>& s, const std::string& name)
        : scan_ (s), name_ (name) {}

    // Scan the buildfile line and return the scanned fragment. Optionally,
    // specify an additional stop character. Leave the newline (or the stop
    // character) in the stream. Throw buildfile_scanning on error
    // (unterminated quoted sequence, etc).
    //
    std::string
    scan_line (char stop = '\0');

    // Scan the buildfile line until an unbalanced ')' character is encountered
    // and return the scanned fragment, leaving ')' in the stream. Throw
    // buildfile_scanning on error or if eos or newline is reached.
    //
    std::string
    scan_eval ();

    // Scan the buildfile block until an unbalanced block closing '}' character
    // is encountered and return the scanned fragment. Throw buildfile_scanning
    // on error or if eos is reached.
    //
    // Note that the block opening '{' and closing '}' characters are only
    // considered as such, if they are the only characters on the line besides
    // whitespaces and comments. Also note that the fragment terminating '}'
    // line is consumed from the stream but is not included into the fragment.
    //
    std::string
    scan_block ();

  private:
    using scanner = butl::char_scanner<V, N>;
    using xchar = typename scanner::xchar;

    xchar
    peek ();

    // Scan the buildfile line, saving the scanned characters into the
    // specified string, leaving newline and the stop character, if specified,
    // in the stream. Return '{' if this line is a block-opening curly brace,
    // '}' if it is a block-closing curly brace, and '\0' otherwise.
    //
    char
    scan_line (std::string& l, char stop = '\0');

  private:
    scanner&           scan_;
    const std::string& name_;
    std::string        ebuf_; // Error message buffer.
  };
}

#include <libbpkg/buildfile-scanner.txx>

#endif // LIBBPKG_BUILDFILE_SCANNER_HXX
