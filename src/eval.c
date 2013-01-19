//
//  Copyright (C) 2013  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "phase.h"
#include "util.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#define MAX_BUILTIN_ARGS 2
#define VTABLE_SZ        16

static ident_t std_bool_i = NULL;
static ident_t builtin_i  = NULL;
static ident_t result_i   = NULL;

typedef struct vtable vtable_t;
typedef struct vtframe vtframe_t;

struct vtframe {
   struct {
      ident_t name;
      tree_t  value;
   } binding[VTABLE_SZ];

   size_t     size;
   vtframe_t *down;
};

struct vtable {
   vtframe_t *top;
};

static void eval_stmt(tree_t t, vtable_t *v);
static tree_t eval_expr(tree_t t, vtable_t *v);

static void vtable_push(vtable_t *v)
{
   vtframe_t *f = xmalloc(sizeof(vtframe_t));
   f->size = 0;
   f->down = v->top;

   v->top = f;
}

static void vtable_pop(vtable_t *v)
{
   vtframe_t *f = v->top;
   v->top = f->down;
   free(f);
}

static void vtable_bind(vtable_t *v, ident_t name, tree_t value)
{
   vtframe_t *f = v->top;
   if (f == NULL)
      return;

   for (size_t i = 0; i < f->size; i++) {
      if (f->binding[i].name == name) {
         f->binding[i].value = value;
         return;
      }
   }

   assert(f->size < VTABLE_SZ);
   f->binding[f->size].name  = name;
   f->binding[f->size].value = value;
   ++(f->size);
}

static tree_t vtframe_get(vtframe_t *f, ident_t name)
{
   if (f == NULL)
      return NULL;
   else {
      for (size_t i = 0; i < f->size; i++) {
         if (f->binding[i].name == name)
            return f->binding[i].value;
      }
      return vtframe_get(f->down, name);
   }
}

static tree_t vtable_get(vtable_t *v, ident_t name)
{
   return vtframe_get(v->top, name);
}

static bool folded_int(tree_t t, literal_t *l)
{
   if (tree_kind(t) == T_LITERAL) {
      *l = tree_literal(t);
      return (l->kind == L_INT);
   }
   else
      return false;
}

static bool folded_real(tree_t t, literal_t *l)
{
   if (tree_kind(t) == T_LITERAL) {
      *l = tree_literal(t);
      return (l->kind == L_REAL);
   }
   else
      return false;
}

static bool folded_bool(tree_t t, bool *b)
{
   if (tree_kind(t) == T_REF) {
      tree_t decl = tree_ref(t);
      if (tree_kind(decl) == T_ENUM_LIT
          && type_ident(tree_type(decl)) == std_bool_i) {
         if (b != NULL)
            *b = (tree_pos(decl) == 1);
         return true;
      }
   }

   return false;
}

static bool folded_agg(tree_t t)
{
   if (tree_kind(t) == T_AGGREGATE) {
      for (unsigned i = 0; i < tree_assocs(t); i++) {
         assoc_t a = tree_assoc(t, i);
         literal_t dummy;
         switch (a.kind) {
         case A_NAMED:
            if (!folded_int(a.name, &dummy))
               return false;
            break;
         case A_RANGE:
            if (!folded_int(a.range.left, &dummy)
                || !folded_int(a.range.right, &dummy))
               return false;
            break;
         default:
            break;
         }
      }
      return true;
   }
   else
      return false;
}

static bool folded(tree_t t)
{
   tree_kind_t kind = tree_kind(t);
   if (kind == T_LITERAL)
      return true;
   else if (kind == T_AGGREGATE)
      return folded_agg(t);
   else if (kind == T_REF)
      return folded_bool(t, NULL);
   else
      return false;
}

static tree_t get_int_lit(tree_t t, int64_t i)
{
   tree_t fdecl = tree_ref(t);
   assert(tree_kind(fdecl) == T_FUNC_DECL);

   literal_t l;
   l.kind = L_INT;
   l.i = i;

   tree_t f = tree_new(T_LITERAL);
   tree_set_loc(f, tree_loc(t));
   tree_set_literal(f, l);
   tree_set_type(f, tree_type(t));

   return f;
}

