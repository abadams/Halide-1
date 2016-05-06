#include <set>
#include <stdlib.h>
#include <atomic>

#include "IR.h"
#include "Function.h"
#include "Scope.h"
#include "CSE.h"
#include "Random.h"
#include "Introspection.h"
#include "IRPrinter.h"
#include "IRMutator.h"
#include "ParallelRVar.h"
#include "Var.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::string;
using std::set;
using std::map;

typedef map<IntrusivePtr<FunctionContents>, IntrusivePtr<FunctionContents>> DeepCopyMap;
void deep_copy_update_definition_helper(UpdateDefinition &dst,
                                        const UpdateDefinition &src,
                                        DeepCopyMap &copied_map);
void deep_copy_extern_func_argument_helper(ExternFuncArgument &dst,
                                           const ExternFuncArgument &src,
                                           DeepCopyMap &copied_map);
void deep_copy_function_contents_helper(IntrusivePtr<FunctionContents> &dst,
                                        const IntrusivePtr<FunctionContents> &src,
                                        DeepCopyMap &copied_map);
IntrusivePtr<FunctionContents> deep_copy_function_contents_helper(
    const IntrusivePtr<FunctionContents> &src, DeepCopyMap &copied);

struct FunctionContents {
    mutable RefCount ref_count;
    std::string name;
    std::vector<std::string> args;
    std::vector<Expr> values;
    std::vector<Type> output_types;
    Schedule schedule;

    std::vector<UpdateDefinition> updates;

    std::string debug_file;

    std::vector<Parameter> output_buffers;

    std::vector<ExternFuncArgument> extern_arguments;
    std::string extern_function_name;
    bool extern_is_c_plus_plus;

    bool trace_loads, trace_stores, trace_realizations;

    bool frozen;

    FunctionContents() : extern_is_c_plus_plus(false), trace_loads(false),
                         trace_stores(false), trace_realizations(false),
                         frozen(false) {}

    void accept(IRVisitor *visitor) const {
        for (Expr i : values) {
            i.accept(visitor);
        }

        schedule.accept(visitor);

        for (UpdateDefinition update : updates) {
            for (Expr i : update.values) {
                i.accept(visitor);
            }
            for (Expr i : update.args) {
                i.accept(visitor);
            }

            if (update.domain.defined()) {
                for (ReductionVariable rv : update.domain.domain()) {
                    rv.min.accept(visitor);
                    rv.extent.accept(visitor);
                }
                update.domain.predicate().accept(visitor);
            }

            update.schedule.accept(visitor);
        }

        if (!extern_function_name.empty()) {
            for (ExternFuncArgument i : extern_arguments) {
                if (i.is_func()) {
                    i.func.ptr->accept(visitor);
                } else if (i.is_expr()) {
                    i.expr.accept(visitor);
                }
            }
        }

        for (Parameter i : output_buffers) {
            for (size_t j = 0; j < args.size() && j < 4; j++) {
                if (i.min_constraint(j).defined()) {
                    i.min_constraint(j).accept(visitor);
                }
                if (i.stride_constraint(j).defined()) {
                    i.stride_constraint(j).accept(visitor);
                }
                if (i.extent_constraint(j).defined()) {
                    i.extent_constraint(j).accept(visitor);
                }
            }
        }
    }

    void mutate(IRMutator *mutator) {
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = mutator->mutate(values[i]);
        }

        // Mutate schedule
        for (Split &s : schedule.splits()) {
            if (s.factor.defined()) {
                s.factor = mutator->mutate(s.factor);
            }
        }
        for (Bound &b : schedule.bounds()) {
            if (b.min.defined()) {
                b.min = mutator->mutate(b.min);
            }
            if (b.extent.defined()) {
                b.extent = mutator->mutate(b.extent);
            }
        }
        for (Specialization &s : schedule.specializations()) {
            s.condition = mutator->mutate(s.condition);
        }

