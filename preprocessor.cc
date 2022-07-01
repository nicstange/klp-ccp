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
#include <functional>
#include "preprocessor.hh"
#include "target/target.hh"
#include "pp_except.hh"
#include "parse_except.hh"
#include "semantic_except.hh"
#include "path.hh"
#include "pp_expr_parser_driver.hh"

using namespace klp::ccp;

class preprocessor::_pp_token
{
private:
  template <pp_token::type... types>
  struct __is_type_any_of;

public:
  static constexpr raw_pp_token_index not_passed_through =
    std::numeric_limits<raw_pp_token_index>::max();

  _pp_token(const pp_token::type type, const std::string &value,
	    const raw_pp_tokens_range &token_source,
	    const pp_result::used_macros &eh,
	    const raw_pp_token_index passed_through_raw_token
		    = not_passed_through);

  _pp_token(const pp_token::type type, const std::string &value,
	    const raw_pp_tokens_range &token_source,
	    const raw_pp_token_index passed_through_raw_token
		    = not_passed_through);

  _pp_token(const pp_token::type type, std::string &&value,
	    const raw_pp_tokens_range &token_source,
	    const raw_pp_token_index passed_through_raw_token
		    = not_passed_through) noexcept;

  pp_token::type get_type() const noexcept
  {
    return _type;
  }

  const std::string& get_value() const noexcept
  {
    return _value;
  }

  std::string&& grab_value() noexcept
  {
    return std::move(_value);
  }

  void set_type_and_value(const pp_token::type type,
			  const std::string &value);

  const class pp_result::used_macros& expansion_history() const noexcept
  {
    return _expansion_history;
  }

  class pp_result::used_macros& expansion_history() noexcept
  {
    return _expansion_history;
  }

  const raw_pp_tokens_range& get_token_source() const noexcept
  {
    return _token_source;
  }

  void set_macro_invocation(pp_result::macro_invocation *mi)
    noexcept;

  pp_result::macro_invocation * get_macro_invocation()
    const noexcept;

  raw_pp_token_index get_passed_through_raw_token() const noexcept
  { return _passed_through_raw_token; }

  operator bool() const noexcept
  {
    return _type != pp_token::type::eof;
  }

  template <typename T>
  bool is_punctuator(T &&s) const noexcept
  {
    return (_type == pp_token::type::punctuator &&
	    _value == std::forward<T>(s));
  }

  template <pp_token::type... types>
  bool is_type_any_of() const noexcept
  { return __is_type_any_of<types...>()(*this); }

  bool is_id() const noexcept
  {
    return is_type_any_of<pp_token::type::id>();
  }

  bool is_ws() const noexcept
  {
    return is_type_any_of<pp_token::type::ws>();
  }

  bool is_newline() const noexcept
  {
    return is_type_any_of<pp_token::type::newline>();
  }

  bool is_eof() const noexcept
  {
    return is_type_any_of<pp_token::type::eof>();
  }

  bool is_empty() const noexcept
  {
    return is_type_any_of<pp_token::type::empty>();
  }

  std::string stringify() const;

private:
  template <pp_token::type... types>
  struct __is_type_any_of
  {
    // This is the sizeof...(types) == 0 case,
    // c.f. the specialization below.
    bool operator()(const _pp_token &) const noexcept
    { return false; }
  };

  template <pp_token::type type0, pp_token::type... types>
  struct __is_type_any_of<type0, types...>
  {
    bool operator()(const _pp_token &tok) const noexcept
    {
      return tok._type == type0 || __is_type_any_of<types...>()(tok);
    }
  };

  pp_token::type _type;
  std::string _value;
  raw_pp_tokens_range _token_source;
  pp_result::macro_invocation * _macro_invocation;
  raw_pp_token_index _passed_through_raw_token;

  // Used for avoiding macro recursion as appropriate.
  pp_result::used_macros _expansion_history;
};

class preprocessor::_macro_expander
{
public:
  static std::vector<_pp_token>
  expand(preprocessor &preprocessor,
	 const pp_result::macro &m,
	 pp_result::macro_invocation &mi,
	 std::vector<_pp_tokens> &&args,
	 std::vector<_pp_tokens> &&exp_args,
	 const raw_pp_tokens_range &invocation_range,
	 code_remarks &remarks);

private:
  _macro_expander() = delete;

  static std::vector<_pp_token>
  _expand_builtin_special(preprocessor &preprocessor,
			  const pp_result::macro &macro,
			  const raw_pp_token_index id_tok);

  static std::string _stringify(const std::vector<_pp_token> &arg_toks);

  static _pp_token _concat(_pp_token &&tok1, _pp_token &&tok2,
			   preprocessor &preprocessor,
			   const raw_pp_tokens_range &invocation_range,
			   code_remarks &remarks);
};


struct preprocessor::_cond_incl_state
{
  _cond_incl_state(pp_result::conditional_inclusion_node &_incl_node);

  std::reference_wrapper<pp_result::conditional_inclusion_node> incl_node;
  pp_result::used_macros um;
  pp_result::macro_nondef_constraints mnc;
  pp_result::conditional_inclusion_node::directive_ranges_type
	directive_ranges;
  bool branch_active;
  pp_result::conditional_inclusion_node::directive_ranges_type::size_type
	taken_branch;
  bool has_else_branch;
};

struct preprocessor::_expansion_state
{
  _expansion_state();

  typedef std::vector<_pp_token> expanded_tokens_type;

  struct stack_entry
  {
    stack_entry(expanded_tokens_type &&_tokens)
      : tokens(std::move(_tokens)), next_token(0)
    {}

    expanded_tokens_type tokens;
    expanded_tokens_type::size_type next_token;
  };

  std::vector<stack_entry> expansion_stack;
};


preprocessor::preprocessor(const header_resolver &header_resolver,
			   const target &tgt)
  : _header_resolver(header_resolver), _tgt(tgt),
    _pp_result(new pp_result{}),
    _cur_header_inclusion_root(_pp_result->get_header_inclusion_roots()
			       .begin()),
    _cond_incl_nesting(0),
    _root_expansion_state(new _expansion_state{}),
    _cur_macro_invocation(nullptr), __counter__(0),
    _maybe_pp_directive(true), _line_empty(true),
    _eof(false), _eof_index(std::numeric_limits<raw_pp_token_index>::max()),
    _peeked_begin(_none_peeked)
{
  // Populate the special builtin macros.
  for (const auto &name : { "__FILE__",
			    "__BASE_FILE__",
			    "__LINE__",
			    "__INCLUDE_LEVEL__",
			    "__COUNTER__" }) {
    const pp_result::macro &m =
      _pp_result->_add_macro(name, pp_result::macro::builtin_special_tag{});
    _macros.insert(std::make_pair(m.get_name(), std::ref(m)));
  }
}

preprocessor::preprocessor(preprocessor &&pp) = default;

preprocessor::~preprocessor() noexcept = default;

void preprocessor::add_root_source(const std::string &filename,
				   const bool is_preinclude)
{
  pp_result::header_inclusion_roots &roots =
    _pp_result->get_header_inclusion_roots();
  assert(_cur_header_inclusion_root == roots.begin());
  _pp_result->get_header_inclusion_roots().add(filename, is_preinclude);
  _cur_header_inclusion_root = roots.begin();
}

void preprocessor::set_base_file(const std::string &filename)
{
  assert(_base_file.empty());
  assert(_tokenizers.empty());
  _base_file = filename;
}

pp_token_index preprocessor::read_next_token()
{
  // Basically, this is just a wrapper around _expand()
  // which removes or adds whitespace as needed:
  // - Whitespace at the end of lines gets removed.
  // - Multiple whitespace tokens get collated into one.
  // - Empty lines are removed.
  // - Whitespace gets inserted inbetween certain kind of tokens
  //   in order to guarantee idempotency of preprocessing.
  //
  // A queue of pending tokens is maintained in ::_pending_tokens.
  //
  // The following patters are possible for _pending_tokens at each
  // invocation:
  // - a single whitespace token,
  // - a non-whitespace token from lookahead,
  // - (added) whitespace + some punctuator, pp_number or id from lookahead
  //
  assert(_pending_tokens.empty() ||
	 (_pending_tokens.size() == 1) ||
	 (_pending_tokens.size() == 2 && _pending_tokens.front().is_ws() &&
	  (_pending_tokens.back().get_type() == pp_token::type::punctuator ||
	   _pending_tokens.back().get_type() == pp_token::type::pp_number ||
	   _pending_tokens.back().get_type() == pp_token::type::id)));
  if (!_pending_tokens.empty() &&
      (_pending_tokens.size() == 2 ||
       !(_pending_tokens.front()
	 .is_type_any_of<pp_token::type::ws,
			 pp_token::type::punctuator,
			 pp_token::type::pp_number,
			 pp_token::type::id>()))) {
    auto tok = std::move(_pending_tokens.front());
    _pending_tokens.pop();
    assert(!_line_empty);
    if (tok.is_newline())
      _line_empty = true;
    return _emit_pp_token(*_pp_result, std::move(tok));
  }
  assert(_pending_tokens.empty() ||
	 (_pending_tokens.size() == 1 &&
	  (_pending_tokens.front().is_type_any_of<pp_token::type::ws,
						  pp_token::type::punctuator,
						  pp_token::type::pp_number,
						  pp_token::type::id>())));

 read_next:
  _pp_token next_tok = _expand(*_root_expansion_state,
			       std::bind(&preprocessor::_read_next_plain_token,
					 this,
					 std::placeholders::_1));
  bool empty_seen = false;
  while (next_tok.is_empty()) {
    empty_seen = true;
    next_tok = _expand(*_root_expansion_state,
		       std::bind(&preprocessor::_read_next_plain_token,
				 this, std::placeholders::_1));
  }
  next_tok.expansion_history().clear();

  // First, handle pending whitespace.
  if (!_pending_tokens.empty() && _pending_tokens.front().is_ws()) {
    if (next_tok.is_newline()) {
      // Queued whitespace at end of line. Drop it.
      _pending_tokens.pop();
      assert(_pending_tokens.empty());
      if (!_line_empty) {
	_line_empty = true;
	return _emit_pp_token(*_pp_result, std::move(next_tok));
      }
      goto read_next;

    } else if (next_tok.is_eof()) {
      // Queued whitespace at end of file. Drop it.
      _pending_tokens.pop();
      assert(_pending_tokens.empty());
      return _emit_pp_token(*_pp_result, std::move(next_tok));

    }  else if (next_tok.is_ws()) {
      // Multiple consecutive whitespace tokens.
      goto read_next;
    }

    // Something which is not an EOF, newline, whitespace or empty
    // token. Emit the pending whitespace and queue the current
    // token.
    _line_empty = false;
    _pending_tokens.push(std::move(next_tok));
    next_tok = std::move(_pending_tokens.front());
    _pending_tokens.pop();
    return _emit_pp_token(*_pp_result, std::move(next_tok));
  }

  // At this point, a pending token, if any, is a punctuator,
  // pp_number or id which might have to get separated from the
  // subsequent token by extra whitespace.
  assert(_pending_tokens.empty() ||
	 (_pending_tokens.size() == 1 &&
	  (_pending_tokens.front().is_type_any_of<pp_token::type::punctuator,
						  pp_token::type::pp_number,
						  pp_token::type::id>())));
  // There are certain token combinations where GNU cpp emits an extra
  // space in order to make preprocessing idempotent:
  // pp_number {-, +, --, ++, -=, +=, ., id, pp_number}
  // . pp_number
  // id {id, pp_number)
  // + {+, ++, =}, - {-, --, =, >}, ! =, * =, / =, % =, ^ =,
  // < {<, =, <=, <<=, :, %}, << =, > {>, =, >=, >>=}, >> =,
  // & {=, &}, | {=, |},
  // % {=, >, :},  %: %:, : >,
  // : :, . ., # # if any of these is coming from macro expansion
  //                or they're separated by any empties
  if (next_tok.is_ws()) {
    if (!_pending_tokens.empty()) {
      std::swap(next_tok, _pending_tokens.front());
      return _emit_pp_token(*_pp_result, std::move(next_tok));
    } else {
      _pending_tokens.push(std::move(next_tok));
      goto read_next;
    }
  } else if (next_tok.is_newline()) {
    if (_line_empty) {
      assert(_pending_tokens.empty());
      goto read_next;
    } else {
      if (!_pending_tokens.empty())
	std::swap(next_tok, _pending_tokens.front());
      return _emit_pp_token(*_pp_result, std::move(next_tok));
    }
  } else if (!next_tok.is_type_any_of<pp_token::type::punctuator,
				      pp_token::type::pp_number,
				      pp_token::type::id>()) {
    _line_empty = false;
    if (!_pending_tokens.empty())
      std::swap(next_tok, _pending_tokens.front());
    return _emit_pp_token(*_pp_result, std::move(next_tok));
  } else if (_pending_tokens.empty()) {
    _line_empty = false;
    _pending_tokens.push(std::move(next_tok));
    goto read_next;
  }

  // There is a pending punctuator, pp_numer or id and the next token
  // is also of this type. Check whether extra whitespace needs to get
  // inserted.
  _line_empty = false;
  _pp_token prev_tok = std::move(_pending_tokens.front());
  _pending_tokens.pop();
  if ((prev_tok.is_type_any_of<pp_token::type::pp_number>() &&
       (next_tok.is_punctuator("-") || next_tok.is_punctuator("+") ||
	next_tok.is_punctuator("--") || next_tok.is_punctuator("++") ||
	next_tok.is_punctuator("-=") || next_tok.is_punctuator("+=") ||
	next_tok.is_punctuator(".") || next_tok.is_id() ||
	next_tok.is_type_any_of<pp_token::type::pp_number>())) ||
      (prev_tok.is_punctuator(".") &&
       next_tok.is_type_any_of<pp_token::type::pp_number>()) ||
      (prev_tok.is_id() &&
       (next_tok.is_id() ||
	next_tok.is_type_any_of<pp_token::type::pp_number>())) ||
      (prev_tok.is_type_any_of<pp_token::type::punctuator>() &&
       next_tok.is_type_any_of<pp_token::type::punctuator>() &&
       ((prev_tok.is_punctuator("+") &&
	 (next_tok.is_punctuator("+") || next_tok.is_punctuator("++") ||
	  next_tok.is_punctuator("="))) ||
	(prev_tok.is_punctuator("-") &&
	 (next_tok.is_punctuator("-") || next_tok.is_punctuator("--") ||
	  next_tok.is_punctuator("=") || next_tok.is_punctuator(">"))) ||
	(prev_tok.is_punctuator("!") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("*") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("/") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("%") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("^") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("<") &&
	 (next_tok.is_punctuator("<") || next_tok.is_punctuator("=") ||
	  next_tok.is_punctuator("<=") || next_tok.is_punctuator("<<=") ||
	  next_tok.is_punctuator(":") || next_tok.is_punctuator("%"))) ||
	(prev_tok.is_punctuator("<<") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator(">") &&
	 (next_tok.is_punctuator(">") || next_tok.is_punctuator("=") ||
	  next_tok.is_punctuator(">=") || next_tok.is_punctuator(">>="))) ||
	(prev_tok.is_punctuator(">>") && next_tok.is_punctuator("=")) ||
	(prev_tok.is_punctuator("&") &&
	 (next_tok.is_punctuator("=") || next_tok.is_punctuator("&"))) ||
	(prev_tok.is_punctuator("|") &&
	 (next_tok.is_punctuator("=") || next_tok.is_punctuator("|"))) ||
	(prev_tok.is_punctuator("%") &&
	 (next_tok.is_punctuator("=") || next_tok.is_punctuator(">") ||
	  next_tok.is_punctuator(":"))) ||
	(prev_tok.is_punctuator("%:") && next_tok.is_punctuator("%:")) ||
	(prev_tok.is_punctuator(":") && next_tok.is_punctuator(">")) ||
	(empty_seen &&
	 ((prev_tok.is_punctuator(":") && next_tok.is_punctuator(":")) ||
	  (prev_tok.is_punctuator(".") && next_tok.is_punctuator(".")) ||
	  (prev_tok.is_punctuator("#") && next_tok.is_punctuator("#"))))))) {

    // Insert a ws token. Be careful to set the source range to keep
    // the source ranges sorted, otherwise the lookup from
    // raw_pp_tokens_ranges to pp_token_ranges will break.
    const raw_pp_tokens_range r =
      (prev_tok.get_token_source() == next_tok.get_token_source() ?
       next_tok.get_token_source() :
       raw_pp_tokens_range{next_tok.get_token_source().begin,
			   next_tok.get_token_source().begin});

    _pp_token extra_ws_tok{pp_token::type::ws, " ", r};
    extra_ws_tok.set_macro_invocation(next_tok.get_macro_invocation());
    _pending_tokens.push(std::move(extra_ws_tok));
  }

  _pending_tokens.push(std::move(next_tok));
  return _emit_pp_token(*_pp_result, std::move(prev_tok));
}

