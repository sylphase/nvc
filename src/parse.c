//
//  Copyright (C) 2011-2014  Nick Gasson
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

#include "util.h"
#include "phase.h"
#include "token.h"
#include "common.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

static const char *perm_linebuf = NULL;
static const char *perm_file_name = NULL;
static int         n_token_next_start = 0;
static int         n_row = 0;
static bool        last_was_newline = true;
static loc_t       yylloc;
static loc_t       start_loc;
static const char *read_ptr;
static const char *file_start;
static size_t      file_sz;
static int         n_errors = 0;
static token_t     peek_tok;
static bool        peek_valid = false;
static token_t     peek2_tok;
static bool        peek2_valid = false;
static const char *hint_str = NULL;
static int         n_correct = 0;
static tree_t      assert_viol = NULL;

yylval_t yylval;

int yylex(void);

#define F(list) list, ARRAY_LEN(list)
#define scan(...) _scan(1, __VA_ARGS__, -1)
#define expect(...) _expect(1, __VA_ARGS__, -1)
#define one_of(...) _one_of(1, __VA_ARGS__, -1)

#define RECOVER_THRESH 5
#define TRACE_PARSE    0

#if TRACE_PARSE
static int depth = 0;
#endif

typedef void (*add_func_t)(tree_t, tree_t);

typedef struct {
   const char *old_hint;
   loc_t       old_start_loc;
} state_t;

#define EXTEND(s)                                                      \
   __attribute__((cleanup(_pop_state))) const state_t _state =         \
      { hint_str, start_loc };                                         \
   hint_str = s;                                                       \
   _push_state(&_state);

#define BEGIN(s)                                                       \
   EXTEND(s);                                                          \
   start_loc = LOC_INVALID;

#define CURRENT_LOC _diff_loc(&start_loc, &yylloc)

static tree_t p_expression(void);
static tree_t p_sequential_statement(void);

static void _pop_state(const state_t *s)
{
#if TRACE_PARSE
   depth--;
   for (int i = 0; i < depth; i++)
      printf(" ");
   printf("<-- %s\n", hint_str);
#endif
   hint_str  = s->old_hint;
   start_loc = s->old_start_loc;
}

static void _push_state(const state_t *s)
{
#if TRACE_PARSE
   for (int i = 0; i < depth; i++)
      printf(" ");
   printf("--> %s\n", hint_str);
   depth++;
#endif
}

static const char *token_str(token_t tok)
{
   static const char *token_strs[] = {
      "end of file", "identifier", "entity", "is", "end", "generic", "port",
      "constant", "component", "configuration", "architecture", "of", "begin",
      "for", "type", "to", "all", "in", "out", "buffer", "bus", "unaffected",
      "signal", "downto", "process", "postponed", "wait", "report", "(", ")",
      ";", ":=", ":", ",", "integer", "string", "error", "inout", "linkage",
      "variable", "if", "range", "subtype", "units", "package", "library",
      "use", ".", "null", "'", "function", "impure", "return", "pure", "array",
      "<>", "=>", "others", "assert", "severity", "on", "map", "then", "else",
      "elsif", "body", "while", "loop", "after", "alias", "attribute",
      "procedure", "exit", "next", "when", "case", "label", "group", "literal",
      "|", "[", "]", "inertial", "transport", "reject", "bit string", "block",
      "with", "select", "generate", "access", "file", "open", "real", "until",
      "record", "new", "shared", "and", "or", "nand", "nor", "xor", "xnor",
      "=", "/=", "<", "<=", ">", ">=", "+", "-", "&", "**", "/", "sll", "srl"
   };

   if ((size_t)tok >= ARRAY_LEN(token_strs))
      return "???";
   else
      return token_strs[tok];
}

static token_t peek(void)
{
   if (peek_valid)
      return peek_tok;
   else {
      assert(!peek2_valid);
      peek_valid = true;
      return (peek_tok = yylex());
   }
}

static token_t peek2(void)
{
   if (!peek_valid) {
      assert(!peek2_valid);
      (void)peek();
   }

   if (peek2_valid)
      return peek2_tok;
   else {
      peek2_valid = true;
      return (peek2_tok = yylex());
   }
}

static bool consume(token_t tok)
{
   const token_t got = peek();
   if (tok != got) {
      if (n_correct >= RECOVER_THRESH) {
         error_at(&yylloc, "expected $yellow$%s$$ but found $yellow$%s$$ while "
                  "parsing $yellow$%s$$",
                  token_str(tok), token_str(got), hint_str);
         n_errors++;
      }
      n_correct = 0;
   }
   else
      n_correct++;

   if (start_loc.linebuf == NULL)
      start_loc = yylloc;

   if (peek2_valid) {
      assert(peek_valid);
      peek_tok = peek2_tok;
   }
   else
      peek_valid = false;

   peek2_valid = false;

   return (tok == got);
}

static bool optional(token_t tok)
{
   if (peek() == tok) {
      consume(tok);
      return true;
   }
   else
      return false;
}

static void _vexpect(va_list ap)
{
   LOCAL_TEXT_BUF tb = tb_new();

   tb_printf(tb, "unexpected $yellow$%s$$ while parsing $yellow$%s$$, "
             "expecting one of ", token_str(peek()), hint_str);

   bool first = true;
   for (;;) {
      const int tok = va_arg(ap, int);
      if (tok == -1)
         break;

      if (!first)
         tb_printf(tb, ", ");

      tb_printf(tb, "$yellow$%s$$", token_str(tok));

      first = false;
   }

   if (n_correct >= RECOVER_THRESH) {
      error_at(&yylloc, tb_get(tb));
      n_errors++;
   }

   n_correct = 0;
}

static void _expect(int dummy, ...)
{
   va_list ap;
   va_start(ap, dummy);
   _vexpect(ap);
   va_end(ap);
}

static bool _scan(int dummy, ...)
{
   va_list ap;
   va_start(ap, dummy);

   token_t p = peek();
   bool found = false;

   while (!found) {
      const token_t tok = va_arg(ap, token_t);
      if (tok == -1)
         break;
      else if (p == tok)
         found = true;
   }

   va_end(ap);
   return found;
}

