#include <solidity-ast-frontend/solidity_convert.h>
#include <solidity-ast-frontend/typecast.h>
#include <util/arith_tools.h>
#include <util/bitvector.h>
#include <util/c_types.h>
#include <util/expr_util.h>
#include <util/i2string.h>
#include <util/mp_arith.h>
#include <util/std_code.h>
#include <util/std_expr.h>
#include <iomanip>

solidity_convertert::solidity_convertert(contextt &_context, nlohmann::json &_ast_json):
  context(_context),
  ns(context),
  ast_json(_ast_json),
  current_functionDecl(nullptr)
{
}

static std::string get_decl_name(const NamedDeclTracker &nd)
{
  if (nd.get_hasIdentifier())
  {
    return nd.get_name();
  }
  else
  {
    assert(!"Unsupported - nd.get_hasIdentifier() returns false");
    return "Error"; // just to make come old gcc compiler happy
  }
  return "Error"; // just to make come old gcc compiler happy
}

bool solidity_convertert::convert()
{
  // This method convert each declarations in ast_json to symbols and add them to the context.

  if (!ast_json.contains("nodes")) // check json file contains AST nodes as Solidity might change
    assert(!"JSON file does not contain any AST nodes");

  if (!ast_json.contains("absolutePath")) // check json file contains AST nodes as Solidity might change
    assert(!"JSON file does not contain absolutePath");

  absolute_path = ast_json["absolutePath"].get<std::string>();

  // By now the context should have the symbols of all ESBMC's intrinsics and the dummy main
  // We need to convert Solidity AST nodes to the equivalent symbols and add them to the context
  nlohmann::json nodes = ast_json["nodes"];
  unsigned index = 0;
  nlohmann::json::iterator itr = nodes.begin();
  for (; itr != nodes.end(); ++itr, ++index)
  {
    std::string node_type = (*itr)["nodeType"].get<std::string>();
    if (node_type == "ContractDefinition") // contains AST nodes we need
    {
      if(convert_ast_nodes(*itr))
        return true; // 'true' indicates something goes wrong.
    }
  }

  assert(!"all symbols done?");

  return false; // 'false' indicates successful completion.
}

bool solidity_convertert::convert_ast_nodes(nlohmann::json &contract_def)
{
  unsigned index = 0;
  nlohmann::json ast_nodes = contract_def["nodes"];
  nlohmann::json::iterator itr = ast_nodes.begin();
  for (; itr != ast_nodes.end(); ++itr, ++index)
  {
    nlohmann::json ast_node = *itr;
    std::string node_name = ast_node["name"].get<std::string>();
    std::string node_type = ast_node["nodeType"].get<std::string>();
    printf("@@ Converting node[%u]: name=%s, nodeType=%s ...\n",
        index, node_name.c_str(), node_type.c_str());
    //print_json_array_element(ast_node, node_type, index);
    exprt dummy_decl;
    if(get_decl(ast_node, dummy_decl))
      return true;
  }

  return false;
}

// This method convert declarations. They are called when those declarations
// are to be added to the context. If a variable or function is being called
// but then get_decl_expr is called instead
bool solidity_convertert::get_decl(nlohmann::json &ast_node, exprt &new_expr)
{
  new_expr = code_skipt();

  if (!ast_node.contains("nodeType"))
    assert(!"Missing \'nodeType\' filed in ast_node");

  SolidityTypes::declKind decl_kind =
    SolidityTypes::get_decl_kind(static_cast<std::string>(ast_node.at("nodeType")));
  assert(decl_kind != SolidityTypes::DeclKindError);

  switch(decl_kind)
  {
    case SolidityTypes::declKind::DeclVar:
    {
      auto vd_tracker = std::make_shared<VarDeclTracker>(ast_node); // VariableDeclaration tracker
      vd_tracker->config(absolute_path);
      global_vars.push_back(vd_tracker); // remember this var in case future DeclRefExprTacker uses it
      return get_var(vd_tracker, new_expr);
    }
    case SolidityTypes::declKind::DeclFunction:
    {
      auto fd_tracker = std::make_shared<FunctionDeclTracker>(ast_node); // FunctionDeclaration tracker
      fd_tracker->config(absolute_path);
      return get_function(fd_tracker, new_expr);
    }
    default:
      printf("failed to convert declaration in Solidity AST: %s\n", SolidityTypes::declKind_to_str(decl_kind));
      assert(!"Unimplemented declaration type");
      return true;
  }

  assert(!"get_decl done?");
  return false;
}