        // Mutate update definition
        for (UpdateDefinition &update : updates) {
            for (size_t i = 0; i < update.values.size(); ++i) {
                update.values[i] = mutator->mutate(update.values[i]);
            }
            for (size_t i = 0; i < update.args.size(); ++i) {
                update.args[i] = mutator->mutate(update.args[i]);
            }

            if (update.domain.defined()) {
                for (ReductionVariable &rv : update.domain.domain()) {
                    rv.min = mutator->mutate(rv.min);
                    rv.extent = mutator->mutate(rv.extent);
                }
                update.domain.set_predicate(mutator->mutate(update.domain.predicate()));
            }

            // Mutate update definition's schedule
            for (Split &s : update.schedule.splits()) {
                if (s.factor.defined()) {
                    s.factor = mutator->mutate(s.factor);
                }
            }
            for (Bound &b : update.schedule.bounds()) {
                if (b.min.defined()) {
                    b.min = mutator->mutate(b.min);
                }
                if (b.extent.defined()) {
                    b.extent = mutator->mutate(b.extent);
                }
            }
            for (Specialization &s : update.schedule.specializations()) {
                s.condition = mutator->mutate(s.condition);
            }

            // Don't need to mutate the schedule's reduction domain since it is
            // the same as UpdateDefinition's domain. Do the check to make sure
            // it is the case.
            internal_assert(update.domain.same_as(update.schedule.reduction_domain()))
                << "UpdateDefinition should point to the same reduction domain as its schedule\n";
        }

        if (!extern_function_name.empty()) {
            for (ExternFuncArgument &i : extern_arguments) {
                if (i.is_func()) {
                    i.func.ptr->mutate(mutator);
                } else if (i.is_expr()) {
                    i.expr = mutator->mutate(i.expr);
                }
            }
        }
    }
};

template<>
EXPORT RefCount &ref_count<FunctionContents>(const FunctionContents *f) {
    return f->ref_count;
}

template<>
EXPORT void destroy<FunctionContents>(const FunctionContents *f) {
    delete f;
}

// All variables present in any part of a function definition must
// either be pure args, elements of the reduction domain, parameters
// (i.e. attached to some Parameter object), or part of a let node
// internal to the expression
struct CheckVars : public IRGraphVisitor {
    vector<string> pure_args;
    ReductionDomain reduction_domain;
    Scope<int> defined_internally;
    const std::string name;
    bool unbound_reduction_vars_ok = false;

    CheckVars(const std::string &n) :
        name(n) {}

    using IRVisitor::visit;

    void visit(const Let *let) {
        let->value.accept(this);
        defined_internally.push(let->name, 0);
        let->body.accept(this);
        defined_internally.pop(let->name);
    }

    void visit(const Call *op) {
        IRGraphVisitor::visit(op);
        if (op->name == name && op->call_type == Call::Halide) {
            for (size_t i = 0; i < op->args.size(); i++) {
                const Variable *var = op->args[i].as<Variable>();
                if (!pure_args[i].empty()) {
                    user_assert(var && var->name == pure_args[i])
                        << "In definition of Func \"" << name << "\":\n"
                        << "All of a functions recursive references to itself"
                        << " must contain the same pure variables in the same"
                        << " places as on the left-hand-side.\n";
                }
            }
        }
    }

    void visit(const Variable *var) {
        // Is it a parameter?
        if (var->param.defined()) return;

        // Was it defined internally by a let expression?
        if (defined_internally.contains(var->name)) return;

        // Is it a pure argument?
        for (size_t i = 0; i < pure_args.size(); i++) {
            if (var->name == pure_args[i]) return;
        }

        // Is it in a reduction domain?
        if (var->reduction_domain.defined()) {
            if (!reduction_domain.defined()) {
                reduction_domain = var->reduction_domain;
                return;
            } else if (var->reduction_domain.same_as(reduction_domain)) {
                // It's in a reduction domain we already know about
                return;
            } else {
                user_error << "Multiple reduction domains found in definition of Func \"" << name << "\"\n";
            }
        } else if (reduction_domain.defined() && unbound_reduction_vars_ok) {
            // Is it one of the RVars from the reduction domain we already
            // know about (this can happen in the RDom predicate).
            for (const ReductionVariable &rv : reduction_domain.domain()) {
                if (rv.var == var->name) {
                    return;
                }
            }
        }

        user_error << "Undefined variable \"" << var->name << "\" in definition of Func \"" << name << "\"\n";
    }
};

struct DeleteSelfReferences : public IRMutator {
    IntrusivePtr<FunctionContents> func;

    // Also count the number of self references so we know if a Func
    // has a recursive definition.
    int count = 0;

    using IRMutator::visit;

