%{
/*
 * Copyright (c) 2009      The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 */
#include "dplasma.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct symbol_stack_elt {
    symbol_t                *sym;
    struct symbol_stack_elt *next;
} symbol_stack_elt_t;

static symbol_stack_elt_t *dplasma_symbol_stack = NULL;

static dplasma_t* global_dplasma = NULL;
static int global_lists_index = 0;
static int global_indep_index = 0;
static int global_outdep_index = 0;
static int global_call_params_index = 0;
static char inout_type;
extern int dplasma_lineno;

void external_hook(void);

/*
static expr_t *global_expr_stack[MAX_EXPR_STACK_COUNT];
static unsigned int global_expr_stack_size = 0;
*/

extern int yyparse(void);
extern int yylex(void);

static void yyerror(const char *str)
{
    fprintf(stderr, "parse error at line %d: %s\n", dplasma_lineno, str);
}

int yywrap()
{
	return 1;
}

int main2(int argc, char *argv[])
{
    dplasma_lineno = 1;
	yyparse();

    /*symbol_dump_all("");*/
    /*dplasma_dump_all();*/
    {
        dplasma_execution_context_t exec_context;
        /* I know what I'm doing ;) */
        exec_context.function = (dplasma_t*)dplasma_find("POTRF");
        dplasma_set_initial_execution_context(&exec_context);
        dplasma_execute(&exec_context);
    }
/*    external_hook(); */

	return 0;
}
%}

%union
{
    int        number;
    char*      string;
    char       operand;
    expr_t*    expr;
    dplasma_t* dplasma;
}

%token DPLASMA_COMMA DPLASMA_OPEN_PAR DPLASMA_CLOSE_PAR DPLASMA_RANGE
%token DPLASMA_EQUAL  DPLASMA_ASSIGNMENT DPLASMA_QUESTION DPLASMA_COLON
%token <number>  DPLASMA_INT
%token <string>  DPLASMA_VAR
%token <string>  DPLASMA_BODY
%token <operand> DPLASMA_OP
%token <operand> DPLASMA_DEPENDENCY_TYPE
%token <operand> DPLASMA_ARROW

%type  <expr>    expr

%nonassoc DPLASMA_ASSIGNMENT
%nonassoc DPLASMA_RANGE
%left DPLASMA_EQUAL
%left DPLASMA_OP

%%

prog:
    dplasma prog
    | DPLASMA_VAR DPLASMA_ASSIGNMENT expr
            {
                dplasma_add_global_symbol( $1, $3 );
            } prog
    |
;

dplasma:
     DPLASMA_VAR {
                     global_dplasma = dplasma_find_or_create($1);
                     global_lists_index = 0;
                 }
     DPLASMA_OPEN_PAR varlist DPLASMA_CLOSE_PAR
     execution_space  {
                          global_lists_index = 0;
                      }
     partitioning {
                      global_lists_index = 0;
                  }
     params {
                global_lists_index = 0;
            } 
     DPLASMA_BODY
                {
                    global_dplasma->body = $12;
                }
;

varlist:   DPLASMA_VAR DPLASMA_COMMA {
                                        symbol_stack_elt_t *s;
                                        if( global_lists_index == MAX_LOCAL_COUNT ) {
                                            fprintf(stderr,
                                                    "Internal Error while parsing at line %d:\n"
                                                    "  Maximal variable list count reached: %d (I told you guys this will happen)\n",
                                                    dplasma_lineno,
                                                    global_lists_index);
                                            YYERROR;
                                        } else {
                                            global_dplasma->locals[global_lists_index] = (symbol_t*)calloc(1, sizeof(symbol_t));
                                            global_dplasma->locals[global_lists_index]->name = $1;
                                            
                                            s = (symbol_stack_elt_t*)calloc(1, sizeof(symbol_stack_elt_t));
                                            s->sym = global_dplasma->locals[global_lists_index];
                                            s->next = dplasma_symbol_stack;
                                            dplasma_symbol_stack = s;

                                            global_lists_index++;
                                        }
                                     } varlist
         | DPLASMA_VAR {
                          symbol_stack_elt_t *s;
                          if( global_lists_index == MAX_LOCAL_COUNT ) {
                               fprintf(stderr,
                                       "Internal Error while parsing at line %d:\n"
                                       "  Maximal variable list count reached: %d (I told you guys this will happen)\n",
                                       dplasma_lineno,
                                       global_lists_index);
                               YYERROR;
                          } else {
                              global_dplasma->locals[global_lists_index] = (symbol_t*)calloc(1, sizeof(symbol_t));
                              global_dplasma->locals[global_lists_index]->name = $1;

                              s = (symbol_stack_elt_t*)calloc(1, sizeof(symbol_stack_elt_t));
                              s->sym = global_dplasma->locals[global_lists_index];
                              s->next = dplasma_symbol_stack;
                              dplasma_symbol_stack = s;

                              global_lists_index++;
                          }
                       }
         |
