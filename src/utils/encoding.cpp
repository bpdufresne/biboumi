#include <utils/encoding.hpp>

#include <utils/scopeguard.hpp>

#include <stdexcept>

#include <cassert>
#include <string.h>
#include <iconv.h>
#include <cerrno>

#include <map>
#include <bitset>

/**
 * The UTF-8-encoded character used as a place holder when a character conversion fails.
 * This is U+FFFD � "replacement character"
 */
static const char* invalid_char = "\xef\xbf\xbd";
static const size_t invalid_char_len = 3;

namespace utils
{
  /**
   * Based on http://en.wikipedia.org/wiki/UTF-8#Description
   */
  std::size_t get_next_codepoint_size(const unsigned char c)
  {
    if ((c & 0b11111000) == 0b11110000)          // 4 bytes:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
      return 4;
    else if ((c & 0b11110000) == 0b11100000)     // 3 bytes:  1110xxx 10xxxxxx 10xxxxxx
      return 3;
    else if ((c & 0b11100000) == 0b11000000)     // 2 bytes:  110xxxxx 10xxxxxx
      return 2;
    return 1;                                    // 1 byte:  0xxxxxxx
  }

  bool is_valid_utf8(const char* s)
  {
    if (!s)
      return false;

    const unsigned char* str = reinterpret_cast<const unsigned char*>(s);

    while (*str)
      {
        const auto codepoint_size = get_next_codepoint_size(str[0]);
        if (codepoint_size == 4)
          {
            if (!str[1] || !str[2] || !str[3]
                || ((str[1] & 0b11000000u) != 0b10000000u)
                || ((str[2] & 0b11000000u) != 0b10000000u)
                || ((str[3] & 0b11000000u) != 0b10000000u))
              return false;
          }
        else if (codepoint_size == 3)
          {
            if (!str[1] || !str[2]
                || ((str[1] & 0b11000000u) != 0b10000000u)
                || ((str[2] & 0b11000000u) != 0b10000000u))
              return false;
          }
        else if (codepoint_size == 2)
          {
            if (!str[1] ||
                ((str[1] & 0b11000000) != 0b10000000))
              return false;
          }
        else if ((str[0] & 0b10000000) != 0)
          return false;
        str += codepoint_size;
      }
    return true;
  }

  std::string remove_invalid_xml_chars(const std::string& original)
  {
    // The given string MUST be a valid utf-8 string
    std::vector<char> res(original.size(), '\0');

    // pointer where we write valid chars
    char* r = res.data();

    const unsigned char* str = reinterpret_cast<const unsigned char*>(original.c_str());
    std::bitset<20> codepoint;

    while (*str)
      {
        // 4 bytes:  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if ((str[0] & 0b11111000) == 0b11110000)
          {
            codepoint  = ((str[0] & 0b00000111u) << 18u);
            codepoint |= ((str[1] & 0b00111111u) << 12u);
            codepoint |= ((str[2] & 0b00111111u) << 6u );
            codepoint |= ((str[3] & 0b00111111u) << 0u );
            if (codepoint.to_ulong() <= 0x10FFFF)
              {
                ::memcpy(r, str, 4);
                r += 4;
              }
            str += 4;
          }
        // 3 bytes:  1110xxx 10xxxxxx 10xxxxxx
        else if ((str[0] & 0b11110000) == 0b11100000)
          {
            codepoint  = ((str[0] & 0b00001111u) << 12u);
            codepoint |= ((str[1] & 0b00111111u) << 6u);
            codepoint |= ((str[2] & 0b00111111u) << 0u );
            if (codepoint.to_ulong() <= 0xD7FF ||
                (codepoint.to_ulong() >= 0xE000 && codepoint.to_ulong() <= 0xFFFD))
              {
                ::memcpy(r, str, 3);
                r += 3;
              }
            str += 3;
          }
        // 2 bytes:  110xxxxx 10xxxxxx
        else if (((str[0]) & 0b11100000) == 0b11000000)
          {
            // All 2 bytes char are valid, don't even bother calculating
            // the codepoint
            ::memcpy(r, str, 2);
            r += 2;
            str += 2;
          }
        // 1 byte:  0xxxxxxx
        else if ((str[0] & 0b10000000) == 0)
          {
            codepoint = ((str[0] & 0b01111111));
            if (codepoint.to_ulong() == 0x09 ||
                codepoint.to_ulong() == 0x0A ||
                codepoint.to_ulong() == 0x0D ||
                codepoint.to_ulong() >= 0x20)
              {
                ::memcpy(r, str, 1);
                r += 1;
              }
            str += 1;
          }
        else
          throw std::runtime_error("Invalid UTF-8 passed to remove_invalid_xml_chars");
      }
    return {res.data(), static_cast<size_t>(r - res.data())};
  }