    void visit(const Call *c) {
        IRMutator::visit(c);
        c = expr.as<Call>();
        internal_assert(c);
        if (c->func.same_as(func)) {
            expr = Call::make(c->type, c->name, c->args, c->call_type,
                              nullptr, c->value_index,
                              c->image, c->param);
            count++;
        }
    }
};

// Mark all functions found in an expr as frozen.
class FreezeFunctions : public IRGraphVisitor {
    using IRGraphVisitor::visit;

    const string &func;

    void visit(const Call *op) {
        IRGraphVisitor::visit(op);
        if (op->call_type == Call::Halide &&
            op->func.defined() &&
            op->name != func) {
            Function f(op->func);
            f.freeze();
        }
    }
public:
    FreezeFunctions(const string &f) : func(f) {}
};

// A counter to use in tagging random variables
namespace {
static std::atomic<int> rand_counter;
}

Function::Function() : contents(new FunctionContents) {
}

Function::Function(const IntrusivePtr<FunctionContents> &ptr) : contents(ptr) {
    internal_assert(ptr.defined())
        << "Can't construct Function from undefined FunctionContents ptr\n";
}

Function::Function(const std::string &n) : contents(new FunctionContents) {
    for (size_t i = 0; i < n.size(); i++) {
        user_assert(n[i] != '.')
            << "Func name \"" << n << "\" is invalid. "
            << "Func names may not contain the character '.', "
            << "as it is used internally by Halide as a separator\n";
    }
    contents.ptr->name = n;
}

void deep_copy_update_definition_helper(IntrusivePtr<FunctionContents> &contents,
                                        UpdateDefinition &dst,
                                        const UpdateDefinition &src,
                                        DeepCopyMap &copied_map) {
    dst.values = src.values;
    dst.args = src.args;
    dst.schedule = src.schedule.deep_copy(copied_map);
    // UpdateDefinition's domain is the same as the one pointed by its schedule
    internal_assert(src.schedule.reduction_domain().same_as(src.domain))
        << "UpdateDefinition should point to the same reduction domain as its schedule\n";
    dst.domain = dst.schedule.reduction_domain();
}

void deep_copy_extern_func_argument_helper(ExternFuncArgument &dst,
                                           const ExternFuncArgument &src,
                                           DeepCopyMap &copied_map) {
    dst.arg_type = src.arg_type;
    dst.buffer = src.buffer;
    dst.expr = src.expr;
    dst.image_param = src.image_param;

    if (!src.func.defined()) { // No need to copy the func
        internal_assert(!src.is_func())
            << "ExternFuncArgument has type FuncArg but has no function definition\n";
        return;
    }

    IntrusivePtr<FunctionContents> &copied_func = copied_map[src.func];
    if (copied_func.defined()) {
        dst.func = copied_func;
    } else {
        dst.func = IntrusivePtr<FunctionContents>(new FunctionContents);
        deep_copy_function_contents_helper(dst.func, src.func, copied_map);
        copied_map[src.func] = dst.func;
    }
}

IntrusivePtr<FunctionContents> deep_copy_function_contents_helper(
        const IntrusivePtr<FunctionContents> &src, DeepCopyMap &copied) {

    IntrusivePtr<FunctionContents> copy(new FunctionContents);
    deep_copy_function_contents_helper(copy, src, copied);
    return copy;
}

void deep_copy_function_contents_helper(IntrusivePtr<FunctionContents> &dst,
                                        const IntrusivePtr<FunctionContents> &src,
                                        DeepCopyMap &copied_map) {
    debug(4) << "Deep-copy function contents: \"" << src.ptr->name << "\"\n";

    internal_assert(dst.defined() && src.defined()) << "Cannot deep-copy undefined Function\n";

    dst.ptr->name = src.ptr->name;
    dst.ptr->args = src.ptr->args;
    dst.ptr->values = src.ptr->values;
    dst.ptr->output_types = src.ptr->output_types;
    dst.ptr->debug_file = src.ptr->debug_file;
    dst.ptr->extern_function_name = src.ptr->extern_function_name;
    dst.ptr->extern_is_c_plus_plus = src.ptr->extern_is_c_plus_plus;
    dst.ptr->trace_loads = src.ptr->trace_loads;
    dst.ptr->trace_stores = src.ptr->trace_stores;
    dst.ptr->trace_realizations = src.ptr->trace_realizations;
    dst.ptr->frozen = src.ptr->frozen;
    dst.ptr->output_buffers = src.ptr->output_buffers;

    dst.ptr->schedule = src.ptr->schedule.deep_copy(copied_map);

    for (const auto &u : src.ptr->updates) {
        UpdateDefinition u_copy;
        deep_copy_update_definition_helper(dst, u_copy, u, copied_map);
        internal_assert(u_copy.domain.same_as(u_copy.schedule.reduction_domain()))
            << "UpdateDefinition should point to the same reduction domain as its schedule\n";
        dst.ptr->updates.push_back(std::move(u_copy));
    }
    for (const auto &e : src.ptr->extern_arguments) {
        ExternFuncArgument e_copy;
        deep_copy_extern_func_argument_helper(e_copy, e, copied_map);
        dst.ptr->extern_arguments.push_back(std::move(e_copy));
    }
}