;

execution_space: assignment execution_space
         | 
;

assignment: DPLASMA_VAR DPLASMA_ASSIGNMENT expr {
                                                    int i;
                                                    for(i = 0; (i < MAX_LOCAL_COUNT) &&
                                                               (NULL != global_dplasma->locals[i]); i++) {
                                                        if( strcmp(global_dplasma->locals[i]->name, $1) ) {
                                                            continue;
                                                        }
                                                        break;
                                                    }
                                                    if( i == MAX_LOCAL_COUNT ) {
                                                        fprintf(stderr,
                                                                "Parse Error at line %d:\n"
                                                                "  '%s' is an unbound variable\n",
                                                                dplasma_lineno,
                                                                $1);
                                                        YYERROR;
                                                    }
                                                    if( EXPR_OP_BINARY_RANGE == $3->op ) {
                                                        global_dplasma->locals[i]->min = $3->bop1;
                                                        global_dplasma->locals[i]->max = $3->bop2;
                                                        free($3);
                                                    } else {
                                                        global_dplasma->locals[i]->min = $3;
                                                        global_dplasma->locals[i]->max = $3;
                                                    }
                                                    /* Mark it as standalone if it's the case */
                                                    if( 0 == dplasma_symbol_is_standalone(global_dplasma->locals[i]) ) {
                                                        global_dplasma->locals[i]->flags |= DPLASMA_SYMBOL_IS_STANDALONE;
                                                    }
                                                }
;

partitioning: DPLASMA_COLON expr  {
                                       if( global_lists_index == MAX_PRED_COUNT ) {
                                           fprintf(stderr,
                                                   "Internal Error while parsing at line %d:\n"
                                                   "  Maximal predicate list count reached: %d (I told you guys this will happen)\n",
                                                   dplasma_lineno,
                                                   global_lists_index);
                                           YYERROR;
                                       } else {
                                           global_dplasma->preds[global_lists_index] = $2;                             
                                           global_lists_index++;
                                       }
                                   }
              partitioning
         | 
;

params: param { global_lists_index++; }
        params
         |
;

param: DPLASMA_DEPENDENCY_TYPE {
                                   /* we can't use global_lists_index for both the params and the deps of each param */
                                   global_indep_index = 0;
                                   global_outdep_index = 0;
                                   if( global_lists_index == MAX_PARAM_COUNT ) {
                                       fprintf(stderr,
                                               "Internal Error while parsing at line %d:\n"
                                               "  Maximal parameter list count reached: %d (Oh no! Thomas told us this will happen)\n",
                                               dplasma_lineno,
                                               global_lists_index);
                                       YYERROR;
                                   } else {
                                         global_dplasma->params[global_lists_index] = (param_t*)calloc(1, sizeof(param_t));
                                   }

                                   if( SYM_IN == $1 ){
                                       global_dplasma->params[global_lists_index]->sym_type = $1;
                                   }else if( SYM_OUT == $1 ){
                                       global_dplasma->params[global_lists_index]->sym_type = $1;
                                   }else if( SYM_INOUT == $1 ){
                                       global_dplasma->params[global_lists_index]->sym_type = SYM_INOUT;
                                   }else{
                                       fprintf(stderr,
                                               "Internal Error while parsing at line %d:\n"
                                               "  Unknown type of dependency.\n",
                                               dplasma_lineno);
                                       YYERROR;
                                   }

                               }
       DPLASMA_VAR {
                       global_dplasma->params[global_lists_index]->sym_name = $3;
                       global_dplasma->dependencies_mask |= (1 << global_lists_index);
                   }
       dependencies