static int _one_of(int dummy, ...)
{
   va_list ap;
   va_start(ap, dummy);

   token_t p = peek();
   bool found = false;

   while (!found) {
      const token_t tok = va_arg(ap, token_t);
      if (p == tok)
         found = true;
   }

   va_end(ap);

   if (found) {
      consume(p);
      return p;
   }
   else {
      va_start(ap, dummy);
      _vexpect(ap);
      va_end(ap);

      return -1;
   }
}

static const loc_t *_diff_loc(const loc_t *start, const loc_t *end)
{
   static loc_t result;

   result.first_line   = start->first_line;
   result.first_column = start->first_column;
   result.last_line    = end->last_line;
   result.last_column  = end->last_column;
   result.file         = start->file;
   result.linebuf      = start->linebuf;

   return &result;
}

static tree_t str_to_agg(const char *start, const char *end, const loc_t *loc)
{
   tree_t t = tree_new(T_AGGREGATE);
   tree_set_loc(t, loc);

   for (const char *p = start; *p != '\0' && p != end; p++) {
      if (*p == -127)
         continue;

      const char ch[] = { '\'', *p, '\'', '\0' };

      tree_t ref = tree_new(T_REF);
      tree_set_ident(ref, ident_new(ch));
      tree_set_loc(ref, loc);

      tree_t a = tree_new(T_ASSOC);
      tree_set_subkind(a, A_POS);
      tree_set_value(a, ref);

      tree_add_assoc(t, a);
   }

   return t;
}

static ident_t loc_to_ident(const loc_t *loc)
{
   int bufsz = 128;
   char *buf = xmalloc(bufsz);
   checked_sprintf(buf, bufsz, "line_%d", loc->first_line);
   const int nprint = strlen(buf);

   for (int i = 0; ident_interned(buf); i++) {
      if (nprint + 1 > bufsz) {
         bufsz *= 2;
         buf = xrealloc(buf, bufsz);
      }
      buf[nprint] = 'a' + i;
      buf[nprint + 1] = '\0';
   }

   ident_t ident = ident_new(buf);
   free(buf);
   return ident;
}

static void set_label_and_loc(tree_t t, ident_t label, const loc_t *loc)
{
   tree_set_loc(t, loc);

   if (label == NULL)
      label = loc_to_ident(loc);
   tree_set_ident(t, label);
}

static ident_t p_identifier(void)
{
   // basic_identifier | extended_identifier

   if (consume(tID)) {
      ident_t i = ident_new(yylval.s);
      free(yylval.s);
      return i;
   }
   else
      return ident_new("error");
}

static ident_list_t *p_identifier_list(void)
{
   // identifier { , identifier }

   ident_list_t *result = NULL;

   ident_list_add(&result, p_identifier());

   while (optional(tCOMMA))
      ident_list_push(&result, p_identifier());

   return result;
}

static void p_library_clause(void)
{

}

static void p_use_clause(void)
{

}

static void p_context_item(void)
{
   // library_clause | use_clause

   BEGIN("context item");

   switch (peek()) {
   case tLIBRARY:
      p_library_clause();
      break;

   case tUSE:
      p_use_clause();
      break;

   default:
      expect(tLIBRARY, tUSE);
   }
}

static void p_context_clause(void)
{
   // { context_item }

   BEGIN("context clause");

   while (scan(tLIBRARY, tUSE)) {
      p_context_item();
   }
}

static port_mode_t p_mode(void)
{
   // in | out | inout | buffer | linkage

   switch (one_of(tIN, tOUT, tINOUT, tBUFFER, tLINKAGE)) {
   case tIN:
      return PORT_IN;
   case tOUT:
      return PORT_OUT;
   case tINOUT:
      return PORT_INOUT;
   case tBUFFER:
      return PORT_BUFFER;
   case tLINKAGE:
      return PORT_LINKAGE;
   default:
      return PORT_INVALID;
   }
}

static tree_t p_simple_name(void)
{
   // identifier

   tree_t t = tree_new(T_REF);
   tree_set_ident(t, p_identifier());
   tree_set_loc(t, CURRENT_LOC);

   return t;
}

static ident_t p_operator_symbol(void)
{
   // string_literal

   consume(tSTRING);

   for (char *p = yylval.s; *p != '\0'; p++)
      *p = tolower((int)*p);
   ident_t id = ident_new(yylval.s);
   free(yylval.s);

   return id;
}

static tree_t p_slice_name(ident_t prefix, tree_t expr1)
{
   // prefix ( discrete_range )

   EXTEND("slice name");

   tree_t t = tree_new(T_ARRAY_SLICE);

   range_t r = { .left = expr1 };

   switch (one_of(tTO, tDOWNTO)) {
   case tTO:
      r.kind = RANGE_TO;
      break;

   case tDOWNTO:
      r.kind = RANGE_DOWNTO;
      break;
   }

   r.right = p_expression();

   consume(tRPAREN);

   tree_set_range(t, r);
   tree_set_loc(t, CURRENT_LOC);
   return t;
}

static tree_t p_function_call(ident_t prefix, tree_t expr1)
{
   // name [ ( actual_parameter_part ) ]

   EXTEND("function call");

   tree_t t = tree_new(T_FCALL);

   add_param(t, expr1, P_POS, NULL);

   while (optional(tCOMMA)) {
      add_param(t, p_expression(), P_POS, NULL);
   }

   consume(tRPAREN);

   tree_set_loc(t, CURRENT_LOC);
   return t;
}

static tree_t p_name(void)
{
   // simple_name | operator_symbol | selected_name | indexed_name
   //   | slice_name | attribute_name

   BEGIN("name");

   switch (peek()) {
   case tSTRING:
      {
         ident_t op = p_operator_symbol();

         if (optional(tLPAREN))
            return p_function_call(op, p_expression());
         else {
            tree_t t = tree_new(T_REF);
            tree_set_ident(t, op);
            tree_set_loc(t, CURRENT_LOC);
            return t;
         }
      }

   case tID:
      break;

   default:
      expect(tSTRING, tID);
      return tree_new(T_OPEN);
   }

   switch (peek2()) {
   case tLPAREN:
      break;

   default:
      return p_simple_name();
   }

   ident_t prefix = p_identifier();

   consume(tLPAREN);

   tree_t expr1 = p_expression();

   switch (peek()) {
   case tRPAREN:
   case tCOMMA:
      return p_function_call(prefix, expr1);

   case tTO:
   case tDOWNTO:
      return p_slice_name(prefix, expr1);

   default:
      expect(tRPAREN, tCOMMA, tTO, tDOWNTO);
      return tree_new(T_OPEN);
   }

   consume(tRPAREN);
}