void Function::deep_copy(Function &copy, std::map<Function, Function> &copied_map) const {
    internal_assert(copy.contents.defined() && contents.defined()) << "Cannot deep-copy undefined Function\n";
    DeepCopyMap copied_funcs_map;
    for (const auto &iter : copied_map) {
        copied_funcs_map[iter.first.contents] = iter.second.contents;
    }
    // Add reference to its copy in case of self-reference.
    copied_funcs_map[contents] = copy.contents;

    deep_copy_function_contents_helper(copy.contents, contents, copied_funcs_map);

    for (const auto &iter : copied_funcs_map) {
        Function old_func = Function(iter.first);
        if (copied_map.count(old_func)) {
            internal_assert(copied_map[old_func].contents.same_as(iter.second))
                << old_func.name() << " is deep-copied twice\n";
            continue;
        }
        copied_map[old_func] = Function(iter.second);
    }
}

void Function::define(const vector<string> &args, vector<Expr> values) {
    user_assert(!frozen())
        << "Func " << name() << " cannot be given a new pure definition, "
        << "because it has already been realized or used in the definition of another Func.\n";
    user_assert(!has_extern_definition())
        << "In pure definition of Func \"" << name() << "\":\n"
        << "Func with extern definition cannot be given a pure definition.\n";
    user_assert(!name().empty()) << "A Func may not have an empty name.\n";
    for (size_t i = 0; i < values.size(); i++) {
        user_assert(values[i].defined())
            << "In pure definition of Func \"" << name() << "\":\n"
            << "Undefined expression in right-hand-side of definition.\n";
    }

    // Make sure all the vars in the value are either args or are
    // attached to some parameter
    CheckVars check(name());
    check.pure_args = args;
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&check);
    }

    // Freeze all called functions
    FreezeFunctions freezer(name());
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&freezer);
    }

    // Make sure all the vars in the args have unique non-empty names
    for (size_t i = 0; i < args.size(); i++) {
        user_assert(!args[i].empty())
            << "In pure definition of Func \"" << name() << "\":\n"
            << "In left-hand-side of definition, argument "
            << i << " has an empty name.\n";
        for (size_t j = 0; j < i; j++) {
            user_assert(args[i] != args[j])
                << "In pure definition of Func \"" << name() << "\":\n"
                << "In left-hand-side of definition, arguments "
                << i << " and " << j
                << " both have the name \"" + args[i] + "\"\n";
        }
    }

    for (size_t i = 0; i < values.size(); i++) {
        values[i] = common_subexpression_elimination(values[i]);
    }

    // Tag calls to random() with the free vars
    int tag = rand_counter++;
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = lower_random(values[i], args, tag);
    }

    user_assert(!check.reduction_domain.defined())
        << "In pure definition of Func \"" << name() << "\":\n"
        << "Reduction domain referenced in pure function definition.\n";

    if (!contents.defined()) {
        contents = new FunctionContents;
        contents.ptr->name = unique_name('f');
    }

    user_assert(contents.ptr->values.empty())
        << "In pure definition of Func \"" << name() << "\":\n"
        << "Func is already defined.\n";

    contents.ptr->values = values;
    contents.ptr->args = args;

    contents.ptr->output_types.resize(values.size());
    for (size_t i = 0; i < contents.ptr->output_types.size(); i++) {
        contents.ptr->output_types[i] = values[i].type();
    }

    for (size_t i = 0; i < args.size(); i++) {
        Dim d = {args[i], ForType::Serial, DeviceAPI::None, true};
        contents.ptr->schedule.dims().push_back(d);
        StorageDim sd = {args[i]};
        contents.ptr->schedule.storage_dims().push_back(sd);
    }

    // Add the dummy outermost dim
    {
        Dim d = {Var::outermost().name(), ForType::Serial, DeviceAPI::None, true};
        contents.ptr->schedule.dims().push_back(d);
    }

    for (size_t i = 0; i < values.size(); i++) {
        string buffer_name = name();
        if (values.size() > 1) {
            buffer_name += '.' + std::to_string((int)i);
        }
        Parameter output(values[i].type(), true, args.size(), buffer_name);
        contents.ptr->output_buffers.push_back(output);
    }
}

