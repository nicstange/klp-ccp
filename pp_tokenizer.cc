/*
 * Copyright (C) 2019  SUSE Software Solutions Germany GmbH
 *
 * This file is part of klp-ccp.
 *
 * klp-ccp is free software: you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * klp-ccp is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klp-ccp. If not, see <https://www.gnu.org/licenses/>.
 */

#include <cassert>
#include <cerrno>
#include <system_error>
#include <limits>
#include "pp_tokenizer.hh"
#include "pp_except.hh"
#include "source_reader.hh"

using namespace klp::ccp;
using namespace klp::ccp::impl;

_pp_tokenizer::_pp_tokenizer()
  : _buf{}, _buf_it{_buf.cbegin()},
    _line_length{0}, _expect_qh_str{expect_qh_str::newline},
    _pending{0}, _queue{0}, _queue_locs{0},
    _next_queue_loc{0}, _queue_pos{0}
{}

_pp_tokenizer::~_pp_tokenizer() noexcept = default;

void _pp_tokenizer::init_state()
{
  // Pure virtual ->read_raw() can't be called from the base class
  // constructor. Provide this init_state() to be called from
  // the derived class' constructor.
  // As the _buf is still empty(), we need to have a non-zero char in
  // the "last" position to get _read_next_char_raw() going, otherwise
  // it would report an EOF.
  _queue[_queue_capacity - 1] = std::numeric_limits<char>::max();
  _queue[0] = _read_next_char();
  std::size_t i;
  for (i = 1; i < _queue_capacity && !_buf.empty(); ++i) {
    _queue_locs[i] = _next_queue_loc;
    _queue[i] = _read_next_char();
  }
  for (; i < _queue_capacity; ++i) {
    _queue_locs[i] = _queue_locs[i - 1];
    _queue[i] = 0;
  }
}

char _pp_tokenizer::_read_next_char_raw()
{
  if (_pending[0]) {
    const char c = _pending[0];
    _pending[0] = _pending[1];
    _pending[1] = 0;
    return c;
  }

  if (_buf_it == _buf.cend()) {
    // If an EOF has already been seen, don't invoke the source_reader
    // again. The additional, necessary condition that the last
    // character read is zero is needed to get things going at
    // initialization.
    if ((_queue[(_queue_pos + _queue_capacity - 1) & (_queue_capacity - 1)]
	 == 0)
	&& _buf.empty()) {
      return 0;
    }
    _buf = this->read_raw();
    _buf_it = _buf.cbegin();
    if (_buf.empty())
      return 0;
  }

  const char c = static_cast<char>(*_buf_it++);
  ++_line_length;

  if (c == '\n') {
    this->add_line(_line_length);
    _line_length = 0;
  }

  return c;
}

char _pp_tokenizer::_read_next_char()
{
  char c = _read_next_char_raw();
  _next_queue_loc += 1;

  // Collate back-to-back continuation lines and report the fact that
  // some appeared in the input to the upper layers.
  bool is_continuation_line = false;
  while (c == '\\') {
    const char next = _read_next_char_raw();
    assert(!_pending[0]);
    assert(!_pending[1]);
    if (!next) {
      this->report_warning("no newline after continuation", _next_queue_loc);
      return 0;
    }
    if (next != '\n') {
      if (!is_continuation_line) {
	_pending[0] = next;
	return c;
      } else {
	_pending[0] = c;
	_pending[1] = next;
	return 0;
      }
    } else {
      // As _next_queue_loc had been incremented once above, add only
      // one more in the first iteration.
      _next_queue_loc += 1 + is_continuation_line;
      is_continuation_line = true;
      c = _read_next_char_raw();
      if (c != '\\')
	_pending[0] = c;
    }
  }

  return !is_continuation_line ? c : 0;
}

void _pp_tokenizer::_advance_queue_to_next_char()
{
  const auto backfill_pos = _queue_pos;
  ++_queue_pos;
  _queue_pos &= _queue_capacity - 1;

  _queue_locs[backfill_pos] = _next_queue_loc;
  _queue[backfill_pos] = _read_next_char();
}

void _pp_tokenizer::_advance_queue_to_pos(const std::size_t new_queue_pos)
{
  const std::size_t n = (new_queue_pos - _queue_pos) & (_queue_capacity - 1);
  for (std::size_t i = 0; i < n; ++i)
    _advance_queue_to_next_char();
}