static tree_t get_real_lit(tree_t t, double r)
{
   tree_t fdecl = tree_ref(t);
   assert(tree_kind(fdecl) == T_FUNC_DECL);

   literal_t l;
   l.kind = L_REAL;
   l.r = r;

   tree_t f = tree_new(T_LITERAL);
   tree_set_loc(f, tree_loc(t));
   tree_set_literal(f, l);
   tree_set_type(f, tree_type(t));

   return f;
}

static tree_t get_bool_lit(tree_t t, bool v)
{
   tree_t fdecl = tree_ref(t);
   assert(tree_kind(fdecl) == T_FUNC_DECL);

   type_t std_bool = type_result(tree_type(fdecl));

   assert(type_ident(std_bool) == std_bool_i);
   assert(type_enum_literals(std_bool) == 2);

   tree_t lit = type_enum_literal(std_bool, v ? 1 : 0);

   tree_t b = tree_new(T_REF);
   tree_set_loc(b, tree_loc(t));
   tree_set_ref(b, lit);
   tree_set_type(b, std_bool);
   tree_set_ident(b, tree_ident(lit));

   return b;
}

static tree_t simp_fcall_log(tree_t t, ident_t builtin, bool *args)
{
   if (icmp(builtin, "not"))
      return get_bool_lit(t, !args[0]);
   else if (icmp(builtin, "and"))
      return get_bool_lit(t, args[0] && args[1]);
   else if (icmp(builtin, "nand"))
      return get_bool_lit(t, !(args[0] && args[1]));
   else if (icmp(builtin, "or"))
      return get_bool_lit(t, args[0] || args[1]);
   else if (icmp(builtin, "nor"))
      return get_bool_lit(t, !(args[0] || args[1]));
   else if (icmp(builtin, "xor"))
      return get_bool_lit(t, args[0] ^ args[1]);
   else if (icmp(builtin, "xnor"))
      return get_bool_lit(t, !(args[0] ^ args[1]));
   else
      return t;
}

static tree_t simp_fcall_real(tree_t t, ident_t builtin, literal_t *args)
{
   const int lkind = args[0].kind;  // Assume all types checked same
   assert(lkind == L_REAL);

   if (icmp(builtin, "mul")) {
      return get_real_lit(t, args[0].r * args[1].r);
   }
   else if (icmp(builtin, "div")) {
      return get_real_lit(t, args[0].r / args[1].r);
   }
   else if (icmp(builtin, "add")) {
      return get_real_lit(t, args[0].r + args[1].r);
   }
   else if (icmp(builtin, "sub")) {
      return get_real_lit(t, args[0].r - args[1].r);
   }
   else if (icmp(builtin, "neg")) {
      return get_real_lit(t, -args[0].r);
   }
   else if (icmp(builtin, "identity")) {
      return get_real_lit(t, args[0].r);
   }
   else if (icmp(builtin, "eq")) {
      return get_bool_lit(t, args[0].r == args[1].r);
   }
   else if (icmp(builtin, "neq")) {
      return get_bool_lit(t, args[0].r != args[1].r);
   }
   else if (icmp(builtin, "gt")) {
      return get_bool_lit(t, args[0].r > args[1].r);
   }
   else if (icmp(builtin, "lt")) {
      return get_bool_lit(t, args[0].r < args[1].r);
   }
   else
      return t;
}

static tree_t simp_fcall_int(tree_t t, ident_t builtin, literal_t *args)
{
   const int lkind = args[0].kind;  // Assume all types checked same
   assert(lkind == L_INT);

   if (icmp(builtin, "mul")) {
      return get_int_lit(t, args[0].i * args[1].i);
   }
   else if (icmp(builtin, "div")) {
      return get_int_lit(t, args[0].i / args[1].i);
   }
   else if (icmp(builtin, "add")) {
      return get_int_lit(t, args[0].i + args[1].i);
   }
   else if (icmp(builtin, "sub")) {
      return get_int_lit(t, args[0].i - args[1].i);
   }
   else if (icmp(builtin, "neg")) {
      return get_int_lit(t, -args[0].i);
   }
   else if (icmp(builtin, "identity")) {
      return get_int_lit(t, args[0].i);
   }
   else if (icmp(builtin, "eq")) {
      return get_bool_lit(t, args[0].i == args[1].i);
   }
   else if (icmp(builtin, "neq")) {
      return get_bool_lit(t, args[0].i != args[1].i);
   }
   else if (icmp(builtin, "gt")) {
      return get_bool_lit(t, args[0].i > args[1].i);
   }
   else if (icmp(builtin, "lt")) {
      return get_bool_lit(t, args[0].i < args[1].i);
   }
   else if (icmp(builtin, "leq"))
      return get_bool_lit(t, args[0].i <= args[1].i);
   else if (icmp(builtin, "geq"))
      return get_bool_lit(t, args[0].i >= args[1].i);
   else
      return t;
}