void Function::define_update(const vector<Expr> &_args, vector<Expr> values) {
    int update_idx = static_cast<int>(contents.ptr->updates.size());

    user_assert(!name().empty())
        << "Func has an empty name.\n";
    user_assert(has_pure_definition())
        << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
        << "Can't add an update definition without a pure definition first.\n";
    user_assert(!frozen())
        << "Func " << name() << " cannot be given a new update definition, "
        << "because it has already been realized or used in the definition of another Func.\n";

    for (size_t i = 0; i < values.size(); i++) {
        user_assert(values[i].defined())
            << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
            << "Undefined expression in right-hand-side of update.\n";

    }

    // Check the dimensionality matches
    user_assert((int)_args.size() == dimensions())
        << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
        << "Dimensionality of update definition must match dimensionality of pure definition.\n";

    user_assert(values.size() == contents.ptr->values.size())
        << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
        << "Number of tuple elements for update definition must "
        << "match number of tuple elements for pure definition.\n";

    for (size_t i = 0; i < values.size(); i++) {
        // Check that pure value and the update value have the same
        // type.  Without this check, allocations may be the wrong size
        // relative to what update code expects.
        Type pure_type = contents.ptr->values[i].type();
        if (pure_type != values[i].type()) {
            std::ostringstream err;
            err << "In update definition " << update_idx << " of Func \"" << name() << "\":\n";
            if (values.size()) {
                err << "Tuple element " << i << " of update definition has type ";
            } else {
                err << "Update definition has type ";
            }
            err << values[i].type() << ", but pure definition has type " << pure_type;
            user_error << err.str() << "\n";
        }
        values[i] = common_subexpression_elimination(values[i]);
    }

    vector<Expr> args(_args.size());
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = common_subexpression_elimination(_args[i]);
    }

    // The pure args are those naked vars in the args that are not in
    // a reduction domain and are not parameters and line up with the
    // pure args in the pure definition.
    bool pure = true;
    vector<string> pure_args(args.size());
    for (size_t i = 0; i < args.size(); i++) {
        pure_args[i] = ""; // Will never match a var name
        user_assert(args[i].defined())
            << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
            << "Argument " << i
            << " in left-hand-side of update definition is undefined.\n";
        if (const Variable *var = args[i].as<Variable>()) {
            if (!var->param.defined() &&
                !var->reduction_domain.defined() &&
                var->name == contents.ptr->args[i]) {
                pure_args[i] = var->name;
            } else {
                pure = false;
            }
        } else {
            pure = false;
        }
    }

    // Make sure all the vars in the args and the value are either
    // pure args, in the reduction domain, or a parameter. Also checks
    // that recursive references to the function contain all the pure
    // vars in the LHS in the correct places.
    CheckVars check(name());
    check.pure_args = pure_args;
    for (size_t i = 0; i < args.size(); i++) {
        args[i].accept(&check);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&check);
    }
    if (check.reduction_domain.defined()) {
        check.unbound_reduction_vars_ok = true;
        check.reduction_domain.predicate().accept(&check);
    }

    // Freeze all called functions
    FreezeFunctions freezer(name());
    for (size_t i = 0; i < args.size(); i++) {
        args[i].accept(&freezer);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i].accept(&freezer);
    }

    // Freeze the reduction domain if defined
    if (check.reduction_domain.defined()) {
        check.reduction_domain.predicate().accept(&freezer);
        check.reduction_domain.freeze();
    }

    // Tag calls to random() with the free vars
    vector<string> free_vars;
    for (size_t i = 0; i < pure_args.size(); i++) {
        if (!pure_args[i].empty()) {
            free_vars.push_back(pure_args[i]);
        }
    }
    if (check.reduction_domain.defined()) {
        for (size_t i = 0; i < check.reduction_domain.domain().size(); i++) {
            string rvar = check.reduction_domain.domain()[i].var;
            free_vars.push_back(rvar);
        }
    }
    int tag = rand_counter++;
    for (size_t i = 0; i < args.size(); i++) {
        args[i] = lower_random(args[i], free_vars, tag);
    }
    for (size_t i = 0; i < values.size(); i++) {
        values[i] = lower_random(values[i], free_vars, tag);
    }
    if (check.reduction_domain.defined()) {
        check.reduction_domain.set_predicate(lower_random(check.reduction_domain.predicate(), free_vars, tag));
    }

    UpdateDefinition r;
    r.args = args;
    r.values = values;
    r.domain = check.reduction_domain;
    r.schedule.set_reduction_domain(r.domain);

    // The update value and args probably refer back to the
    // function itself, introducing circular references and hence
    // memory leaks. We need to break these cycles.
    DeleteSelfReferences deleter;
    deleter.func = contents;
    deleter.count = 0;
    for (size_t i = 0; i < args.size(); i++) {
        r.args[i] = deleter.mutate(r.args[i]);
    }
    for (size_t i = 0; i < values.size(); i++) {
        r.values[i] = deleter.mutate(r.values[i]);
    }
    if (r.domain.defined()) {
        r.domain.set_predicate(deleter.mutate(r.domain.predicate()));
    }

    // First add any reduction domain
    if (r.domain.defined()) {
        for (size_t i = 0; i < r.domain.domain().size(); i++) {
            // Is this RVar actually pure (safe to parallelize and
            // reorder)? It's pure if one value of the RVar can never
            // access from the same memory that another RVar is
            // writing to.
            const string &v = r.domain.domain()[i].var;

            bool pure = can_parallelize_rvar(v, name(), r);

            Dim d = {v, ForType::Serial, DeviceAPI::None, pure};
            r.schedule.dims().push_back(d);
        }
    }

    // Then add the pure args outside of that
    for (size_t i = 0; i < pure_args.size(); i++) {
        if (!pure_args[i].empty()) {
            Dim d = {pure_args[i], ForType::Serial, DeviceAPI::None, true};
            r.schedule.dims().push_back(d);
        }
    }

    // Then the dummy outermost dim
    {
        Dim d = {Var::outermost().name(), ForType::Serial, DeviceAPI::None, true};
        r.schedule.dims().push_back(d);
    }

    // If there's no recursive reference, no reduction domain, and all
    // the args are pure, then this definition completely hides
    // earlier ones!
    if (!r.domain.defined() &&
        deleter.count == 0 &&
        pure) {
        user_warning
            << "In update definition " << update_idx << " of Func \"" << name() << "\":\n"
            << "Update definition completely hides earlier definitions, "
            << " because all the arguments are pure, it contains no self-references, "
            << " and no reduction domain. This may be an accidental re-definition of "
            << " an already-defined function.\n";
    }

    contents.ptr->updates.push_back(r);

}