std::size_t _pp_tokenizer::_next_queued_non_continuation_pos(std::size_t pos)
  const noexcept
{
  pos &= _queue_capacity - 1;
  const unsigned char c = static_cast<unsigned char>(_queue[pos]);
  return (pos + (c == 0)) & (_queue_capacity - 1);
}

bool _pp_tokenizer::_at_eof() const noexcept
{
  return ((_queue[_queue_pos] |
	   _queue[(_queue_pos + 1) & (_queue_capacity - 1)])
	  == 0);
}

raw_pp_token _pp_tokenizer::
_tokenize_string(const range_in_file::loc_type begin_loc,
		 const char delim,
		 const bool delim_escapable,
		 const pp_token::type tok_type)
{
  std::string value;
  bool in_escape = false;

  _advance_queue_to_pos(_next_queued_non_continuation_pos(_queue_pos));
  char cur = _queue[_queue_pos];
  while(cur) {
    if (cur != delim || (delim_escapable && in_escape)) {
      if (in_escape) {
	in_escape = false;
      } else {
	in_escape = (cur == '\\');
      }
      value.push_back(cur);
      _advance_queue_to_pos(_next_queued_non_continuation_pos(_queue_pos + 1));
      cur = _queue[_queue_pos];
    } else {
      _advance_queue_to_next_char();
      return raw_pp_token(tok_type, value,
			  range_in_file(begin_loc, _queue_locs[_queue_pos]));
    }
  }

  throw this->report_fatal("encountered EOF while searching for end of string",
			   _queue_locs[_queue_pos]);
}

raw_pp_token _pp_tokenizer::_tokenize_punctuator()
{
  std::string value;
  const char cur = _queue[_queue_pos];
  const auto begin_loc = _queue_locs[_queue_pos];
  auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
  const char next = _queue[next_pos];
  auto consumed_queue_end = _queue_pos + 1;
  if (cur == '-' && next == '>') {
    value = "->";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '+' && next == '+') {
    value = "++";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '-' && next == '-') {
    value = "--";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '<' && next == '<') {
    const auto next_next_pos = _next_queued_non_continuation_pos(next_pos + 1);
    const char next_next = _queue[next_next_pos];
    if (next_next != '=') {
      value = "<<";
      consumed_queue_end = next_pos + 1;
    } else {
      value = "<<=";
      consumed_queue_end = next_next_pos + 1;
    }
  } else if (cur == '>' && next == '>') {
    const auto next_next_pos = _next_queued_non_continuation_pos(next_pos + 1);
    const char next_next = _queue[next_next_pos];
    if (next_next != '=') {
      value = ">>";
      consumed_queue_end = next_pos + 1;
    } else {
      value = ">>=";
      consumed_queue_end = next_next_pos + 1;
    }
  } else if (cur == '<' && next == '=') {
    value = "<=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '>' && next == '=') {
    value = ">=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '=' && next == '=') {
    value = "==";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '!' && next == '=') {
    value = "!=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '*' && next == '=') {
    value = "*=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '/' && next == '=') {
    value = "/=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '%' && next == '=') {
    value = "%=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '+' && next == '=') {
    value = "+=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '-' && next == '=') {
    value = "-=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '&' && next == '=') {
    value = "&=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '^' && next == '=') {
    value = "^=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '|' && next == '=') {
    value = "|=";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '&' && next == '&') {
    value = "&&";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '|' && next == '|') {
    value = "||";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '.' && next == '.') {
    const auto next_next_pos = _next_queued_non_continuation_pos(next_pos + 1);
    const char next_next = _queue[next_next_pos];
    if (next_next == '.') {
      value = "...";
      consumed_queue_end = next_next_pos + 1;
    } else {
      value = ".";
    }
  } else if (cur == '#' && next == '#') {
    value = "##";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '<' && next == ':') {
    value = "<:";
    consumed_queue_end = next_pos + 1;
  } else if (cur == ':' && next == '>') {
    value = ":>";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '<' && next == '%') {
    value = "<%";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '%' && next == '>') {
    value = "%>";
    consumed_queue_end = next_pos + 1;
  } else if (cur == '%' && next == ':') {
    _advance_queue_to_pos(next_pos + 1);
    consumed_queue_end = next_pos + 1;
    const auto next_next_pos = _next_queued_non_continuation_pos(next_pos + 1);
    const auto next_next_next_pos =
      _next_queued_non_continuation_pos(next_next_pos + 1);
    const char next_next = _queue[next_next_pos];
    const char next_next_next = _queue[next_next_next_pos];
    if (next_next == '%' && next_next_next == ':') {
      consumed_queue_end = next_next_next_pos + 1;
      value = "%:%:";
    } else {
      value = "%:";
    }
  } else {
    value = cur;
  }
  _advance_queue_to_pos(consumed_queue_end);
  return raw_pp_token(pp_token::type::punctuator, value,
		      range_in_file(begin_loc, _queue_locs[_queue_pos]));
}

