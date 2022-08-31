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
#include "pp_result.hh"
#include "raw_pp_token.hh"
#include "source_reader.hh"
#include <limits>

using namespace klp::ccp;


pp_result::directive::directive(const raw_pp_tokens_range &directive_range)
  noexcept
  : _directive_range(directive_range)
{}


pp_result::macro::macro(const std::string &name,
			const bool func_like,
			const bool variadic,
			std::vector<std::string> &&arg_names,
			raw_pp_tokens &&repl,
			const raw_pp_tokens_range &directive_range)
  : _name(name), _arg_names(std::move(arg_names)),
    _repl(std::move(repl)),
    _o(_origin::o_source),
    _directive_range(directive_range),
    _func_like(func_like),
    _variadic(variadic)
{
  _init_do_expand_args();
}

pp_result::macro::macro(const std::string &name,
			const unsigned long predefinition_pos,
			const builtin_special_tag&)
  : _name(name),
    _o(_origin::o_builtin_special),
    _predefinition_pos(predefinition_pos),
    _func_like(false),
    _variadic(false)
{}

pp_result::macro::macro(const std::string &name,
			std::vector<std::string> &&arg_names,
			const unsigned long predefinition_pos,
			const builtin_special_tag&)
  : _name(name), _arg_names(std::move(arg_names)),
    _o(_origin::o_builtin_special),
    _predefinition_pos(predefinition_pos),
    _func_like(true),
    _variadic(false)
{}

pp_result::macro::macro(const std::string &name,
			const bool func_like,
			const bool variadic,
			std::vector<std::string> &&arg_names,
			raw_pp_tokens &&repl,
			const unsigned long predefinition_pos,
			const builtin_tag&)
  : macro(name, func_like, variadic, std::move(arg_names), std::move(repl),
	  predefinition_pos, _origin::o_builtin)
{}

pp_result::macro::macro(const std::string &name,
	      const bool func_like,
	      const bool variadic,
	      std::vector<std::string> &&arg_names,
	      raw_pp_tokens &&repl,
	      const unsigned long predefinition_pos,
	      const predefined_user_tag&)
  : macro(name, func_like, variadic, std::move(arg_names), std::move(repl),
	  predefinition_pos, _origin::o_predefined_user)
{}

pp_result::macro::macro(const std::string &name,
			const bool func_like,
			const bool variadic,
			std::vector<std::string> &&arg_names,
			raw_pp_tokens &&repl,
			const unsigned long predefinition_pos,
			const _origin o)
  : _name(name), _arg_names(std::move(arg_names)), _repl(std::move(repl)),
    _o(o), _predefinition_pos(predefinition_pos),
    _func_like(func_like), _variadic(variadic)
{
  assert(this->is_predefined());
  _init_do_expand_args();
}

bool pp_result::macro::operator==(const macro &rhs) const noexcept
{
  if (!this->is_builtin_special() && !rhs.is_builtin_special()) {
    return (_name == rhs._name &&
	    _func_like == rhs._func_like &&
	    _arg_names == rhs._arg_names &&
	    _variadic == rhs._variadic &&
	    _repl == rhs._repl);
  } else {
    return (this->is_builtin_special() && rhs.is_builtin_special() &&
	    _name == rhs._name);
  }
}

const std::vector<std::string>& pp_result::macro::get_arg_names() const noexcept
{
  return _arg_names;
}

size_t pp_result::macro::non_va_arg_count() const noexcept
{
  assert(_func_like);
  if (_variadic)
    return _do_expand_args.size() - 1;

  return _do_expand_args.size();
}

bool pp_result::macro::shall_expand_arg(const size_t pos) const noexcept
{
  assert(_func_like);
  assert(pos <= _do_expand_args.size());
  return _do_expand_args[pos];
}

void pp_result::macro::_init_do_expand_args()
{
  // Due to the restriction that a ## concatenated token must again
  // yield a valid preprocessing token, macro evaluation can fail and
  // thus yield an error. Hence macro arguments must not get macro
  // expanded if not needed. An argument needs to get expanded if it
  // appears in the replacement list, ignoring operands of ## and
  // #. Determine those.
  std::set<std::string> do_expand;

  auto &&is_stringification =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      return _func_like && it_repl->is_punctuator("#");
    };

  auto &&skip_ws =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      if (it_repl != _repl.cend() && it_repl->is_ws()) {
	// No whitespace at beginning or end of replacement list.
	assert(it_repl + 1 != _repl.cend());
	// No multiple consecutive whitespace tokens in replacement
	// list.
	assert(!(it_repl + 1)->is_ws());
	return it_repl + 1;
      }
      return it_repl;
    };

  auto &&advance_it_repl =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      assert(it_repl != _repl.cend());
      if (is_stringification(it_repl)) {
	assert(skip_ws(it_repl + 1) != _repl.cend());
	assert(skip_ws(it_repl + 1)->is_id());
	return skip_ws(skip_ws(it_repl + 1) + 1);
      }

      return skip_ws(it_repl + 1);
    };

  auto &&is_vaargs =
    [&](const raw_pp_tokens::const_iterator &it_repl) {
      return (_variadic && it_repl->is_id() &&
	      it_repl->get_value() == _arg_names.back());
    };

  for (raw_pp_tokens::const_iterator it = _repl.cbegin(), it_next_no_ws;
       it != _repl.cend(); it = it_next_no_ws) {
    it_next_no_ws = advance_it_repl(it);

    if (it_next_no_ws != _repl.cend() && it_next_no_ws->is_punctuator("##")) {
      do {
	auto it_next_concat_tok = skip_ws(it_next_no_ws + 1);
	assert(it_next_concat_tok != _repl.cend());

	// Take care of the special
	//  , ## __VA_ARGS__
	// construct.
	if (it->is_punctuator(",") && is_vaargs(it_next_concat_tok)) {
	  const auto it_repl_lookahead = skip_ws(it_next_concat_tok + 1);

	  if (it_repl_lookahead == _repl.cend() ||
	      !it_repl_lookahead->is_punctuator("##")) {
	    it_next_no_ws = it_next_concat_tok; // Continue at __VA_ARGS__
	    break;
	  }
	}

	it = it_next_concat_tok;
	if (it->is_punctuator("##")) {
	  // Multiple consecutive ## tokens.
	  it_next_no_ws = it;
	} else {
	  it_next_no_ws = advance_it_repl(it);
	}
      } while (it_next_no_ws != _repl.cend() &&
	       it_next_no_ws->is_punctuator("##"));

    } else if (it->is_id()) {
      if (std::find(_arg_names.cbegin(), _arg_names.cend(), it->get_value())
	  != _arg_names.cend()) {
	do_expand.insert(it->get_value());
      }

    }
  }

  _do_expand_args.resize(_arg_names.size(), false);
  for (size_t i = 0; i < _arg_names.size(); ++i) {
    if (do_expand.count(_arg_names[i]))
      _do_expand_args[i] = true;
  }
}


pp_result::macro_undef::macro_undef(const std::string &name,
				    const raw_pp_tokens_range &directive_range)
  : _name(name), _o(_origin::o_source), _directive_range(directive_range)
{}

pp_result::macro_undef::macro_undef(const std::string &name,
				    const unsigned long predefinition_pos,
				    const predefined_user_tag&)
  : _name(name), _o(_origin::o_predefined_user),
    _predefinition_pos(predefinition_pos)
{}

pp_result::used_macros::used_macros(_used_macros_type &&um)
  : _used_macros(std::move(um))
{}

void pp_result::used_macros::clear() noexcept
{
  _used_macros.clear();
}

std::size_t pp_result::used_macros::count(const macro &m) const noexcept
{
  return _used_macros.count(std::ref(m));
}

pp_result::used_macros&
pp_result::used_macros::operator+=(const used_macros &rhs)
{
  _used_macros.insert(rhs._used_macros.cbegin(),
		      rhs._used_macros.cend());
  return *this;
}

pp_result::used_macros&
pp_result::used_macros::operator+=(const macro &rhs)
{
  _used_macros.insert(std::ref(rhs));
  return *this;
}

bool pp_result::used_macros::_compare::
operator()(const std::reference_wrapper<const macro> a,
	   const std::reference_wrapper<const macro> b) const noexcept
{
  return &a.get() < &b.get();
}


pp_result::macro_nondef_constraint::
macro_nondef_constraint(const std::string &id, bool func_like_allowed)
  : _id(id), _func_like_allowed(func_like_allowed)
{}

pp_result::macro_nondef_constraints::
macro_nondef_constraints(macro_nondef_constraints &&mnc)
  : _macro_nondef_constraints(std::move(mnc._macro_nondef_constraints))
{}

bool pp_result::macro_nondef_constraint::
operator<(const macro_nondef_constraint &rhs)
  const
{
  return this->_id < rhs._id;
}

pp_result::macro_nondef_constraints&
pp_result::macro_nondef_constraints::operator=(macro_nondef_constraints &&rhs)
{
  if (this == &rhs)
    return *this;

  this->_macro_nondef_constraints = std::move(rhs._macro_nondef_constraints);
  return *this;
}

pp_result::macro_nondef_constraints& pp_result::macro_nondef_constraints::
operator+=(const macro_nondef_constraints &rhs)
{
  for (auto &mnc : rhs._macro_nondef_constraints)
    *this += mnc;
  return *this;
}