void preprocessor::register_builtin_macro(const std::string &name,
					  const std::string &repl)
{
  const raw_pp_tokens tokenized_repl =
    pp_string_tokenizer::tokenize_builtin(repl);
  raw_pp_tokens normalized_repl =
    _normalize_macro_repl(tokenized_repl.begin(), tokenized_repl.end(),
			  false, std::vector<std::string>{},
			  [](const std::string&,
			     const raw_pp_tokens::const_iterator&)
				-> pp_except {
			    assert(0);
			    __builtin_unreachable();
			  });

  const pp_result::macro &m =
    _pp_result->_add_macro(name, false, false, std::vector<std::string>{},
			   pp_string_tokenizer::tokenize_builtin(repl),
			   pp_result::macro::builtin_tag{});
  if (!_macros.insert(std::make_pair(m.get_name(), std::ref(m))).second)
    assert(0);
}

void preprocessor::
register_builtin_macro(const std::string &name,
		       const std::initializer_list<const char *> &arg_list,
		       const bool variadic,
		       const std::string &repl)
{
  std::vector<std::string> _arg_list;
  _arg_list.reserve(arg_list.size());
  for (const auto a : arg_list)
    _arg_list.emplace_back(a);

  const raw_pp_tokens tokenized_repl =
    pp_string_tokenizer::tokenize_builtin(repl);
  raw_pp_tokens normalized_repl =
    _normalize_macro_repl(tokenized_repl.begin(), tokenized_repl.end(),
			  true, _arg_list,
			  [](const std::string&,
			     const raw_pp_tokens::const_iterator&)
				-> pp_except {
			    assert(0);
			    __builtin_unreachable();
			  });

  const pp_result::macro &m =
    _pp_result->_add_macro(name, true, variadic, std::move(_arg_list),
			   std::move(normalized_repl),
			   pp_result::macro::builtin_tag{});
  if (!_macros.insert(std::make_pair(m.get_name(), std::ref(m))).second)
    assert(0);
}

void preprocessor::register_predefined_macro
	(const std::string &signature,
	 const std::string &repl,
	 const std::function<void(const std::string)> &report_warning)
{
  const auto &report_fatal =
    [](const std::string &s) {
      return pp_except{s};
    };

  pp_string_tokenizer signature_tokenizer{
	signature, report_warning, report_fatal
  };
  raw_pp_tokens signature_toks;
  while (true) {
    raw_pp_token tok = signature_tokenizer.read_next_token();
    if (tok.is_eof())
      break;

    signature_toks.push_back(std::move(tok));
  }

  pp_string_tokenizer repl_tokenizer{
	repl, report_warning, report_fatal
  };
  raw_pp_tokens repl_toks;
  while (true) {
    raw_pp_token tok = repl_tokenizer.read_next_token();
    if (tok.is_eof())
      break;

    repl_toks.push_back(std::move(tok));
  }

  const auto &skip_ws
    = [](const raw_pp_tokens::const_iterator &it,
	 const raw_pp_tokens::const_iterator &end) {
	if (it == end || !it->is_ws()) {
	  return it;
	}
	// No need to loop here: pp_tokenizer won't emit more than one
	// consecutive whitespace.
	assert(it + 1 == end || !(it + 1)->is_ws());
	return it + 1;
      };

  raw_pp_tokens::const_iterator signature_end;
  std::string name;
  bool func_like;
  std::vector<std::string> arg_names;
  bool variadic;
  std::tie(signature_end, name, func_like, arg_names, variadic)
    = _parse_macro_signature(skip_ws(signature_toks.cbegin(),
				     signature_toks.cend()),
			     signature_toks.cend(),
			     [&](const std::string &msg,
				 const raw_pp_tokens::const_iterator&) {
			       return report_fatal(msg);
			     });
  if (skip_ws(signature_end, signature_toks.cend()) !=
      signature_toks.cend()) {
    throw report_fatal("garbage after macro signature");
  }

  raw_pp_tokens normalized_repl =
    _normalize_macro_repl(skip_ws(repl_toks.cbegin(), repl_toks.cend()),
			  repl_toks.cend(),
			  func_like, arg_names,
			  [&](const std::string &msg,
			     const raw_pp_tokens::const_iterator&) {
			    return report_fatal(msg);
			  });

  const pp_result::macro &m =
    _pp_result->_add_macro(name, func_like, variadic, std::move(arg_names),
			   std::move(normalized_repl),
			   pp_result::macro::predefined_user_tag{});

  const auto it_existing = _macros.find(name);
  if (it_existing != _macros.end() && it_existing->second.get() != m) {
    throw report_fatal("macro redefined in an incompatible way");
  }

  _macros.insert(std::make_pair(m.get_name(), std::ref(m)));
}

void preprocessor::register_predefined_macro_undef
	(const std::string &name,
	 const std::function<void(const std::string)> &report_warning)
{
  const auto &report_fatal =
    [](const std::string &s) {
      return pp_except{s};
    };

  pp_string_tokenizer name_tokenizer{
	name, report_warning, report_fatal
  };
  raw_pp_tokens name_toks;
  while (true) {
    raw_pp_token tok = name_tokenizer.read_next_token();
    if (tok.is_eof())
      break;

    name_toks.push_back(std::move(tok));
  }

  const auto &skip_ws
    = [](const raw_pp_tokens::const_iterator &it,
	 const raw_pp_tokens::const_iterator &end) {
	if (it == end || !it->is_ws()) {
	  return it;
	}
	// No need to loop here: pp_tokenizer won't emit more than one
	// consecutive whitespace.
	assert(it + 1 == end || !(it + 1)->is_ws());
	return it + 1;
      };

  const auto it_name = skip_ws(name_toks.cbegin(), name_toks.cend());
  if (it_name == name_toks.cend() ||
      !it_name->is_id() ||
      skip_ws(it_name + 1, name_toks.cend()) != name_toks.end()) {
    throw report_fatal("invalid macro name");
  }

  _pp_result->_add_macro_undef(it_name->get_value(),
			       pp_result::macro_undef::predefined_user_tag{});
  _macros.erase(it_name->get_value());
}

template<typename T>
void preprocessor::_grab_remarks_from(T &from)
{
  _remarks += from.get_remarks();
  from.get_remarks().clear();
}


pp_token_index preprocessor::_emit_pp_token(pp_result &r, _pp_token &&tok)
{
  r._append_token
    (pp_token{tok.get_type(), tok.grab_value(),
	      tok.get_token_source()});

  const pp_token_index tok_index = r._get_last_pp_index();
  // Amend the macro_invocation's map of passed through raw macro
  // argument tokens.
  if (tok.get_macro_invocation() &&
      tok.get_passed_through_raw_token() != _pp_token::not_passed_through) {
    tok.get_macro_invocation()->_add_passed_through_macro_arg_token
      (tok.get_passed_through_raw_token(), tok_index);
  }

  return tok_index;
}