raw_pp_token _pp_tokenizer::_tokenize_pp_number()
{
  std::string value;
  bool done = false;

  assert(_queue[_queue_pos] == '.' ||
	 ('0' <= _queue[_queue_pos] && _queue[_queue_pos] <= '9'));
  const auto begin_loc = _queue_locs[_queue_pos];
  while(!done) {
    const auto cur_pos = _next_queued_non_continuation_pos(_queue_pos);
    const char cur = _queue[cur_pos];
    switch (cur) {
    case 'e':
    case 'E':
    case 'p':
    case 'P':
      {
	const auto next_pos = _next_queued_non_continuation_pos(cur_pos + 1);
	const char next = _queue[next_pos];
	if (next == '+' || next == '-') {
	  value.push_back(cur);
	  value.push_back(next);
	  _advance_queue_to_pos(next_pos + 1);
	} else {
	  value.push_back(cur);
	  _advance_queue_to_pos(cur_pos + 1);
	}
      }
      break;

    case '.':
    case '0' ... '9':
    case '_':
    case 'a' ... 'd':
    case 'f' ... 'o':
    case 'q' ... 'z':
    case 'A' ... 'D':
    case 'F' ... 'O':
    case 'Q' ... 'Z':
      value.push_back(cur);
      _advance_queue_to_pos(cur_pos + 1);
      break;

    default:
      done = true;
    }
  }

  return raw_pp_token(pp_token::type::pp_number, value,
		      range_in_file(begin_loc, _queue_locs[_queue_pos]));
}

raw_pp_token _pp_tokenizer::_tokenize_id()
{
  std::string value;
  bool done = false;
  assert(_queue[_queue_pos] == '_' ||
	 ('a' <= _queue[_queue_pos] && _queue[_queue_pos] <= 'z') ||
	 ('A' <= _queue[_queue_pos] && _queue[_queue_pos] <= 'Z'));
  const auto begin_loc = _queue_locs[_queue_pos];
  while (!done) {
    const auto cur_pos = _next_queued_non_continuation_pos(_queue_pos);
    const char cur = _queue[cur_pos];
    switch (cur) {
    case '_':
    case 'a' ... 'z':
    case 'A' ... 'Z':
    case '0' ... '9':
      value.push_back(cur);
      _advance_queue_to_pos(cur_pos + 1);
      break;

    default:
      done = true;
    }
  }

  return raw_pp_token(pp_token::type::id, value,
		      range_in_file(begin_loc, _queue_locs[_queue_pos]));
}

std::size_t _pp_tokenizer::_skip_c_comment(const std::size_t n_trailing_spaces)
{
  assert(_queue[_queue_pos] == '/');
  const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
  assert(_queue[next_pos] == '*');
  std::size_t n_chars_in_last_line =
    ((next_pos == ((_queue_pos + 1) & (_queue_capacity - 1))) ?
     n_trailing_spaces + 2 :
     1);
  _advance_queue_to_pos(next_pos + 1);

  while (true) {
    const auto cur = _queue[_queue_pos];
    if (cur == '*') {
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      if (_queue[next_pos] == '/') {
	n_chars_in_last_line =
	  ((next_pos == ((_queue_pos + 1) & (_queue_capacity - 1))) ?
	   n_trailing_spaces + 2 :
	   1);
	_advance_queue_to_pos(next_pos + 1);
	break;
      }
    }

    ++n_chars_in_last_line;
    if (cur == '\n' || cur == 0) {
      if (cur == 0 && _at_eof()) {
	throw this->report_fatal("EOF within C-style comment",
				 _queue_locs[_queue_pos]);
      }
      n_chars_in_last_line = 0;
    }
    _advance_queue_to_pos(_queue_pos + 1);
  }

  return n_chars_in_last_line;
}