pp_result::macro_nondef_constraints& pp_result::macro_nondef_constraints::
operator+=(const macro_nondef_constraint &rhs)
{
  auto it = _macro_nondef_constraints.find(rhs);

  if (it != _macro_nondef_constraints.end()) {
    if (it->is_func_like_allowed() && !rhs.is_func_like_allowed()) {
      it = _macro_nondef_constraints.erase(it);
      _macro_nondef_constraints.insert(it, rhs);
    }
  } else {
      _macro_nondef_constraints.insert(rhs);
  }

  return *this;
}

pp_result::macro_nondef_constraints::const_iterator
pp_result::macro_nondef_constraints::find(const std::string &id) const noexcept
{
  return const_iterator{
    _macro_nondef_constraints.find(id)
  };
}


pp_result::macro_invocation::
macro_invocation(const macro &m,
		 const raw_pp_tokens_range &invocation_range,
		 const std::size_t src_length)
  : _invocation_range{invocation_range},
    _expansion_forest(m, invocation_range.begin, src_length)
{
  _used_macros += m;
}

const pp_result::macro_invocation::passed_through_arg_token*
pp_result::macro_invocation::
lookup_passed_through_arg_token(const pp_token_index tok) const noexcept
{
  for (const auto &pta : _passed_through_arg_tokens) {
    for (const auto emerged_tok : pta.get_emerged_tokens()) {
      if (emerged_tok == tok)
	return &pta;
    }
  }
  return nullptr;
}

void pp_result::macro_invocation::
_add_passed_through_macro_arg_token(const raw_pp_token_index arg_tok_raw,
				    const pp_token_index emerged_token)
{
  auto it_pta =
    std::lower_bound(_passed_through_arg_tokens.begin(),
		     _passed_through_arg_tokens.end(),
		     arg_tok_raw);
  if (it_pta == _passed_through_arg_tokens.end() ||
      it_pta->get_arg_tok_raw() != arg_tok_raw) {
    it_pta =
      _passed_through_arg_tokens.insert(it_pta,
					passed_through_arg_token{arg_tok_raw});
  }

  it_pta->_add_emerged_tok(emerged_token);
}

void pp_result::macro_invocation::
_add_non_passthrough_macro_arg_token(const raw_pp_token_index arg_tok_raw)
{
  // Add an impossible emerged pp_token_index for signalling that the
  // specified raw macro argument token had been an operand to the
  // concatenation or stringification operator.
  _add_passed_through_macro_arg_token
    (arg_tok_raw, std::numeric_limits<pp_token_index>::max());
}


pp_result::macro_invocation::passed_through_arg_token::
passed_through_arg_token(const raw_pp_token_index arg_tok_raw) noexcept
  : _arg_tok_raw(arg_tok_raw)
{}

bool pp_result::macro_invocation::passed_through_arg_token::
operator<(const raw_pp_token_index rhs) const noexcept
{
  return this->_arg_tok_raw < rhs;
}

void pp_result::macro_invocation::passed_through_arg_token::
_add_emerged_tok(const pp_token_index tok)
{
  auto it_emerged_tok
    = std::lower_bound(_emerged_tokens.begin(), _emerged_tokens.end(), tok);

  if (it_emerged_tok != _emerged_tokens.end() && *it_emerged_tok == tok)
    return;

  _emerged_tokens.insert(it_emerged_tok, tok);
}


using expansion_forest=
  pp_result::macro_invocation::expansion_forest;

expansion_forest::_forest_nodes::_forest_nodes(const macro &m,
					       const std::size_t src_begin,
					       const std::size_t src_length)
{
  // Add initial root node.
  add_rescan_invocation(size_type::invalid(), m, src_begin, src_length);
}

expansion_forest::_rescan_macro_invocation&
expansion_forest::_forest_nodes::operator[](const size_type n) noexcept
{
  assert(n._n <= _nodes.size());
  return _nodes[n._n];
}

const expansion_forest::_rescan_macro_invocation&
expansion_forest::_forest_nodes::operator[](const size_type n)
  const noexcept
{
  assert(n._n <= _nodes.size());
  return _nodes[n._n];
}


expansion_forest::_forest_nodes::size_type expansion_forest::_forest_nodes::
add_rescan_invocation(const _forest_nodes::size_type rescan_parent,
		      const macro &m,
		      const std::size_t src_begin,
		      const std::size_t src_length)
{
  assert(_nodes.empty() ||
	 (rescan_parent != size_type::invalid() &&
	  rescan_parent._n < _nodes.size()));
  assert(_nodes.empty() ||
	 (*this)[rescan_parent].out_length() > src_begin);
  _nodes.emplace_back(_rescan_macro_invocation{
      rescan_parent, m, src_begin, src_length
    });
  return size_type{_nodes.size() - 1};
}