static type_t p_type_mark(void)
{
   // name

   type_t t = type_new(T_UNRESOLVED);
   type_set_ident(t, p_identifier());
   return t;
}

static type_t p_subtype_indication(void)
{
   // [ name ] type_mark [ constraint ]

   BEGIN("subtype indication");

   type_t type = NULL;
   if ((peek() == tID) && (peek2() == tID)) {
      type = type_new(T_SUBTYPE);

      tree_t rname = p_name();
      // XXX: check name is resolution_function_name
      type_set_resolution(type, rname);

      type_t base = p_type_mark();
      type_set_base(type, base);
   }
   else
      type = p_type_mark();

   // p_constraint()

   return type;
}

static tree_t p_abstract_literal(void)
{
   // decimal_literal | based_literal

   BEGIN("abstract literal");

   tree_t t = tree_new(T_LITERAL);

   switch (one_of(tINT, tREAL)) {
   case tINT:
      tree_set_subkind(t, L_INT);
      tree_set_ival(t, yylval.n);
      break;

   case tREAL:
      tree_set_subkind(t, L_REAL);
      tree_set_dval(t, yylval.d);
      break;
   }

   tree_set_loc(t, CURRENT_LOC);
   return t;
}

static tree_t p_physical_literal(tree_t mult)
{
   // [ abstract_literal ] name

   EXTEND("physical literal");

   tree_t unit = tree_new(T_REF);
   tree_set_ident(unit, p_identifier());
   tree_set_loc(unit, CURRENT_LOC);

   tree_t t = tree_new(T_FCALL);
   tree_set_loc(t, CURRENT_LOC);
   tree_set_ident(t, ident_new("\"*\""));

   add_param(t, mult, P_POS, NULL);
   add_param(t, unit, P_POS, NULL);

   return t;
}

static tree_t p_numeric_literal(void)
{
   // abstract_literal | physical_literal

   BEGIN("numeric literal");

   tree_t abs = NULL;
   if (scan(tINT, tREAL))
      abs = p_abstract_literal();

   if (peek() == tID)
      return p_physical_literal(abs);
   else
      return abs;
}

static tree_t p_literal(void)
{
   // numeric_literal | enumeration_literal | string_literal
   // | bit_string_literal | null

   BEGIN("literal");

   switch (peek()) {
   case tNULL:
      {
         consume(tNULL);

         tree_t t = tree_new(T_LITERAL);
         tree_set_loc(t, CURRENT_LOC);
         tree_set_subkind(t, L_NULL);
         return t;
      }

   case tINT:
   case tREAL:
      return p_numeric_literal();

   case tSTRING:
      {
         consume(tSTRING);

         char *p = yylval.s;
         size_t len = strlen(p);
         tree_t t = str_to_agg(p + 1, p + len - 1, CURRENT_LOC);
         free(p);

         return t;
      }

   default:
      expect(tNULL, tINT, tREAL);
      return tree_new(T_OPEN);
   }
}

static tree_t p_primary(void)
{
   // name | literal | aggregate | function_call | qualified_expression
   //   | type_conversion | allocator | ( expression )

   BEGIN("primary");

   switch (peek()) {
   case tLPAREN:
      {
         consume(tLPAREN);
         tree_t sub = p_expression();
         consume(tRPAREN);
         return sub;
      }

   case tINT:
   case tREAL:
   case tNULL:
      return p_literal();

   case tSTRING:
      return (peek2() == tLPAREN) ? p_name() : p_literal();

   case tID:
      return p_name();

   default:
      expect(tLPAREN, tLITERAL, tINT, tREAL, tNULL, tID, tSTRING);
      return tree_new(T_OPEN);
   }
}

static tree_t p_factor(void)
{
   // primary [ ** primary ] | abs primary | not primary

   BEGIN("factor");

   tree_t operand = p_primary();

   // XXX
   return operand;
}

static ident_t p_multiplying_operator(void)
{
   switch (one_of(tTIMES, tOVER, tMOD, tREM)) {
   case tTIMES:
      return ident_new("\"*\"");
   case tOVER:
      return ident_new("\"/\"");
   case tMOD:
      return ident_new("\"mod\"");
   case tREM:
      return ident_new("\"rem\"");
   default:
      return ident_new("error");
   }
}

static tree_t p_term(void)
{
   // factor { multiplying_operator factor }

   BEGIN("term");

   tree_t term = p_factor();

   while (scan(tTIMES, tOVER, tMOD, tREM)) {
      ident_t op   = p_multiplying_operator();
      tree_t left  = term;
      tree_t right = p_factor();

      term = tree_new(T_FCALL);
      tree_set_ident(term, op);
      tree_set_loc(term, CURRENT_LOC);

      add_param(term, left, P_POS, NULL);
      add_param(term, right, P_POS, NULL);
   }

   return term;
}

static ident_t p_adding_operator(void)
{
   switch (one_of(tPLUS, tMINUS, tAMP)) {
   case tPLUS:
      return ident_new("\"+\"");
   case tMINUS:
      return ident_new("\"-\"");
   case tAMP:
      return ident_new("\"&\"");
   default:
      return ident_new("error");
   }
}

static tree_t p_simple_expression(void)
{
   // [ sign ] term { adding_operator term }

   BEGIN("simple expression");

   // p_sign()

   tree_t expr = p_term();

   while (scan(tPLUS, tMINUS, tAMP)) {
      ident_t op   = p_adding_operator();
      tree_t left  = expr;
      tree_t right = p_term();

      expr = tree_new(T_FCALL);
      tree_set_ident(expr, op);
      tree_set_loc(expr, CURRENT_LOC);

      add_param(expr, left, P_POS, NULL);
      add_param(expr, right, P_POS, NULL);
   }

   return expr;
}

static tree_t p_shift_expression(void)
{
   // simple_expression [ shift_operator simple_expression ]

   BEGIN("shift expression");

   tree_t left = p_simple_expression();

   // XXX
   return left;
}

