/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HHBBC_REPRESENTATION_H_
#define incl_HHBBC_REPRESENTATION_H_

#include <list>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/variant.hpp>

#include "hphp/util/md5.h"
#include "hphp/runtime/base/user-attributes.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/runtime/vm/type-alias.h"
#include "hphp/runtime/vm/type-constraint.h"

#include "hphp/hhbbc/bc.h"
#include "hphp/hhbbc/misc.h"
#include "hphp/hhbbc/src-loc.h"

namespace HPHP { namespace HHBBC {
namespace php {

//////////////////////////////////////////////////////////////////////

struct Func;
struct ExnNode;
struct Unit;

//////////////////////////////////////////////////////////////////////

struct SrcInfo {
  LineRange loc;
  LSString docComment;
};

//////////////////////////////////////////////////////////////////////

/*
 * A basic block in our factored control flow graph.
 *
 * Blocks terminate on control flow, except exceptional control flow.
 * We keep a set of "factored edges" representing all possible early
 * exits due to exceptional control flow.
 */
struct Block {
  /*
   * Blocks in HHBC are each part of a bytecode "section".  The section
   * is either the "primary function body", or a fault funclet.  We
   * represent fault funclet sections with unique ids.
   *
   * Each section must be a contiguous region of bytecode, with the
   * primary function body first.  These ids are tracked just to
   * maintain this invariant at emit time.
   */
  enum class Section : uint32_t { Main = 0 };
  Section section;

  /*
   * Blocks have unique ids within a given function.
   */
  uint32_t id;

  /*
   * Instructions in the block.  Never empty guarantee.
   */
  std::vector<Bytecode> hhbcs;

  /*
   * The pointer for this block's exception region, or nullptr if
   * there is none.
   */
  borrowed_ptr<ExnNode> exnNode;

  /*
   * Edges coming out of blocks are repesented in three ways:
   *
   *  - fallthrough edges (the end of the block unconditionally jumps
   *    to the named block).  If fallthroughNS is true, this edge
   *    represents a no-surprise jump.
   *
   *  - Taken edges (these are encoded in the last instruction in hhbcs).
   *
   *  - factoredExits (these represent edges traversed for exceptions
   *    mid-block)
   *
   * For the idea behind the factored exit edge thing, see "Efficient
   * and Precise Modeling of Exceptions for the Analysis of Java
   * Programs" (http://dl.acm.org/citation.cfm?id=316171).
   */
  BlockId fallthrough{NoBlockId};
  bool fallthroughNS = false;
  std::vector<BlockId> factoredExits;
};

//////////////////////////////////////////////////////////////////////

/*
 * Exception regions.
 *
 * Each block in the program body can have a pointer to a node in the
 * exception handler tree.  This means they are in all the "exception
 * regions" for each node in the tree down to that node.  This
 * information is used to construct exception handling regions at emit
 * time.
 *
 * There are two types of regions; TryRegions and FaultRegions.  These
 * correspond to the two types of regions described in
 * bytecode.specification.  Note though that although it's not
 * specified there, in addition to a fault entry offset, fault regions
 * optionally list some information about iterators if the reason the
 * fault region is there is to free iterator variables.
 *
 * Exceptional control flow is also represented more explicitly with
 * factored exit edges (see php::Block).  This tree structure just
 * exists to get the EHEnts right.
 *
 * Note: blocks in fault funclets will have factored edges to the
 * blocks listed as handlers in any ExnNode that contained the
 * fault-protected region, since those control flow paths are
 * possible.  Generally they will have nullptr for their exnNode
 * pointers, however, although they may also have other EH-protected
 * regions inside of them (this currently occurs in the case of
 * php-level finally blocks cloned into fault funclets).
 */

struct FaultRegion { BlockId faultEntry;
                     Id iterId;
                     bool itRef; };

using CatchEnt     = std::pair<const StringData*,BlockId>;
struct TryRegion   { std::vector<CatchEnt> catches; };

struct ExnNode {
  uint32_t id;

  borrowed_ptr<ExnNode> parent;
  std::vector<std::unique_ptr<ExnNode>> children;

  boost::variant<FaultRegion,TryRegion> info;
};

//////////////////////////////////////////////////////////////////////

/*
 * Metadata about a parameter to a php function.
 */
struct Param {
  /*
   * Default value for this parameter, or KindOfUninit if it has no
   * default value.
   */
  Cell defaultValue;

  /*
   * Pointer to the block we'll enter for default-value initialization
   * of this parameter, or nullptr if this parameter had no default
   * value initializer.
   */
  BlockId dvEntryPoint;