;

dependencies: DPLASMA_ARROW {
                                char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                                if( (sym_type == SYM_OUT) && ($1 == '<') ){
                                     fprintf(stderr,
                                             "Internal Error while parsing at line %d:\n"
                                             "  Dependency declared as OUT but symbol \"<-\" used.\n",
                                             dplasma_lineno);
                                     YYERROR;
                                }
                                if( (sym_type == SYM_IN) && ($1 == '>') ){
                                     fprintf(stderr,
                                             "Internal Error while parsing at line %d:\n"
                                             "  Dependency declared as IN but symbol \"->\" used.\n",
                                             dplasma_lineno);
                                     YYERROR;
                                }
                                if( (sym_type == SYM_INOUT) ){
                                    inout_type = ($1 == '>') ? SYM_OUT : SYM_IN;
                                }
                            }
              dependency {
                         }
              dependencies
        | 
;

dependency: call {
                     char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                     if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                         global_dplasma->params[global_lists_index]->dep_in[global_indep_index]->cond = NULL;
                         global_indep_index++;
                     }
                     if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                         global_dplasma->params[global_lists_index]->dep_out[global_outdep_index]->cond = NULL;
                         global_outdep_index++;
                     }
                 }
        | expr DPLASMA_QUESTION call {
                                         dep_t *curr_dep = NULL;
                                         char sym_type = global_dplasma->params[global_lists_index]->sym_type;

                                         if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                                             curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index];
                                             global_indep_index++;
                                         }
                                         if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                                             curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index];
                                             global_outdep_index++;
                                         }
                                         curr_dep->cond = $1;
                                     }
          DPLASMA_COLON call {
                                 dep_t *curr_dep = NULL;
                                 char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                                 if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                                     curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index];
                                     global_indep_index++;
                                 }
                                 if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                                     curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index];
                                     global_outdep_index++;
                                 }
                                 curr_dep->cond = expr_new_unary( '!', $1);
                             }
;

call: DPLASMA_VAR DPLASMA_VAR  {
                                   dep_t *curr_dep = NULL;
                                   char sym_type = global_dplasma->params[global_lists_index]->sym_type;

                                   if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                                       global_dplasma->params[global_lists_index]->dep_in[global_indep_index] = (dep_t*)calloc(1, sizeof(dep_t));
                                       curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index];
                                   }
                                   if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                                       global_dplasma->params[global_lists_index]->dep_out[global_outdep_index] = (dep_t*)calloc(1, sizeof(dep_t));
                                       curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index]; 
                                   }
                                   curr_dep->sym_name = $1;
                                   curr_dep->dplasma = dplasma_find_or_create($2);
                               }
      DPLASMA_OPEN_PAR {
                           global_call_params_index = 0;
                       }
      expr_list DPLASMA_CLOSE_PAR
      | DPLASMA_DEPENDENCY_TYPE {  /* Special case for IN() and OUT() */
                                       dep_t *curr_dep = NULL; 
                                       char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                                       if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                                           global_dplasma->params[global_lists_index]->dep_in[global_indep_index] = (dep_t*)calloc(1, sizeof(dep_t));
                                           curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index]; 
                                       }
                                       if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                                           global_dplasma->params[global_lists_index]->dep_out[global_outdep_index] = (dep_t*)calloc(1, sizeof(dep_t)); 
                                           curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index]; 
                                       }

                                       if( $1 == SYM_IN ) {
                                           curr_dep->dplasma = dplasma_find_or_create("IN");
                                           global_dplasma->flags |= DPLASMA_HAS_IN_IN_DEPENDENCIES;
                                       } else if( $1 == SYM_OUT ) {
                                           curr_dep->dplasma = dplasma_find_or_create("OUT");
                                           global_dplasma->flags |= DPLASMA_HAS_OUT_OUT_DEPENDENCIES;
                                       } else {
                                           fprintf(stderr,
                                                   "Internal Error while parsing at line %d:\n"
                                                   "  Expecting either IN(...) our OUT(...) dependency.\n",
                                                   dplasma_lineno);
                                           YYERROR;
                                       }
                                       curr_dep->sym_name = NULL;
                                  }
          DPLASMA_OPEN_PAR{ 
                              global_call_params_index = 0;
                          }
          expr_list DPLASMA_CLOSE_PAR