static tree_t simp_fcall_agg(tree_t t, ident_t builtin)
{
   bool agg_low  = icmp(builtin, "agg_low");
   bool agg_high = icmp(builtin, "agg_high");

   if (agg_low || agg_high) {
      int64_t low = INT64_MAX, high = INT64_MIN;
      param_t p = tree_param(t, 0);
      for (unsigned i = 0; i < tree_assocs(p.value); i++) {
         assoc_t a = tree_assoc(p.value, i);
         switch (a.kind) {
         case A_NAMED:
            {
               int64_t tmp = assume_int(a.name);
               if (tmp < low) low = tmp;
               if (tmp > high) high = tmp;
            }
            break;

         case A_RANGE:
            {
               int64_t low_r, high_r;
               range_bounds(a.range, &low_r, &high_r);
               if (low_r < low) low = low_r;
               if (high_r > high) high = high_r;
            }
            break;

         default:
            assert(false);
         }
      }

      return get_int_lit(t, agg_low ? low : high);
   }
   else
      return t;
}

static void eval_func_body(tree_t t, vtable_t *v)
{
   printf("eval_func_body %s\n", istr(tree_ident(t)));

   const int ndecls = tree_decls(t);
   for (int i = 0; i < ndecls; i++) {
      tree_t decl = tree_decl(t, i);
      if (tree_kind(decl) == T_VAR_DECL)
         vtable_bind(v, tree_ident(decl), eval_expr(tree_value(decl), v));
   }

   const int nstmts = tree_stmts(t);
   for (int i = 0; i < nstmts; i++) {
      eval_stmt(tree_stmt(t, i), v);
      if (vtable_get(v, result_i))
         return;
   }
}

static tree_t eval_fcall(tree_t t, vtable_t *v)
{
   tree_t decl = tree_ref(t);
   assert(tree_kind(decl) == T_FUNC_DECL
          || tree_kind(decl) == T_FUNC_BODY);

   ident_t builtin = tree_attr_str(decl, builtin_i);
   if (builtin == NULL) {
      if (tree_kind(decl) != T_FUNC_BODY)
         return t;

      vtable_push(v);

      const int nports = tree_ports(decl);
      for (int i = 0; i < nports; i++) {
         tree_t port  = tree_port(decl, i);
         tree_t value = tree_param(t, i).value;

         tree_kind_t value_kind = tree_kind(value);
         if (value_kind != T_LITERAL) {
            vtable_pop(v);
            return t;    // Cannot fold this
         }
         else
            vtable_bind(v, tree_ident(port), value);
      }

      eval_func_body(decl, v);
      tree_t result = vtable_get(v, result_i);
      vtable_pop(v);

      return ((result != NULL) && folded(result)) ? result : t;
   }

   if (tree_params(t) > MAX_BUILTIN_ARGS)
      return t;

   bool can_fold_int  = true;
   bool can_fold_log  = true;
   bool can_fold_agg  = true;
   bool can_fold_real = true;
   literal_t largs[MAX_BUILTIN_ARGS];
   bool bargs[MAX_BUILTIN_ARGS];
   for (unsigned i = 0; i < tree_params(t); i++) {
      param_t p = tree_param(t, i);
      assert(p.kind == P_POS);
      tree_t val = eval_expr(p.value, v);
      can_fold_int  = can_fold_int && folded_int(val, &largs[i]);
      can_fold_log  = can_fold_log && folded_bool(val, &bargs[i]);
      can_fold_agg  = can_fold_agg && folded_agg(val);
      can_fold_real = can_fold_real && folded_real(val, &largs[i]);
   }

   printf("can_fold_int=%d\n", can_fold_int);

   if (can_fold_int)
      return simp_fcall_int(t, builtin, largs);
   else if (can_fold_log)
      return simp_fcall_log(t, builtin, bargs);
   else if (can_fold_agg)
      return simp_fcall_agg(t, builtin);
   else if (can_fold_real)
      return simp_fcall_real(t, builtin, largs);
   else
      return t;
}