void _pp_tokenizer::_skip_cpp_comment()
{
  assert(_queue[_queue_pos] == '/');
  const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
  assert(_queue[next_pos] == '/');
  _advance_queue_to_pos(next_pos + 1);

  while(_queue[_queue_pos] != '\n') {
    if (!_queue[_queue_pos] && _at_eof()) {
      this->report_warning("EOF within C++-style comment",
			   _queue_locs[_queue_pos]);
    }

    _advance_queue_to_pos(_queue_pos + 1);
  }
}

raw_pp_token _pp_tokenizer::_tokenize_ws()
{
  // According to the standard (5.1.1.2(3)):
  // - Each comment is replaced replaced by a single space.
  // - Newlines are retained.
  // - Implementations may replace whitespace sequences other than
  //   newlines with a single space.
  //
  // GNU cpp does the following:
  // At the beginning of lines:
  //   - Comments are transformed into a sequence of space characters
  //     equal in number to the number of comment's characters after its
  //     last newline.
  //   - Any whitespace character is transformed into a space.
  // At the end of lines:
  //   - Any space, including any arising from comments, is stripped.
  // In the middle of lines:
  //   - Comments are replaced with a single space
  //   - Sequences of whitespace are replaced with a single space.
  //
  // In addition, GNU CPP collapses multiple newlines into a single one.
  // We don't do this here.
  std::size_t n_spaces = 0;
  bool done = false;

  char cur = _queue[_queue_pos];
  {
    const char next = _queue[_next_queued_non_continuation_pos(_queue_pos + 1)];
    assert(cur == ' ' || cur == '\t' || cur == '\v' || cur == '\f' ||
	   (cur == '/' && (next == '*' || next == '/')));
  }
  const auto begin_loc = _queue_locs[_queue_pos];
  while (!done) {
    cur = _queue[_queue_pos];
    switch (cur) {
    case '\n':
      // Whitespace at end of line, return a single newline.
      _advance_queue_to_pos(_queue_pos + 1);
      _expect_qh_str = expect_qh_str::newline;
      return raw_pp_token(pp_token::type::newline, "\n",
			  range_in_file(begin_loc, _queue_locs[_queue_pos]));

    case '/':
      {
	const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
	const char next = _queue[next_pos];
	if (next == '*') {
	  n_spaces = _skip_c_comment(n_spaces);
	} else if (next == '/') {
	  // Whitespace + comment at end of line, return a single newline.
	  _skip_cpp_comment();
	  assert(_at_eof() || _queue[_queue_pos] == '\n');
	  _advance_queue_to_pos(_queue_pos + 1);
	  _expect_qh_str = expect_qh_str::newline;
	  return raw_pp_token(pp_token::type::newline, "\n",
			      range_in_file(begin_loc,
					    _queue_locs[_queue_pos]));
	} else {
	  done = true;
	}
      }
      break;

    case ' ':
    case '\t':
    case '\v':
    case '\f':
      ++n_spaces;
	  _advance_queue_to_pos(_queue_pos + 1);
      break;

    default:
      done = true;
    }
  }

  return raw_pp_token(pp_token::type::ws,
	std::string(_expect_qh_str == expect_qh_str::newline ? n_spaces : 1,
		    ' '),
	range_in_file(begin_loc, _queue_locs[_queue_pos]));
}

