/*******************************************************************\

Module: Symbolic Execution

Author: Daniel Kroening, kroening@kroening.com
		Lucas Cordeiro, lcc08r@ecs.soton.ac.uk

\*******************************************************************/

#include <assert.h>

#include <expr_util.h>
#include <std_expr.h>

#include "goto_symex.h"

/*******************************************************************\

Function: goto_symext::symex_goto

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_goto(statet &state, execution_statet &ex_state, unsigned node_id)
{
  const goto_programt::instructiont &instruction=*state.source.pc;

  exprt old_guard=instruction.guard;

  replace_dynamic_allocation(state, old_guard);
  dereference(old_guard, state, false, node_id);

  exprt new_guard=old_guard;
  state.rename(new_guard, ns,node_id);
  do_simplify(new_guard);

  target->location(state.guard, state.source);

  if(new_guard.is_false() ||
     state.guard.is_false())
  {
    // reset unwinding counter
    state.unwind_map[state.source]=0;

    // next instruction
    state.source.pc++;

    return; // nothing to do
  }

  assert(!instruction.targets.empty());

  // we only do deterministic gotos for now
  if(instruction.targets.size()!=1)
    throw "no support for non-deterministic gotos";

  goto_programt::const_targett goto_target=
    instruction.targets.front();

  bool forward=
    state.source.pc->location_number <
    goto_target->location_number;

  if(!forward) // backwards?
  {
    unsigned unwind;

    unwind = state.unwind_map[state.source];
    unwind++;
    state.unwind_map[state.source] = unwind;

    if(get_unwind(state.source, unwind))
    {
      loop_bound_exceeded(state, new_guard, node_id);

      // reset unwinding
      state.unwind_map[state.source] = unwind;

      // next instruction
      state.source.pc++;
      return;
    }

    if(new_guard.is_true())
    {
      state.source.pc=goto_target;
      return; // nothing else to do
    }
  }

  goto_programt::const_targett new_state_pc, state_pc;

  if(forward)
  {
    new_state_pc=goto_target; // goto target instruction
    state_pc=state.source.pc;
    state_pc++; // next instruction
  }
  else
  {
    new_state_pc=state.source.pc;
    new_state_pc++;
    state_pc=goto_target;
  }

  state.source.pc=state_pc;

  // put into state-queue
  statet::goto_state_listt &goto_state_list=
    state.top().goto_state_map[new_state_pc];

  goto_state_list.push_back(statet::goto_statet(state));
  statet::goto_statet &new_state=goto_state_list.back();

  // adjust guards
  if(new_guard.is_true())
  {
    state.guard.make_false();
#if 1
	  guardt if_guard;
	  if(!state.if_guard_stack.empty())
			if_guard.add(state.if_guard_stack.top().as_expr());
	  if_guard.add(state.guard.as_expr());
	  state.if_guard_stack.push(if_guard);
#endif
  }
  else
  {
    // produce new guard symbol
    exprt guard_expr;

    if(new_guard.id()=="symbol" ||
           (new_guard.id()=="not" &&
            new_guard.operands().size()==1 &&
            new_guard.op0().id()=="symbol"))
      guard_expr=new_guard;
    else
    {
      guard_expr=symbol_exprt(guard_identifier(state), bool_typet());
      exprt new_rhs=new_guard,
            rhs=old_guard;
      new_rhs.make_not();
      rhs.make_not();

      exprt new_lhs=guard_expr;

      state.assignment(new_lhs, new_rhs, ns, false, ex_state, node_id);

      guardt guard;

      target->assignment(
        guard,
        new_lhs, guard_expr,
        new_rhs,
        state.source,
        symex_targett::HIDDEN);

      guard_expr.make_not();
      state.rename(guard_expr, ns,node_id);
    }

    if(forward)
    {
      new_state.guard.add(guard_expr);
      guard_expr.make_not();
      state.guard.add(guard_expr);

#if 1
	  guardt if_guard;
	  if(!state.if_guard_stack.empty())
		if_guard.add(state.if_guard_stack.top().as_expr());
	  if_guard.add(guard_expr);
	  state.if_guard_stack.push(if_guard);
#endif

    }
    else
    {
      state.guard.add(guard_expr);
#if 1
	  guardt if_guard;
	  if(!state.if_guard_stack.empty())
			if_guard.add(state.if_guard_stack.top().as_expr());
	  if_guard.add(guard_expr);
	  state.if_guard_stack.push(if_guard);
#endif
      guard_expr.make_not();
      new_state.guard.add(guard_expr);
    }
  }
}

/*******************************************************************\

Function: goto_symext::symex_step_goto

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_step_goto(statet &state, bool taken,unsigned node_id)
{
  const goto_programt::instructiont &instruction=*state.source.pc;

  exprt guard(instruction.guard);
  dereference(guard, state, false, node_id);
  state.rename(guard, ns,node_id);

  if(!taken) guard.make_not();

  state.guard.guard_expr(guard);
  do_simplify(guard);

  target->assumption(state.guard, guard, state.source);
}

/*******************************************************************\

Function: goto_symext::merge_gotos

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::merge_gotos(statet &state, execution_statet &ex_state, unsigned node_id)
{
  statet::framet &frame=state.top();

  // first, see if this is a target at all
  statet::goto_state_mapt::iterator state_map_it=
    frame.goto_state_map.find(state.source.pc);

  if(state_map_it==frame.goto_state_map.end())
    return; // nothing to do

  // we need to merge
  statet::goto_state_listt &state_list=state_map_it->second;

  for(statet::goto_state_listt::reverse_iterator
      list_it=state_list.rbegin();
      list_it!=state_list.rend();
      list_it++)
  {
    statet::goto_statet &goto_state=*list_it;

    // do SSA phi functions
    phi_function(goto_state, state, ex_state, node_id);

    merge_value_sets(goto_state, state);

    // adjust guard
    state.guard|=goto_state.guard;

    // adjust depth
    state.depth=std::min(state.depth, goto_state.depth);
  }

  // clean up to save some memory
  frame.goto_state_map.erase(state_map_it);
}


#if 0
void goto_symext::merge_gotos(statet &state)
{

  statet::framet &frame=state.top();

  // first, see if this is a target at all
  statet::goto_state_mapt::iterator state_map_it=
    frame.goto_state_map.find(state.source.pc);

  if(state_map_it==frame.goto_state_map.end())
    return; // nothing to do

  // we need to merge
  statet::goto_state_listt &state_list=state_map_it->second;

  for(statet::goto_state_listt::reverse_iterator
      list_it=state_list.rbegin();
      list_it!=state_list.rend();
      list_it++)
  {
	  	statet::goto_statet &goto_state=*list_it;

	    // adjust guard
	    state.guard |= goto_state.guard;

	    // adjust depth
	    state.depth=std::min(state.depth, goto_state.depth);

	    state.if_guard_stack.pop();
  }

  // clean up to save some memory
  frame.goto_state_map.erase(state_map_it);
}
#endif
/*******************************************************************\

Function: goto_symext::merge_value_sets

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::merge_value_sets(
  const statet::goto_statet &src,
  statet &dest)
{
  if(dest.guard.is_false())
  {
    dest.value_set=src.value_set;
    return;
  }

  dest.value_set.make_union(src.value_set);
}

/*******************************************************************\

Function: goto_symext::phi_function

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::phi_function(
  const statet::goto_statet &goto_state,
  statet &state,
  execution_statet &ex_state,
   unsigned node_id)
{
  // go over all variables to see what changed
  std::set<irep_idt> variables;

  goto_state.level2.get_variables(variables);
  state.level2->get_variables(variables);

  for(std::set<irep_idt>::const_iterator
      it=variables.begin();
      it!=variables.end();
      it++)
  {
    if(goto_state.level2.current_number(*it)==
       state.level2->current_number(*it))
      continue; // not changed

    if(*it==guard_identifier(state))
      continue; // just a guard

    irep_idt original_identifier = state.get_original_name(*it);
    try
    {
    	// changed!
    	const symbolt &symbol=ns.lookup(original_identifier);
//    	std::cout << "================================================= meger goto state : " << original_identifier << std::endl;

		typet type(symbol.type);

		// type may need renaming
		state.rename(type, ns,node_id);

		exprt rhs;

		if(state.guard.is_false())
		{
		  rhs=symbol_exprt(state.current_name(goto_state, symbol.name,node_id), type);
		}
		else if(goto_state.guard.is_false())
		{
		  rhs=symbol_exprt(state.current_name(symbol.name,node_id), type);
		}
		else
		{
		  guardt tmp_guard(goto_state.guard);

		  // this gets the diff between the guards
		  tmp_guard-=state.guard;

		  rhs=if_exprt();
		  rhs.type()=type;
		  rhs.op0()=tmp_guard.as_expr();
		  rhs.op1()=symbol_exprt(state.current_name(goto_state, symbol.name,node_id), type);
		  rhs.op2()=symbol_exprt(state.current_name(symbol.name,node_id), type);
		}

		exprt lhs(symbol_expr(symbol));
		exprt new_lhs(lhs);

		state.assignment(new_lhs, rhs, ns, false, ex_state, node_id);

		guardt true_guard;

		target->assignment(
		  true_guard,
		  new_lhs, lhs,
		  rhs,
		  state.source,
		  symex_targett::HIDDEN);

	//	std::cout << "+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++" << std::endl;
    }
    catch(const std::string e)
    {
    	continue;
    }
	//	std::cout << "+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++" << std::endl;
  }
}

/*******************************************************************\

Function: goto_symext::loop_bound_exceeded

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::loop_bound_exceeded(
  statet &state,
  const exprt &guard, unsigned node_id)
{
  const irep_idt &loop_id=state.source.pc->location.get("loop-id");

  exprt negated_cond;

  if(guard.is_true())
    negated_cond=false_exprt();
  else
    negated_cond=gen_not(guard);

  bool unwinding_assertions=
    options.get_bool_option("unwinding-assertions");

  bool partial_loops=
    options.get_bool_option("partial-loops");

  if(!partial_loops)
  {
    if(unwinding_assertions)
    {
      // generate unwinding assertion
      claim(negated_cond,
            "unwinding assertion loop "+id2string(loop_id),
            state, node_id);
    }
    else
    {
      // generate unwinding assumption, unless we permit partial loops
      exprt guarded_expr=negated_cond;
      state.guard.guard_expr(guarded_expr);
      target->assumption(state.guard, guarded_expr, state.source);
    }

    // add to state guard to prevent further assignments
    state.guard.add(negated_cond);
  }
}

/*******************************************************************\

Function: goto_symext::get_unwind

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/
#if 1
bool goto_symext::get_unwind(
  const symex_targett::sourcet &source,
  unsigned unwind)
{
  return false;
}
#endif