static ident_t p_relational_operator(void)
{
   switch (one_of(tEQ, tNEQ, tLT, tLE, tGT, tGE)) {
   case tEQ:
      return ident_new("\"=\"");
   case tNEQ:
      return ident_new("\"/=\"");
   case tLT:
      return ident_new("\"<\"");
   case tLE:
      return ident_new("\"<=\"");
   case tGT:
      return ident_new("\">\"");
   case tGE:
      return ident_new("\">=\"");
   default:
      return ident_new("error");
   }
}

static tree_t p_relation(void)
{
   // shift_expression [ relational_operator shift_expression ]

   BEGIN("relation");

   tree_t rel = p_shift_expression();

   while (scan(tEQ, tNEQ, tLT, tLE, tGT, tGE)) {
      ident_t op   = p_relational_operator();
      tree_t left  = rel;
      tree_t right = p_shift_expression();

      rel = tree_new(T_FCALL);
      tree_set_ident(rel, op);
      tree_set_loc(rel, CURRENT_LOC);

      add_param(rel, left, P_POS, NULL);
      add_param(rel, right, P_POS, NULL);
   }

   return rel;
}

static tree_t p_expression(void)
{
   // relation { and relation } | relation { or relation }
   //   | relation { xor relation } | relation [ nand relation ]
   //   | relation [ nor relation ] | relation { xnor relation }

   BEGIN("expression");

   tree_t left = p_relation();

   // XXX
   return left;
}

static void p_interface_constant_declaration(tree_t parent, add_func_t addf)
{
   // [ constant ] identifier_list : [ in ] subtype_indication [ := expression ]

   BEGIN("interface constant declaration");

   optional(tCONSTANT);

   LOCAL_IDENT_LIST ids = p_identifier_list();

   consume(tCOLON);
   optional(tIN);

   type_t type = p_subtype_indication();

   tree_t init = NULL;
   if (optional(tASSIGN))
      init = p_expression();

   const loc_t *loc = CURRENT_LOC;

   for (ident_list_t *it = ids; it != NULL; it = it->next) {
      tree_t d = tree_new(T_PORT_DECL);
      tree_set_ident(d, it->ident);
      tree_set_loc(d, loc);
      tree_set_subkind(d, PORT_IN);
      tree_set_type(d, type);

      if (init != NULL)
         tree_set_value(d, init);

      (*addf)(parent, d);
   }
}

static void p_interface_signal_declaration(tree_t parent, add_func_t addf)
{
   // [signal] identifier_list : [ mode ] subtype_indication [ bus ]
   //    [ := expression ]

   BEGIN("interface signal declaration");

   optional(tSIGNAL);

   LOCAL_IDENT_LIST ids = p_identifier_list();

   consume(tCOLON);

   port_mode_t mode = PORT_IN;
   if (scan(tIN, tOUT, tINOUT, tBUFFER, tLINKAGE))
      mode = p_mode();

   type_t type = p_subtype_indication();

   optional(tBUS);

   tree_t init = NULL;
   if (optional(tASSIGN))
      init = p_expression();

   const loc_t *loc = CURRENT_LOC;

   for (ident_list_t *it = ids; it != NULL; it = it->next) {
      tree_t d = tree_new(T_PORT_DECL);
      tree_set_ident(d, it->ident);
      tree_set_loc(d, loc);
      tree_set_subkind(d, mode);
      tree_set_type(d, type);
      tree_set_class(d, C_SIGNAL);

      if (init != NULL)
         tree_set_value(d, init);

      tree_add_port(parent, d);
   }
}

static void p_interface_variable_declaration(tree_t parent)
{
   // [variable] identifier_list : [ mode ] subtype_indication [ := expression ]

   BEGIN("interface variable declaration");
}

static void p_interface_file_declaration(tree_t parent)
{
   // file identifier_list : subtype_indication

   BEGIN("interface file declaration");
}

static void p_interface_declaration(class_t def_class, tree_t parent,
                                    add_func_t addf)
{
   // interface_constant_declaration | interface_signal_declaration
   //   | interface_variable_declaration | interface_file_declaration

   BEGIN("interface declaration");

   const token_t p = peek();
   switch (p) {
   case tCONSTANT:
      p_interface_constant_declaration(parent, addf);
      break;

   case tSIGNAL:
      p_interface_signal_declaration(parent, addf);
      break;

   case tVARIABLE:
      p_interface_variable_declaration(parent);
      break;

   case tFILE:
      p_interface_file_declaration(parent);
      break;

   case tID:
      {
         switch (def_class) {
         case C_CONSTANT:
            p_interface_constant_declaration(parent, addf);
            break;

         case C_SIGNAL:
            p_interface_signal_declaration(parent, addf);
            break;

         case C_VARIABLE:
            p_interface_variable_declaration(parent);
            break;

         default:
            assert(false);
         }
      }
      break;

   default:
      expect(tCONSTANT, tSIGNAL, tVARIABLE, tFILE, tID);
   }
}

static void p_interface_element(class_t def_class, tree_t parent,
                                add_func_t addf)
{
   // interface_declaration

   BEGIN("interface element");

   p_interface_declaration(def_class, parent, addf);
}

static void p_interface_list(class_t def_class, tree_t parent, add_func_t addf)
{
   // interface_element { ; interface_element }

   BEGIN("interface list");

   p_interface_element(def_class, parent, addf);

   while (optional(tSEMI))
      p_interface_element(def_class, parent, addf);
}

static void p_port_list(tree_t parent)
{
   // port_list ::= interface_list

   BEGIN("port list");

   p_interface_list(C_SIGNAL, parent, tree_add_port);
}

static void p_port_clause(tree_t parent)
{
   // port ( port_list ) ;

   BEGIN("port clause");

   consume(tPORT);
   consume(tLPAREN);

   p_port_list(parent);

   consume(tRPAREN);
   consume(tSEMI);
}

static void p_generic_list(tree_t parent)
{
   // generic_list ::= interface_list

   BEGIN("generic list");

   p_interface_list(C_CONSTANT, parent, tree_add_generic);
}

static void p_generic_clause(tree_t parent)
{
   // generic ( generic_list ) ;

   BEGIN("generic clause");

   consume(tGENERIC);
   consume(tLPAREN);

   p_generic_list(parent);

   consume(tRPAREN);
   consume(tSEMI);
}