bool solidity_convertert::get_function(funDeclTrackerPtr &fd, exprt &new_expr)
{
  // Don't convert if the function was implicitly converted
  if(fd->get_isImplicit()) // for func_overflow, it returns false
    return false;

  // If the function is not defined but this is not the definition, skip it
  if(fd->get_isDefined() && !fd->get_isADefinition()) // for func_overflow, it's (true && !true).
    return false;

  // Save old_functionDecl, to be restored at the end of this method
  std::shared_ptr<FunctionDeclTracker> old_functionDecl = current_functionDecl;
  current_functionDecl = fd;

  // Set initial variable name, it will be used for variables' name
  // This will be reset every time a function is parsed
  current_scope_var_num = 1; // might need to change for Solidity when considering the scopes

  // Build function's type
  code_typet type;

  // Return type
  if(get_type(fd->get_qualtype_tracker(), type.return_type()))
    return true;

  if(fd->get_isVariadic()) // for func_overflow, it's false
    type.make_ellipsis();

  if(fd->get_isInlined()) // for func_overflow, it's false
    type.inlined(true);

  locationt location_begin;
  get_location_from_decl(fd->get_sl_tracker(), location_begin);

  std::string id, name; // __ESBMC_assume and c:@F@__ESBMC_assume
  get_decl_name(fd->get_nameddecl_tracker(), name, id);

  symbolt symbol;
  std::string debug_modulename = get_modulename_from_path(location_begin.file().as_string()); // for func_overflow, it's just the file name "overflow_2"
  get_default_symbol(
    symbol,
    debug_modulename,
    type,
    name,
    id,
    location_begin);

  symbol.lvalue = true;
  symbol.is_extern = fd->get_storage_class() == SolidityTypes::SC_Extern ||
                     fd->get_storage_class() == SolidityTypes::SC_PrivateExtern;
  symbol.file_local = (fd->get_storage_class() == SolidityTypes::SC_Static);

  symbolt &added_symbol = *move_symbol_to_context(symbol);

  // We convert the parameters first so their symbol are added to context
  // before converting the body, as they may appear on the function body
  if (fd->get_num_args())
    assert(!"come back and continue - add support for function arguments");

  // Apparently, if the type has no arguments, we assume ellipsis
  if(!type.arguments().size())
    type.make_ellipsis();

  added_symbol.type = type;

  // We need: a type, a name, and an optional body
  if(fd->get_hasBody()) // for func_overflow, this returns true. for __ESBMC_assume, fd.hasBody() return false
  {
    exprt body_exprt;
    get_expr(fd->get_body(), body_exprt);

    added_symbol.value = body_exprt;
  }

  // Restore old functionDecl
  current_functionDecl = old_functionDecl; // for __ESBMC_assume, old_functionDecl == null

  assert(!"done - get_funciton?");
  return false;
}