void Function::define_extern(const std::string &function_name,
                             const std::vector<ExternFuncArgument> &args,
                             const std::vector<Type> &types,
                             int dimensionality,
                             bool is_c_plus_plus) {

    user_assert(!has_pure_definition() && !has_update_definition())
        << "In extern definition for Func \"" << name() << "\":\n"
        << "Func with a pure definition cannot have an extern definition.\n";

    user_assert(!has_extern_definition())
        << "In extern definition for Func \"" << name() << "\":\n"
        << "Func already has an extern definition.\n";

    contents.ptr->extern_function_name = function_name;
    contents.ptr->extern_arguments = args;
    contents.ptr->output_types = types;
    contents.ptr->extern_is_c_plus_plus = is_c_plus_plus;

    for (size_t i = 0; i < types.size(); i++) {
        string buffer_name = name();
        if (types.size() > 1) {
            buffer_name += '.' + std::to_string((int)i);
        }
        Parameter output(types[i], true, dimensionality, buffer_name);
        contents.ptr->output_buffers.push_back(output);
    }

    // Make some synthetic var names for scheduling purposes (e.g. reorder_storage).
    contents.ptr->args.resize(dimensionality);
    for (int i = 0; i < dimensionality; i++) {
        string arg = unique_name('e');
        contents.ptr->args[i] = arg;
        StorageDim sd = {arg};
        contents.ptr->schedule.storage_dims().push_back(sd);
    }
}