static void p_entity_header(tree_t entity)
{
   // [ generic_clause ] [ port_clause ]

   BEGIN("entity header");

   if (scan(tGENERIC))
      p_generic_clause(entity);

   if (scan(tPORT))
      p_port_clause(entity);
}

static void p_attribute_declaration(ident_t head, tree_t parent)
{
   // attribute identifier : type_mark ;

   BEGIN("attribute declaration");

   tree_t t = tree_new(T_ATTR_DECL);
   tree_set_ident(t, head);
   tree_set_type(t, p_type_mark());

   consume(tSEMI);

   tree_set_loc(t, CURRENT_LOC);

   tree_add_decl(parent, t);
}

static class_t p_entity_class(void)
{
   // entity | procedure | type | signal | label | group | architecture
   //   | function | subtype | variable | literal | file | configuration
   //   | package | constant | component | units

   BEGIN("entity class");

   switch (one_of(tENTITY, tPROCEDURE, tSIGNAL, tLABEL)) {
   case tENTITY:
      return C_ENTITY;
   case tPROCEDURE:
      return C_PROCEDURE;
   case tSIGNAL:
      return C_SIGNAL;
   case tLABEL:
      return C_LABEL;
   default:
      return C_DEFAULT;
   }
}

static ident_list_t *p_entity_specification(class_t *class)
{
   // entity_name_list : entity_class

   BEGIN("entity specification");

   ident_list_t *ids = p_identifier_list();

   consume(tCOLON);

   *class = p_entity_class();
   return ids;
}

static void p_attribute_specification(ident_t head, tree_t parent)
{
   // attribute attribute_designator of entity_specification is expression ;

   BEGIN("attribute specification");

   class_t class;
   LOCAL_IDENT_LIST ids = p_entity_specification(&class);

   consume(tIS);

   tree_t value = p_expression();

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;

   for (ident_list_t *it = ids; it != NULL; it = it->next) {
      tree_t t = tree_new(T_ATTR_SPEC);
      tree_set_loc(t, loc);
      tree_set_class(t, class);
      tree_set_ident(t, head);
      tree_set_value(t, value);

      tree_add_decl(parent, t);
   }
}

static void p_entity_declarative_item(tree_t entity)
{
   // subprogram_declaration | subprogram_body | type_declaration
   //   | subtype_declaration | constant_declaration | signal_declaration
   //   | shared_variable_declaration | file_declaration | alias_declaration
   //   | attribute_declaration | attribute_specification
   //   | disconnection_specification | use_clause | group_template_declaration
   //   | group_declaration

   BEGIN("entity declarative item");

   switch (peek()) {
   case tATTRIBUTE:
      {
         consume(tATTRIBUTE);
         ident_t head = p_identifier();
         switch (one_of(tCOLON, tOF)) {
         case tCOLON:
            p_attribute_declaration(head, entity);
            break;

         case tOF:
            p_attribute_specification(head, entity);
            break;

         default:
            break;
         }
      }
      break;

   default:
      expect(tATTRIBUTE);
   }
}

static void p_entity_declarative_part(tree_t entity)
{
   // { entity_declarative_item }

   BEGIN("entity declarative part");

   while (scan(tATTRIBUTE))
      p_entity_declarative_item(entity);
}

static tree_t p_assertion(void)
{
   // assert condition [ report expression ] [ severity expression ]

   BEGIN("assertion");

   tree_t s = tree_new(T_ASSERT);

   consume(tASSERT);

   tree_set_value(s, p_expression());

   if (optional(tREPORT))
      tree_set_message(s, p_expression());
   else {
      if (assert_viol == NULL)
         assert_viol = str_to_agg("Assertion violation.", NULL, &LOC_INVALID);

      tree_set_message(s, assert_viol);
   }

   if (optional(tSEVERITY))
      tree_set_severity(s, p_expression());
   else {
      tree_t sev = tree_new(T_REF);
      tree_set_ident(sev, ident_new("ERROR"));

      tree_set_severity(s, sev);
   }

   tree_set_loc(s, CURRENT_LOC);
   return s;
}

static tree_t p_concurrent_assertion_statement(void)
{
   // [ label : ] [ postponed ] assertion ;

   BEGIN("concurrent assertion statement");

   const bool postponed = optional(tPOSTPONED);

   tree_t s = p_assertion();
   tree_change_kind(s, T_CASSERT);

   consume(tSEMI);

   tree_set_loc(s, CURRENT_LOC);

   if (postponed)
      tree_add_attr_int(s, ident_new("postponed"), 1);

   return s;
}

static tree_t p_entity_statement(void)
{
   // concurrent_assertion_statement | concurrent_procedure_call_statement
   //   | process_statement

   BEGIN("entity statement");

   switch (peek()) {
   case tASSERT:
      return p_concurrent_assertion_statement();

   case tPOSTPONED:
      {
         consume(tPOSTPONED);

         switch (one_of(tPROCESS, tASSERT)) {
         case tPROCESS:
            // p_process_statement()
            return tree_new(T_NULL);
         case tASSERT:
            return p_concurrent_assertion_statement();
         default:
            return tree_new(T_NULL);
         }
      }

   default:
      expect(tASSERT, tPOSTPONED);
      return tree_new(T_NULL);
   }
}

static void p_entity_statement_part(tree_t entity)
{
   // { entity_statement }

   BEGIN("entity statement part");

   while (scan(tASSERT, tPROCESS, tPOSTPONED))
      tree_add_stmt(entity, p_entity_statement());
}

static tree_t p_entity_declaration(void)
{
   // entity identifier is entity_header entity_declarative_part
   //   [ begin entity_statement_part ] end [ entity ] [ entity_simple_name ] ;

   BEGIN("entity declaration");

   tree_t t = tree_new(T_ENTITY);

   consume(tENTITY);

   ident_t id = p_identifier();
   tree_set_ident(t, id);

   consume(tIS);

   p_entity_header(t);
   p_entity_declarative_part(t);

   if (optional(tBEGIN))
      p_entity_statement_part(t);

   consume(tEND);
   optional(tENTITY);

   if (peek() == tID) {
      ident_t tail_id = p_identifier();
      (void)tail_id;
      // XXX: test me
   }

   consume(tSEMI);

   tree_set_loc(t, CURRENT_LOC);
   return t;
}