  /*
   * Information about the parameter's typehint, if any.
   *
   * NOTE: this is represented in the repo as a string type name and
   * some TypeConstraint::Flags.
   */
  TypeConstraint typeConstraint;

  /*
   * User-visible version of the type constraint as a string.
   * Propagated for reflection.
   */
  LSString userTypeConstraint;

  /*
   * Evalable php code that will give the default argument.  This is
   * redundant with the dv initializer, but gets propagated through
   * for reflection.
   */
  LSString phpCode;

  /*
   * Each parameter of a func can have arbitrary user attributes.
   */
  UserAttributeMap userAttributes;

  /*
   * The type of the arguments for builtin functions, or for HNI
   * functions with a native implementation.  folly::none for
   * non-builtins.
   */
  folly::Optional<DataType> builtinType;

  /*
   * Whether this parameter is passed by reference.
   */
  bool byRef: 1;

  /*
   * Whether this parameter is a variadic capture.
   */
  bool isVariadic: 1;
};

/*
 * Metadata about a local variable in a function.  Name may be
 * nullptr, for unnamed locals.
 */
struct Local {
  LSString  name;
  uint32_t id     : 31;
  uint32_t killed : 1;
};

/*
 * Static local information.  For each static local, we need to keep
 * the php code around for reflection.
 */
struct StaticLocalInfo {
  LSString name;
  LSString phpCode;
};

/*
 * Extra information for function with a HNI native implementation.
 */
struct NativeInfo {
  /*
   * Return type from the C++ implementation function, as an optional DataType;
   * folly::none stands for a Variant return.
   */
  folly::Optional<DataType> returnType;

  /*
   * Associated dynamic call wrapper function. Used to catch dynamic calls to
   * caller frame affecting functions.
   */
  Id dynCallWrapperId;
};

/*
 * Representation of a function, class method, or pseudomain function.
 */
struct Func {
  /*
   * Basic information about the function.
   */
  LSString name;
  SrcInfo srcInfo;
  Attr attrs;

  /*
   * Parameters, locals, and iterators.
   *
   * There are at least as many locals as parameters (parameters are
   * also locals---the names of parameters are stored in the locals
   * vector).
   */
  IterId             numIters;
  std::vector<Param> params;
  std::vector<Local> locals;
  std::vector<StaticLocalInfo> staticLocals;

  /*
   * Which unit defined this function.  If it is a method, the cls
   * field will be set to the class that contains it.
   */
  borrowed_ptr<Unit> unit;
  borrowed_ptr<Class> cls;

  /*
   * All owning pointers to blocks are in this vector, which has the
   * blocks in an unspecified order.  Blocks have borrowed pointers to
   * each other to represent control flow arcs.
   */
  std::vector<std::unique_ptr<Block>> blocks;

  /*
   * Try and fault regions form a tree structure.  The tree is hanging
   * off the func here, with children pointers.  Each block that is
   * within a try or fault region has a pointer to the inner-most
   * ExnNode protecting it.
   */
  std::vector<std::unique_ptr<ExnNode>> exnNodes;

  /*
   * Entry point blocks for default value initializers.
   *
   * Note that in PHP you can declare functions where some of the
   * earlier parameters have default values, and later ones don't.  In
   * this case we'll have NoBlockIds after the first valid entry here.
   */
  std::vector<BlockId> dvEntries;

  /*
   * Entry point to the function when the number of passed args is
   * equal to the number of parameters.
   */
  BlockId mainEntry;

  /*
   * User-visible return type specification as a string.  This is only
   * passed through to expose it to reflection.
   */
  LSString returnUserType;

  /*
   * If traits are being flattened by hphpc, we keep the original
   * filename of a function (the file that defined the trait) so
   * backtraces and things work correctly.  Otherwise this is nullptr.
   */
  LSString originalFilename;

  /*
   * Whether or not this function is a top-level function.  (Defined
   * outside of any other function body.)
   */
  bool top : 1;

  /*
   * This is the generated function for a closure body.  I.e. this
   * function contains the code that should run when the closure is
   * invoked.
   */
  bool isClosureBody : 1;

  /*
   * This is an async function.
   */
  bool isAsync : 1;

  /*
   * This is a generator.
   */
  bool isGenerator : 1;

  /*
   * This generator yields key value pairs.
   */
  bool isPairGenerator : 1;

  /*
   * This is an HNI function
   */
  bool isNative : 1;

  bool isMemoizeWrapper : 1;

  /*
   * Return type specified in the source code (ex. "function foo(): Bar").
   * HHVM checks if the a function's return value matches it's return type
   * constraint via the VerifyRetType* instructions.
   */
  TypeConstraint retTypeConstraint;

  /*
   * User attribute list.
   */
  UserAttributeMap userAttributes;