preprocessor::_pp_token preprocessor::
_read_next_plain_token(const _read_ctl ctl)
{
 again:
  // Has the final eof token been seen?
  if (_eof) {
    return _pp_token{pp_token::type::eof, std::string{""},
		     raw_pp_tokens_range{_eof_index, _eof_index}};
  }

  if (_tokenizers.empty()) {
    // First time _read_next_plain_token() gets called.
    // Setup the tokenizer for the first root file.
    assert(_cur_header_inclusion_root ==
	   _pp_result->get_header_inclusion_roots().begin());
    assert(!_pp_result->get_header_inclusion_roots().empty());
    _pp_result->_enter_root(*_cur_header_inclusion_root, 0);
    _tokenizers.emplace(*_cur_header_inclusion_root);
    _inclusions.emplace(std::ref(*_cur_header_inclusion_root));
  }

  if (ctl == _read_ctl::peek && _peeked_begin == _none_peeked) {
    // First in a series of peek requests. Setup everything so that
    // the peeked tokens can later get replayed in regular,
    // non-peeking mode.
    _peeked_begin = _pp_result->get_raw_tokens().size();
    _peeked_maybe_pp_directive = _maybe_pp_directive;
  }

  if (_peeked_begin != _none_peeked && ctl != _read_ctl::peek &&
      _peeked_begin == _pp_result->get_raw_tokens().size()) {
    // All previously peeked tokens have been replayed and the peek
    // series had been terminated by an EOF from the tokenizer, which
    // doesn't get recorded. Read it again below, this time for real.
    _peeked_begin = _none_peeked;
  }

  auto raw_tok = [&]() -> const raw_pp_token * {
    if (_peeked_begin == _none_peeked || ctl == _read_ctl::peek) {
      try {
	auto tok = _tokenizers.top().read_next_token();

	_grab_remarks_from(_tokenizers.top());

	if (tok.is_eof()) {
	  if (ctl != _read_ctl::peek)
	    _handle_eof_from_tokenizer(tok);

	  return nullptr;
	}

	_pp_result->_append_token(std::move(tok));
	return &_pp_result->get_raw_tokens().back();

      } catch (const pp_except&) {
	_grab_remarks_from(_tokenizers.top());
	throw;
      }

    } else {
      const raw_pp_token *peeked_tok =
	&_pp_result->get_raw_tokens()[_peeked_begin];
      if (++_peeked_begin == _pp_result->get_raw_tokens().size())
	_peeked_begin = _none_peeked;
      _maybe_pp_directive = _peeked_maybe_pp_directive;
      return peeked_tok;

    }
  }();

  if (!raw_tok) {
    _maybe_pp_directive = true;

    if (!_eof && ctl != _read_ctl::normal) {
      const raw_pp_token_index eof_index
	= _pp_result->_get_last_raw_index() + 1;
      return _pp_token{pp_token::type::eof, std::string{""},
		       raw_pp_tokens_range{eof_index, eof_index}};
    }

    goto again;
  }

  const bool is_pp_directive =
    _maybe_pp_directive && raw_tok->is_punctuator("#");
  if (raw_tok->is_newline()) {
    _maybe_pp_directive = true;

  } else if (is_pp_directive) {
    // In peek mode, crossing preprocessor directives is not allowed.
    if (ctl == _read_ctl::peek) {
      const raw_pp_token_index eof_index
	= _pp_result->_get_last_raw_index();
      return _pp_token{pp_token::type::eof, std::string{""},
		       raw_pp_tokens_range{eof_index, eof_index}};
    }

    _handle_pp_directive(ctl);
    goto again;

  } else if (!raw_tok->is_ws()) {
    _maybe_pp_directive = false;

  }

  if (ctl != _read_ctl::peek) {
    // If in the course of replaying some previously peeked tokens,
    // _peeked_maybe_pp_directive must be updated. Otherwise there's
    // no harm in doing so.
    _peeked_maybe_pp_directive = _maybe_pp_directive;
  }

  if (_cond_incl_inactive())
    goto again;

  const raw_pp_token_index raw_token_index =
    raw_tok - &_pp_result->get_raw_tokens()[0];
  return _pp_token{raw_tok->get_type(), raw_tok->get_value(),
		   raw_pp_tokens_range{raw_token_index},
		   raw_token_index};
}

void preprocessor::_handle_eof_from_tokenizer(const raw_pp_token &eof_tok)
{
  // If the topmost inclusion tree node is an (unfinished) conditional
  // inclusion, that's an error.
  if (_cur_incl_node_is_cond()) {
    _fixup_inclusion_node_ranges();
    const raw_pp_token_index eof_index = _pp_result->get_raw_tokens().size();
    const raw_pp_tokens_range eof_range{eof_index, eof_index};
    const auto it_src
      =  _pp_result->intersecting_sources_begin(eof_range);
    assert(it_src != _pp_result->intersecting_sources_end(eof_range));
    code_remark remark(code_remark::severity::fatal,
		       "missing #endif at end of file",
		       *it_src, eof_tok.get_range_in_file());
    _remarks.add(remark);
    throw pp_except(remark);
  }

  _tokenizers.pop();
  const raw_pp_token_index cur_raw_pos =
    (!_pp_result->get_raw_tokens().empty() ?
     _pp_result->_get_last_raw_index() + 1 :
     0);
  _inclusions.top().get()._set_range_end(cur_raw_pos);
  _inclusions.pop();

  if (_tokenizers.empty()) {
    if (++_cur_header_inclusion_root ==
	_pp_result->get_header_inclusion_roots().end()) {
      _eof = true;
      _eof_index = _pp_result->get_raw_tokens().size();
      return;
    }

    _pp_result->_enter_root(*_cur_header_inclusion_root, cur_raw_pos);
    _tokenizers.emplace(*_cur_header_inclusion_root);
    _inclusions.emplace(std::ref(*_cur_header_inclusion_root));
  }
}

void preprocessor::_fixup_inclusion_node_ranges() noexcept
{
  // Fixup the all pending inclusion nodes' range information such
  // that those intersecting a given query range can be found.
  if (_cur_header_inclusion_root ==
      _pp_result->get_header_inclusion_roots().end()) {
    return;
  }
  const raw_pp_token_index cur_raw_end = _pp_result->_raw_tokens.size();
  for (auto it_root = _cur_header_inclusion_root + 1;
       it_root != _pp_result->get_header_inclusion_roots().end();
       ++it_root) {
    it_root->_range.begin = cur_raw_end;
    it_root->_range.end = cur_raw_end;
  }

  for (pp_result::inclusion_node *n = &_inclusions.top().get(); n;
       n = n->_parent) {
    n->_range.end = cur_raw_end;
  }
}

const pp_result::header_inclusion_node& preprocessor::
_raw_tok_it_to_source(const raw_pp_tokens::const_iterator &it)
{
  const raw_pp_token_index index = it - _pp_result->get_raw_tokens().begin();
  return get_pending_token_source(index);
}

const pp_result::header_inclusion_node&
preprocessor::_raw_pp_tokens_range_to_source(const raw_pp_tokens_range &r)
{
  const pp_result::header_inclusion_node &s
    = get_pending_token_source(r.begin);
  assert(r.begin == r.end || &s == &get_pending_token_source(r.end - 1));
  return s;
}

range_in_file preprocessor::
_raw_pp_tokens_range_to_range_in_file(const raw_pp_tokens_range &r)
  const noexcept
{
    if (r.begin == r.end + 1) {
    // A single token, emit "point" range for its start.
    const raw_pp_token &tok = _pp_result->get_raw_tokens()[r.begin];
    const range_in_file &tok_rif = tok.get_range_in_file();
    return range_in_file{tok_rif.begin};
  } else if (r.begin == r.end) {
    // An empty range, used for EOFs. Emit a "point" range for the
    // preceeding token's end.
    assert(r.begin > 0);
    assert(r.begin <= _pp_result->get_raw_tokens().size());
    const raw_pp_token &tok = _pp_result->get_raw_tokens()[r.begin - 1];
    const range_in_file &tok_rif = tok.get_range_in_file();
    return range_in_file{tok_rif.end};
  } else {
    // A real range. It must come from a single header.
    assert(r.end <= _pp_result->get_raw_tokens().size());
    const raw_pp_token &first_tok = _pp_result->get_raw_tokens()[r.begin];
    const raw_pp_token &last_tok = _pp_result->get_raw_tokens()[r.end - 1];
    const range_in_file &first_tok_rif = first_tok.get_range_in_file();
    const range_in_file &last_tok_rif = last_tok.get_range_in_file();
    return range_in_file{first_tok_rif.begin, last_tok_rif.end};
  }
}

const pp_result::header_inclusion_node&
preprocessor::_pp_token_to_source(const _pp_token &tok)
{
  return _raw_pp_tokens_range_to_source(tok.get_token_source());
}

range_in_file preprocessor::_pp_token_to_range_in_file(const _pp_token &tok)
  const noexcept
{
  return _raw_pp_tokens_range_to_range_in_file(tok.get_token_source());
}