raw_pp_token _pp_tokenizer::read_next_token()
{
  const auto begin_loc = _queue_locs[_queue_pos];
  switch (_queue[_queue_pos]) {
  case 0:
    if (!_at_eof()) {
      // It's a continuation line between tokens, return an empty
      // token for recording that.
      _advance_queue_to_pos(_queue_pos + 1);
      return raw_pp_token(pp_token::type::empty, "",
			  range_in_file(begin_loc, _queue_locs[_queue_pos]));
    } else {
      return raw_pp_token(pp_token::type::eof, "",
			  range_in_file(_queue_locs[_queue_pos],
					_queue_locs[_queue_pos]));
    }

  case '\n':
    {
      _expect_qh_str = expect_qh_str::newline;
      _advance_queue_to_pos(_queue_pos + 1);
      return raw_pp_token(pp_token::type::newline,
			  std::string(1, '\n'),
			  range_in_file(begin_loc, _queue_locs[_queue_pos]));
    }

  case ' ':
  case '\t':
  case '\v':
  case '\f':
    return _tokenize_ws();

  case '/':
    {
      const auto next =
	_queue[_next_queued_non_continuation_pos(_queue_pos + 1)];
      if (next == '*') {
	return _tokenize_ws();
      } else if (next == '/') {
	return _tokenize_ws();
      } else {
	_expect_qh_str = expect_qh_str::no;
	return _tokenize_punctuator();
      }
    }

  case '\'':
    _expect_qh_str = expect_qh_str::no;
    _advance_queue_to_pos(_queue_pos + 1);
    return _tokenize_string(begin_loc, '\'', true, pp_token::type::chr);

  case '\"':
    _advance_queue_to_pos(_queue_pos + 1);
    if (_expect_qh_str != expect_qh_str::expect_qh_str) {
      _expect_qh_str = expect_qh_str::no;
      return _tokenize_string(begin_loc, '"', true, pp_token::type::str);
    } else {
      _expect_qh_str = expect_qh_str::no;
      return _tokenize_string(begin_loc, '"', false, pp_token::type::qstr);
    }

  case 'L':
    {
      _expect_qh_str = expect_qh_str::no;
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      const auto next = _queue[next_pos];
	if (next == '"') {
	  _advance_queue_to_pos(next_pos + 1);
	  return _tokenize_string(begin_loc, '"', true, pp_token::type::wstr);
	} else if (next == '\'') {
	  _advance_queue_to_pos(next_pos + 1);
	  return _tokenize_string(begin_loc, '\'', true, pp_token::type::wchr);
	} else {
	  return _tokenize_id();
	}
    }

  case 'u':
    {
      _expect_qh_str = expect_qh_str::no;
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      const auto next = _queue[next_pos];
      if (next == '8') {
	const auto next_next_pos =
	  _next_queued_non_continuation_pos(next_pos + 1);
	const auto next_next = _queue[next_next_pos];
	if (next_next == '"') {
	  _advance_queue_to_pos(next_next_pos + 1);
	  return _tokenize_string(begin_loc, '"', true, pp_token::type::ustr8);
	} else {
	  return _tokenize_id();
	}
      } else if (next == '"') {
	_advance_queue_to_pos(next_pos + 1);
	return _tokenize_string(begin_loc, '"', true, pp_token::type::ustr16);
      } else if (next == '\'') {
	_advance_queue_to_pos(next_pos + 1);
	return _tokenize_string(begin_loc, '\'', true, pp_token::type::uchr16);
      } else {
	return _tokenize_id();
      }
    }

  case 'U':
    {
      _expect_qh_str = expect_qh_str::no;
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      const auto next = _queue[next_pos];
      if (next == '"') {
	_advance_queue_to_pos(next_pos + 1);
	return _tokenize_string(begin_loc, '"', true, pp_token::type::ustr32);
      } else if (next == '\'') {
	_advance_queue_to_pos(next_pos + 1);
	return _tokenize_string(begin_loc, '\'', true, pp_token::type::uchr32);
      } else {
	return _tokenize_id();
      }
    }

  case '_':
  case 'a' ... 't':
  case 'v' ... 'z':
  case 'A' ... 'K':
  case 'M' ... 'T':
  case 'V' ... 'Z':
    {
      raw_pp_token tok = _tokenize_id();
      if (_expect_qh_str == expect_qh_str::sharp_seen &&
	  tok.get_value() == "include") {
	_expect_qh_str = expect_qh_str::expect_qh_str;
      } else {
	_expect_qh_str = expect_qh_str::no;
      }
      return tok;
    }

  case '0' ... '9':
    _expect_qh_str = expect_qh_str::no;
    return _tokenize_pp_number();

  case '.':
    {
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      const auto next = _queue[next_pos];
      _expect_qh_str = expect_qh_str::no;
      if ('0' <= next && next <= '9') {
	return _tokenize_pp_number();
      } else {
	return _tokenize_punctuator();
      }
    }

  case '#':
    {
      const auto next_pos = _next_queued_non_continuation_pos(_queue_pos + 1);
      const auto next = _queue[next_pos];
      if (_expect_qh_str == expect_qh_str::newline && next != '#') {
	_expect_qh_str = expect_qh_str::sharp_seen;
      } else {
	_expect_qh_str = expect_qh_str::no;
      }
      return _tokenize_punctuator();
    }

  case '<':
    if (_expect_qh_str != expect_qh_str::expect_qh_str) {
      _expect_qh_str = expect_qh_str::no;
      return _tokenize_punctuator();
    } else {
      _expect_qh_str = expect_qh_str::no;
      _advance_queue_to_pos(_queue_pos + 1);
      return _tokenize_string(begin_loc, '>', false, pp_token::type::hstr);
    }

  case '[':
  case ']':
  case '(':
  case ')':
  case '{':
  case '}':
  case '-':
  case '+':
  case '&':
  case '*':
  case '~':
  case '!':
  case '%':
  case '>':
  case '=':
  case '^':
  case '|':
  case '?':
  case ':':
  case ';':
  case ',':
    _expect_qh_str = expect_qh_str::no;
    return _tokenize_punctuator();

  default:
    {
      const char cur = _queue[_queue_pos];
      _expect_qh_str = expect_qh_str::no;
      _advance_queue_to_pos(_queue_pos + 1);
      return raw_pp_token(pp_token::type::non_ws_char,
			  std::string(1, cur),
			  range_in_file(begin_loc, _queue_locs[_queue_pos]));
    }
  }
  // unreachable
}