static tree_t p_primary_unit(void)
{
   // entity_declaration | configuration_declaration | package_declaration

   BEGIN("primary unit");

   switch (peek()) {
   case tENTITY:
      return p_entity_declaration();

   default:
      expect(tENTITY);
      return NULL;
   }
}

static void p_signal_declaration(tree_t parent)
{
   // signal identifier_list : subtype_indication [ signal_kind ]
   //   [ := expression ] ;

   BEGIN("signal declaration");

   consume(tSIGNAL);

   LOCAL_IDENT_LIST ids = p_identifier_list();

   consume(tCOLON);

   type_t type = p_subtype_indication();

   // [ signal_kind ]

   tree_t init = NULL;
   if (optional(tASSIGN))
      init = p_expression();

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;

   for (ident_list_t *it = ids; it != NULL; it = it->next) {
      tree_t t = tree_new(T_SIGNAL_DECL);
      tree_set_loc(t, loc);
      tree_set_ident(t, it->ident);
      tree_set_type(t, type);

      if (init != NULL)
         tree_set_value(t, init);

      tree_add_decl(parent, t);
   }
}

static void p_block_declarative_item(tree_t parent)
{
   // subprogram_declaration | subprogram_body | type_declaration
   //   | subtype_declaration | constant_declaration | signal_declaration
   //   | shared_variable_declaration | file_declaration | alias_declaration
   //   | component_declaration | attribute_declaration
   //   | attribute_specification | configuration_specification
   //   | disconnection_specification | use_clause | group_template_declaration
   //   | group_declaration

   BEGIN("block declarative item");

   switch (peek()) {
   case tSIGNAL:
      p_signal_declaration(parent);
      break;

   default:
      expect(tSIGNAL);
   }
}

static void p_variable_declaration(tree_t parent)
{
   // [ shared ] variable identifier_list : subtype_indication
   //   [ := expression ] ;

   BEGIN("variable declaration");

   consume(tVARIABLE);

   LOCAL_IDENT_LIST ids = p_identifier_list();

   consume(tCOLON);

   type_t type = p_subtype_indication();

   tree_t init = NULL;
   if (optional(tASSIGN))
      init = p_expression();

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;

   for (ident_list_t *it = ids; it != NULL; it = it->next) {
      tree_t t = tree_new(T_VAR_DECL);
      tree_set_loc(t, loc);
      tree_set_ident(t, it->ident);
      tree_set_type(t, type);

      if (init != NULL)
         tree_set_value(t, init);

      tree_add_decl(parent, t);
   }
}

static void p_process_declarative_item(tree_t proc)
{
   // subprogram_declaration | subprogram_body | type_declaration
   //   | subtype_declaration | constant_declaration | variable_declaration
   //   | file_declaration | alias_declaration | attribute_declaration
   //   | attribute_specification | use_clause | group_template_declaration
   //   | group_declaration

   BEGIN("process declarative item");

   switch (peek()) {
   case tVARIABLE:
      p_variable_declaration(proc);
      break;

   default:
      expect(tVARIABLE);
   }
}

static void p_process_declarative_part(tree_t proc)
{
   // { process_declarative_item }

   BEGIN("process declarative part");

   while (scan(tVARIABLE))
      p_process_declarative_item(proc);
}

static tree_t p_target(tree_t name)
{
   // name | aggregate

   BEGIN("target");

   if (name == NULL)
      assert(false); // p_aggregate()
   else
      return name;
}

static tree_t p_variable_assignment_statement(ident_t label, tree_t name)
{
   // [ label : ] target := expression ;

   EXTEND("variable assignment statement");

   tree_t t = tree_new(T_VAR_ASSIGN);

   tree_set_target(t, p_target(name));

   consume(tASSIGN);

   tree_set_value(t, p_expression());

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;
   tree_set_loc(t, loc);

   if (label == NULL)
      label = loc_to_ident(loc);
   tree_set_ident(t, label);

   return t;
}

static tree_t p_waveform_element(void)
{
   // expression [ after expression ] | null [ after expression ]

   BEGIN("waveform element");

   tree_t w = tree_new(T_WAVEFORM);
   tree_set_value(w, p_expression());

   if (optional(tAFTER))
      tree_set_delay(w, p_expression());

   tree_set_loc(w, CURRENT_LOC);

   return w;
}

static void p_waveform(tree_t stmt)
{
   // waveform_element { , waveform_element } | unaffected

   BEGIN("waveform");

   if (optional(tUNAFFECTED))
      return;

   tree_add_waveform(stmt, p_waveform_element());

   while (optional(tCOMMA))
      tree_add_waveform(stmt, p_waveform_element());
}

static tree_t p_signal_assignment_statement(ident_t label, tree_t name)
{
   // [ label : ] target <= [ delay_mechanism ] waveform ;

   EXTEND("signal assignment statement");

   tree_t t = tree_new(T_SIGNAL_ASSIGN);

   tree_set_target(t, p_target(name));

   consume(tLE);

   p_waveform(t);

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;
   tree_set_loc(t, loc);

   if (label == NULL)
      label = loc_to_ident(loc);
   tree_set_ident(t, label);

   return t;
}

static void p_sensitivity_list(tree_t proc)
{
   // name { , name }

   BEGIN("sensitivity list");

   tree_add_trigger(proc, p_name());

   while (optional(tCOMMA))
      tree_add_trigger(proc, p_name());
}

static void p_sensitivity_clause(tree_t wait)
{
   // on sensitivity_list

   BEGIN("sensitivity clause");

   consume(tON);
   p_sensitivity_list(wait);
}

static void p_condition_clause(tree_t wait)
{
   // until condition

   BEGIN("condition clause");

   consume(tUNTIL);
   tree_set_value(wait, p_expression());
}

static void p_timeout_clause(tree_t wait)
{
   // for expression

   BEGIN("timeout clause");

   consume(tFOR);
   tree_set_delay(wait, p_expression());
}