  /*
   * For HNI-based extensions, additional information for functions
   * with a native-implementation is here.  If this isn't a function
   * with an HNI-based native implementation, this will be nullptr.
   */
  std::unique_ptr<NativeInfo> nativeInfo;
};

//////////////////////////////////////////////////////////////////////

/*
 * A class property.
 *
 * Both static and instance properties use this structure.
 */
struct Prop {
  LSString name;
  Attr attrs;
  LSString docComment;

  /*
   * Properties can have string type constraints, which we need to
   * propagate through just for reflection purposes.
   */
  LSString typeConstraint;

  /*
   * The default value of the property, for properties with scalar
   * initializers.  May be KindOfUninit in some cases where the
   * property should not have an initial value (i.e. not even null).
   */
  Cell val;
};

/*
 * A class constant.
 */
struct Const {
  LSString name;

  // The class that defined this constant.
  borrowed_ptr<php::Class> cls;

  /*
   * The value will be KindOfUninit if the class constant is defined
   * using an 86cinit method.
   *
   * The lack of a value represents an abstract class constant.
   */
  folly::Optional<Cell> val;

  /*
   * We pass through eval'able php code and a string type constraint,
   * only for exposure to reflection.
   */
  LSString phpCode;
  LSString typeConstraint;

  bool isTypeconst;
};

/*
 * Representation of a php class declaration.
 */
struct Class {
  /*
   * Basic information about the class.
   */
  LSString name;
  SrcInfo srcInfo;
  Attr attrs;

  /*
   * The id used to reference the class within its unit
   */
  int32_t id;

  /*
   * Which unit defined this class.
   */
  borrowed_ptr<Unit> unit;

  /*
   * Hoistability of this class.  See the description in class.h
   * formation on hoistability.
   */
  PreClass::Hoistable hoistability;

  /*
   * Name of the parent class.
   */
  LSString parentName;

  /*
   * If this class represents a closure, this points to the class that
   * lexically contains the closure, if there was one.  If this class
   * doesn't represent a closure, this will be nullptr.
   *
   * The significance of this is that closures created lexically
   * inside of a class run as if they were part of that class context
   * (with regard to access checks, etc).
   */
  borrowed_ptr<php::Class> closureContextCls;

  /*
   * Names of inherited interfaces.
   */
  std::vector<LowStringPtr> interfaceNames;

  /*
   * Names of used traits, number of declared (i.e., non-trait, non-inherited)
   * methods, trait alias/precedence rules (if any).
   *
   * This is using the exact structures from the runtime PreClass.  In
   * WholeProgram mode, we won't see these because traits will already be
   * flattened.
   */
  std::vector<LowStringPtr> usedTraitNames;
  std::vector<PreClass::ClassRequirement> requirements;
  std::vector<PreClass::TraitPrecRule> traitPrecRules;
  std::vector<PreClass::TraitAliasRule> traitAliasRules;
  int32_t numDeclMethods;

  /*
   * Methods on the class.
   */
  std::vector<std::unique_ptr<php::Func>> methods;

  /*
   * Properties defined on this class.
   */
  std::vector<Prop> properties;

  /*
   * Constants defined on this class.
   */
  std::vector<Const> constants;

  /*
   * User attributes for this class declaration.
   */
  UserAttributeMap userAttributes;

  /*
   * The underlying base type, if this is an enum
   */
  TypeConstraint enumBaseTy;
};

//////////////////////////////////////////////////////////////////////

using TypeAlias = ::HPHP::TypeAlias;

//////////////////////////////////////////////////////////////////////

/*
 * Representation of a php file (normal compilation unit).
 */
struct Unit {
  MD5 md5;
  LSString filename;
  bool isHHFile{false};
  bool useStrictTypes{false};
  int preloadPriority{0};
  std::unique_ptr<Func> pseudomain;
  std::vector<std::unique_ptr<Func>> funcs;
  std::vector<std::unique_ptr<Class>> classes;
  std::vector<std::unique_ptr<TypeAlias>> typeAliases;
  std::vector<SrcLoc> srcLocs;
};

/*
 * A php Program is a set of compilation units.
 */
struct Program {
  std::vector<std::unique_ptr<Unit>> units;
};

//////////////////////////////////////////////////////////////////////

std::string show(const Func&);
std::string show(const Class&);
std::string show(const Unit&);
std::string show(const Program&);
std::string local_string(const Func&, LocalId);

//////////////////////////////////////////////////////////////////////

bool check(const Func&);
bool check(const Class&);
bool check(const Unit&);
bool check(const Program&);

//////////////////////////////////////////////////////////////////////

}}}

#endif