bool solidity_convertert::get_expr(const StmtTracker* stmt, exprt &new_expr)
{
  static int call_expr_times = 0; // TODO: remove debug
  locationt location;
  get_start_location_from_stmt(stmt, location);
  assert(stmt);

  switch(stmt->get_stmt_class())
  {
    case SolidityTypes::stmtClass::CompoundStmtClass:
    {
      printf("	@@@ got Expr: SolidityTypes::stmtClass::CompoundStmtClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);
      const CompoundStmtTracker* compound_stmt =
        static_cast<const CompoundStmtTracker*>(stmt); // pointer to const CompoundStmtTracker: can modify ptr but not the object content

      code_blockt block;
      unsigned ctr = 0;
      for (const auto &stmt : compound_stmt->get_statements())
      {
        exprt statement;
        if(get_expr(stmt, statement))
          return true;

        convert_expression_to_code(statement);
        block.operands().push_back(statement);
        ++ctr;
      }

      printf(" \t @@@ CompoundStmt has %u statements\n", ctr);

      // Set the end location for blocks
      locationt location_end;
      // TODO: get_final_location_from_stmt
      //get_final_location_from_stmt(stmt, location_end);

      block.end_location(location_end);

      new_expr = block;
      assert(!"done - all CompoundStmtClass?");
      break;
    }

    // Binary expression such as a+1, a-1 and assignments
    case SolidityTypes::stmtClass::BinaryOperatorClass:
    {
      printf("	@@@ got Expr: SolidityTypes::stmtClass::BinaryOperatorClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);
      const BinaryOperatorTracker* binop =
        static_cast<const BinaryOperatorTracker*>(stmt); // pointer to const CompoundStmtTracker: can modify ptr but not the object content

      if(get_binary_operator_expr(binop, new_expr))
        return true;

      break;
    }

    // Reference to a declared object, such as functions or variables
    case SolidityTypes::stmtClass::DeclRefExprClass:
    {
      printf("	@@@ got Expr: SolidityTypes::stmtClass::DeclRefExprClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);

      const DeclRefExprTracker* decl =
        static_cast<const DeclRefExprTracker*>(stmt);

      // associate previous VarDecl AST node with this DeclRefExpr
      // In order to get the referenced declaration, we want two key information: name and type.
      // We need to do the followings to achieve this goal:
      //  1. find the associated AST node json object
      //  2. use that json object to populate NamedDeclTracker and QualTypeTracker
      //     of this DeclRefExprTracker
      assert(decl->get_decl_ref_id() != DeclRefExprTracker::declRefIdInvalid);
      assert(decl->get_decl_ref_kind() != SolidityTypes::declRefError);

      if(get_decl_ref(decl, new_expr))
        return true;

      break;
    }

    // Casts expression:
    // Implicit: float f = 1; equivalent to float f = (float) 1;
    // CStyle: int a = (int) 3.0;
    case SolidityTypes::stmtClass::ImplicitCastExprClass:
    {
      printf("	@@@ got Expr: SolidityTypes::stmtClass::ImplicitCastExprClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);
      const ImplicitCastExprTracker* cast =
        static_cast<const ImplicitCastExprTracker*>(stmt);

      if(get_cast_expr(cast, new_expr))
        return true;

      break;
    }

    // An integer value
    case SolidityTypes::stmtClass::IntegerLiteralClass:
    {
      printf("	@@@ got Expr: SolidityTypes::stmtClass::IntegerLiteralClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);
      const IntegerLiteralTracker* integer_literal =
        static_cast<const IntegerLiteralTracker*>(stmt);

      if(convert_integer_literal(integer_literal, new_expr))
        return true;

      break;
    }

    // adjust_expr::adjust_side_effect_function_call
    case SolidityTypes::stmtClass::CallExprClass:
    {
      printf("	@@@ got Expr: clang::Stmt::CallExprClass, ");
      printf("  call_expr_times=%d\n", call_expr_times++);
      const CallExprTracker* function_call =
        static_cast<const CallExprTracker*>(stmt);

      exprt callee_expr;
      if(get_expr(function_call->get_callee(), callee_expr))
        return true;

      // TODO: in order to do the concept proof, note this part is hard coded based on the RSH as in
      // "assert( (int) ((int)(unsigned)sum > (int)100));"
      // manually unroll the program here
      typet type;
      type = int_type();
      std::string c_type = "signed_int";
      type.set("#cpp_type", c_type);

      side_effect_expr_function_callt call;
      call.function() = callee_expr;
      call.type() = type;

      assert(!"args for - CallExprClass case?");
      new_expr = call;
      break;
    }

    default:
      std::cerr << "Conversion of unsupported Solidity expr: \"";
      printf("failed to convert Solidity expr: %s\n", SolidityTypes::stmtClass_to_str(stmt->get_stmt_class()));
      assert(!"Unimplemented expr type");
      return true;
  }

  new_expr.location() = location;
  return false;
}

bool solidity_convertert::get_cast_expr(
  const ImplicitCastExprTracker* cast,
  exprt &new_expr)
{
  exprt expr;
  if(get_expr(cast->get_sub_expr(), expr))
    return true;

  typet type;
  if(get_type(cast->get_qualtype_tracker(), type))
    return true;

  switch(cast->get_implicit_cast_kind()) // "_x=100;" it returns CK_IntegralCast
  {
    case SolidityTypes::CK_FunctionToPointerDecay:
      break;

    case SolidityTypes::CK_IntegralCast:
      solidity_gen_typecast(ns, expr, type);
      break;

    default:
      assert(!"Conversion of unsupported cast operator");
      return true;
  }

  new_expr = expr;
  return false;
}

bool solidity_convertert::get_decl_ref(const DeclRefExprTracker* dcl, exprt &new_expr)
{
  if (dcl->get_decl_ref_id() != (DeclRefExprTracker::declRefIdInvalid - 1))
  {
    auto matched_dcl = get_matched_decl_ref(dcl->get_decl_ref_id());
    assert(matched_dcl); // must be a valid decl ref pointer

    // TODO: enum
    if (dcl->get_decl_ref_kind() == SolidityTypes::EnumConstantDecl)
    {
      assert(!"unsupported enum decl ref types");
      return false;
    }

    // Everything else should be a value decl
    if (dcl->get_decl_ref_kind() == SolidityTypes::ValueDecl)
    {
      // Everything else should be a value decl
      std::string name, id; // for "_x = 100;", id="c:@_x" and name="_x".
      get_decl_name(matched_dcl->get_nameddecl_tracker(), name, id);

      typet type;
      if(get_type(matched_dcl->get_qualtype_tracker(), type))
        return true;

      new_expr = exprt("symbol", type);
      new_expr.identifier(id);
      new_expr.cmt_lvalue(true);
      new_expr.name(name);
      return false;
    }
  }
  else
  {
    // for decl with -ve ref id in Solidity (e.g. built-in "assert")
    // TODO: in order to do the concept proof, note this part is hard coded based on the RSH as in
    // "assert( (int) ((int)(unsigned)sum > (int)100));"

    // Everything else should be a value decl
    if (dcl->get_decl_ref_kind() == SolidityTypes::ValueDecl)
    {
      // Everything else should be a value decl
      std::string name, id; // for "_x = 100;", id="c:@_x" and name="_x".
      get_decl_name(dcl->get_nameddecl_tracker(), name, id);

      typet type;
      if(get_type(dcl->get_qualtype_tracker(), type))
        return true;

      new_expr = exprt("symbol", type);
      new_expr.identifier(id);
      new_expr.cmt_lvalue(true);
      new_expr.name(name);
      return false;
    }
  }

  assert(!"Conversion of unsupported Solidity decl ref");
  return true;
}

bool solidity_convertert::get_binary_operator_expr(
  const BinaryOperatorTracker* binop,
  exprt &new_expr)
{
  exprt lhs;
  if(get_expr(binop->get_LHS(), lhs))
    return true;

  exprt rhs;
  if(get_expr(binop->get_RHS(), rhs))
    return true;

  typet t;
  if(get_type(binop->get_qualtype_tracker(), t))
    return true;

  switch(binop->get_binary_opcode()) // for "_x=100;", it returns "BO_Assign"
  {
    case SolidityTypes::BO_Assign:
    {
      printf("  @@@ got binop.getOpcode: clang::BO_Assign\n");
      // If we use code_assignt, it will reserve two operands,
      // and the copy_to_operands method call at the end of
      // this method will put lhs and rhs in positions 2 and 3,
      // instead of 0 and 1 :/
      new_expr = side_effect_exprt("assign", t);
      break;
    }
    case SolidityTypes::BO_Add:
    {
      printf("  @@@ got binop.getOpcode: clang::BO_Add\n");
      if(t.is_floatbv())
        new_expr = exprt("ieee_add", t);
      else
        new_expr = exprt("+", t);
      break;
    }
    default:
    {
      assert(!"Unimplemented opcode in BinaryOperatorExpr");
    }
  }

  new_expr.copy_to_operands(lhs, rhs);
  return false;
}

void solidity_convertert::get_start_location_from_stmt(
  const StmtTracker* stmt,
  locationt &location)
{
  std::string function_name;

  if (current_functionDecl)
    function_name = ::get_decl_name(current_functionDecl->get_nameddecl_tracker()); // for func_overflow, name is "func_overflow"

  // In clang, we need to get PLoc first.
  // For Solidity, we've already extracted the information during decl tracker config phase.
  // TODO: we should use the slm (source location manager) of the StmtTracker, instead of the fucntion decl tracker.

  set_location(current_functionDecl->get_sl_tracker(), function_name, location); // for __ESBMC_assume, function_name is still empty after this line.
}

bool solidity_convertert::get_var(varDeclTrackerPtr vd, exprt &new_expr)
{
  // Get type
  typet t;
  if(get_type(vd->get_qualtype_tracker(), t))
    return true;

  assert(!vd->get_hasAttrs() && "expecting false hasAttrs but got true"); // to be extended in the future

  std::string id, name;
  get_decl_name(vd->get_nameddecl_tracker(), name, id);

  locationt location_begin;
  get_location_from_decl(vd->get_sl_tracker(), location_begin);

  std::string debug_modulename = get_modulename_from_path(absolute_path);
  symbolt symbol;
  get_default_symbol(
    symbol,
    debug_modulename,
    t,
    name,
    id,
    location_begin);

  symbol.lvalue = true;
  assert(vd->get_storage_class() == SolidityTypes::SC_None); // hard coded, may need to change for the future
  symbol.static_lifetime =
    (vd->get_storage_class() == SolidityTypes::SC_Static) || vd->get_hasGlobalStorage();
  assert(vd->get_hasExternalStorage() == false); // hard coded, may need to change for the future
  symbol.is_extern = vd->get_hasExternalStorage();
  symbol.file_local = (vd->get_storage_class() == SolidityTypes::SC_Static) ||
                      (!vd->get_isExternallyVisible() && !vd->get_hasGlobalStorage());

  if(symbol.static_lifetime && !symbol.is_extern && !vd->get_hasInit())
  {
    // Initialize with zero value, if the symbol has initial value,
    // it will be add later on this method
    symbol.value = gen_zero(t, true);
    symbol.value.zero_initializer(true);
  }

  // We have to add the symbol before converting the initial assignment
  // because we might have something like 'int x = x + 1;' which is
  // completely wrong but allowed by the language
  symbolt &added_symbol = *move_symbol_to_context(symbol);

  code_declt decl(symbol_expr(added_symbol));

  if(vd->get_hasInit())
  {
    assert(!"unsupported - vd got init value");
  }

  decl.location() = location_begin;

  new_expr = decl;

  return false;
}

void solidity_convertert::get_decl_name(
  const NamedDeclTracker &nd,
  std::string &name,
  std::string &id)
{
  id = name = ::get_decl_name(nd);

  switch(nd.get_named_decl_kind()) // it's our declClass in Solidity! same as being used in get_decl
  {
    case SolidityTypes::declKind::DeclVar:
    {
      if(name.empty())
      {
        assert(!"unsupported - name is empty in get_decl_name for DeclVar");
      }
      break;
    }
    default:
      if(name.empty()) // print name gives the function name when this is part of get_function back traces
      {
        assert(!"Declaration has empty name - unsupported named_decl_kind");;
      }
  }

  // get DeclUSR to be used in C context
  if(!generate_decl_usr(nd, name, id))
    return;

  // Otherwise, abort
  assert(!"Unable to generate USR");
  abort();
}

// Auxiliary function to mimic clang's clang::index::generateUSRForDecl()
bool solidity_convertert::generate_decl_usr(
  const NamedDeclTracker &nd,
  std::string &name,
  std::string &id)
{
  switch(nd.get_named_decl_kind()) // it's our declClass in Solidity! same as being used in get_decl
  {
    case SolidityTypes::declKind::DeclVar:
    {
      std::string id_prefix = "c:@";
      id = id_prefix + name;
      return false;
    }
    case SolidityTypes::declKind::DeclFunction:
    {
      std::string id_prefix = "c:@F@";
      id = id_prefix + name;
      return false;
    }
    default:
      assert(!"unsupported named_decl_kind when generating declUSR");
  }

  return true;
}

bool solidity_convertert::get_type(
  const QualTypeTracker &q_type,
  typet &new_type)
{
  if(get_sub_type(q_type, new_type))
    return true;

  if(q_type.get_isConstQualified()) // TODO: what's Solidity's equivalent?
    new_type.cmt_constant(true);

  if(q_type.get_isVolatileQualified())
    new_type.cmt_volatile(true);

  if(q_type.get_isRestrictQualified())
    new_type.restricted(true);

  return false;
}

bool solidity_convertert::get_sub_type(const QualTypeTracker &q_type, typet &new_type)
{
  switch(q_type.get_type_class())
  {
    case SolidityTypes::typeClass::TypeBuiltin:
    {
      if(get_builtin_type(q_type, new_type))
        return true;
      break;
    }
    case SolidityTypes::typeClass::FunctionNoProto:
    {
      // TODO: in order to do the concept proof, note this part is hard coded based on the RSH as in
      // "assert( (int) ((int)(unsigned)sum > (int)100));"
      code_typet type;

      // Return type
      typet return_type;

      assert(q_type.get_sub_qualtype_class() == SolidityTypes::TypeBuiltin);
      assert(q_type.get_sub_qualtype_bt_kind() == SolidityTypes::BuiltinInt);
      // manually unrolled recursion here
      // type config for Builtin && Int
      return_type = int_type();
      std::string c_type = "signed_int";
      return_type.set("#cpp_type", c_type);

      type.return_type() = return_type;

      // Apparently, if the type has no arguments, we assume ellipsis
      if(!type.arguments().size())
        type.make_ellipsis();

      new_type = type;

      break;
    }
    case SolidityTypes::typeClass::Pointer:
    {
      // TODO: in order to do the concept proof, note this part is hard coded based on the RSH as in
      // "assert( (int) ((int)(unsigned)sum > (int)100));"
      typet sub_type;

      // manually unrolled recursion here
      // FunctionNoProto: first embedded QualType object
      code_typet type;
      typet return_type;
      return_type = int_type();
      std::string c_type = "signed_int"; // BuiltIn && Int: second embedded QualType object
      return_type.set("#cpp_type", c_type);
      type.return_type() = return_type;
      if(!type.arguments().size())
        type.make_ellipsis();
      sub_type = type;

      new_type = gen_pointer_type(sub_type);
      break;
    }
    default:
      assert(!"Conversion of unsupported node qual type");
      return true;
  }

  return false;
}

bool solidity_convertert::get_builtin_type(
  const QualTypeTracker &q_type,
  typet &new_type)
{
  std::string c_type;

  switch(q_type.get_bt_kind())
  {
    case SolidityTypes::builInTypesKind::BuiltinVoid:
    {
      new_type = empty_typet();
      c_type = "void";
      break;
    }
    case SolidityTypes::builInTypesKind::BuiltInUChar:
    {
      new_type = unsigned_char_type();
      c_type = "unsigned_char";
      break;
    }
    case SolidityTypes::builInTypesKind::BuiltinInt:
    {
      new_type = int_type();
      c_type = "signed_int";
      break;
    }
    default:
      std::cerr << "Unrecognized clang builtin type "
                << SolidityTypes::builInTypesKind_to_str(q_type.get_bt_kind())
                << std::endl;
      return true;
  }

  new_type.set("#cpp_type", c_type);
  return false;
}

void solidity_convertert::get_location_from_decl(
  const SourceLocationTracker &decl_slm,
  locationt &location)
{
  std::string function_name;

  if (decl_slm.get_isFunctionOrMethod())
  {
    assert(!"unsupported get_location_from_decl for function");
    // TODO: set function namae here
  }

  // In clang, we need to get PLoc first.
  // For Solidity, we've already extracted the information during decl tracker config phase.
  // We just need to set the location

  set_location(decl_slm, function_name, location); // for __ESBMC_assume, function_name is still empty after this line.
}

void solidity_convertert::set_location(
  const SourceLocationTracker &decl_slm,
  std::string &function_name,
  locationt &location)
{
  if(!decl_slm.get_isValid())
  {
    location.set_file("<invalid sloc>");
    return;
  }

  location.set_line(decl_slm.get_line_number()); // line number : unsigned signed. For _x, Ploc.getLine() returns 1
  location.set_file(decl_slm.get_file_name()); // string : path + file name. For _x, PLoc.getFilename() returns "overflow_2.c"

  if(!function_name.empty()) // for _x, this statement returns false
  {
    location.set_function(function_name);
  }
}

std::string solidity_convertert::get_modulename_from_path(std::string path)
{
  std::string filename = get_filename_from_path(path);

  if(filename.find_last_of('.') != std::string::npos)
    return filename.substr(0, filename.find_last_of('.'));

  return filename;
}

std::string solidity_convertert::get_filename_from_path(std::string path)
{
  if(path.find_last_of('/') != std::string::npos)
    return path.substr(path.find_last_of('/') + 1);

  return path; // for _x, it just returns "overflow_2.c" because the test program is in the same dir as esbmc binary
}

void solidity_convertert::get_default_symbol(
  symbolt &symbol,
  std::string module_name,
  typet type,
  std::string name,
  std::string id,
  locationt location)
{
  symbol.mode = "C";
  symbol.module = module_name;
  symbol.location = std::move(location);
  symbol.type = std::move(type);
  symbol.name = name;
  symbol.id = id;
}

symbolt *solidity_convertert::move_symbol_to_context(symbolt &symbol)
{
  symbolt *s = context.find_symbol(symbol.id);
  if(s == nullptr)
  {
    if(context.move(symbol, s))
    {
      std::cerr << "Couldn't add symbol " << symbol.name
                << " to symbol table\n";
      symbol.dump();
      abort();
    }
  }
  else
  {
    // types that are code means functions
    if(s->type.is_code())
    {
      if(symbol.value.is_not_nil() && !s->value.is_not_nil())
        s->swap(symbol);
    }
    else if(s->is_type)
    {
      if(symbol.type.is_not_nil() && !s->type.is_not_nil())
        s->swap(symbol);
    }
  }

  return s;
}

void solidity_convertert::convert_expression_to_code(exprt &expr)
{
  if(expr.is_code())
    return;

  codet code("expression");
  code.location() = expr.location();
  code.move_to_operands(expr);

  expr.swap(code);
}

std::shared_ptr<VarDeclTracker> solidity_convertert::get_matched_decl_ref(unsigned ref_id)
{
  for(auto var_decl : global_vars)
  {
    if (var_decl->get_id() == ref_id)
      return var_decl;
  }

  assert(!"should not be here - no matching decl ref id");
}

void solidity_convertert::print_json_element(nlohmann::json &json_in, const unsigned index,
    const std::string &key, const std::string& json_name)
{
  printf("### %s element[%u] content: key=%s, size=%lu ###\n",
      json_name.c_str(), index, key.c_str(), json_in.size());
  std::cout << std::setw(2) << json_in << '\n'; // '2' means 2x indentations in front of each line
  printf("\n");
}

void solidity_convertert::print_json_array_element(nlohmann::json &json_in,
    const std::string& node_type, const unsigned index)
{
  printf("### node[%u]: nodeType=%s ###\n", index, node_type.c_str());
  std::cout << std::setw(2) << json_in << '\n'; // '2' means 2x indentations in front of each line
  printf("\n");
}