static tree_t p_wait_statement(ident_t label)
{
   // [ label : ] wait [ sensitivity_clause ] [ condition_clause ]
   //   [ timeout_clause ] ;

   EXTEND("wait statement");

   tree_t t = tree_new(T_WAIT);

   consume(tWAIT);

   if (peek() == tON)
      p_sensitivity_clause(t);

   if (peek() == tUNTIL)
      p_condition_clause(t);

   if (peek() == tFOR)
      p_timeout_clause(t);

   consume(tSEMI);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_assertion_statement(ident_t label)
{
   // [ label : ] assertion ;

   EXTEND("assertion statement");

   tree_t t = p_assertion();
   consume(tSEMI);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_report_statement(ident_t label)
{
   // [ label : ] report expression [ severity expression ] ;

   EXTEND("report statement");

   tree_t t = tree_new(T_ASSERT);

   consume(tREPORT);

   tree_set_message(t, p_expression());

   if (optional(tSEVERITY))
      tree_set_severity(t, p_expression());
   else {
      tree_t sev = tree_new(T_REF);
      tree_set_ident(sev, ident_new("NOTE"));

      tree_set_severity(t, sev);
   }

   consume(tSEMI);

   tree_t false_ref = tree_new(T_REF);
   tree_set_ident(false_ref, ident_new("FALSE"));

   tree_set_value(t, false_ref);
   tree_add_attr_int(t, ident_new("is_report"), 1);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static void p_sequence_of_statements(tree_t parent, add_func_t addf)
{
   // { sequential_statement }

   BEGIN("sequence of statements");

   while (scan(tID, tWAIT, tASSERT, tREPORT, tIF, tNULL, tRETURN,
               tWHILE, tFOR, tLOOP))
      (*addf)(parent, p_sequential_statement());
}

static tree_t p_if_statement(ident_t label)
{
   // [ label : ] if condition then sequence_of_statements
   //   { elsif condition then sequence_of_statements }
   //   [ else sequence_of_statements ] end if [ label ] ;

   EXTEND("if statement");

   tree_t t = tree_new(T_IF);

   consume(tIF);

   tree_set_value(t, p_expression());

   consume(tTHEN);

   p_sequence_of_statements(t, tree_add_stmt);

   tree_t tail = t;

   while (optional(tELSIF)) {
      tree_t elsif = tree_new(T_IF);
      tree_set_ident(elsif, ident_new("elsif"));
      tree_set_value(elsif, p_expression());

      consume(tTHEN);

      p_sequence_of_statements(elsif, tree_add_stmt);

      tree_set_loc(elsif, CURRENT_LOC);

      tree_add_else_stmt(tail, elsif);
      tail = elsif;
   }

   if (optional(tELSE))
      p_sequence_of_statements(tail, tree_add_else_stmt);

   consume(tEND);
   consume(tIF);

   if (peek() == tID) {
      ident_t tail_id = p_identifier();
      (void)tail_id;
      // XXX: test me
   }

   consume(tSEMI);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_null_statement(ident_t label)
{
   // [ label : ] null ;

   EXTEND("null statement");

   consume(tNULL);
   consume(tSEMI);

   tree_t t = tree_new(T_NULL);
   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_iteration_scheme(void)
{
   // while condition | for loop_parameter_specification

   BEGIN("iteration scheme");

   if (optional(tWHILE)) {
      tree_t t = tree_new(T_WHILE);
      tree_set_value(t, p_expression());
      return t;
   }
   else if (optional(tFOR)) {
      tree_t t = tree_new(T_FOR);

      //p_loop_parameter_specification(t);

      return t;
   }
   else {
      tree_t true_ref = tree_new(T_REF);
      tree_set_ident(true_ref, ident_new("TRUE"));

      tree_t t = tree_new(T_WHILE);
      tree_set_value(t, true_ref);
      return t;
   }
}

static tree_t p_loop_statement(ident_t label)
{
   // [ loop_label : ] [ iteration_scheme ] loop sequence_of_statements
   //   end loop [ loop_label ] ;

   tree_t t = p_iteration_scheme();

   consume(tLOOP);

   p_sequence_of_statements(t, tree_add_stmt);

   consume(tEND);
   consume(tLOOP);

   if (peek() == tID) {
      ident_t tail_id = p_identifier();
      (void)tail_id;
      // XXX: test me
   }

   consume(tSEMI);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_return_statement(ident_t label)
{
   // [ label : ] return [ expression ] ;

   EXTEND("return statement");

   consume(tRETURN);

   tree_t t = tree_new(T_RETURN);

   if (peek() != tSEMI)
      tree_set_value(t, p_expression());

   consume(tSEMI);

   set_label_and_loc(t, label, CURRENT_LOC);
   return t;
}

static tree_t p_sequential_statement(void)
{
   // wait_statement | assertion_statement | report_statement
   //   | signal_assignment_statement | variable_assignment_statement
   //   | procedure_call_statement | if_statement | case_statement
   //   | loop_statement | next_statement | exit_statement | return_statement
   //   | null_statement

   BEGIN("sequential statement");

   ident_t label = NULL;
   if ((peek() == tID) && (peek2() == tCOLON)) {
      label = p_identifier();
      consume(tCOLON);
   }

   switch (peek()) {
   case tWAIT:
      return p_wait_statement(label);

   case tASSERT:
      return p_assertion_statement(label);

   case tREPORT:
      return p_report_statement(label);

   case tIF:
      return p_if_statement(label);

   case tNULL:
      return p_null_statement(label);

   case tRETURN:
      return p_return_statement(label);

   case tWHILE:
   case tLOOP:
   case tFOR:
      return p_loop_statement(label);

   case tID:
      break;

   default:
      expect(tWAIT, tID);
      return tree_new(T_NULL);
   }

   tree_t name = p_name();

   switch (peek()) {
   case tASSIGN:
      return p_variable_assignment_statement(label, name);

   case tLE:
      return p_signal_assignment_statement(label, name);

   default:
      expect(tASSIGN, tLE);
      return tree_new(T_NULL);
   }
}

static void p_process_statement_part(tree_t proc)
{
   // { sequential_statement }

   BEGIN("process statement part");

   p_sequence_of_statements(proc, tree_add_stmt);
}

static tree_t p_process_statement(ident_t label)
{
   // [ process_label : ] [ postponed ] process [ ( sensitivity_list ) ] [ is ]
   //   process_declarative_part begin process_statement_part end [ postponed ]
   //   process [ process_label ] ;

   BEGIN("process statement");

   tree_t t = tree_new(T_PROCESS);

   const bool postponed = optional(tPOSTPONED);

   consume(tPROCESS);

   if (optional(tLPAREN)) {
      p_sensitivity_list(t);
      consume(tRPAREN);
   }

   optional(tIS);

   p_process_declarative_part(t);

   consume(tBEGIN);

   p_process_statement_part(t);

   consume(tEND);
   if (postponed)
      optional(tPOSTPONED);
   consume(tPROCESS);

   if (peek() == tID) {
      ident_t tail_id = p_identifier();
      (void)tail_id;
      // XXX: test me
   }

   consume(tSEMI);

   const loc_t *loc = CURRENT_LOC;
   tree_set_loc(t, loc);

   if (label == NULL)
      label = loc_to_ident(loc);
   tree_set_ident(t, label);

   if (postponed)
      tree_add_attr_int(t, ident_new("postponed"), 1);

   return t;
}

static tree_t p_concurrent_statement(void)
{
   // block_statement | process_statement | concurrent_procedure_call_statement
   //   | concurrent_assertion_statement
   //   | concurrent_signal_assignment_statement
   //   | component_instantiation_statement | generate_statement

   BEGIN("concurrent statement");

   ident_t label = NULL;
   if ((peek() == tID) && (peek2() == tCOLON)) {
      label = p_identifier();
      consume(tCOLON);
   }

   tree_t name = NULL;
   if (peek() == tID)
      name = p_name();

   (void)name;

   switch (peek()) {
   case tPROCESS:
   case tPOSTPONED:
      return p_process_statement(label);

   default:
      expect(tPROCESS);
      return tree_new(T_BLOCK);
   }
}

static void p_architecture_declarative_part(tree_t arch)
{
   // { block_declarative_item }

   BEGIN("architecture declarative part");

   while (scan(tSIGNAL, tPROCESS))
      p_block_declarative_item(arch);
}

static void p_architecture_statement_part(tree_t arch)
{
   // { concurrent_statement }

   BEGIN("architecture statement part");

   while (scan(tID, tPROCESS, tPOSTPONED))
      tree_add_stmt(arch, p_concurrent_statement());
}

static tree_t p_architecture_body(void)
{
   // architecture identifier of entity_name is architecture_declarative_part
   //   begin architecture_statement_part end [ architecture ]
   //   [ architecture_simple_name ] ;

   BEGIN("architecture body");

   tree_t t = tree_new(T_ARCH);

   consume(tARCHITECTURE);
   tree_set_ident(t, p_identifier());
   consume(tOF);
   tree_set_ident2(t, p_identifier());
   consume(tIS);

   p_architecture_declarative_part(t);

   consume(tBEGIN);

   p_architecture_statement_part(t);

   consume(tEND);
   optional(tARCHITECTURE);

   if (peek() == tID) {
      ident_t tail_id = p_identifier();
      (void)tail_id;
      // XXX: test me
   }

   consume(tSEMI);

   tree_set_loc(t, CURRENT_LOC);
   return t;
}

static tree_t p_secondary_unit(void)
{
   // architecture_body | package_body

   BEGIN("secondary unit");

   switch (peek()) {
   case tARCHITECTURE:
      return p_architecture_body();

   default:
      expect(tARCHITECTURE);
      return NULL;
   }
}

static tree_t p_library_unit(void)
{
   // primary_unit | secondary_unit

   BEGIN("library unit");

   switch (peek()) {
   case tENTITY:
      return p_primary_unit();

   case tARCHITECTURE:
      return p_secondary_unit();

   default:
      expect(tENTITY, tARCHITECTURE);
      return NULL;
   }
}

static tree_t p_design_unit(void)
{
   BEGIN("design unit");

   p_context_clause();
   tree_t unit = p_library_unit();

   return unit;
}

void begin_token(char *tok)
{
   const char *newline = strrchr(tok, '\n');
   int n_token_start, n_token_length;
   if (newline != NULL) {
      n_token_start = 0;
      n_token_length = strlen(tok) - (newline - tok);
      n_token_next_start = n_token_length - 1;
   }
   else {
      n_token_start = n_token_next_start;
      n_token_length = strlen(tok);
      n_token_next_start += n_token_length;
   }

   const int last_col = n_token_start + n_token_length - 1;

   yylloc.first_line   = MIN(n_row, LINE_INVALID);
   yylloc.first_column = MIN(n_token_start, COLUMN_INVALID);
   yylloc.last_line    = MIN(n_row, LINE_INVALID);
   yylloc.last_column  = MIN(last_col, COLUMN_INVALID);
   yylloc.file         = perm_file_name;
   yylloc.linebuf      = perm_linebuf;
}

int get_next_char(char *b, int max_buffer)
{
   if (last_was_newline) {
      n_row += 1;
      perm_linebuf = read_ptr;
      last_was_newline = false;
   }

   const bool eof = read_ptr >= file_start + file_sz;
   if (eof)
      return 0;
   else
      *b = *read_ptr++;

   if (perm_linebuf == NULL)
      perm_linebuf = read_ptr;

   if (*b == '\n')
      last_was_newline = true;

   return *b == 0 ? 0 : 1;
}

void input_from_file(const char *file)
{
   int fd = open(file, O_RDONLY);
   if (fd < 0)
      fatal_errno("opening %s", file);

   struct stat buf;
   if (fstat(fd, &buf) != 0)
      fatal_errno("fstat");

   if (!S_ISREG(buf.st_mode))
      fatal("opening %s: not a regular file", file);

   file_sz = buf.st_size;

   file_start = mmap(NULL, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
   if (file_start == MAP_FAILED)
      fatal_errno("mmap");

   read_ptr           = file_start;
   last_was_newline   = true;
   perm_file_name     = strdup(file);
   n_row              = 0;
   n_token_next_start = 0;
}

tree_t parse(void)
{
   n_errors  = 0;
   n_correct = RECOVER_THRESH;

   assert_viol = NULL;

   if (peek() == tEOF)
      return NULL;

   tree_t unit = p_design_unit();
   if (n_errors > 0)
      return NULL;
   else
      return unit;
}

int parse_errors(void)
{
   return n_errors;
}