pp_tokenizer::pp_tokenizer(pp_result::header_inclusion_node &file)
  : _file(file), _sr(_file.create_source_reader())
{
  init_state();
}

pp_tokenizer::~pp_tokenizer() noexcept = default;

raw_pp_token pp_tokenizer::read_next_token()
{
  return impl::_pp_tokenizer::read_next_token();
}

_pp_tokenizer::buffer_type pp_tokenizer::read_raw()
{
  return _sr->read();
}

void pp_tokenizer::add_line(const std::streamoff length)
{
  _file.add_line(length);
}

void pp_tokenizer::report_warning(const std::string &msg,
				  const range_in_file::loc_type loc)
{
  _remarks.add(code_remark(code_remark::severity::warning,
			   msg, _file, range_in_file{loc}));
}

pp_except pp_tokenizer::report_fatal(const std::string &msg,
				     const range_in_file::loc_type loc)
{
  code_remark remark{code_remark::severity::fatal,
		     msg, _file, range_in_file{loc}};
  _remarks.add(remark);
  return pp_except{remark};
}


pp_string_tokenizer::
pp_string_tokenizer(const std::string &s,
		    const report_warning_type &report_warning,
		    const report_fatal_type &report_fatal)
  : _buf(s.begin(), s.end()),
    _report_warning(report_warning), _report_fatal(report_fatal)
{
  init_state();
}

pp_string_tokenizer::~pp_string_tokenizer() noexcept = default;

raw_pp_token pp_string_tokenizer::read_next_token()
{
  return impl::_pp_tokenizer::read_next_token();
}

raw_pp_tokens pp_string_tokenizer::tokenize_builtin(const std::string &s)
{
  pp_string_tokenizer tokenizer{s,
				[](const std::string&) {
				  assert(0);
				  __builtin_unreachable();
				},
				[](const std::string&) -> pp_except {
				  assert(0);
				  __builtin_unreachable();
				}};

  raw_pp_tokens result;
  while (true) {
    raw_pp_token tok = tokenizer.read_next_token();
    if (tok.is_eof())
      break;

    result.push_back(std::move(tok));
  }

  return result;
}

_pp_tokenizer::buffer_type pp_string_tokenizer::read_raw()
{
  return std::move(_buf);
}

void pp_string_tokenizer::add_line(const std::streamoff)
{}

void pp_string_tokenizer::report_warning(const std::string &msg,
					 const range_in_file::loc_type)
{
  _report_warning(msg);
}

pp_except pp_string_tokenizer::report_fatal(const std::string &msg,
					    const range_in_file::loc_type)
{
  return _report_fatal(msg);
}