static tree_t eval_ref(tree_t t, vtable_t *v)
{
   tree_t binding = vtable_get(v, tree_ident(tree_ref(t)));
   if (binding == NULL)
      fatal_at(tree_loc(t), "cannot constant fold reference");
   return binding;
}

static tree_t eval_aggregate(tree_t t, vtable_t *v)
{
   if (folded_agg(t))
      return t;
   else
      fatal_at(tree_loc(t), "aggregate is not constant");
}

static tree_t eval_expr(tree_t t, vtable_t *v)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      return eval_fcall(t, v);
   case T_REF:
      return eval_ref(t, v);
   case T_AGGREGATE:
      return eval_aggregate(t, v);
   case T_LITERAL:
      return t;
   default:
      fatal_at(tree_loc(t), "cannot evaluate expression %s",
               tree_kind_str(tree_kind(t)));
   }
}

static void eval_return(tree_t t, vtable_t *v)
{
   printf("eval_return\n");
   if (tree_has_value(t))
      vtable_bind(v, result_i, eval_expr(tree_value(t), v));
}

static void eval_if(tree_t t, vtable_t *v)
{
   tree_t cond = eval_expr(tree_value(t), v);
   bool cond_b;
   if (!folded_bool(cond, &cond_b))
      fatal_at(tree_loc(cond), "cannot constant fold expression");

   printf("eval_if cond=%d\n", cond_b);

   if (cond_b) {
      const int nstmts = tree_stmts(t);
      for (int i = 0; i < nstmts; i++)
         eval_stmt(tree_stmt(t, i), v);
   }
   else {
      const int nstmts = tree_else_stmts(t);
      for (int i = 0; i < nstmts; i++)
         eval_stmt(tree_else_stmt(t, i), v);
   }
}

static void eval_while(tree_t t, vtable_t *v)
{
   tree_t value = tree_value(t);
   for (;;) {
      tree_t cond = eval_expr(value, v);
      bool cond_b;
      if (!folded_bool(cond, &cond_b))
         fatal_at(tree_loc(value), "cannot constant fold expression");

      printf("eval_while cond=%d\n", cond_b);

      if (!cond_b)
         break;

      const int nstmts = tree_stmts(t);
      for (int i = 0; i < nstmts; i++)
         eval_stmt(tree_stmt(t, i), v);
   }
}

static void eval_var_assign(tree_t t, vtable_t *v)
{
   tree_t target = tree_target(t);
   if (tree_kind(target) != T_REF)
      fatal_at(tree_loc(target), "cannot evaluate this target");

   tree_t value = tree_value(t);
   tree_t updated = eval_expr(value, v);
   if (!folded(updated))
      fatal_at(tree_loc(value), "cannot constant fold expression");

   vtable_bind(v, tree_ident(tree_ref(target)), updated);
}

static void eval_stmt(tree_t t, vtable_t *v)
{
   switch (tree_kind(t)) {
   case T_RETURN:
      eval_return(t, v);
      break;
   case T_WHILE:
      eval_while(t, v);
      break;
   case T_IF:
      eval_if(t, v);
      break;
   case T_VAR_ASSIGN:
      eval_var_assign(t, v);
      break;
   default:
      fatal_at(tree_loc(t), "cannot evaluate statement %s",
               tree_kind_str(tree_kind(t)));
   }
}

static void eval_intern_strings(void)
{
   // Intern some commonly used strings

   std_bool_i = ident_new("STD.STANDARD.BOOLEAN");
   builtin_i  = ident_new("builtin");
}

tree_t eval(tree_t fcall)
{
   assert(tree_kind(fcall) == T_FCALL);

   static bool have_interned = false;
   if (!have_interned) {
      eval_intern_strings();
      have_interned = true;
   }

   vtable_t vt = {
      .top = NULL
   };
   return eval_fcall(fcall, &vt);
}