  std::string convert_to_utf8(const std::string& str, const char* charset)
  {
    std::string res;

    const iconv_t cd = iconv_open("UTF-8", charset);
    if (cd == (iconv_t)-1)
      throw std::runtime_error("Cannot convert into UTF-8");

    // Make sure cd is always closed when we leave this function
    const auto sg = utils::make_scope_guard([&cd](){ iconv_close(cd); });

    size_t inbytesleft = str.size();

    // iconv will not attempt to modify this buffer, but some plateform
    // require a char** anyway
#ifdef ICONV_SECOND_ARGUMENT_IS_CONST
    const char* inbuf_ptr = str.c_str();
#else
    char* inbuf_ptr = const_cast<char*>(str.c_str());
#endif

    size_t outbytesleft = str.size() * 4;
    char* outbuf = new char[outbytesleft];
    char* outbuf_ptr = outbuf;

    // Make sure outbuf is always deleted when we leave this function
    const auto sg2 = utils::make_scope_guard([outbuf](){ delete[] outbuf; });

    bool done = false;
    while (done == false)
      {
        size_t error = iconv(cd, &inbuf_ptr, &inbytesleft, &outbuf_ptr, &outbytesleft);
        if ((size_t)-1 == error)
          {
            switch (errno)
              {
              case EILSEQ:
                // Invalid byte found. Insert a placeholder instead of the
                // converted character, jump one byte and continue
                memcpy(outbuf_ptr, invalid_char, invalid_char_len);
                outbuf_ptr += invalid_char_len;
                inbytesleft--;
                inbuf_ptr++;
                break;
              case EINVAL:
                // A multibyte sequence is not terminated, but we can't
                // provide any more data, so we just add a placeholder to
                // indicate that the character is not properly converted,
                // and we stop the conversion
                memcpy(outbuf_ptr, invalid_char, invalid_char_len);
                outbuf_ptr += invalid_char_len;
                outbuf_ptr++;
                done = true;
                break;
              case E2BIG:  // This should never happen
              default:     // This should happen even neverer
                done = true;
                break;
              }
          }
        else
          {
            // The conversion finished without any error, stop converting
            done = true;
          }
      }
    // Terminate the converted buffer, and copy that buffer it into the
    // string we return
    *outbuf_ptr = '\0';
    res = outbuf;
    return res;
  }

}

namespace xep0106
{
  static const std::map<const char, const std::string> encode_map = {
    {' ', "\\20"},
    {'"', "\\22"},
    {'&', "\\26"},
    {'\'',"\\27"},
    {'/', "\\2f"},
    {':', "\\3a"},
    {'<', "\\3c"},
    {'>', "\\3e"},
    {'@', "\\40"},
  };

  void decode(std::string& s)
  {
    std::string::size_type pos;
    for (const auto& pair: encode_map)
      while ((pos = s.find(pair.second)) != std::string::npos)
        s.replace(pos, pair.second.size(),
                  1, pair.first);
  }

  void encode(std::string& s)
  {
    std::string::size_type pos;
    while ((pos = s.find_first_of(" \"&'/:<>@")) != std::string::npos)
      {
        auto it = encode_map.find(s[pos]);
        assert(it != encode_map.end());
        s.replace(pos, 1, it->second);
      }
  }
}