void Function::accept(IRVisitor *visitor) const {
    contents.ptr->accept(visitor);
}

const std::string &Function::name() const {
    return contents.ptr->name;
}

const std::vector<std::string> &Function::args() const {
    return contents.ptr->args;
}

const std::vector<Type> &Function::output_types() const {
    return contents.ptr->output_types;
}

const std::vector<Expr> &Function::values() const {
    return contents.ptr->values;
}

Schedule &Function::schedule() {
    return contents.ptr->schedule;
}

const Schedule &Function::schedule() const {
    return contents.ptr->schedule;
}

const std::vector<Parameter> &Function::output_buffers() const {
    return contents.ptr->output_buffers;
}

Schedule &Function::update_schedule(int idx) {
    return contents.ptr->updates[idx].schedule;
}

const std::vector<UpdateDefinition> &Function::updates() const {
    return contents.ptr->updates;
}

bool Function::has_update_definition() const {
    return !contents.ptr->updates.empty();
}

bool Function::has_extern_definition() const {
    return !contents.ptr->extern_function_name.empty();
}

bool Function::extern_definition_is_c_plus_plus() const {
    return contents.ptr->extern_is_c_plus_plus;
}

const std::vector<ExternFuncArgument> &Function::extern_arguments() const {
    return contents.ptr->extern_arguments;
}

const std::string &Function::extern_function_name() const {
    return contents.ptr->extern_function_name;
}

const std::string &Function::debug_file() const {
    return contents.ptr->debug_file;
}

std::string &Function::debug_file() {
    return contents.ptr->debug_file;
}

void Function::trace_loads() {
    contents.ptr->trace_loads = true;
}
void Function::trace_stores() {
    contents.ptr->trace_stores = true;
}
void Function::trace_realizations() {
    contents.ptr->trace_realizations = true;
}
bool Function::is_tracing_loads() const {
    return contents.ptr->trace_loads;
}
bool Function::is_tracing_stores() const {
    return contents.ptr->trace_stores;
}
bool Function::is_tracing_realizations() const {
    return contents.ptr->trace_realizations;
}

void Function::freeze() {
    contents.ptr->frozen = true;
}

bool Function::frozen() const {
    return contents.ptr->frozen;
}

const map<string, IntrusivePtr<FunctionContents>> &Function::wrappers() const {
    return contents.ptr->schedule.wrappers();
}

void Function::add_wrapper(const Function &wrapper, const std::string &f) {
    contents.ptr->schedule.add_wrapper(wrapper.contents, f);
}

Function &Function::substitute_calls(const map<Function, Function> &substitutions) {
    // Replace all calls to functions listed in 'substitutions' with their wrappers.
    class SubstituteCalls : public IRMutator {
        using IRMutator::visit;

        map<Function, Function> substitutions;

        void visit(const Call *c) {
            IRMutator::visit(c);
            c = expr.as<Call>();
            internal_assert(c);

            if ((c->call_type == Call::Halide) && c->func.defined() && substitutions.count(Function(c->func))) {
                const Function &subs = substitutions[Function(c->func)];
                debug(4) << "...Replace call to Func \"" << c->name << "\" with "
                         << "\"" << subs.name() << "\"\n";
                expr = Call::make(subs, c->args, c->value_index);
            }
        }
    public:
        SubstituteCalls(const map<Function, Function> &substitutions)
            : substitutions(substitutions) {}
    };

    debug(4) << "Substituting calls in " << name() << "\n";

    if (substitutions.empty()) {
        return *this;
    }
    SubstituteCalls subs_calls(substitutions);
    contents.ptr->mutate(&subs_calls);
    return *this;
}

Function &Function::substitute_calls(const Function &orig, const Function &substitute) {
    map<Function, Function> substitutions;
    substitutions.emplace(orig, substitute);
    return substitute_calls(substitutions);
}

}
}