;

expr_list: expr {
                     dep_t *curr_dep = NULL;
                     char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                     /* expr_t *e = $1; */

                     if( global_call_params_index == MAX_CALL_PARAM_COUNT ) {
                         fprintf(stderr,
                                 "Internal Error while parsing at line %d:\n"
                                 "  Found %d parameters when expecting less than %d.\n",
                                 dplasma_lineno,
                                 global_call_params_index,
                                 MAX_CALL_PARAM_COUNT);
                         YYERROR;
                     }
 
                     if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                         curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index];
                     }
                     if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                         curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index];
                     }
                     curr_dep->call_params[global_call_params_index++] = $1;
                }
           DPLASMA_COMMA expr_list
        | expr {
                  dep_t *curr_dep = NULL;
                  char sym_type = global_dplasma->params[global_lists_index]->sym_type;
                  /* expr_t *e = $1; */

                  if( global_call_params_index == MAX_CALL_PARAM_COUNT ) {
                      fprintf(stderr,
                              "Internal Error while parsing at line %d:\n"
                              "  Found %d parameters when expecting less than %d.\n",
                              dplasma_lineno,
                              global_call_params_index,
                              MAX_CALL_PARAM_COUNT);
                      YYERROR;
                  }

                  if( (sym_type == SYM_IN) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_IN) ) ) {
                      curr_dep = global_dplasma->params[global_lists_index]->dep_in[global_indep_index];
                  }
                  if( (sym_type == SYM_OUT) || ( (sym_type == SYM_INOUT) && (inout_type == SYM_OUT) ) ) {
                      curr_dep = global_dplasma->params[global_lists_index]->dep_out[global_outdep_index];
                  }
                  curr_dep->call_params[global_call_params_index++] = $1;
              }

;

expr:     DPLASMA_VAR                                {
                                                         symbol_stack_elt_t *s = dplasma_symbol_stack;
                                                         const symbol_t     *symbol;
                                                         
                                                         /* TODO: No need for the stack_elt. Instead use the locals and globals */
                                                         while( NULL != s && strcmp(s->sym->name, $1) ) {
                                                             s = s->next;
                                                         }
                                                         if( NULL == s ) {
                                                             /* The only alternative is to use a global symbol */
                                                             symbol = dplasma_search_global_symbol($1);
                                                             if( NULL == symbol ) {
                                                                 fprintf( stderr,
                                                                          "Add expression based on unknown symbol %s at line %d\n",
                                                                          $1, dplasma_lineno );
                                                                 YYERROR;
                                                             }
                                                         } else {
                                                             symbol = s->sym;
                                                         }
                                                         $$ = expr_new_var(symbol);
                                                         free($1);
                                                     }
        | DPLASMA_INT                                { $$ = expr_new_int($1); }
        | expr DPLASMA_OP expr                       { $$ = expr_new_binary($1, $2, $3); }
        | DPLASMA_OPEN_PAR expr DPLASMA_CLOSE_PAR    { $$ = $2; }
        | expr DPLASMA_EQUAL expr                    { $$ = expr_new_binary($1, '=', $3); }
        | expr DPLASMA_RANGE expr                    { $$ = expr_new_binary($1, '.', $3);; }
;

%%