expansion_forest::_forest_nodes::size_type expansion_forest::_forest_nodes::
add_macro_arg_tree(const _forest_nodes::size_type owner,
		   const std::size_t src_begin,
		   const std::size_t src_length)
{
  assert(owner != size_type::invalid() && owner._n < _nodes.size());
  assert((*this)[owner].src_begin() <= src_begin);
  assert((*this)[owner].src_begin() + (*this)[owner].src_length() >=
	 src_begin + src_length);
  assert(!(*this)[owner].out_length());
  _nodes.emplace_back(_rescan_macro_invocation{src_begin, src_length});
  ++(*this)[owner]._nargs;
  return size_type{_nodes.size() - 1};
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::first_arg_of(const size_type node)
  const noexcept
{
  assert((*this)[node].nargs() > 0);
  assert(node._n != _nodes.size());
  const size_type arg_root{node._n + 1};
  assert((*this)[arg_root].is_rescan_tree_root());

  return arg_root;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::next_arg(const size_type prev_arg_root)
  const noexcept
{
  assert(prev_arg_root != size_type::invalid() &&
	 prev_arg_root._n > 0 && prev_arg_root._n < _nodes.size());
  assert((*this)[prev_arg_root].is_rescan_tree_root());
  const size_type next_arg_root = _rescan_subtree_end(prev_arg_root);
  assert(next_arg_root._n != _nodes.size());
  assert((*this)[next_arg_root].is_rescan_tree_root());

  return next_arg_root;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::arg_owner(const size_type arg_root)
  const noexcept
{
  assert(arg_root != size_type::invalid() &&
	 arg_root._n > 0 && arg_root._n < _nodes.size());
  assert((*this)[arg_root].is_rescan_tree_root());
  _container_type::size_type pending_arg_roots = 1;
  _container_type::const_iterator it;

  it = _nodes.cbegin() + arg_root._n;
  while (it != _nodes.cbegin()) {
    const _container_type::const_iterator nit = it - 1;

    if (nit->nargs() >= pending_arg_roots)
      break;

    pending_arg_roots -= nit->nargs();
    pending_arg_roots += nit->is_rescan_tree_root();

    it = nit;
  }
  assert(it != _nodes.cbegin());

  return size_type{
    static_cast<_container_type::size_type>(it - 1 - _nodes.cbegin())
  };
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
rescan_tree_root(const size_type descendant_node) const noexcept
{
  assert(descendant_node != size_type::invalid());
  size_type node = descendant_node;

  while (!(*this)[node].is_rescan_tree_root())
    node = (*this)[node].get_rescan_parent();

  return node;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
next_rescan_subtree_dfs_pre_node(const size_type subtree_root,
				 const size_type prev_node) const noexcept
{
  const size_type next_node =
    _next_rescan_subtree_dfs_pre_node(subtree_root, prev_node);

  assert(next_node == size_type::invalid() ||
	 _prev_rescan_subtree_dfs_pre_node(next_node) == prev_node);
  return next_node;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
prev_rescan_subtree_dfs_pre_node(const size_type subtree_root,
				 const size_type next_node) const noexcept
{
  const size_type prev_node = _prev_rescan_subtree_dfs_pre_node(next_node);

  assert(prev_node == size_type::invalid() ||
	 (_next_rescan_subtree_dfs_pre_node(subtree_root, prev_node) ==
	  next_node));
  return prev_node;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
last_rescan_subtree_dfs_pre_node(const size_type root_node)
  const noexcept
{
  return _last_rescan_subtree_dfs_pre_node(root_node);
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
_next_rescan_subtree_dfs_pre_node(const size_type subtree_root,
				  const size_type prev_node) const noexcept
{
  assert(subtree_root._n < _nodes.size());
  assert(prev_node != size_type::invalid());

  _container_type::const_iterator it;
  for (it = _nodes.cbegin() + prev_node._n + 1; it != _nodes.cend(); ++it) {
    const auto node_parent = it->get_rescan_parent();
    if (node_parent != size_type::invalid() && node_parent._n <= prev_node._n) {
      if (node_parent._n < subtree_root._n)
	return size_type::invalid();
      break;
    }
  }
  if (it == _nodes.cend())
    return size_type::invalid();

  return size_type{
    static_cast<_container_type::size_type>(it - _nodes.cbegin())
  };
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
_prev_rescan_subtree_dfs_pre_node(const size_type next_node) const noexcept
{
  const size_type next_node_parent = (*this)[next_node].get_rescan_parent();
  return __last_rescan_subtree_dfs_pre_node(next_node_parent, next_node);
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
_last_rescan_subtree_dfs_pre_node(const size_type subtree_root)
  const noexcept
{
  return __last_rescan_subtree_dfs_pre_node(subtree_root,
					    _rescan_subtree_end(subtree_root));
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
__last_rescan_subtree_dfs_pre_node(const size_type subtree_root,
				   const size_type subtree_end)
  const noexcept
{
  assert(subtree_end._n <= _nodes.size());
  assert(subtree_end._n > subtree_root._n);
  size_type node = size_type{subtree_end._n - 1};
  size_type node_ancestor_or_self = node;
  while (node_ancestor_or_self._n != 0 &&
	 node_ancestor_or_self != subtree_root) {
    if ((*this)[node_ancestor_or_self].is_rescan_tree_root()) {
      assert(node_ancestor_or_self._n > 0);
      node = size_type{node_ancestor_or_self._n - 1};
      node_ancestor_or_self = node;
    } else {
      node_ancestor_or_self =
	(*this)[node_ancestor_or_self].get_rescan_parent();
    }
  }

  return node;
}

const expansion_forest::_forest_nodes::size_type
expansion_forest::_forest_nodes::
_rescan_subtree_end(const size_type subtree_root) const noexcept
{
  assert(subtree_root._n < _nodes.size());
  // Find the position right after the last node from the subtree
  // located at subtree_root or from any of the argument trees
  // (recursively) referenced therefrom.
  // Optimization for the case subtree_root refer's to the initial
  // rescan_tree's root.
  if (!subtree_root._n)
    return size_type{_nodes.size()};

  _container_type::size_type skip_arg_roots = (*this)[subtree_root].nargs();
  _container_type::const_iterator it;
  for (it = _nodes.cbegin() + subtree_root._n + 1; it != _nodes.cend(); ++it) {
    const bool is_arg_root = it->is_rescan_tree_root();

    // Per the DFS pre serialization order, subtree_root's (rescan
    // tree) next sibling, if any, will have its parent located before
    // subtree_root.
    if (!skip_arg_roots &&
	(is_arg_root || it->get_rescan_parent()._n < subtree_root._n)) {
      break;
    }
    assert(is_arg_root || it->get_rescan_parent()._n >= subtree_root._n);

    skip_arg_roots -= is_arg_root;
    skip_arg_roots += it->nargs();
  }

  return size_type{
    static_cast<_container_type::size_type>(it - _nodes.cbegin())
  };
}


expansion_forest::_rescan_macro_invocation::
_rescan_macro_invocation
(const _forest_nodes::size_type rescan_parent, const macro &m,
 const std::size_t src_begin, const std::size_t src_length)
  : _rescan_parent(rescan_parent), _nargs(0),
    _src_begin(src_begin), _src_length(src_length), _m(&m)
{}

expansion_forest::_rescan_macro_invocation::
_rescan_macro_invocation(const std::size_t src_begin,
			 const std::size_t src_length)
  : _rescan_parent(_forest_nodes::size_type::invalid()), _nargs(0),
    _src_begin(src_begin), _src_length(src_length), _m(nullptr)
{}

std::size_t expansion_forest::_rescan_macro_invocation::out_length()
  const noexcept
{
  // The roots for argument expansions are dummy identity mappings.
  if (is_arg_tree_root())
    return _src_length;

  return _output.out_length();
}

bool expansion_forest::_rescan_macro_invocation::
is_rescan_tree_root() const noexcept
{
  return _rescan_parent == _forest_nodes::size_type::invalid();
}

bool expansion_forest::_rescan_macro_invocation::is_arg_tree_root()
  const noexcept
{
  assert(_m || is_rescan_tree_root());
  return !_m;
}

expansion_forest::_forest_nodes::size_type
expansion_forest::_rescan_macro_invocation::get_rescan_parent() const noexcept
{
  return _rescan_parent;
}

const pp_result::macro& expansion_forest::_rescan_macro_invocation::
get_macro() const noexcept
{
  assert(_m);
  return *_m;
}


expansion_forest::_rescan_macro_invocation_ref::
_rescan_macro_invocation_ref(const _forest_nodes &forest,
			     const _forest_nodes::size_type node) noexcept
  : _forest(forest), _node(node)
{}

bool expansion_forest::_rescan_macro_invocation_ref::
operator==(const _rescan_macro_invocation_ref &rhs)
  const noexcept
{
  return &_forest.get() == &rhs._forest.get() && _node == rhs._node;
}

const expansion_forest::_rescan_macro_invocation&
expansion_forest::_rescan_macro_invocation_ref::deref() const noexcept
{
  return _forest.get()[_node];
}

expansion_forest::expansion_forest(const macro &m,
				   const std::size_t src_begin,
				   const std::size_t src_length)
  : _forest(m, src_begin, src_length)
{}


expansion_forest::rescan_output_segment::
rescan_output_segment(const _rescan_macro_invocation_ref &emitting_node,
		      const std::size_t begin,
		      const std::size_t length) noexcept
  : _emitting_node(emitting_node), _begin(begin),
    _length(length)
{}


expansion_forest::rescan_macro_invocation::
rescan_macro_invocation(const _rescan_macro_invocation_ref &ref) noexcept
  : _ref(ref)
{}

bool expansion_forest::rescan_macro_invocation::
operator==(const rescan_macro_invocation &rhs) const noexcept
{
  return _ref == rhs._ref;
}

expansion_forest::rescan_subtree::
rescan_subtree(const _rescan_macro_invocation_ref &subtree_root) noexcept
  : _subtree_root(subtree_root)
{}


pp_result::inclusion_node::inclusion_node()
  : _parent(nullptr)
{}

pp_result::inclusion_node::inclusion_node(inclusion_node * const parent,
					  const raw_pp_token_index range_begin)
  : _parent(parent), _range(range_begin, range_begin)
{}

pp_result::inclusion_node::~inclusion_node() noexcept = default;

pp_result::inclusion_node::recursive_header_inclusion_iterator
pp_result::inclusion_node::header_inclusions_recursive_begin() const
{
  return recursive_header_inclusion_iterator{_recursive_child_iterator{*this}};
}


expansion_forest::rescan_macro_invocation::output::fragment::repl_ref::
repl_ref() noexcept
  : _begin(std::numeric_limits<std::size_t>::max()), _length(0)
{}

expansion_forest::rescan_macro_invocation::output::fragment::repl_ref::
repl_ref(const std::size_t begin) noexcept
  : _begin(begin), _length(1)
{}

expansion_forest::rescan_macro_invocation::output::fragment::repl_ref::
repl_ref(const std::size_t begin, const std::size_t length) noexcept
  : _begin(begin), _length(length)
{}

void expansion_forest::rescan_macro_invocation::output::fragment::repl_ref::
_extend(const std::size_t tail_begin) noexcept
{
  assert(_begin + _length == tail_begin);
  ++_length;
}


expansion_forest::rescan_macro_invocation::output::fragment::arg_ref::
arg_ref(const std::size_t arg_index) noexcept
  : _arg_index(arg_index)
{}


expansion_forest::rescan_macro_invocation::output::fragment::stringification::
stringification(const arg_ref &arg) noexcept
  : _arg(arg)
{}


expansion_forest::rescan_macro_invocation::output::fragment::concat_sequence::
component::component(repl_ref &&repl, const repl_tag&) noexcept
  : _op(op::repl), _repl(std::move(repl))
{}

expansion_forest::rescan_macro_invocation::output::fragment::concat_sequence::
component::component(repl_ref &&repl, arg_ref &&arg, const arg_tag&) noexcept
  : _op(op::arg), _repl(std::move(repl)), _arg(std::move(arg))
{}

expansion_forest::rescan_macro_invocation::output::fragment::concat_sequence::
component::component(repl_ref &&repl, stringification &&s,
		     const stringification_tag&) noexcept
  : _op(op::stringification), _repl(std::move(repl)),
    _stringification(std::move(s))
{}

expansion_forest::rescan_macro_invocation::output::fragment::concat_sequence::
component::component(repl_ref &&repl, const bool skipped,
		     const conditional_comma_tag&) noexcept
  : _op(op::conditional_comma), _repl(std::move(repl)),
    _conditional_comma_skipped(skipped)
{}


void
expansion_forest::rescan_macro_invocation::output::fragment::concat_sequence::
_extend(component &&c)
{
  _components.emplace_back(std::move(c));
}


expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder::
_concat_builder(fragment &frag) noexcept
  : _frag(frag)
{
  assert(_frag.get()._op == op::concat);
}

void
expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder::
add_component(const std::size_t repl_begin, const repl_tag&)
{
  auto &frag = _frag.get();
  auto &c = frag._concat_sequence;

  c._extend(concat_sequence::component{
      repl_ref{repl_begin}, repl_tag{}
    });

  if (!frag._out_length)
    frag._out_length = 1;

  frag._repl._length = repl_begin + 1 - frag._repl._begin;
}

void
expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder::
add_component(const std::size_t repl_begin, const std::size_t arg_index,
	      const std::size_t arg_length, const arg_tag&)
{
  auto &frag = _frag.get();
  auto &c = frag._concat_sequence;

  c._extend(concat_sequence::component{
      repl_ref{repl_begin}, arg_ref{arg_index}, arg_tag{}
    });

  if (arg_length) {
    if (!frag._out_length)
      frag._out_length = 1;

    frag._out_length += arg_length - 1;
  }

  frag._repl._length = repl_begin + 1 - frag._repl._begin;
}

void
expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder::
add_component(const std::size_t repl_begin, const std::size_t repl_end,
	      const std::size_t arg_index, const stringification_tag&) noexcept
{
  auto &frag = _frag.get();
  auto &c = frag._concat_sequence;

  c._extend(concat_sequence::component{
      repl_ref{repl_begin, repl_end - repl_begin},
      stringification{arg_ref{arg_index}}, stringification_tag{}
    });

  if (!frag._out_length)
    frag._out_length = 1;

  frag._repl._length = repl_end - frag._repl._begin;
}

void
expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder::
add_component(const std::size_t repl_begin, const std::size_t repl_end,
	      const bool skipped, const conditional_comma_tag&) noexcept
{
  auto &frag = _frag.get();
  auto &c = frag._concat_sequence;

  c._extend(concat_sequence::component{
      repl_ref{repl_begin, repl_end - repl_begin}, skipped,
      conditional_comma_tag{}
    });

  if (!frag._out_length && !skipped)
    frag._out_length = 1;

  frag._repl._length = repl_end - frag._repl._begin;
}


expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t repl_begin, const std::size_t out_begin,
	 const repl_tag&)
  : _op(op::repl), _out_begin(out_begin), _out_length(1),
    _repl(repl_begin)
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t arg_ref_repl_pos,
	 const std::size_t out_begin, const std::size_t out_length,
	 const std::size_t arg_index, const arg_tag&)
  : _op(op::arg), _out_begin(out_begin), _out_length(out_length),
    _repl(arg_ref_repl_pos), _arg(arg_index)
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t repl_begin, const std::size_t repl_length,
	 const std::size_t out_begin, const std::size_t arg_index,
	 const stringification_tag&)
  : _op(op::stringification), _out_begin(out_begin), _out_length(1),
    _repl(repl_begin, repl_length), _stringification(arg_ref{arg_index})
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t repl_begin, const std::size_t out_begin,
	 const concat_tag&)
  : _op(op::concat), _out_begin(out_begin), _out_length(0),
    _repl(repl_begin), _concat_sequence{}
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t repl_begin, const std::size_t repl_length,
	 const std::size_t out_begin, const bool skipped,
	 const conditional_comma_tag&)
  : _op(op::conditional_comma), _out_begin(out_begin), _out_length(!skipped),
    _repl(repl_begin, repl_length)
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t out_begin, const empty_tag&)
  : _op(op::empty), _out_begin(out_begin), _out_length(1)
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(const std::size_t out_begin, const std::size_t out_length,
	 const builtin_special_tag&)
  : _op(op::builtin_special), _out_begin(out_begin), _out_length(out_length)
{}

expansion_forest::rescan_macro_invocation::output::fragment::
fragment(fragment &&seg)
  : _op(seg._op), _out_begin(seg._out_begin), _out_length(seg._out_length),
    _repl(seg._repl)
{
  switch (_op) {
  case op::repl:
    break;

  case op::arg:
    new (&_arg) arg_ref(std::move(seg._arg));
    break;

  case op::stringification:
    new (&_stringification) stringification(std::move(seg._stringification));
    break;

  case op::concat:
    new (&_concat_sequence) concat_sequence(std::move(seg._concat_sequence));
    break;

  case op::conditional_comma:
    break;

  case op::empty:
    break;

  case op::builtin_special:
    break;
  }
}

expansion_forest::rescan_macro_invocation::output::fragment::
~fragment() noexcept
{
  switch (_op) {
  case op::repl:
    break;

  case op::arg:
    _arg.~arg_ref();
    break;

  case op::stringification:
    _stringification.~stringification();
    break;

  case op::concat:
    _concat_sequence.~concat_sequence();
    break;

  case op::conditional_comma:
    break;

  case op::empty:
    break;

  case op::builtin_special:
    break;
  }
}



expansion_forest::rescan_macro_invocation::output::output() noexcept
  = default;

expansion_forest::rescan_macro_invocation::output::output(output &&) noexcept
  = default;

expansion_forest::rescan_macro_invocation::output::~output() noexcept
  = default;

std::size_t expansion_forest::rescan_macro_invocation::output::out_length()
  const noexcept
{
  return _next_fragment_out_begin();
}

std::size_t
expansion_forest::rescan_macro_invocation::output::_next_fragment_out_begin()
  const noexcept
{
  if (_fragments.empty())
    return 0;

  const auto &last = _fragments.back();
  return last._out_begin + last._out_length;
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t repl_begin, const fragment::repl_tag&)
{
  // If the last fragment added had been of type op::repl already, extend it.
  if (!_fragments.empty() && _fragments.back()._op == fragment::op::repl) {
    auto &last = _fragments.back();
    assert(last._repl.begin() + last._repl.length() == repl_begin);
    ++last._repl._length;
    ++last._out_length;
    return;
  }

  _fragments.emplace_back(fragment{
      repl_begin, _next_fragment_out_begin(), fragment::repl_tag{}
    });
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t arg_ref_repl_pos, const std::size_t arg_index,
	      const std::size_t out_length, const fragment::arg_tag&)
{
  _fragments.emplace_back(fragment{
      arg_ref_repl_pos, _next_fragment_out_begin(), out_length, arg_index,
      fragment::arg_tag{}
    });
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t repl_begin, const std::size_t repl_end,
	      const std::size_t arg_index, const fragment::stringification_tag&)
{
  _fragments.emplace_back(fragment{
      repl_begin, repl_end - repl_begin, _next_fragment_out_begin(), arg_index,
      fragment::stringification_tag{}
    });
}

expansion_forest::rescan_macro_invocation::output::fragment::_concat_builder
expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t repl_begin, const fragment::concat_tag&)
{
  _fragments.emplace_back(fragment{
      repl_begin, _next_fragment_out_begin(), fragment::concat_tag{}
    });
  return fragment::_concat_builder{_fragments.back()};
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t repl_begin, const std::size_t repl_end,
	     const bool skipped,
	     const fragment::conditional_comma_tag&)
{
  _fragments.emplace_back(fragment{
      repl_begin, repl_end - repl_begin, _next_fragment_out_begin(), skipped,
      fragment::conditional_comma_tag{}
    });
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const fragment::empty_tag&)
{
  _fragments.emplace_back(fragment{
      _next_fragment_out_begin(), fragment::empty_tag{}
    });
}

void expansion_forest::rescan_macro_invocation::output::
_add_fragment(const std::size_t out_length,
	      const fragment::builtin_special_tag&)
{
  _fragments.emplace_back(fragment{
      _next_fragment_out_begin(), out_length, fragment::builtin_special_tag{}
    });
}

pp_result::inclusion_node::recursive_header_inclusion_iterator
pp_result::inclusion_node::header_inclusions_recursive_end() const
{
  return recursive_header_inclusion_iterator{};
}


pp_result::header_inclusion_child& pp_result::inclusion_node::
_add_header_inclusion(const std::string &filename,
		      const raw_pp_tokens_range &directive_range,
		      used_macros &&used_macros,
		      macro_nondef_constraints &&macro_nondef_constraints,
		      pp_result &container)
{
  std::unique_ptr<header_inclusion_child> new_child
     (new header_inclusion_child(*this, filename,
				 directive_range,
				 std::move(used_macros),
				 std::move(macro_nondef_constraints),
				 container._next_header_node_id++));
  header_inclusion_child &_new_child = *new_child;
  _children.emplace_back(std::move(new_child));
  return _new_child;
}

pp_result::conditional_inclusion_node& pp_result::inclusion_node::
_add_conditional_inclusion(const raw_pp_token_index range_begin)
{
  std::unique_ptr<conditional_inclusion_node> new_child
     (new conditional_inclusion_node(*this, range_begin));
  conditional_inclusion_node &_new_child = *new_child;
  _children.emplace_back(std::move(new_child));
  return _new_child;
}

void pp_result::inclusion_node::
_set_range_begin(const raw_pp_token_index range_begin) noexcept
{
  _range.begin = range_begin;
}

void pp_result::inclusion_node::
_set_range_end(const raw_pp_token_index range_end) noexcept
{
  _range.end = range_end;
}

pp_result::inclusion_node::_child::
_child(std::unique_ptr<header_inclusion_child> &&_h) noexcept
  : k(kind::header), h(std::move(_h))
{}

pp_result::inclusion_node::_child::
_child(std::unique_ptr<conditional_inclusion_node> &&_c) noexcept
  : k(kind::conditional), c(std::move(_c))
{}

pp_result::inclusion_node::_child::_child(_child &&chld) noexcept
  : k(chld.k), h(nullptr)
{
  switch(k) {
  case kind::header:
    h = std::move(chld.h);
    break;

  case kind::conditional:
    h.~unique_ptr<header_inclusion_child>();
    new (&c) std::unique_ptr<conditional_inclusion_node>{std::move(chld.c)};
    break;
  }
}

pp_result::inclusion_node::_child::~_child() noexcept
{
  switch (k) {
  case kind::header:
    h.~unique_ptr<header_inclusion_child>();
    break;

  case kind::conditional:
    c.~unique_ptr<conditional_inclusion_node>();
    break;
  }
}

const pp_result::inclusion_node&
pp_result::inclusion_node::_child::get_inclusion_node() const noexcept
{
  switch (k) {
  case kind::header:
    return *h;

  case kind::conditional:
    return *c;
  }
}


pp_result::inclusion_node::_recursive_child_iterator::
_recursive_child_iterator() = default;

pp_result::inclusion_node::_recursive_child_iterator::
_recursive_child_iterator(const inclusion_node &n)
{
  if (!n._children.empty()) {
    _walk_stack.push_back(std::make_pair(n._children.begin(),
					 n._children.end()));
  }
}

pp_result::inclusion_node::_recursive_child_iterator&
pp_result::inclusion_node::_recursive_child_iterator::operator++()
{
  // DFS in preorder
  const _children_container_type &children
    = (_walk_stack.back().first->k == _child::kind::header ?
       _walk_stack.back().first->h->_children :
       _walk_stack.back().first->c->_children);

  if (!children.empty()) {
    _walk_stack.push_back(std::make_pair(children.begin(), children.end()));
    return *this;
  }

  ++_walk_stack.back().first;
  while (_walk_stack.back().first == _walk_stack.back().second) {
    _walk_stack.pop_back();
    if (_walk_stack.empty())
      return *this;
    ++_walk_stack.back().first;
  }

  return *this;
}

const pp_result::inclusion_node::_recursive_child_iterator
pp_result::inclusion_node::_recursive_child_iterator::operator++(int)
{
  _recursive_child_iterator it = *this;
  ++*this;
  return it;
}


pp_result::inclusion_node::recursive_header_inclusion_iterator::
recursive_header_inclusion_iterator() = default;

pp_result::inclusion_node::recursive_header_inclusion_iterator::
recursive_header_inclusion_iterator(_recursive_child_iterator &&it)
  : _it(std::move(it))
{
  _skip_non_headers();
}

pp_result::inclusion_node::recursive_header_inclusion_iterator&
pp_result::inclusion_node::recursive_header_inclusion_iterator::operator++()
{
  ++_it;
  _skip_non_headers();
  return *this;
}

const pp_result::inclusion_node::recursive_header_inclusion_iterator
pp_result::inclusion_node::recursive_header_inclusion_iterator::operator++(int)
{
  recursive_header_inclusion_iterator it = *this;
  ++*this;
  return it;
}

void pp_result::inclusion_node::recursive_header_inclusion_iterator::
_skip_non_headers()
{
  while(!_it.is_end() && _it->k != _child::kind::header)
    ++_it;
}


pp_result::header_inclusion_node::
header_inclusion_node(const std::string &filename)
  : _filename(filename), _id(std::numeric_limits<unsigned long>::max())
{}

pp_result::header_inclusion_node::
header_inclusion_node(inclusion_node &parent,
		      const raw_pp_token_index range_begin,
		      const std::string &filename,
		      const unsigned long id)
  : inclusion_node(&parent, range_begin), _filename(filename), _id(id)
{}

pp_result::header_inclusion_node::~header_inclusion_node() noexcept = default;

const pp_result::header_inclusion_node&
pp_result::header_inclusion_node::get_containing_header() const noexcept
{
  return *this;
}

void pp_result::header_inclusion_node::add_line(const std::streamoff length)
{
  _offset_to_line_col_map.add_line(length);
}

std::pair<std::streamoff, std::streamoff>
pp_result::header_inclusion_node::offset_to_line_col(const std::streamoff off)
  const noexcept
{
  return _offset_to_line_col_map.offset_to_line_col(off);
}

std::unique_ptr<source_reader>
pp_result::header_inclusion_node::create_source_reader() const
{
  return std::unique_ptr<source_reader>(new file_source_reader(_filename));
}

unsigned long pp_result::header_inclusion_node::get_id() const noexcept
{
  assert(_id != std::numeric_limits<unsigned long>::max());
  return _id;
}

void pp_result::header_inclusion_node::_set_id(const unsigned long id) noexcept
{
  assert(_id == std::numeric_limits<unsigned long>::max());
  _id = id;
}


pp_result::header_inclusion_root::
header_inclusion_root(const std::string &filename, const bool is_preinclude)
  : header_inclusion_node(filename), _is_preinclude(is_preinclude)
{}

pp_result::header_inclusion_root::~header_inclusion_root() noexcept = default;

const pp_result::conditional_inclusion_node*
pp_result::header_inclusion_root::get_containing_conditional_inclusion()
  const noexcept
{
  return nullptr;
}


pp_result::header_inclusion_child::
header_inclusion_child(inclusion_node &parent,
		       const std::string &filename,
		       const raw_pp_tokens_range &directive_range,
		       used_macros &&used_macros,
		       macro_nondef_constraints &&macro_nondef_constraints,
		       const unsigned long id)
  : header_inclusion_node(parent, directive_range.end, filename, id),
    _directive_range(directive_range),
    _used_macros(std::move(used_macros)),
    _macro_nondef_constraints(std::move(macro_nondef_constraints))
{}

pp_result::header_inclusion_child::~header_inclusion_child() noexcept = default;

const pp_result::conditional_inclusion_node*
pp_result::header_inclusion_child::get_containing_conditional_inclusion()
  const noexcept
{
  return this->get_parent()->get_containing_conditional_inclusion();
}


pp_result::conditional_inclusion_node::
conditional_inclusion_node(inclusion_node &parent,
			   const raw_pp_token_index range_begin)
  : inclusion_node(&parent, range_begin), _taken_branch(0)
{}

pp_result::conditional_inclusion_node::~conditional_inclusion_node() = default;

const pp_result::header_inclusion_node&
pp_result::conditional_inclusion_node::get_containing_header() const noexcept
{
  return _parent->get_containing_header();
}

const pp_result::conditional_inclusion_node*
pp_result::conditional_inclusion_node::get_containing_conditional_inclusion()
  const noexcept
{
  return this;
}

pp_result::conditional_inclusion_node::directive_ranges_type::size_type
pp_result::conditional_inclusion_node::nbranches() const noexcept
{
  return _directive_ranges.size() - 1;
}

const raw_pp_tokens_range& pp_result::conditional_inclusion_node::
get_branch_directive_range(const directive_ranges_type::size_type i)
  const noexcept
{
  assert(i < _directive_ranges.size());
  return _directive_ranges[i];
}

bool pp_result::conditional_inclusion_node::has_taken_branch() const noexcept
{
  return _taken_branch;
}

pp_result::conditional_inclusion_node::directive_ranges_type::size_type
pp_result::conditional_inclusion_node::get_taken_branch() const noexcept
{
  assert(_taken_branch);
  assert(_taken_branch < _directive_ranges.size());
  return _taken_branch - 1;
}

void pp_result::conditional_inclusion_node::
_finalize(const raw_pp_token_index range_end,
	  used_macros &&used_macros,
	  macro_nondef_constraints &&macro_nondef_constraints,
	  directive_ranges_type &&directive_ranges,
	  const directive_ranges_type::size_type taken_branch,
	  const bool has_else_branch)
{
  _set_range_end(range_end);
  _used_macros = std::move(used_macros);
  _macro_nondef_constraints = std::move(macro_nondef_constraints);
  _directive_ranges = std::move(directive_ranges);
  _taken_branch = taken_branch;
  _has_else_branch = has_else_branch;
}


pp_result::header_inclusion_roots::header_inclusion_roots() = default;

pp_result::header_inclusion_roots::
header_inclusion_roots(header_inclusion_roots &&hirs)
  : _roots(std::move(hirs._roots))
{}

pp_result::header_inclusion_roots::~header_inclusion_roots() noexcept = default;

pp_result::header_inclusion_root&
pp_result::header_inclusion_roots::add(const std::string &filename,
				       const bool is_preinclude)
{
  std::unique_ptr<header_inclusion_root> n{
    new header_inclusion_root{filename, is_preinclude}
  };

  _roots.emplace_back(std::move(n));
  return *_roots.back();
}


template <typename node_type>
pp_result::_const_intersecting_child_node_iterator<node_type>::
_const_intersecting_child_node_iterator(const header_inclusion_root &root,
					const raw_pp_tokens_range &range)
  : _range(range), _pos(_range.begin), _cur_val(nullptr)
{
  auto chlds_sr = _intersecting_chlds_subrange(root._children.cbegin(),
					       root._children.end());
  if (chlds_sr.first != chlds_sr.second) {
    // Descend
    _walk_stack.emplace_back(_walk_stack_entry{chlds_sr.first,
			     chlds_sr.first,
			     chlds_sr.second});
    _descend_walk_stack();
    _fwd_walk_stack_to_node_kind();
  }

  if (!_walk_stack.empty()) {
    _cur_val = _find_deepest_covering_pos();
    if (!_cur_val)
      _cur_val = _find_node_after_pos();
    assert(_cur_val);
    _pos = _cur_val->get_range().begin;
  } else {
    _pos = _range.end;
  }
}

template <typename node_type>
pp_result::_const_intersecting_child_node_iterator<node_type>::
_const_intersecting_child_node_iterator(const raw_pp_tokens_range &range)
  noexcept
  : _range(range), _pos(_range.end), _cur_val(nullptr)
{}

template <typename node_type>
bool
pp_result::_const_intersecting_child_node_iterator<node_type>::
operator==(const _const_intersecting_child_node_iterator &rhs) const noexcept
{
  assert(this->_range == rhs._range);
  return (this->_pos == rhs._pos);
}

template <typename node_type>
void pp_result::_const_intersecting_child_node_iterator<node_type>::
advance()
{
  assert(!_walk_stack.empty());
  assert(_walk_stack.back().chlds_it->k == _get_node_kind());

  if (_pos < _access_chld(*_walk_stack.back().chlds_it).get_range().begin) {
    // The current _pos is somewhere to the left of the current
    // leaf node. Find the deepest node of the desired type coming next.
    _cur_val = _find_node_after_pos();
    assert(_cur_val);
    _pos = _cur_val->get_range().begin;

  } else {
    assert(_access_chld(*_walk_stack.back().chlds_it).get_range().begin <=
	   _pos);
    assert(_pos <=
	   _access_chld(*_walk_stack.back().chlds_it).get_range().end);
    const raw_pp_token_index end_pos =
      _walk_stack.back().chlds_it->h->get_range().end;
    if (_range.end <= end_pos) {
      // We're done.
      _pos = _range.end;
      _walk_stack.clear();
      _cur_val = nullptr;
      return;
    }

    _pos = end_pos;

    // Move the walk stack to the next DFS post node of the desired
    // kind not ending at end_pos.
    do {
      if (_walk_stack.back().chlds_it + 1 != _walk_stack.back().chlds_end) {
	++_walk_stack.back().chlds_it;
	_descend_walk_stack();
	_fwd_walk_stack_to_node_kind();
      } else {
	_walk_stack.pop_back();
	if (!_walk_stack.empty())
	  _fwd_walk_stack_to_node_kind();
      }
    } while (!_walk_stack.empty() &&
	     (_access_chld(*_walk_stack.back().chlds_it).get_range().end ==
	      end_pos));

    if (_walk_stack.empty()) {
      _pos = _range.end;
      _walk_stack.clear();
      _cur_val = nullptr;
      return;
    }

    // There are two possible cases now: some of the nodes of the
    // desired kind on the walk stack either overlap with the current
    // _pos or there is a gap at the top level.
    _cur_val = _find_deepest_covering_pos();
    if (!_cur_val) {
      // There aren't any. Advance past the top level gap.
      assert(_pos <
	     _access_chld(*_walk_stack.back().chlds_it).get_range().begin);
      advance();
    }
  }
}

template <>
pp_result::inclusion_node::_child::kind
pp_result::_const_intersecting_child_node_iterator
				<pp_result::header_inclusion_child>::
_get_node_kind() noexcept
{
  return inclusion_node::_child::kind::header;
}

template <>
pp_result::inclusion_node::_child::kind
pp_result::_const_intersecting_child_node_iterator
				<pp_result::conditional_inclusion_node>::
_get_node_kind() noexcept
{
  return inclusion_node::_child::kind::conditional;
}

template <>
const pp_result::header_inclusion_child&
pp_result::_const_intersecting_child_node_iterator
				<pp_result::header_inclusion_child>::
_access_chld(const inclusion_node::_child &c) noexcept
{
  assert(c.k == _get_node_kind());
  return *c.h;
}

template <>
const pp_result::conditional_inclusion_node&
pp_result::_const_intersecting_child_node_iterator
				<pp_result::conditional_inclusion_node>::
_access_chld(const inclusion_node::_child &c) noexcept
{
  assert(c.k == _get_node_kind());
  return *c.c;
}

template <typename node_type>
std::pair<pp_result::inclusion_node::_children_container_type::const_iterator,
	  pp_result::inclusion_node::_children_container_type::const_iterator>
pp_result::_const_intersecting_child_node_iterator<node_type>::
_intersecting_chlds_subrange
(const inclusion_node::_children_container_type::const_iterator &b,
 const inclusion_node::_children_container_type::const_iterator &e) noexcept
{
  const auto srb =
    std::lower_bound(b, e, _range,
		     [](const inclusion_node::_child &c,
			const raw_pp_tokens_range &r) {
		       return (c.get_inclusion_node().get_range() < r);
		     });
  const auto sre =
    std::upper_bound(srb, e, _range,
		     [](const raw_pp_tokens_range &r,
			const inclusion_node::_child &c) {
		       return r < c.get_inclusion_node().get_range();
		     });
  return std::make_pair(srb, sre);
}

template <typename node_type>
void pp_result::_const_intersecting_child_node_iterator<node_type>::
_descend_walk_stack()
{
  assert(!_walk_stack.empty());
  const inclusion_node::_child *cur = &*_walk_stack.back().chlds_it;
  const inclusion_node *n = &cur->get_inclusion_node();
  auto chlds_sr = _intersecting_chlds_subrange(n->_children.cbegin(),
					       n->_children.end());
  while (chlds_sr.first != chlds_sr.second) {
    // Descend
    _walk_stack.emplace_back(_walk_stack_entry{chlds_sr.first,
			     chlds_sr.first,
			     chlds_sr.second});
    cur = &*_walk_stack.back().chlds_it;
    n = &cur->get_inclusion_node();
    chlds_sr = _intersecting_chlds_subrange(n->_children.cbegin(),
					    n->_children.end());
  }
}

template <typename node_type>
void pp_result::_const_intersecting_child_node_iterator<node_type>::
_fwd_walk_stack_to_node_kind()
{
  // Move the walk stack to the next (in DFS post order) node of the
  // desired kind.
  typedef inclusion_node::_child _child;
  assert(!_walk_stack.empty());
  assert(_walk_stack.back().chlds_it != _walk_stack.back().chlds_end);
  while (!_walk_stack.empty() &&
	 _walk_stack.back().chlds_it->k != _get_node_kind()) {
    if (_walk_stack.back().chlds_it + 1 != _walk_stack.back().chlds_end) {
      ++_walk_stack.back().chlds_it;
      _descend_walk_stack();
    } else {
      _walk_stack.pop_back();
    }
  }
}

template <typename node_type>
const node_type *
pp_result::_const_intersecting_child_node_iterator<node_type>::
_find_deepest_covering_pos() noexcept
{
  // Find the deepest node of the desired kind on the walk stack
  // which begins before or at the current _pos.
  auto deepest_covering_pos =
    std::find_if(_walk_stack.crbegin(), _walk_stack.crend(),
		 [&](const _walk_stack_entry &e) {
		   return (e.chlds_it->k == _get_node_kind() &&
			   (_access_chld(*e.chlds_it).get_range().begin
			    <= _pos));
		 });

  if (deepest_covering_pos != _walk_stack.crend())
    return &_access_chld(*deepest_covering_pos->chlds_it);
  else
    return nullptr;
}

template <typename node_type>
const node_type *
pp_result::_const_intersecting_child_node_iterator<node_type>::
_find_node_after_pos() noexcept
{
  // Find the first node of the desired kind on the walk stack
  // which begins after the current _pos. If there are multiple
  // (nested) candidates, take the innermost one.
  const auto topmost_gt_pos =
    std::find_if(_walk_stack.cbegin(), _walk_stack.cend(),
		 [&](const _walk_stack_entry &e) {
		   return (e.chlds_it->k == _get_node_kind() &&
			   (_access_chld(*e.chlds_it).get_range().begin
			    > _pos));
		 });
  assert(topmost_gt_pos != _walk_stack.cend());
  const auto next_pos =
    _access_chld(*topmost_gt_pos->chlds_it).get_range().begin;
  const auto deepest_at_pos =
    std::find_if(_walk_stack.crbegin(), _walk_stack.crend(),
		 [&](const _walk_stack_entry &e) {
		   return (e.chlds_it->k == _get_node_kind() &&
			   (_access_chld(*e.chlds_it).get_range().begin
			    == next_pos));
		 });
  assert(deepest_at_pos != _walk_stack.crend());
  return &_access_chld(*deepest_at_pos->chlds_it);
}


pp_result::const_intersecting_source_iterator::
const_intersecting_source_iterator(const header_inclusion_roots &roots,
				   const raw_pp_tokens_range &range,
				   const init_begin_iterator_tag&)
  : _range(range),
    _roots(_intersecting_roots_subrange(roots.begin(), roots.end())),
    _roots_it(_roots.first),
    _child_it(_range), _max_pos_seen_from_chlds(_range.begin)
{

  if (_roots.first != _roots.second) {
    _child_it =
      _const_intersecting_child_node_iterator<header_inclusion_child> {
	*_roots_it, _range
      };
  } else {
    // Range is an empty range at EOF, special case it.
    --_roots.first;
    _roots_it = _roots.first;
    assert(range.begin == range.end);
    assert(range.begin == _roots.first->get_range().end);
  }
}

pp_result::const_intersecting_source_iterator::
const_intersecting_source_iterator(const header_inclusion_roots &roots,
				   const raw_pp_tokens_range &range,
				   const init_end_iterator_tag&) noexcept
  : _range(range),
    _roots(_intersecting_roots_subrange(roots.begin(), roots.end())),
    _roots_it(_roots.second),
    _child_it(_range), _max_pos_seen_from_chlds(_range.end)
{
  if (_roots.first == roots.end()) {
    // C.f. begin iterator construction
    --_roots.first;
  }
}

bool pp_result::const_intersecting_source_iterator::
operator==(const const_intersecting_source_iterator &rhs) const noexcept
{
  assert(this->_range == rhs._range);
  return (this->_roots_it == rhs._roots_it &&
	  this->_child_it == rhs._child_it);
}

pp_result::const_intersecting_source_iterator::reference
pp_result::const_intersecting_source_iterator::operator*() const noexcept
{
  assert(_roots_it != _roots.second);

  if (_child_it.get_value() &&
      _max_pos_seen_from_chlds >= _child_it.get_value()->get_range().begin) {
    return *_child_it.get_value();
  } else {
    // Current position is within some gap not covered by any child
    // header.
    return *_roots_it;
  }
}

pp_result::const_intersecting_source_iterator&
pp_result::const_intersecting_source_iterator::operator++()
{
  assert(_roots_it != _roots.second);
  if (_child_it.get_value()) {
    if (_max_pos_seen_from_chlds < _child_it.get_value()->get_range().begin) {
      // We're currently within some range not covered by any
      // children. Move on to the next child.
      _max_pos_seen_from_chlds = _child_it.get_value()->get_range().begin;
    } else {
      _max_pos_seen_from_chlds =
	std::max(_child_it.get_value()->get_range().end,
		 _max_pos_seen_from_chlds);
      _child_it.advance();

      if (!_child_it.get_value() &&
	  (_max_pos_seen_from_chlds >= _roots_it->get_range().end ||
	   _max_pos_seen_from_chlds >= _range.end)) {
	  _next_root();
      }
    }
  } else {
    // Current position is within the current root header, after all
    // of its children, if any.
    _next_root();
  }
  return *this;
}

const pp_result::const_intersecting_source_iterator
pp_result::const_intersecting_source_iterator::operator++(int)
{
  const_intersecting_source_iterator it = *this;
  ++*this;
  return it;
}

std::pair<pp_result::header_inclusion_roots::const_iterator,
	  pp_result::header_inclusion_roots::const_iterator>
pp_result::const_intersecting_source_iterator::_intersecting_roots_subrange
			(const header_inclusion_roots::const_iterator &b,
			 const header_inclusion_roots::const_iterator &e)
  noexcept
{
  const auto srb =
    std::lower_bound(b, e, _range,
		     [&](const header_inclusion_root &root,
			 const raw_pp_tokens_range &range) {
		       return root.get_range() < range;
		     });
  const auto sre =
    std::upper_bound(srb, e, _range,
		     [&](const raw_pp_tokens_range &range,
			 const header_inclusion_root &root) {
		       return range < root.get_range();
		     });

  return std::make_pair(srb, sre);
}

void pp_result::const_intersecting_source_iterator::_next_root()
{
  assert(_roots_it != _roots.second);
  ++_roots_it;

  if (_roots_it != _roots.second) {
    _child_it =
      _const_intersecting_child_node_iterator<header_inclusion_child> {
	*_roots_it, _range
      };
    _max_pos_seen_from_chlds = _roots_it->get_range().begin;

  } else {
    _child_it =
      _const_intersecting_child_node_iterator<header_inclusion_child> {
	_range
      };
    _max_pos_seen_from_chlds = _range.end;

  }
}


pp_result::const_intersecting_conditional_iterator::
const_intersecting_conditional_iterator(const header_inclusion_roots &roots,
					const raw_pp_tokens_range &range,
					const init_begin_iterator_tag&)
  : _range(range),
    _roots(_intersecting_roots_subrange(roots.begin(), roots.end())),
    _roots_it(_roots.first),
    _child_it(_range)
{
  _enter_root();
}

pp_result::const_intersecting_conditional_iterator::
const_intersecting_conditional_iterator(const header_inclusion_roots &roots,
					const raw_pp_tokens_range &range,
					const init_end_iterator_tag&)
  noexcept
  : _range(range),
    _roots(_intersecting_roots_subrange(roots.begin(), roots.end())),
    _roots_it(_roots.second),
    _child_it(_range)
{}

bool pp_result::const_intersecting_conditional_iterator::
operator==(const const_intersecting_conditional_iterator &rhs) const noexcept
{
  assert(this->_range == rhs._range);
  return (this->_roots_it == rhs._roots_it &&
	  this->_child_it == rhs._child_it);
}

pp_result::const_intersecting_conditional_iterator::reference
pp_result::const_intersecting_conditional_iterator::operator*() const noexcept
{
  assert(_child_it.get_value());
  return *_child_it.get_value();
}

pp_result::const_intersecting_conditional_iterator&
pp_result::const_intersecting_conditional_iterator::operator++()
{
  assert(_child_it.get_value());
  _child_it.advance();
  if (!_child_it.get_value()) {
    ++_roots_it;
    _enter_root();
  }
  return *this;
}

const pp_result::const_intersecting_conditional_iterator
pp_result::const_intersecting_conditional_iterator::operator++(int)
{
  const_intersecting_conditional_iterator it = *this;
  ++*this;
  return it;
}

std::pair<pp_result::header_inclusion_roots::const_iterator,
	  pp_result::header_inclusion_roots::const_iterator>
pp_result::const_intersecting_conditional_iterator::
_intersecting_roots_subrange(const header_inclusion_roots::const_iterator &b,
			     const header_inclusion_roots::const_iterator &e)
  noexcept
{
  const auto srb =
    std::lower_bound(b, e, _range,
		     [&](const header_inclusion_root &root,
			 const raw_pp_tokens_range &range) {
		       return root.get_range() < range;
		     });
  const auto sre =
    std::upper_bound(srb, e, _range,
		     [&](const raw_pp_tokens_range &range,
			 const header_inclusion_root &root) {
		       return range < root.get_range();
		     });

  return std::make_pair(srb, sre);
}

void pp_result::const_intersecting_conditional_iterator::_enter_root()
{
  if (_roots_it == _roots.second) {
    assert(!_child_it.get_value());
    return;
  }

  // Skip roots without any condition inclusion descendants.
  do {
    _child_it =
      _const_intersecting_child_node_iterator<conditional_inclusion_node> {
	*_roots_it, _range
      };
  } while (!_child_it.get_value() && ++_roots_it != _roots.second);
}


pp_result::pp_result()
  : _next_header_node_id(0), _next_predefinition_pos(0)
{}

pp_result::pp_result(header_inclusion_roots &&header_inclusion_roots)
  : _header_inclusion_roots(std::move(header_inclusion_roots)),
    _next_header_node_id(0), _next_predefinition_pos(0)
{}

std::pair<pp_result::const_directive_iterator,
	  pp_result::const_directive_iterator>
pp_result::find_overlapping_directives(const raw_pp_tokens_range &r)
  const noexcept
{
  return std::equal_range(directives_begin(), directives_end(), r);
}

std::pair<pp_result::const_macro_iterator, pp_result::const_macro_iterator>
pp_result::find_overlapping_macros(const raw_pp_tokens_range &r) const noexcept
{
  return std::equal_range(macros_begin(), macros_end(), r);
}

std::pair<pp_result::const_macro_undef_iterator,
	  pp_result::const_macro_undef_iterator>
pp_result::find_overlapping_macro_undefs(const raw_pp_tokens_range &r)
  const noexcept
{
  return std::equal_range(macro_undefs_begin(), macro_undefs_end(), r);
}


std::pair<pp_result::const_macro_invocation_iterator,
	  pp_result::const_macro_invocation_iterator>
pp_result::find_overlapping_macro_invocations(const raw_pp_tokens_range &r)
  const noexcept
{
  return std::equal_range(macro_invocations_begin(), macro_invocations_end(),
			  r);
}

pp_result::const_intersecting_source_iterator
pp_result::intersecting_sources_begin(const raw_pp_tokens_range &range) const
{
  return (const_intersecting_source_iterator
	  {_header_inclusion_roots, range,
	   const_intersecting_source_iterator::init_begin_iterator_tag{}});
}

pp_result::const_intersecting_source_iterator
pp_result::intersecting_sources_end(const raw_pp_tokens_range &range)
  const noexcept
{
  return (const_intersecting_source_iterator
	  {_header_inclusion_roots, range,
	   const_intersecting_source_iterator::init_end_iterator_tag{}});
}

pp_result::const_intersecting_conditional_iterator
pp_result::intersecting_conditionals_begin(const raw_pp_tokens_range &range)
  const
{
  return (const_intersecting_conditional_iterator
	  {_header_inclusion_roots, range,
	   const_intersecting_conditional_iterator::init_begin_iterator_tag{}});
}

pp_result::const_intersecting_conditional_iterator
pp_result::intersecting_conditionals_end(const raw_pp_tokens_range &range)
  const noexcept
{
  return (const_intersecting_conditional_iterator
	  {_header_inclusion_roots, range,
	   const_intersecting_conditional_iterator::init_end_iterator_tag{}});
}

const pp_result::header_inclusion_node&
pp_result::get_raw_token_source(const raw_pp_token_index tok_index) const
{
  const auto it =
    this->intersecting_sources_begin(raw_pp_tokens_range{tok_index});
  assert(it != this->intersecting_sources_end(raw_pp_tokens_range{tok_index}));
  return *it;
}

const raw_pp_tokens_range
pp_result::pp_tokens_range_to_raw(const pp_tokens_range &r) const noexcept
{
  if (r.begin != r.end) {
    return raw_pp_tokens_range {
		_pp_tokens[r.begin].get_token_source().begin,
		_pp_tokens[r.end - 1].get_token_source().end
	   };
  } else if (r.end && r.end != _pp_tokens.size()) {
    const raw_pp_tokens_range &before =
      _pp_tokens[r.end - 1].get_token_source();
    const raw_pp_tokens_range &after =
      _pp_tokens[r.end].get_token_source();
    if (before.begin != after.begin) {
      return raw_pp_tokens_range {
		_pp_tokens[r.end - 1].get_token_source().end,
		_pp_tokens[r.end].get_token_source().begin
	     };
    } else {
      // Both, the token before the position as well as the one
      // after it orginate from the same macro invocation.
      return raw_pp_tokens_range {
		before.begin,
		after.end
	     };
    }
  } else if (r.end) {
    assert(r.end == _pp_tokens.size() && !_pp_tokens.empty());
    return raw_pp_tokens_range {
		_pp_tokens.back().get_token_source().end,
		_pp_tokens.back().get_token_source().end
	   };
  } else if (!_pp_tokens.empty()) {
    return raw_pp_tokens_range {
		_pp_tokens.front().get_token_source().begin,
		_pp_tokens.front().get_token_source().begin
	   };
  } else {
    return raw_pp_tokens_range {
		_raw_tokens.size(),
		_raw_tokens.size()
	   };
  }
}

const pp_tokens_range
pp_result::raw_pp_tokens_range_to_nonraw(const raw_pp_tokens_range &r)
  const noexcept
{
  struct comp
  {
    bool operator()(const pp_token &tok, const raw_pp_tokens_range &r) noexcept
    {
      return tok.get_token_source() < r;
    }
    bool operator()(const raw_pp_tokens_range &r, const pp_token &tok) noexcept
    {
      return r < tok.get_token_source();
    }
  };

  const auto toks_range =
    std::equal_range(_pp_tokens.begin(), _pp_tokens.end(), r, comp{});

  return pp_tokens_range {
    static_cast<pp_token_index>(toks_range.first - _pp_tokens.begin()),
    static_cast<pp_token_index>(toks_range.second - _pp_tokens.begin())
   };
}

void pp_result::_enter_root(header_inclusion_root &hir,
			    const raw_pp_token_index range_begin) noexcept
{
  hir._set_range_begin(range_begin);
  hir._set_id(_next_header_node_id++);
}

void pp_result::_append_token(const raw_pp_token &tok)
{
  _raw_tokens.push_back(tok);
}

void pp_result::_append_token(raw_pp_token &&tok)
{
  _raw_tokens.push_back(std::move(tok));
}

void pp_result::_append_token(pp_token &&tok)
{
  _pp_tokens.push_back(std::move(tok));
}

raw_pp_tokens::size_type pp_result::_get_last_raw_index() const noexcept
{
  assert(!_raw_tokens.empty());
  return _raw_tokens.size() - 1;
}

pp_tokens::size_type pp_result::_get_last_pp_index() const noexcept
{
  assert(!_pp_tokens.empty());
  return _pp_tokens.size() - 1;
}

void pp_result::_add_directive(const raw_pp_tokens_range &directive_range)
{
  _directives.emplace_back(directive_range);
}

const pp_result::macro& pp_result::
_add_macro(const std::string &name,
	   const bool func_like,
	   const bool variadic,
	   std::vector<std::string> &&arg_names,
	   raw_pp_tokens &&repl,
	   const raw_pp_tokens_range &directive_range)
{
  _macros.push_back(std::unique_ptr<const macro>{
			new macro{name, func_like, variadic,
				  std::move(arg_names), std::move(repl),
				  directive_range}});
  return *_macros.back();
}

const pp_result::macro& pp_result::
_add_macro(const std::string &name, const macro::builtin_special_tag &tag)
{
  _macros.push_back(std::unique_ptr<const macro>{
			new macro{name, _next_predefinition_pos++, tag}});
  return *_macros.back();
}

const pp_result::macro& pp_result::
_add_macro(const std::string &name, std::vector<std::string> &&arg_names,
	   const macro::builtin_special_tag &tag)
{
  _macros.push_back(std::unique_ptr<const macro>{
			new macro{name, std::move(arg_names),
				  _next_predefinition_pos++, tag}});
  return *_macros.back();
}

const pp_result::macro&
pp_result::_add_macro(const std::string &name,
		      const bool func_like,
		      const bool variadic,
		      std::vector<std::string> &&arg_names,
		      raw_pp_tokens &&repl,
		      const macro::builtin_tag &tag)
{
  _macros.push_back(std::unique_ptr<const macro>{
			new macro{name, func_like, variadic,
				  std::move(arg_names), std::move(repl),
				  _next_predefinition_pos++, tag}});
  return *_macros.back();
}

const pp_result::macro&
pp_result::_add_macro(const std::string &name,
		      const bool func_like,
		      const bool variadic,
		      std::vector<std::string> &&arg_names,
		      raw_pp_tokens &&repl,
		      const macro::predefined_user_tag &tag)
{
  _macros.push_back(std::unique_ptr<const macro>{
			new macro{name, func_like, variadic,
				  std::move(arg_names), std::move(repl),
				  _next_predefinition_pos++, tag}});
  return *_macros.back();
}

void pp_result::
_add_macro_undef(const std::string &name,
		 const raw_pp_tokens_range &directive_range)
{
  _macro_undefs.emplace_back(name, directive_range);
}

void pp_result::_add_macro_undef(const std::string &name,
				 const macro_undef::predefined_user_tag &tag)
{
  _macro_undefs.emplace_back(name, _next_predefinition_pos++, tag);
}

pp_result::macro_invocation& pp_result::
_add_macro_invocation(const macro &m,
		      const raw_pp_tokens_range &invocation_range,
		      const std::size_t src_length)
{
  _macro_invocations.push_back
    (std::unique_ptr<macro_invocation>
	{new macro_invocation{m, invocation_range, src_length}});
  return *_macro_invocations.back();
}

void pp_result::
_extend_macro_invocation_range(macro_invocation &invocation,
			       const raw_pp_token_index new_end)
{
  assert(new_end >= invocation._invocation_range.end);
  invocation._invocation_range.end = new_end;
}

std::pair<pp_result::used_macros, pp_result::macro_nondef_constraints>
pp_result::_drop_pp_tokens_tail(const pp_tokens::size_type new_end)
{
  // The preprocessor temporarily pushed some pp_token instances when
  // doing macro expansion for #include directives or evaluating
  // conditional #if expressions. Purge these alongside with any
  // associated macro_invocations.
  // Collect any used_macros and macro_nondef_constraints of this
  // tail sequence and return it.
  used_macros um;
  macro_nondef_constraints mnc;

  assert(new_end <= _pp_tokens.size());

  const auto tok_begin = _pp_tokens.begin() + new_end;
  const auto tok_end = _pp_tokens.end();
  if (tok_begin == tok_end)
    return std::make_pair(std::move(um), std::move(mnc));

  // Each pp_token is either the result of a macro_invocation or not.
  // If yes, then all used_macros and macro_nondef_constraints can be
  // found at this macro_invocation. Otherwise, the
  // macro_nondef_constraints for identifier tokens can be derived
  // from the pp_token_value itself.
  const auto mis_range
    = (this->find_overlapping_macro_invocations
       (raw_pp_tokens_range{tok_begin->get_token_source().begin,
			    (tok_end - 1)->get_token_source().end}));
  auto it_mi = mis_range.first;
  auto it_tok = tok_begin;
  while (it_tok != tok_end) {
    if (it_mi != mis_range.second &&
	!(it_tok->get_token_source() < it_mi->get_source_range())) {
      um += it_mi->get_used_macros();
      mnc += it_mi->get_macro_nondef_constraints();

      // Advance the current token iterator past the tokens from the
      // macro_invocation just processed.
      while (it_tok != tok_end &&
	     !(it_mi->get_source_range() < it_tok->get_token_source())) {
	++it_tok;
      }
      ++it_mi;

    } else if (it_tok->is_id()) {
      // Look ahead to check whether the identifier token is not
      // followed by an opening parenthesis and thus, a function style
      // definition, even if present, would not alter the result. If
      // the next token is a parenthesis, but it itself or any
      // whitespace before it is the result of some macro invocation,
      // then it can be safely ignored.
      auto it_next_tok = it_tok + 1;
      while (it_next_tok != tok_end &&
	     it_next_tok->is_type_any_of<pp_token::type::ws,
					 pp_token::type::empty,
					 pp_token::type::newline>()) {
	++it_next_tok;
      }

      bool func_like_allowed = false;
      if (it_next_tok == tok_end ||
	  !it_next_tok->is_punctuator("(") ||
	  (it_mi != mis_range.second &&
	   !(it_next_tok->get_token_source() < it_mi->get_source_range()))) {
	func_like_allowed = true;
      }

      mnc += macro_nondef_constraint{it_tok->get_value(), func_like_allowed};
      ++it_tok;

    } else {
      ++it_tok;

    }
  }

  _macro_invocations.erase((_macro_invocations.begin() +
			    (mis_range.first._it -
			     _macro_invocations.cbegin())),
			   (_macro_invocations.begin() +
			    (mis_range.second._it -
			     _macro_invocations.cbegin())));
  _pp_tokens.shrink(new_end);

  return std::make_pair(std::move(um), std::move(mnc));
}