void preprocessor::_handle_pp_directive(const _read_ctl ctl)
{
  const raw_pp_token_index raw_begin = _pp_result->_get_last_raw_index();
  raw_pp_token_index raw_end = raw_begin + 1;
  assert(_pp_result->get_raw_tokens()[raw_begin].is_punctuator("#"));
  bool endif_possible = true;
  bool is_endif = false;
  while (true) {
    assert(!_tokenizers.empty());
    auto tok = _tokenizers.top().read_next_token();
    _grab_remarks_from(_tokenizers.top());

    if (tok.is_eof()) {
      _handle_eof_from_tokenizer(tok);
      break;
    }

    _pp_result->_append_token(tok);
    raw_end = _pp_result->_get_last_raw_index() + 1;

    if (tok.is_newline()) {
      break;

    } else if (endif_possible && tok.get_type() == pp_token::type::id &&
	       tok.get_value() == "endif") {
      // Be careful not to read until the end of line or end of file
      // for #endif directives: _handle_eof_from_tokenizer() will
      // throw an exception if there's some conditional inclusion
      // pending.
      is_endif = true;
      endif_possible = false;

      if (!_cur_incl_node_is_cond()) {
	code_remark remark
	  (code_remark::severity::fatal,
	   "#endif without #if",
	   get_pending_token_source(_pp_result->_get_last_raw_index()),
	   tok.get_range_in_file());
	_remarks.add(remark);
	throw pp_except(remark);
      }

      do {
	tok = _tokenizers.top().read_next_token();
	_grab_remarks_from(_tokenizers.top());
	_pp_result->_append_token(tok);
	raw_end = _pp_result->_get_last_raw_index() + 1;
      } while (tok.is_ws());

      if (tok.is_newline()) {
	_pp_result->_append_token(tok);
	raw_end = _pp_result->_get_last_raw_index() + 1;
      } else if (!tok.is_eof()) {
	code_remark remark
	  (code_remark::severity::warning,
	   "garbage after #endif",
	   get_pending_token_source(_pp_result->_get_last_raw_index()),
	   tok.get_range_in_file());
	_remarks.add(remark);
      }

      if (_cond_incl_nesting == _cond_incl_states.size()) {
	_cond_incl_states.top().directive_ranges.emplace_back(raw_begin,
							      raw_end);
	_pop_cond_incl(raw_end);
      }
      --_cond_incl_nesting;

      // Handling EOF is safe now after the conditional inclusion has
      // been popped.
      if (tok.is_eof())
	_handle_eof_from_tokenizer(tok);

      break;

    } else if (!tok.is_ws()) {
      endif_possible = false;
    }
  }

  _pp_result->_add_directive(raw_pp_tokens_range{raw_begin, raw_end});

  if (is_endif)
    return;

  auto it_tok = _pp_result->get_raw_tokens().begin() + raw_begin + 1;
  if (it_tok == _pp_result->get_raw_tokens().begin() + raw_end) {
    // Null directive terminated by EOF.
    return;
  }

  if (it_tok->is_ws())
    ++it_tok;

  if (it_tok->is_newline() ||
      it_tok == _pp_result->get_raw_tokens().begin() + raw_end) {
    // Null directive
    return;
  }

  assert(!it_tok->is_ws());
  if (!it_tok->is_id()) {
    code_remark remark(code_remark::severity::fatal,
		       "identifier expected in preprocessor directive",
		       _raw_tok_it_to_source(it_tok),
		       it_tok->get_range_in_file());
    _remarks.add(remark);
    throw pp_except(remark);
  }


  // First, process conditional inclusion directives: those
  // must be looked at even if within a non-taken branch of
  // another conditional inclusion.
  if (it_tok->get_value() == "if") {
    ++_cond_incl_nesting;
    if (_cond_incl_inactive())
      return;

    _push_cond_incl(raw_begin);
    _cond_incl_states.top().directive_ranges.emplace_back(raw_begin, raw_end);

    if (_eval_conditional_inclusion(raw_pp_tokens_range{raw_begin, raw_end}))
      _enter_cond_incl();

  } else if (it_tok->get_value() == "ifdef") {
    ++_cond_incl_nesting;
    if (_cond_incl_inactive())
      return;

    ++it_tok;
    if (it_tok->is_ws())
      ++it_tok;

    if (!it_tok->is_id()) {
      code_remark remark(code_remark::severity::fatal,
			 "identifier expected after #ifdef",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    _push_cond_incl(raw_begin);
    _cond_incl_states.top().directive_ranges.emplace_back(raw_begin, raw_end);

    auto it_m = _macros.find(it_tok->get_value());
    if (it_m != _macros.end()) {
      _cond_incl_states.top().um += it_m->second;
      _enter_cond_incl();
    } else {
      _cond_incl_states.top().mnc +=
	pp_result::macro_nondef_constraint{it_tok->get_value()};
    }

  } else if (it_tok->get_value() == "ifndef") {
    ++_cond_incl_nesting;
    if (_cond_incl_inactive())
      return;

    ++it_tok;
    if (it_tok->is_ws())
      ++it_tok;

    if (!it_tok->is_id()) {
      code_remark remark(code_remark::severity::fatal,
			 "identifier expected after #ifndef",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    _push_cond_incl(raw_begin);
    _cond_incl_states.top().directive_ranges.emplace_back(raw_begin, raw_end);

    auto it_m = _macros.find(it_tok->get_value());
    if (it_m != _macros.end()) {
      _cond_incl_states.top().um += it_m->second;
    } else {
      _cond_incl_states.top().mnc +=
	pp_result::macro_nondef_constraint{it_tok->get_value()};
      _enter_cond_incl();
    }

  } else if (it_tok->get_value() == "elif") {
    if (!_cur_incl_node_is_cond()) {
      code_remark remark(code_remark::severity::fatal,
			 "#elif without #if",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    if (_cond_incl_nesting > _cond_incl_states.size())
      return;

    _cond_incl_states.top().directive_ranges.emplace_back(raw_begin, raw_end);

    if (!_cond_incl_states.top().taken_branch) {
      if (_eval_conditional_inclusion(raw_pp_tokens_range{raw_begin, raw_end}))
	_enter_cond_incl();
    } else {
      _cond_incl_states.top().branch_active = false;
    }

  } else if (it_tok->get_value() == "else") {
    if (!_cur_incl_node_is_cond()) {
      code_remark remark(code_remark::severity::fatal,
			 "#else without #if",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    if (_cond_incl_nesting > _cond_incl_states.size())
      return;

    _cond_incl_states.top().directive_ranges.emplace_back(raw_begin, raw_end);

    if (!_cond_incl_states.top().taken_branch) {
      _enter_cond_incl();
    } else {
      _cond_incl_states.top().branch_active = false;
    }
    _cond_incl_states.top().has_else_branch = true;
  }

  // Look at the other directives only if they're not within a
  // non-taken conditional inclusion branch.
  if (_cond_incl_inactive())
    return;

  if (it_tok->get_value() == "define") {
    ++it_tok;
    if (it_tok->is_ws())
      ++it_tok;

    if (!it_tok->is_id()) {
      code_remark remark(code_remark::severity::fatal,
			 "identifier expected after #define",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    const pp_result::macro &m =
      _handle_macro_definition(raw_pp_tokens_range{raw_begin, raw_end});

    auto it_existing = _macros.find(m.get_name());
    if (it_existing != _macros.end() && it_existing->second.get() != m) {
      code_remark remark(code_remark::severity::fatal,
		"macro redefined in an incompatible way",
		_raw_pp_tokens_range_to_source(m.get_directive_range()),
		_raw_pp_tokens_range_to_range_in_file(m.get_directive_range()));
      _remarks.add(remark);
      throw pp_except(remark);
    }

    _macros.insert(std::make_pair(m.get_name(), std::ref(m)));
  } else if (it_tok->get_value() == "undef") {
    ++it_tok;
    if (it_tok->is_ws())
      ++it_tok;

    if (!it_tok->is_id()) {
      code_remark remark(code_remark::severity::fatal,
			 "identifier expected after #undef",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    auto it_tok_id = it_tok;
    ++it_tok;
    if (!it_tok->is_newline() && !it_tok->is_eof()) {
      code_remark remark(code_remark::severity::fatal,
			 "garbage after #undef",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    _pp_result->_add_macro_undef(it_tok_id->get_value(),
				 raw_pp_tokens_range{raw_begin, raw_end});

    auto it_m = _macros.find(it_tok_id->get_value());
    if (it_m != _macros.end()) {
      _macros.erase(it_m);
    }

  } else if (it_tok->get_value() == "include") {
    if (ctl == _read_ctl::no_file_crossing) {
      code_remark remark(code_remark::severity::fatal,
			 "#include directive in macro invocation",
			 _raw_tok_it_to_source(it_tok),
			 it_tok->get_range_in_file());
      _remarks.add(remark);
      throw pp_except(remark);
    }

    _handle_include(raw_pp_tokens_range{raw_begin, raw_end});

  }
}

preprocessor::_pp_token preprocessor::
_expand(_expansion_state &state,
	const std::function<_pp_token(const _read_ctl)> &token_reader,
	const bool from_cond_incl_cond)
{
  const bool state_is_root = (&state == &*_root_expansion_state);
  bool raw_tokens_read = false;
  bool keep_cur_macro_invocation = false;

  bool peeking = false;
  std::reverse_iterator<
	std::vector<_expansion_state::stack_entry>::const_iterator
    > it_peeking_stack_entry;
  std::vector<_pp_token>::size_type peeking_token_index;
  auto read_tok = [&](const _read_ctl ctl) {
    if (ctl != _read_ctl::peek) {
      peeking = false;

      while (!state.expansion_stack.empty()) {
	_expansion_state::stack_entry &top = state.expansion_stack.back();
	if (top.next_token == top.tokens.size()) {
	  state.expansion_stack.pop_back();
	  assert(_cur_macro_invocation);
	  // During macro argument parsing, we can have state !=
	  // _root_expansion_state, but _root_expansion_state still
	  // being empty.
	  if (state_is_root &&
	      _root_expansion_state->expansion_stack.empty() &&
	      !keep_cur_macro_invocation) {
	    _cur_macro_invocation = nullptr;
	  }
	  continue;
	}

	_pp_token tok = top.tokens[top.next_token++];
	return tok;
      }
    } else {
      if (!peeking) {
	// Beginning to peek, do the one-time setup.
	peeking = true;
	it_peeking_stack_entry = state.expansion_stack.rbegin();
	if (!state.expansion_stack.empty())
	  peeking_token_index = it_peeking_stack_entry->next_token;
      }

      while (it_peeking_stack_entry != state.expansion_stack.rend() &&
	     peeking_token_index == it_peeking_stack_entry->tokens.size() &&
	     ++it_peeking_stack_entry != state.expansion_stack.rend()) {
	peeking_token_index = it_peeking_stack_entry->next_token;
      }

      if (it_peeking_stack_entry != state.expansion_stack.rend())
	return it_peeking_stack_entry->tokens[peeking_token_index++];
    }

    raw_tokens_read = state_is_root;
    return token_reader(ctl);
  };

 read_next:
  _pp_token tok =  read_tok(_read_ctl::normal);
  if (tok.is_eof())
    return tok;

  // Deal with possible macro invocation
  if (tok.is_id()) {
    auto m = _macros.find(tok.get_value());
    if (m != _macros.end() && !tok.expansion_history().count(m->second)) {
      if (!m->second.get().is_func_like()) {
	const raw_pp_tokens_range &tok_range = tok.get_token_source();
	if (!_cur_macro_invocation) {
	  assert(_root_expansion_state->expansion_stack.empty());
	  assert(state_is_root);
	  assert(tok_range.begin + 1 == tok_range.end);
	  _cur_macro_invocation =
	    &_pp_result->_add_macro_invocation(m->second, tok_range);
	}
	state.expansion_stack.emplace_back(
		_handle_object_macro_invocation(m->second, std::move(tok)));
	goto read_next;
      } else {
	// It can happen that the tail result of some previous macro
	// invocation combines with some subsequent raw input tokens
	// to form another function-style macro invocation.  In this
	// case, there should be only a single macro_invocation
	// instance covering the whole range => extend the current
	// one.
	pp_result::macro_invocation * const saved_cur_macro_invocation
	  = _cur_macro_invocation;
	raw_tokens_read = false;
	_pp_token next_tok = read_tok(_read_ctl::peek);
	while (next_tok.is_ws() || next_tok.is_newline() ||
	       next_tok.is_empty()) {
	  next_tok = read_tok(_read_ctl::peek);
	}

	if (next_tok.is_punctuator("(")) {
	  assert(raw_tokens_read || _cur_macro_invocation);
	  _cur_macro_invocation = saved_cur_macro_invocation;

	  if (!_cur_macro_invocation) {
	    assert(raw_tokens_read);
	    // We'll need an active _cur_macro_invocation for the macro
	    // argument expansion. Create one now and let
	    // _handle_func_macro_invocation() called below set its
	    // invocation range's end once it is known.
	    const raw_pp_tokens_range &tok_range = tok.get_token_source();
	    assert(tok_range.begin + 1 == tok_range.end);
	    _cur_macro_invocation =
	      &_pp_result->_add_macro_invocation(m->second, tok_range);
	  }

	  keep_cur_macro_invocation = true;
	  state.expansion_stack.emplace_back(_handle_func_macro_invocation(
			m->second,
			tok.get_token_source().begin,
			read_tok, &raw_tokens_read));
	  keep_cur_macro_invocation = false;
	  goto read_next;
	}

	// Identifier token not followed by an opening parenthesis ->
	// not a macro invocation.
	if (saved_cur_macro_invocation) {
	  // This macro is defined as a function style macro, but
	  // won't get expanded, because it isn't followed by an
	  // opening '('. Register a macro_nondef_constraint for
	  // object style macros only.
	  saved_cur_macro_invocation->_macro_nondef_constraints +=
	    pp_result::macro_nondef_constraint{tok.get_value(), true};
	}
      }

    } else if (m == _macros.end() && _cur_macro_invocation) {
      // The current token came from some macro expansion, but doesn't
      // resolve to a macro definition itself. Register a
      // macro_nondef_constraint instance for the
      // _cur_macro_invocation. Look ahead to check whether the
      // identifier token is not followed by an opening parenthesis
      // and thus, a function style definition, even if present, would
      // not alter the result.
      pp_result::macro_invocation * const saved_cur_macro_invocation
	= _cur_macro_invocation;
      _pp_token next_tok = read_tok(_read_ctl::peek);
      while (next_tok.is_ws() || next_tok.is_newline() ||
	     next_tok.is_empty()) {
	next_tok = read_tok(_read_ctl::peek);
      }

      saved_cur_macro_invocation->_macro_nondef_constraints +=
	pp_result::macro_nondef_constraint{tok.get_value(),
					   !next_tok.is_punctuator("(")};
    }

    if (from_cond_incl_cond && tok.get_value() == "defined") {
      assert(state_is_root);
      const _pp_token defined_tok = tok;
      const raw_pp_token_index invocation_begin = tok.get_token_source().begin;
      do {
	tok = read_tok(_read_ctl::normal);
      } while (tok.is_ws() || tok.is_newline() || tok.is_empty());

      std::pair<_pp_tokens, _pp_token> arg =
	[&]() {
	  if (tok.is_punctuator("(")) {
	    auto arg = _read_macro_arg(read_tok, false);
	    const char arg_delim = std::get<1>(arg).get_value()[0];
	    if (arg_delim == ',') {
	      code_remark remark(code_remark::severity::fatal,
				 "too many arguments to \"defined\" operator",
				 _pp_token_to_source(std::get<1>(arg)),
				 _pp_token_to_range_in_file(std::get<1>(arg)));
	      _remarks.add(remark);
	      throw pp_except(remark);
	    }
	    assert(arg_delim == ')');
	    return std::make_pair(std::move(std::get<0>(arg)),
				  std::move(std::get<1>(arg)));

	  } else {
	    // 'defined FOO' form of the defined operator
	    _pp_token delim_tok = tok;
	    _pp_tokens arg;

	    if (!tok.is_eof())
	      arg.emplace_back(std::move(tok));

	    return (std::make_pair
		    (std::move(arg), std::move(delim_tok)));
	  }
	}();

      if (arg.first.size() != 1 ||
	  arg.first[0].get_type() != pp_token::type::id) {
	code_remark remark(code_remark::severity::fatal,
			   "invalid argument to \"defined\" operator",
			   _pp_token_to_source(arg.first[0]),
			   _pp_token_to_range_in_file(arg.first[0]));
	_remarks.add(remark);
	throw pp_except(remark);
      }

      const _pp_token &delim_tok = arg.second;
      if (defined_tok.expansion_history().empty()) {
	// After the expansion, the whole defined(...) sequence will
	// be replaced by either zero or one and there won't be any
	// trace of the "defined" identifier token left. In
	// particular, it will be to impossible to derive a
	// macro_nondef_constraint from the resulting pp_tokens
	// sequence. Add it here.
	_cond_incl_states.top().mnc +=
	  pp_result::macro_nondef_constraint{"defined"};
      }

      const std::string &id = arg.first[0].get_value();
      bool is_defined = false;
      auto it_m = _macros.find(id);
      if (it_m != _macros.end()) {
	assert(!_cond_incl_states.empty());
	_cond_incl_states.top().um += it_m->second;
	is_defined = true;
      } else {
	assert(!_cond_incl_states.empty());
	_cond_incl_states.top().mnc += pp_result::macro_nondef_constraint{id};
      }

      const raw_pp_token_index invocation_end =
	delim_tok.get_token_source().end;
      return _pp_token(pp_token::type::pp_number,
		       is_defined ? "1" : "0",
		       raw_pp_tokens_range{invocation_begin, invocation_end});
    }
  }

  return tok;
}


preprocessor::_pp_token::
_pp_token(const pp_token::type type, const std::string &value,
	  const raw_pp_tokens_range &token_source,
	  const pp_result::used_macros &eh,
	  const raw_pp_token_index passed_through_raw_token)
  : _value(value), _token_source(token_source), _macro_invocation(nullptr),
    _expansion_history(std::move(eh)), _type(type),
    _passed_through_raw_token(passed_through_raw_token)
{}

preprocessor::_pp_token::
_pp_token(const pp_token::type type, const std::string &value,
	  const raw_pp_tokens_range &token_source,
	  const raw_pp_token_index passed_through_raw_token)
  : _value(value), _token_source(token_source), _macro_invocation(nullptr),
    _type(type), _passed_through_raw_token(passed_through_raw_token)
{}

preprocessor::_pp_token::
_pp_token(const pp_token::type type, std::string &&value,
	  const raw_pp_tokens_range &token_source,
	  const raw_pp_token_index passed_through_raw_token) noexcept
  : _value(std::move(value)), _token_source(token_source),
    _macro_invocation(nullptr), _type(type),
    _passed_through_raw_token(passed_through_raw_token)
{}

void preprocessor::_pp_token::set_type_and_value(const pp_token::type type,
						 const std::string &value)
{
  _type = type;
  _value = value;
}

void preprocessor::_pp_token::
set_macro_invocation(pp_result::macro_invocation *mi) noexcept
{
  assert(!_macro_invocation);
  _macro_invocation = mi;
}

pp_result::macro_invocation *
preprocessor::_pp_token::get_macro_invocation() const noexcept
{
  return _macro_invocation;
}

std::string preprocessor::_pp_token::stringify() const
{
  return pp_token::stringify(_type, _value);
}

std::vector<preprocessor::_pp_token>
preprocessor::_handle_object_macro_invocation(const pp_result::macro &macro,
					      _pp_token &&id_tok)
{
  return _macro_expander::expand(*this, macro,
				 *_cur_macro_invocation,
				 std::vector<_pp_tokens>{},
				 std::vector<_pp_tokens>{},
				 id_tok.get_token_source(),
				 _remarks);
}

struct preprocessor::_scanned_macro_invocation
{
  std::vector<std::size_t> args_offsets;
  std::vector<std::vector<_pp_token>> args;

  std::size_t n_tokens;
  raw_pp_token_index invocation_end;
};

std::vector<preprocessor::_pp_token>
preprocessor::_handle_func_macro_invocation(
	const pp_result::macro &macro,
	const raw_pp_token_index invocation_begin,
	const std::function<_pp_token(const _read_ctl)> &token_reader,
	const bool *update_cur_macro_invocation_range)
{
  _scanned_macro_invocation smi =
    _read_func_macro_invocation(macro.non_va_arg_count(), macro.is_variadic(),
				invocation_begin, token_reader);

  std::vector<_pp_tokens> exp_args;
  exp_args.reserve(smi.args.size());
  for (size_t i_arg = 0; i_arg < smi.args.size(); ++i_arg) {
    if (macro.shall_expand_arg(i_arg)) {
      const _pp_tokens &arg = smi.args[i_arg];

      _pp_tokens::const_iterator it_raw_tok = arg.cbegin();
      _pp_tokens::const_iterator it_peeked_raw_tok = arg.cend();
      auto read_arg_tok = [&](const _read_ctl ctl) {
	if (it_peeked_raw_tok != arg.cend() && ctl != _read_ctl::peek) {
	  const auto &tok = *it_peeked_raw_tok;
	  if (++it_peeked_raw_tok == it_raw_tok)
	    it_peeked_raw_tok = arg.cend();

	  return tok;
	}

	if (it_raw_tok == arg.cend()) {
	  return _pp_token(pp_token::type::eof, std::string(),
			   raw_pp_tokens_range{smi.invocation_end});
	}

	if (ctl == _read_ctl::peek && it_peeked_raw_tok == arg.cend())
	  it_peeked_raw_tok = it_raw_tok;

	return *it_raw_tok++;
      };

      _expansion_state state;
      _pp_tokens exp_arg;
      while(true) {
	auto tok = _expand(state, read_arg_tok);
	if (tok.is_eof())
	  break;
	exp_arg.push_back(std::move(tok));
      }
      exp_args.emplace_back(std::move(exp_arg));

    } else {
      exp_args.emplace_back();

    }
  }

  if (*update_cur_macro_invocation_range) {
    _pp_result->_extend_macro_invocation_range(*_cur_macro_invocation,
					       smi.invocation_end);
  }

  return _macro_expander::expand(*this, macro, *_cur_macro_invocation,
				 std::move(smi.args), std::move(exp_args),
				 raw_pp_tokens_range{invocation_begin,
						     smi.invocation_end},
				 _remarks);
}

preprocessor::_scanned_macro_invocation preprocessor::
_read_func_macro_invocation
	(const std::size_t non_va_arg_count,
	 const bool is_variadic,
	 const raw_pp_token_index invocation_begin,
	 const std::function<_pp_token(const _read_ctl)> &token_reader)
{
  _scanned_macro_invocation result;

  result.n_tokens = 1; // For the identifier token

  while (true) {
    result.n_tokens++;

    const auto &tok = token_reader(_read_ctl::no_file_crossing);
    if (tok.is_punctuator("("))
      break;
    assert(tok.is_ws() || tok.is_newline() || tok.is_empty());
  }

  if (non_va_arg_count || is_variadic) {
    const size_t n_args = non_va_arg_count + is_variadic;
    result.args.reserve(n_args);
    char last_delim = 0;
    size_t i_arg;
    for (i_arg = 0; i_arg < non_va_arg_count && last_delim != ')'; ++i_arg) {
      auto cur_arg = _read_macro_arg(token_reader, false);
      result.args.push_back(std::move(std::get<0>(cur_arg)));
      assert(std::get<1>(cur_arg).get_type() == pp_token::type::punctuator);
      assert(std::get<1>(cur_arg).get_value().size() == 1);
      last_delim = std::get<1>(cur_arg).get_value()[0];
      assert(last_delim == ',' || last_delim == ')');
      result.args_offsets.emplace_back(result.n_tokens + std::get<2>(cur_arg));
      result.n_tokens += std::get<2>(cur_arg) + std::get<3>(cur_arg);
      result.invocation_end = std::get<1>(cur_arg).get_token_source().end;
    }

    if (last_delim == ')' && i_arg < non_va_arg_count) {
      const raw_pp_tokens_range invocation_range{invocation_begin,
						 result.invocation_end};
      code_remark remark
	(code_remark::severity::fatal,
	 "too few parameters in macro invocation",
	 _raw_pp_tokens_range_to_source(invocation_range),
	 _raw_pp_tokens_range_to_range_in_file(invocation_range));
      _remarks.add(remark);
      throw pp_except(remark);

    } else if (last_delim == ',' && !is_variadic) {
      const raw_pp_tokens_range invocation_range{invocation_begin,
						 result.invocation_end};
      code_remark remark
	(code_remark::severity::fatal,
	 "too many parameters in macro invocation",
	 _raw_pp_tokens_range_to_source(invocation_range),
	 _raw_pp_tokens_range_to_range_in_file(invocation_range));
      _remarks.add(remark);
      throw pp_except(remark);

    }

    if (is_variadic) {
      if (last_delim != ')') {
	auto cur_arg = _read_macro_arg(token_reader, true);
	result.args.push_back(std::move(std::get<0>(cur_arg)));
	assert(std::get<1>(cur_arg).get_type() == pp_token::type::punctuator);
	assert(std::get<1>(cur_arg).get_value().size() == 1);
	last_delim = std::get<1>(cur_arg).get_value()[0];
	assert(last_delim == ')');
	result.args_offsets.emplace_back(result.n_tokens +
					 std::get<2>(cur_arg));
	result.n_tokens += std::get<2>(cur_arg) + std::get<3>(cur_arg);
	result.invocation_end = std::get<1>(cur_arg).get_token_source().end;

      } else {
	// The variadic argument might simply has been left out, i.e.
	// it's empty.
	result.args.emplace_back();
	result.args_offsets.emplace_back(result.n_tokens);

      }
    }
  } else {
    // Slurp in the tokens between the () and make sure they're all empties.
    auto dummy_arg = _read_macro_arg(token_reader, false);
    assert(std::get<1>(dummy_arg).get_type() == pp_token::type::punctuator);
    bool non_empty_found = false;
    for (auto tok : std::get<0>(dummy_arg)) {
      if (!tok.is_empty() && !tok.is_ws()) {
	non_empty_found = true;
	break;
      }
    }
    if (non_empty_found || std::get<1>(dummy_arg).get_value() != ")") {
      code_remark remark
	(code_remark::severity::fatal,
	 "too many parameters in macro invocation",
	 _pp_token_to_source(std::get<1>(dummy_arg)),
	 _pp_token_to_range_in_file(std::get<1>(dummy_arg)));
      _remarks.add(remark);
      throw pp_except(remark);
    }

    result.n_tokens += std::get<2>(dummy_arg) + std::get<3>(dummy_arg);
    result.invocation_end = std::get<1>(dummy_arg).get_token_source().end;

  }

  return result;
}

std::tuple<preprocessor::_pp_tokens, preprocessor::_pp_token,
	   std::size_t, std::size_t>
preprocessor::
_read_macro_arg(const std::function<_pp_token(const _read_ctl)> &token_reader,
		const bool variadic)
{
  _pp_tokens arg;
  std::size_t offset = 0;
  std::size_t n_read = 0;
  std::size_t lparens = 0;
  _pp_token end_delim(pp_token::type::eof, std::string(),
		      raw_pp_tokens_range());
  _pp_token pending_ws (pp_token::type::eof, std::string(),
			raw_pp_tokens_range());
  bool last_was_ws = true; // Strip leading whitespace.

  while (true) {
    _pp_token tok = token_reader(_read_ctl::no_file_crossing);
    n_read++;

    if (tok.is_eof()) {
      code_remark remark
	(code_remark::severity::fatal,
	 "no closing right parenthesis in macro invocation",
	 _pp_token_to_source(tok), _pp_token_to_range_in_file(tok));
      _remarks.add(remark);
      throw pp_except(remark);
    }

    if (tok.is_empty() || tok.is_newline() || tok.is_ws()) {
      if (arg.empty())
	offset++;

    // Turn all newlines into whitespace.
      if (tok.is_newline())
	tok.set_type_and_value(pp_token::type::ws, " ");

      // Skip multiple consecutive whitespace tokens.
      if (tok.is_ws() && !last_was_ws) {
	// Normalize multiple whitespaces into a single one.
	if (tok.get_value().size() > 1)
	  tok.set_type_and_value(pp_token::type::ws, " ");
	pending_ws = std::move(tok);
	last_was_ws = true;
      }

      continue;
    }

    if (!lparens &&
	((!variadic && tok.is_punctuator(",")) || tok.is_punctuator(")"))) {
      // End of macro argument. Skip queued whitespace.
      end_delim = std::move(tok);
      break;
    }

    if (tok.is_punctuator("("))
      lparens++;
    else if (tok.is_punctuator(")"))
      lparens--;

    if (last_was_ws && pending_ws.is_ws())
      arg.push_back(pending_ws);

    arg.push_back(tok);
    last_was_ws = false;
  }

  return std::make_tuple(std::move(arg), std::move(end_delim),
			 offset, n_read - offset);
}


void preprocessor::_handle_include(const raw_pp_tokens_range &directive_range)
{
  const raw_pp_tokens::const_iterator raw_begin =
    _pp_result->get_raw_tokens().begin() + directive_range.begin;
  const raw_pp_tokens::const_iterator raw_end =
    _pp_result->get_raw_tokens().begin() + directive_range.end;
  auto it_raw_tok = raw_begin;

  assert(it_raw_tok->is_punctuator("#"));
  ++it_raw_tok;

  if (it_raw_tok->is_ws())
    ++it_raw_tok;

  assert(it_raw_tok->is_id());
  assert(it_raw_tok->get_value() == "include");
  ++it_raw_tok;

  if (it_raw_tok->is_ws())
    ++it_raw_tok;

  assert(it_raw_tok != raw_end);

  auto &&raw_it_to_range_in_file =
    [&](const raw_pp_tokens::const_iterator &it_raw) {
      // Shrink range to start location
      return range_in_file{it_raw->get_range_in_file().begin};
    };
  std::string unresolved;
  bool is_qstr;
  pp_result::used_macros um;
  pp_result::macro_nondef_constraints mnc;
  if (it_raw_tok->is_type_any_of<pp_token::type::qstr,
				 pp_token::type::hstr>()) {
    unresolved = it_raw_tok->get_value();
    is_qstr = it_raw_tok->get_type() == pp_token::type::qstr;
    ++it_raw_tok;
    if (it_raw_tok->is_ws())
      ++it_raw_tok;

    if (!it_raw_tok->is_newline() && !it_raw_tok->is_eof()) {
      code_remark remark(code_remark::severity::fatal,
			 "garbage after #include",
			 _raw_tok_it_to_source(it_raw_tok),
			 raw_it_to_range_in_file(it_raw_tok));
      _remarks.add(remark);
      throw pp_except(remark);
    }
  } else {
    // Run a macro expansion. Temporarily append the expanded pp_token
    // instances to _pp_result and drop them afterwards again.
    auto it_peeked_raw_tok = _pp_result->get_raw_tokens().cend();
    auto read_tok = [&](const _read_ctl ctl) {
      if (it_peeked_raw_tok != _pp_result->get_raw_tokens().cend() &&
	  ctl != _read_ctl::peek) {
	const raw_pp_token_index tok_index =
	  it_peeked_raw_tok- _pp_result->get_raw_tokens().cbegin();
	_pp_token tok{it_peeked_raw_tok->get_type(),
		      it_peeked_raw_tok->get_value(),
		      raw_pp_tokens_range{tok_index}};

	if (++it_peeked_raw_tok == it_raw_tok)
	  it_peeked_raw_tok = _pp_result->get_raw_tokens().cend();

	return tok;
      }

      const raw_pp_token_index tok_index =
	it_raw_tok - _pp_result->get_raw_tokens().cbegin();
      if (it_raw_tok->is_newline() || it_raw_tok->is_eof()) {
	return _pp_token(pp_token::type::eof, std::string(),
			 raw_pp_tokens_range{tok_index, tok_index});
      }

      if (ctl == _read_ctl::peek &&
	  it_peeked_raw_tok == _pp_result->get_raw_tokens().cend())
	it_peeked_raw_tok = it_raw_tok;

      _pp_token tok{it_raw_tok->get_type(), it_raw_tok->get_value(),
		    raw_pp_tokens_range{tok_index}};
      ++it_raw_tok;
      return tok;
    };

    assert(_root_expansion_state->expansion_stack.empty());
    auto exp_read_tok =
      [&]() -> pp_token_index {
	_pp_token tok = _expand(*_root_expansion_state, read_tok, false);
	return _emit_pp_token(*_pp_result, std::move(tok));
      };

    const pp_tokens::size_type temp_tokens_tail_begin
      = _pp_result->get_pp_tokens().size();
    while (true) {
      auto tok = exp_read_tok();
      if (_pp_result->get_pp_tokens()[tok].is_eof())
	break;
    }

    auto skip = [&] (pp_tokens::const_iterator it) {
	while(it->is_type_any_of<pp_token::type::ws,
				 pp_token::type::empty>()) {
	assert(!it->is_newline());
	++it;
      }
      return it;
    };

    auto it_tok = _pp_result->get_pp_tokens().cbegin() + temp_tokens_tail_begin;
    it_tok = skip(it_tok);

    if (it_tok->is_eof()) {
      code_remark remark(code_remark::severity::fatal,
			 "macro expansion at #include evaluated to nothing",
			 _raw_tok_it_to_source(raw_begin),
			 raw_it_to_range_in_file(raw_begin));
      _remarks.add(remark);
      throw pp_except(remark);
    }

    if (it_tok->is_punctuator("<")) {
      is_qstr = false;

      ++it_tok;
      for (auto it_next = skip(it_tok + 1); !it_next->is_eof();
	   it_tok = it_next, it_next = skip(it_next + 1)) {
	unresolved += it_tok->stringify();
      }

      assert(!it_tok->is_eof());
      if (!it_tok->is_punctuator(">")) {
	code_remark remark
	  (code_remark::severity::fatal,
	   "macro expansion at #include does not end with '>'",
	   _raw_tok_it_to_source(raw_begin),
	   raw_it_to_range_in_file(raw_begin));
	_remarks.add(remark);
	throw pp_except(remark);
      } else if (unresolved.empty()) {
	code_remark remark(code_remark::severity::fatal,
			       "macro expansion at #include gives \"<>\"",
			       _raw_tok_it_to_source(raw_begin),
			       raw_it_to_range_in_file(raw_begin));
	_remarks.add(remark);
	throw pp_except(remark);
      }
    } else if (it_tok->get_type() == pp_token::type::str) {
      is_qstr = true;
      unresolved = it_tok->get_value();

      it_tok = skip(it_tok + 1);
      if (!it_tok->is_eof()) {
	code_remark remark
	  (code_remark::severity::fatal,
	   "macro expansion at #include yields garbage at end",
	   _raw_tok_it_to_source(raw_begin),
	   raw_it_to_range_in_file(raw_begin));
	_remarks.add(remark);
	throw pp_except(remark);
      }
    } else {
      code_remark remark(code_remark::severity::fatal,
			 "macro expansion at #include doesn't conform",
			 _raw_tok_it_to_source(raw_begin),
			 raw_it_to_range_in_file(raw_begin));
      _remarks.add(remark);
      throw pp_except(remark);
    }

    std::tie(um, mnc) =
      _pp_result->_drop_pp_tokens_tail(temp_tokens_tail_begin);
  }

  std::string resolved
    = (is_qstr ?
       _header_resolver.resolve(unresolved,
				(_tokenizers.top().get_header_inclusion_node()
				 .get_filename())) :
       _header_resolver.resolve(unresolved));

  if (resolved.empty()) {
    code_remark remark(code_remark::severity::fatal,
		       "could not find header from #include",
		       _raw_tok_it_to_source(raw_begin),
		       raw_it_to_range_in_file(raw_begin));
    _remarks.add(remark);
    throw pp_except(remark);
  }

  pp_result::header_inclusion_child &new_header_inclusion_node
    = _inclusions.top().get()._add_header_inclusion(resolved,
						    directive_range,
						    std::move(um),
						    std::move(mnc),
						    *_pp_result);
  _tokenizers.emplace(new_header_inclusion_node);
  _inclusions.emplace(std::ref(new_header_inclusion_node));
}

void preprocessor::_push_cond_incl(const raw_pp_token_index range_begin)
{
  pp_result::conditional_inclusion_node &node
    = _inclusions.top().get()._add_conditional_inclusion(range_begin);
  _cond_incl_states.emplace(std::ref(node));
  _inclusions.emplace(std::ref(node));
}

void preprocessor::_pop_cond_incl(const raw_pp_token_index range_end)
{
  _cond_incl_state &cond_incl_state = _cond_incl_states.top();
  cond_incl_state.incl_node.get()._finalize
    (range_end,
     std::move(cond_incl_state.um), std::move(cond_incl_state.mnc),
     std::move(cond_incl_state.directive_ranges),
     cond_incl_state.taken_branch,
     cond_incl_state.has_else_branch);
  assert(&cond_incl_state.incl_node.get() == &_inclusions.top().get());
  _inclusions.pop();
  _cond_incl_states.pop();
}

bool preprocessor::_cur_incl_node_is_cond() const noexcept
{
  if (_cond_incl_states.empty())
    return false;

  return &_inclusions.top().get() == &_cond_incl_states.top().incl_node.get();
}

void preprocessor::_enter_cond_incl()
{
  _cond_incl_state &cond_incl_state = _cond_incl_states.top();
  cond_incl_state.branch_active = true;
  cond_incl_state.taken_branch = cond_incl_state.directive_ranges.size();;
}

bool preprocessor::_cond_incl_inactive() const noexcept
{
  return !_cond_incl_states.empty() && !_cond_incl_states.top().branch_active;
}

bool preprocessor::
_eval_conditional_inclusion(const raw_pp_tokens_range &directive_range)
{
  const raw_pp_tokens::const_iterator raw_begin =
    _pp_result->get_raw_tokens().begin() + directive_range.begin;
  auto it_raw_tok = raw_begin + 1;
  if (it_raw_tok->is_ws())
    ++it_raw_tok;
  assert(it_raw_tok->get_type() == pp_token::type::id &&
	 (it_raw_tok->get_value() == "if" ||
	  it_raw_tok->get_value() == "elif"));
  ++it_raw_tok;

  // Temporarily append the expanded pp_token instances from the
  // condition to _pp_result. They'll get purged again when we're done
  // with them.
  _fixup_inclusion_node_ranges();
  auto it_peeked_raw_tok = _pp_result->get_raw_tokens().cend();
  auto read_tok =
    [&](const _read_ctl ctl) {
      if (it_peeked_raw_tok != _pp_result->get_raw_tokens().cend() &&
	  ctl != _read_ctl::peek) {
	const raw_pp_token_index tok_index =
	  it_peeked_raw_tok- _pp_result->get_raw_tokens().cbegin();
	_pp_token tok{it_peeked_raw_tok->get_type(),
		      it_peeked_raw_tok->get_value(),
		      raw_pp_tokens_range{tok_index}};

	if (++it_peeked_raw_tok == it_raw_tok)
	  it_peeked_raw_tok = _pp_result->get_raw_tokens().cend();

	return tok;
      }

      const raw_pp_token_index tok_index =
	it_raw_tok - _pp_result->get_raw_tokens().cbegin();
      if (it_raw_tok->is_newline() || it_raw_tok->is_eof()) {
	return _pp_token(pp_token::type::eof, std::string(),
			 raw_pp_tokens_range{tok_index, tok_index});
      }

      if (ctl == _read_ctl::peek &&
	  it_peeked_raw_tok == _pp_result->get_raw_tokens().cend())
	it_peeked_raw_tok = it_raw_tok;

      _pp_token tok{it_raw_tok->get_type(), it_raw_tok->get_value(),
		    raw_pp_tokens_range{tok_index}};
      ++it_raw_tok;
      return tok;
    };

  /*
   * When encountering a conditional inclusion during func-like macro
   * argument parsing, _cur_macro_invocation has already been
   * set. Save it away and restore it after reading the condition
   * expression. Note that if a macro invocation is currently being
   * parsed, then its range has not been extended to cover the current
   * position in the raw token stream. This is important, because the
   * condition might involve the registration of new temporary macro
   * invocations and the sequence must kept sorted at all times:
   * otherwise e.g. the binary search in _drop_pp_tokens_tail() can
   * get confused. Once _drop_pp_tokens_tail() got a chance to remove
   * those temporary macro invocations again, it is safe to extend the
   * currently pending outer invocation.
   */
  assert(!_cur_macro_invocation ||
	 (_cur_macro_invocation->get_source_range().end <
	  it_raw_tok - _pp_result->get_raw_tokens().cbegin()));
  assert(_root_expansion_state->expansion_stack.empty());
  pp_result::macro_invocation * const saved_cur_macro_invocation
    = _cur_macro_invocation;
  _cur_macro_invocation = nullptr;
  auto exp_read_tok =
    [&]() -> pp_token_index {
      _pp_token tok = _expand(*_root_expansion_state, read_tok, true);
      return _emit_pp_token(*_pp_result, std::move(tok));
    };

  const pp_tokens::size_type temp_tokens_tail_begin
    = _pp_result->get_pp_tokens().size();
  yy::pp_expr_parser_driver pd(exp_read_tok, *_pp_result);
  try {
    pd.parse();
  } catch (const pp_except&) {
    _remarks += pd.get_remarks();
    pd.get_remarks().clear();
    throw;
  } catch (const parse_except&) {
    _remarks += pd.get_remarks();
    pd.get_remarks().clear();
    throw;
  }
  _cur_macro_invocation = saved_cur_macro_invocation;

  ast::ast_pp_expr a = pd.grab_result();
  bool result;
  try {
    result = a.evaluate(_tgt);
  } catch (const semantic_except&) {
    _remarks += a.get_remarks();
    a.get_remarks().clear();
    throw;
  }
  _remarks += a.get_remarks();
  a.get_remarks().clear();

  auto um_mnc = _pp_result->_drop_pp_tokens_tail(temp_tokens_tail_begin);
  _cond_incl_states.top().um += std::move(um_mnc.first);
  _cond_incl_states.top().mnc += std::move(um_mnc.second);

  return result;
}

const pp_result::macro& preprocessor::
_handle_macro_definition(const raw_pp_tokens_range &directive_range)
{
  const raw_pp_tokens::const_iterator begin =
    _pp_result->get_raw_tokens().begin() + directive_range.begin;
  const raw_pp_tokens::const_iterator end =
    _pp_result->get_raw_tokens().begin() + directive_range.end - 1;
  assert (begin != end);
  assert(end != _pp_result->get_raw_tokens().end());
  assert(begin->is_punctuator("#"));
  assert(end->is_newline() || end->is_eof());

  auto skip_ws = [&end](const decltype(end) it) {
    if (it == end || !it->is_ws()) {
      return it;
    }
    // No need to loop here:
    // Macro definitions come in a single line and pp_tokenizer
    // won't emit more than one consecutive whitespace.
    assert(it + 1 == end || !(it + 1)->is_ws());
    return it + 1;
  };

  auto it = skip_ws(begin + 1);
  assert(it->is_id() && it->get_value() == "define");
  it = skip_ws(it + 1);

  auto report_fatal =
    [&](const std::string &msg, const raw_pp_tokens::const_iterator &tok) {
      code_remark remark(code_remark::severity::fatal, msg,
			 _raw_tok_it_to_source(tok),
			 tok->get_range_in_file());
      _remarks.add(remark);
      return pp_except{remark};
    };

  std::string name;
  bool func_like;
  std::vector<std::string> arg_names;
  bool variadic;
  std::tie(it, name, func_like, arg_names, variadic)
    = _parse_macro_signature(it, end, report_fatal);

  if (it != end && !it->is_ws()) {
    code_remark remark(code_remark::severity::warning,
		       "no whitespace before macro replacement list",
		       _raw_tok_it_to_source(it),
		       it->get_range_in_file());
    _remarks.add(remark);
  }

  it = skip_ws(it);

  return (_pp_result->_add_macro
	  (name, func_like, variadic, std::move(arg_names),
	   _normalize_macro_repl(it, end, func_like, arg_names, report_fatal),
	   directive_range));
}

std::tuple<raw_pp_tokens::const_iterator,
	   std::string,
	   bool,
	   std::vector<std::string>,
	   bool>
preprocessor::_parse_macro_signature
			(const raw_pp_tokens::const_iterator begin,
			 const raw_pp_tokens::const_iterator end,
			 const _report_fatal_at_raw_tok_type &report_fatal)
{
  auto skip_ws = [&end](const decltype(end) it) {
    if (it == end || !it->is_ws()) {
      return it;
    }
    // No need to loop here:
    // Macro definitions come in a single line and pp_tokenizer
    // won't emit more than one consecutive whitespace.
    assert(it + 1 == end || !(it + 1)->is_ws());
    return it + 1;
  };

  auto it = skip_ws(begin);
  if (it == end || !it->is_id()) {
    throw report_fatal("no identifier in macro definition", begin);
  }

  const std::string &name = it->get_value();
  ++it;

  bool func_like = false;
  bool variadic = false;
  std::vector<std::string> arg_names;
  if (it != end && it->is_punctuator("(")) {
    it = skip_ws(it + 1);
    func_like = true;

    while (it != end && !it->is_punctuator(")")) {
      if (variadic)
	throw report_fatal("garbage in macro argument list after ...", it);

      if (!arg_names.empty() && !it->is_punctuator(",")) {
	throw report_fatal("comma expected in macro argument list", it);
      } else if (it->is_punctuator(",")) {
	if (arg_names.empty())
	  throw report_fatal("leading comma in macro argument list", it);
	it = skip_ws(it + 1);
	if (it == end || it->is_punctuator(")"))
	  throw report_fatal("trailing comma in macro argument list", it);
      }

      if (it->is_punctuator("...")) {
	variadic = true;
	arg_names.push_back("__VA_ARGS__");
	it = skip_ws(it + 1);
      } else if (it->is_id()) {
	if (it->get_value() == "__VA_ARGS__") {
	  throw report_fatal("__VA_ARGS__ not allowed as a macro argument name",
			     it);
	} else if (std::find(arg_names.cbegin(), arg_names.cend(),
			     it->get_value())
		   != arg_names.cend()) {
	  throw report_fatal("duplicate macro argument name", it);
	}

	arg_names.push_back(it->get_value());
	it = skip_ws(it + 1);
	if (it != end && it->is_punctuator("...")) {
	  variadic = true;
	  it = skip_ws(it + 1);
	}
      } else {
	throw report_fatal("garbage in macro argument list", it);
      }
    }
    if (it == end)
      throw report_fatal("macro argument list not closed", it - 1);

    assert(it->is_punctuator(")"));
    ++it;
  }

  return std::make_tuple(it, name, func_like, std::move(arg_names), variadic);
}

raw_pp_tokens preprocessor::
_normalize_macro_repl(const raw_pp_tokens::const_iterator begin,
		      const raw_pp_tokens::const_iterator end,
		      const bool func_like,
		      const std::vector<std::string> &arg_names,
		      const _report_fatal_at_raw_tok_type &report_fatal)
{
  if (begin == end)
    return raw_pp_tokens();

  assert(!begin->is_ws() && !(end-1)->is_ws());

  if (begin->is_punctuator("##")) {
    throw report_fatal("## at beginning of macro replacement list", begin);
  } else if ((end - 1)->is_punctuator("##")) {
    throw report_fatal("## at end of macro replacement list", end - 1);
  }

  // Thanks to pp_tokenizer, we're already free of multiple
  // consecutive whitespace tokens.
  // Remove any whitespace separating either ## or # and their
  // operands. Collapse multiple ## operators into one.
  auto concat_or_stringify_op_pred = [func_like](const raw_pp_token &tok) {
    return (tok.is_punctuator("##") ||
	    (func_like && tok.is_punctuator("#")));
  };

  raw_pp_tokens result;
  auto it_seq_begin = begin;
  while (it_seq_begin != end) {
    auto it_seq_end = std::find_if(it_seq_begin, end,
				   concat_or_stringify_op_pred);
    if (it_seq_end == end) {
      result.insert(result.end(), it_seq_begin, it_seq_end);
    } else if (it_seq_end->is_punctuator("##")) {
      if ((it_seq_end - 1)->is_ws()) {
	assert((it_seq_end - 1) != it_seq_begin);
	result.insert(result.end(), it_seq_begin, it_seq_end - 1);
      } else {
	result.insert(result.end(), it_seq_begin, it_seq_end);
      }
      result.push_back(*it_seq_end);

      while (it_seq_end != end &&
	     (it_seq_end->is_ws() || it_seq_end->is_punctuator("##"))) {
	++it_seq_end;
      }
    } else {
      assert(func_like && it_seq_end->is_punctuator("#"));
      auto it_arg = it_seq_end + 1;
      if (it_arg != end && it_arg->is_ws())
	++it_arg;
      assert(it_arg == end || !it_arg->is_ws());
      if (it_arg == end || !it_arg->is_id() ||
	  (std::find(arg_names.cbegin(), arg_names.cend(), it_arg->get_value())
	   == arg_names.cend())) {
	throw report_fatal("# in func-like macro not followed by parameter",
			   it_arg != end ? it_arg : end - 1);
      }

      result.insert(result.end(), it_seq_begin, it_seq_end + 1);
      result.push_back(*it_arg);
      it_seq_end = it_arg + 1;
    }
    it_seq_begin = it_seq_end;
  }

  return result;
}


std::vector<preprocessor::_pp_token> preprocessor::_macro_expander::
expand(preprocessor &preprocessor,
       const pp_result::macro &m,
       pp_result::macro_invocation &mi,
       std::vector<_pp_tokens> &&args,
       std::vector<_pp_tokens> &&exp_args,
       const raw_pp_tokens_range &invocation_range,
       code_remarks &remarks)
{
  if (m.is_builtin_special()) {
    return _expand_builtin_special(preprocessor, m,
				   invocation_range.begin);
  }

  // This is the core of macro expansion.
  //
  // The macro expansion will insert empty dummy tokens at various
  // locations to allow the higher levels,
  // preprocessor::read_next_token() in particular, to separate
  // certain productions from surrounding context. These locations are
  // - before and after the whole macro expansion
  // - before and after a concatentation chain
  // - before and after the emission of an expanded macro parameter.
  // This is needed such that
  //    #define __concat(a,b) :a ## b:
  //    :__concat(::a,b::):
  // can evaluate to
  //    : ::ab:: :
  // and so on.
  //
  // For the non-expanded argument values, which are used in
  // concatenations and stringifications, the associated _pp_tokens
  // sequence must not contain any
  // - empty token,
  // - multiple consecutive whitespace tokens or
  // - leading or trailing whitespace.
  //
  const auto &repl_toks = m.get_repl();

  auto &&is_stringification =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      return m.is_func_like() && it_repl->is_punctuator("#");
    };

  auto &&advance_it_repl =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      assert(it_repl != repl_toks.end());
      if (is_stringification(it_repl)) {
	assert(it_repl + 1 != repl_toks.end());
	assert((it_repl + 1)->is_id());
	return it_repl + 2;
      }

      return it_repl + 1;
    };

  auto &&is_vaargs =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      return (m.is_variadic() && it_repl->is_id() &&
	      it_repl->get_value() == m.get_arg_names().back());
    };

  std::map<std::string, std::vector<_pp_tokens>::size_type> args_name_map;
  for (std::vector<_pp_tokens>::size_type i_arg = 0;
       i_arg != m.get_arg_names().size(); ++i_arg) {
    assert(m.shall_expand_arg(i_arg) || exp_args[i_arg].empty());
    args_name_map.insert(std::make_pair(m.get_arg_names()[i_arg], i_arg));
  }

  auto &&resolve_arg =
    [&](const std::string &name, const bool expanded) -> const _pp_tokens * {
      if (!m.is_func_like())
	return nullptr;

      auto it_arg_mapping = args_name_map.find(name);
      if (it_arg_mapping == args_name_map.end())
	return nullptr;

      const auto i_arg = it_arg_mapping->second;
      const _pp_tokens *resolved
	= expanded ? &exp_args[i_arg] : &args[i_arg];
      return resolved;
    };

  auto &&stringify =
    [&](const raw_pp_tokens::const_iterator it_arg_id_tok) {
      assert(it_arg_id_tok != repl_toks.end());
      assert(it_arg_id_tok->is_id());
      auto arg = resolve_arg(it_arg_id_tok->get_value(), false);
      assert(arg);

      // Mark macro argument tokens participating in stringifications
      // as non-passthrough.
      for (auto &&it_tok = (*arg).begin(); it_tok != (*arg).end(); ++it_tok) {
	if (it_tok->get_passed_through_raw_token() !=
	    _pp_token::not_passed_through) {
	  mi._add_non_passthrough_macro_arg_token
		(it_tok->get_passed_through_raw_token());
	}
      }

      return _stringify(*arg);
    };

  std::vector<_pp_token> result;
  // Start the macro expansion with an empty token.
  result.emplace_back(pp_token::type::empty, std::string{}, invocation_range);
  bool last_was_ws_or_empty = true;
  for (raw_pp_tokens::const_iterator it_repl = repl_toks.cbegin(), it_repl_next;
       it_repl != repl_toks.end(); it_repl = it_repl_next) {
    it_repl_next = advance_it_repl(it_repl);

    if (it_repl_next != repl_toks.end() && it_repl_next->is_punctuator("##")) {
      assert(it_repl_next + 1 != repl_toks.end());

      // Handle the GNU extension that in a sequence of
      //  , ## __VA_ARGS__
      // the comma gets removed if __VA_ARGS__ is empty.
      if (it_repl->is_punctuator(",") && is_vaargs(it_repl_next + 1)
	  && (it_repl_next + 2 == repl_toks.end() ||
	      !(it_repl_next + 2)->is_punctuator("##"))) {
	if (!args.back().empty()) {
	  result.emplace_back(it_repl->get_type(), it_repl->get_value(),
			      invocation_range);
	  last_was_ws_or_empty = false;
	}

	++it_repl_next;
	continue;
      }

      // Otherwise it's a concatenation.
      _pp_token concat_result{pp_token::type::empty, std::string{},
			      invocation_range};
      assert(!it_repl->is_ws());
      if (is_stringification(it_repl)) {
	concat_result.set_type_and_value(pp_token::type::str,
					 stringify(it_repl + 1));

      } else {
	const std::vector<_pp_token> * arg = nullptr;
	if (it_repl->is_id())
	  arg = resolve_arg(it_repl->get_value(), false);

	if (arg) {
	  // Emit an empty token, if needed.
	  if (!last_was_ws_or_empty) {
	    result.emplace_back(pp_token::type::empty, std::string{},
				invocation_range);
	    last_was_ws_or_empty = true;
	  }
	  if (!arg->empty()) {
	    if (arg->size() > 1) {
	      // Only the argument's last token participates in
	      // concatenation, the rest gets emitted verbatim.
	      for (auto it_arg_tok = arg->begin();
		   it_arg_tok + 1 != arg->end(); ++it_arg_tok) {
		assert(!it_arg_tok->is_empty());
		result.emplace_back(it_arg_tok->get_type(),
				    it_arg_tok->get_value(),
				    invocation_range,
				    it_arg_tok->expansion_history(),
				    it_arg_tok->get_passed_through_raw_token());
	      }
	    }

	    concat_result.set_type_and_value(arg->back().get_type(),
					     arg->back().get_value());
	    concat_result.expansion_history() +=
	      arg->back().expansion_history();

	    // Mark macro argument tokens participating in
	    // concatenations as non-passthrough.
	    if (arg->back().get_passed_through_raw_token() !=
		_pp_token::not_passed_through) {
		mi._add_non_passthrough_macro_arg_token
			(arg->back().get_passed_through_raw_token());
	    }

	  }
	} else {
	  concat_result.set_type_and_value(it_repl->get_type(),
					   it_repl->get_value());
	}
      }

      while (it_repl_next != repl_toks.end() &&
	     it_repl_next->is_punctuator("##")) {
	it_repl = it_repl_next + 1;
	assert(it_repl != repl_toks.end());
	assert(!it_repl->is_ws());
	it_repl_next = advance_it_repl(it_repl);

	if (is_stringification(it_repl)) {
	  concat_result = _concat(std::move(concat_result),
				  _pp_token{
					pp_token::type::str,
					stringify(it_repl + 1),
					invocation_range
				  },
				  preprocessor, invocation_range,
				  remarks);

	} else if (it_repl->is_punctuator(",") &&
		   it_repl_next != repl_toks.end() &&
		   it_repl_next->is_punctuator("##") &&
		   it_repl_next + 1 != repl_toks.end() &&
		   is_vaargs(it_repl_next + 1) &&
		   (it_repl_next + 2 == repl_toks.end() ||
		    !(it_repl_next + 2)->is_punctuator("##"))) {
	  // Handle the GNU extension that in a sequence of
	  //  ... ## , ## __VA_ARGS__,
	  // the comma gets removed if __VA_ARGS__ is empty.
	  if (!args.back().empty()) {
	    concat_result = _concat(std::move(concat_result),
				    _pp_token{
					it_repl->get_type(),
					it_repl->get_value(),
					invocation_range
				    },
				    preprocessor, invocation_range,
				    remarks);
	  }

	  ++it_repl_next;

	} else {
	  const std::vector<_pp_token> * arg = nullptr;
	  if (it_repl->is_id())
	    arg = resolve_arg(it_repl->get_value(), false);
	  if (arg) {
	    if (arg->empty())
	      continue;

	    concat_result = _concat(std::move(concat_result),
				    _pp_token{
					arg->front().get_type(),
					arg->front().get_value(),
					invocation_range,
					arg->front().expansion_history()
				    },
				    preprocessor, invocation_range,
				    remarks);

	    // Mark macro argument tokens participating in
	    // concatenations as non-passthrough.
	    if (arg->front().get_passed_through_raw_token() !=
		_pp_token::not_passed_through) {
		mi._add_non_passthrough_macro_arg_token
			(arg->front().get_passed_through_raw_token());
	    }

	    if (arg->size() == 1)
	      continue;

	    // Only the argument's first token participates in the
	    // pending concatenation. Any intermediate surplus tokens
	    // are emitted verbatim, the last one might start a
	    // subsequent concatenation operation, if any.
	    result.emplace_back(std::move(concat_result));
	    for (auto it_arg_tok = arg->begin() + 1;
		 it_arg_tok + 1 != arg->end(); ++it_arg_tok) {
	      assert(!it_arg_tok->is_empty());
	      result.emplace_back(it_arg_tok->get_type(),
				  it_arg_tok->get_value(),
				  invocation_range,
				  it_arg_tok->expansion_history(),
				  it_arg_tok->get_passed_through_raw_token());
	    }

	    concat_result.set_type_and_value(arg->back().get_type(),
					     arg->back().get_value());
	    concat_result.expansion_history() +=
	      arg->back().expansion_history();

	    if (it_repl_next != repl_toks.end() &&
		it_repl_next->is_punctuator("##")) {
	      // Mark macro argument tokens participating in
	      // concatenations as non-passthrough.
	      if (arg->front().get_passed_through_raw_token() !=
		  _pp_token::not_passed_through) {
		  mi._add_non_passthrough_macro_arg_token
			(arg->front().get_passed_through_raw_token());
	      }
	    }

	  } else {
	    concat_result = _concat(std::move(concat_result),
				    _pp_token{
					it_repl->get_type(),
					it_repl->get_value(),
					invocation_range
				    },
				    preprocessor, invocation_range,
				    remarks);

	  }
	}
      }

      result.emplace_back(std::move(concat_result));
      result.emplace_back(pp_token::type::empty, std::string{},
			  invocation_range);
      last_was_ws_or_empty = true;

    } else if (is_stringification(it_repl)) {
      result.emplace_back(pp_token::type::str,
			  stringify(it_repl + 1),
			  invocation_range);
      last_was_ws_or_empty = false;

    } else {
      const std::vector<_pp_token> * arg = nullptr;

      if (it_repl->is_id())
	arg = resolve_arg(it_repl->get_value(), true);

      if (arg) {
	if (!last_was_ws_or_empty) {
	  result.emplace_back(pp_token::type::empty, std::string{},
			      invocation_range);
	}

	for (const auto &arg_tok : *arg) {
	  result.emplace_back(arg_tok.get_type(), arg_tok.get_value(),
			      invocation_range, arg_tok.expansion_history(),
			      arg_tok.get_passed_through_raw_token());
	}

	if (!arg->empty() && !arg->back().is_empty()) {
	  result.emplace_back(pp_token::type::empty, std::string{},
			      invocation_range);
	}
	last_was_ws_or_empty = true;

      } else {
	result.emplace_back(it_repl->get_type(), it_repl->get_value(),
			    invocation_range);
	last_was_ws_or_empty = it_repl->is_ws();

      }
    }
  }

  // Terminate the macro expansion with an empty token.
  if (!last_was_ws_or_empty) {
    result.emplace_back(pp_token::type::empty, std::string{},
			invocation_range);
  }

  for (auto &tok : result) {
    tok.set_macro_invocation(&mi);
    tok.expansion_history() += m;
  }

  mi._used_macros += m;

  return result;
}

std::vector<preprocessor::_pp_token> preprocessor::_macro_expander::
_expand_builtin_special(preprocessor &preprocessor,
			const pp_result::macro &m,
			const raw_pp_token_index id_tok)
{
  std::vector<preprocessor::_pp_token> result;
  const raw_pp_tokens_range invocation_range{id_tok};

  if (m.get_name() == "__FILE__") {
    const auto &source =
      preprocessor._raw_pp_tokens_range_to_source(invocation_range);

    result.emplace_back(pp_token::type::str, source.get_filename(),
			invocation_range, pp_result::used_macros{});

  } else if (m.get_name() == "__BASE_FILE__") {
    result.emplace_back(pp_token::type::str, preprocessor._base_file,
			invocation_range, pp_result::used_macros{});

  } else if (m.get_name() == "__LINE__") {
      const auto &source =
	preprocessor._raw_pp_tokens_range_to_source(invocation_range);
      const auto &range_in_file =
	preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range);
      const auto line = source.offset_to_line_col(range_in_file.begin).first;

      result.emplace_back(pp_token::type::pp_number, std::to_string(line),
			  invocation_range, pp_result::used_macros{});

  } else if (m.get_name() == "__INCLUDE_LEVEL__") {
    result.emplace_back(pp_token::type::pp_number,
			std::to_string(preprocessor._tokenizers.size() - 1),
			invocation_range, pp_result::used_macros{});

  } else if (m.get_name() == "__COUNTER__") {
    result.emplace_back(pp_token::type::pp_number,
			std::to_string(preprocessor.__counter__++),
			invocation_range, pp_result::used_macros{});

  } else {
      assert(0);
      __builtin_unreachable();
    }

  return result;
}

std::string preprocessor::_macro_expander::
_stringify(const std::vector<_pp_token> &arg_toks)
{
  // Concatenate the stringified argument tokens
  std::string s;
  for (auto &&it_tok = arg_toks.begin(); it_tok != arg_toks.end(); ++it_tok)
    s += it_tok->stringify();

  // And escape '\' and '"' characters
  for (auto &&it = s.begin(); it != s.end();) {
    if (*it == '"' || *it == '\\') {
      it = s.insert(it, '\\');
      it += 2;
    } else {
      ++it;
    }
  }

  return s;
}

preprocessor::_pp_token preprocessor::_macro_expander::
_concat(_pp_token &&tok1, _pp_token &&tok2,
	preprocessor &preprocessor,
	const raw_pp_tokens_range &invocation_range,
	code_remarks &remarks)
{
  assert(tok1.get_type() != pp_token::type::ws);
  assert(tok2.get_type() != pp_token::type::ws);
  assert(tok1.get_type() != pp_token::type::newline);
  assert(tok2.get_type() != pp_token::type::newline);
  assert(tok1.get_type() != pp_token::type::eof);
  assert(tok2.get_type() != pp_token::type::eof);
  assert(tok1.get_type() != pp_token::type::qstr);
  assert(tok2.get_type() != pp_token::type::qstr);
  assert(tok1.get_type() != pp_token::type::hstr);
  assert(tok2.get_type() != pp_token::type::hstr);

  if (tok1.get_type() == pp_token::type::empty) {
    tok2.expansion_history() += tok1.expansion_history();
    return std::move(tok2);
  } else if (tok2.get_type() == pp_token::type::empty) {
    tok1.expansion_history() += tok2.expansion_history();
    return std::move(tok1);
  }

  pp_result::used_macros eh = tok1.expansion_history();
  eh += tok2.expansion_history();
  if (tok1.get_type() == pp_token::type::id &&
      tok2.get_type() == pp_token::type::pp_number) {
    if (!std::all_of(tok2.get_value().cbegin(), tok2.get_value().cend(),
		     [](const char c) {
		       return (c == '_' || ('a' <= c && c <= 'z') ||
			       ('A' <= c && c <= 'Z') ||
			       ('0' <= c && c <= '9'));
		     })) {
      code_remark remark
	(code_remark::severity::fatal,
	 "can't concatenate " + tok1.get_value() + " and " + tok2.get_value(),
	 preprocessor._raw_pp_tokens_range_to_source(invocation_range),
	 preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range));
      remarks.add(remark);
      throw pp_except(remark);
    }

    return _pp_token(pp_token::type::id,
		     tok1.get_value() + tok2.get_value(),
		     invocation_range, std::move(eh));

  } else if (tok1.get_type() == pp_token::type::pp_number &&
	     tok2.get_type() == pp_token::type::id) {
    return _pp_token(pp_token::type::pp_number,
		     tok1.get_value() + tok2.get_value(),
		     invocation_range, std::move(eh));

  } else if (tok1.get_type() == pp_token::type::pp_number &&
	     tok2.get_type() == pp_token::type::punctuator &&
	     (tok2.get_value() == "-" || tok2.get_value() == "+")) {
    return _pp_token(pp_token::type::pp_number,
		     tok1.get_value() + tok2.get_value(),
		     invocation_range, std::move(eh));
  }

  if (tok1.get_type() != tok2.get_type()) {
    code_remark remark
      (code_remark::severity::fatal,
       "can't concatenate tokens of different type",
       preprocessor._raw_pp_tokens_range_to_source(invocation_range),
       preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range));
    remarks.add(remark);
    throw pp_except(remark);
  } else if(tok1.get_type() == pp_token::type::chr ||
	    tok1.get_type() == pp_token::type::wchr) {
    code_remark remark
      (code_remark::severity::fatal,
       "can't concatenate character constants",
       preprocessor._raw_pp_tokens_range_to_source(invocation_range),
       preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range));
    remarks.add(remark);
    throw pp_except(remark);
  } else if (tok1.get_type() == pp_token::type::non_ws_char) {
    code_remark remark
      (code_remark::severity::fatal,
       "can't concatenate " + tok1.get_value() + " and " + tok2.get_value(),
       preprocessor._raw_pp_tokens_range_to_source(invocation_range),
       preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range));
    remarks.add(remark);
    throw pp_except(remark);
  }

  std::string value = tok1.get_value() + tok2.get_value();
  if (tok1.get_type() == pp_token::type::punctuator) {
    if (value != "->" && value != "++" && value != "--" && value != "<<" &&
	value != ">>" && value != "<=" && value != ">=" && value != "==" &&
	value != "!=" && value != "&&" && value != "||" && value != "*=" &&
	value != "/=" && value != "%=" && value != "+=" && value != "-=" &&
	value != "<<=" && value != ">>=" && value != "&=" &&
	value != "^=" && value != "|=" && value != "##" && value != "<:" &&
	value != ":>" && value != "<%" && value != "%>" && value != "%:" &&
	value != "%:%:") {
      code_remark remark
	(code_remark::severity::fatal,
	 "can't concatenate " + tok1.get_value() + " and " + tok2.get_value(),
	 preprocessor._raw_pp_tokens_range_to_source(invocation_range),
	 preprocessor._raw_pp_tokens_range_to_range_in_file(invocation_range));
      remarks.add(remark);
      throw pp_except(remark);
    }
  }

  return _pp_token(tok1.get_type(), std::move(value),
		   invocation_range, std::move(eh));
}


const pp_result::header_inclusion_node&
preprocessor::get_pending_token_source(const raw_pp_token_index tok)
{
  _fixup_inclusion_node_ranges();
  if (tok == _pp_result->get_raw_tokens().size()) {
    // Virtual EOF token.
    assert(!_pp_result->get_header_inclusion_roots().empty());
    return *(_pp_result->get_header_inclusion_roots().end() - 1);
  }
  return _pp_result->get_raw_token_source(tok);
}

preprocessor::_cond_incl_state::
_cond_incl_state(pp_result::conditional_inclusion_node &_incl_node)
  : incl_node(_incl_node), branch_active(false), taken_branch(0),
    has_else_branch(false)
{}


preprocessor::_expansion_state::_expansion_state() = default;
