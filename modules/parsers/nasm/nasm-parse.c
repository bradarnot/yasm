/*
 * NASM-compatible parser
 *
 *  Copyright (C) 2001-2007  Peter Johnson, Michael Urman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <util.h>
RCSID("$Id$");

#include <libyasm.h>

#include <math.h>

#include "modules/parsers/nasm/nasm-parser.h"

typedef enum {
    NORM_EXPR,
    DIR_EXPR,
    DV_EXPR
} expr_type;

static yasm_bytecode *parse_line(yasm_parser_nasm *parser_nasm);
static int parse_directive_valparams(yasm_parser_nasm *parser_nasm,
                                     /*@out@*/ yasm_valparamhead *vps);
static yasm_bytecode *parse_times(yasm_parser_nasm *parser_nasm);
static yasm_bytecode *parse_exp(yasm_parser_nasm *parser_nasm);
static yasm_bytecode *parse_instr(yasm_parser_nasm *parser_nasm);
static yasm_insn_operand *parse_operand(yasm_parser_nasm *parser_nasm);
static yasm_effaddr *parse_memaddr(yasm_parser_nasm *parser_nasm);
static yasm_expr *parse_expr(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_bexpr(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr0(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr1(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr2(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr3(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr4(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr5(yasm_parser_nasm *parser_nasm, expr_type type);
static yasm_expr *parse_expr6(yasm_parser_nasm *parser_nasm, expr_type type);

static void nasm_parser_directive
    (yasm_parser_nasm *parser_nasm, const char *name,
     /*@null@*/ yasm_valparamhead *valparams,
     /*@null@*/ yasm_valparamhead *objext_valparams);
static void define_label(yasm_parser_nasm *parser_nasm, /*@only@*/ char *name,
                         int local);

#define is_eol_tok(tok) ((tok) == '\n' || (tok) == 0)
#define is_eol()        is_eol_tok(curtok)

#define get_next_token()    (curtok = nasm_parser_lex(&curval, parser_nasm))

static void
get_peek_token(yasm_parser_nasm *parser_nasm)
{
    char savech = parser_nasm->tokch;
    if (parser_nasm->peek_token != NONE)
        yasm_internal_error(N_("only can have one token of lookahead"));
    parser_nasm->peek_token =
        nasm_parser_lex(&parser_nasm->peek_tokval, parser_nasm);
    parser_nasm->peek_tokch = parser_nasm->tokch;
    parser_nasm->tokch = savech;
}

static void
destroy_curtok_(yasm_parser_nasm *parser_nasm)
{
    if (curtok < 256)
        ;
    else switch ((enum tokentype)curtok) {
        case INTNUM:
            yasm_intnum_destroy(curval.intn);
            break;
        case FLTNUM:
            yasm_floatnum_destroy(curval.flt);
            break;
        case DIRECTIVE_NAME:
        case FILENAME:
        case ID:
        case LOCAL_ID:
        case SPECIAL_ID:
            yasm_xfree(curval.str_val);
            break;
        case STRING:
            yasm_xfree(curval.str.contents);
            break;
        case INSN:
            yasm_bc_destroy(curval.bc);
            break;
        default:
            break;
    }
    curtok = NONE;          /* sanity */
}
#define destroy_curtok()    destroy_curtok_(parser_nasm)

/* Eat all remaining tokens to EOL, discarding all of them.  If there's any
 * intervening tokens, generates an error (junk at end of line).
 */
static void
demand_eol_(yasm_parser_nasm *parser_nasm)
{
    if (is_eol())
        return;

    yasm_error_set(YASM_ERROR_SYNTAX,
        N_("junk at end of line, first unrecognized character is `%c'"),
        parser_nasm->tokch);

    do {
        destroy_curtok();
        get_next_token();
    } while (!is_eol());
}
#define demand_eol() demand_eol_(parser_nasm)

static int
expect_(yasm_parser_nasm *parser_nasm, int token)
{
    static char strch[] = "` '";
    const char *str;

    if (curtok == token)
        return 1;

    switch (token) {
        case INTNUM:            str = "integer"; break;
        case FLTNUM:            str = "floating point value"; break;
        case DIRECTIVE_NAME:    str = "directive name"; break;
        case FILENAME:          str = "filename"; break;
        case STRING:            str = "string"; break;
        case SIZE_OVERRIDE:     str = "size override"; break;
        case DECLARE_DATA:      str = "DB/DW/etc."; break;
        case RESERVE_SPACE:     str = "RESB/RESW/etc."; break;
        case INCBIN:            str = "INCBIN"; break;
        case EQU:               str = "EQU"; break;
        case TIMES:             str = "TIMES"; break;
        case SEG:               str = "SEG"; break;
        case WRT:               str = "WRT"; break;
        case NOSPLIT:           str = "NOSPLIT"; break;
        case STRICT:            str = "STRICT"; break;
        case INSN:              str = "instruction"; break;
        case PREFIX:            str = "instruction prefix"; break;
        case REG:               str = "register"; break;
        case SEGREG:            str = "segment register"; break;
        case TARGETMOD:         str = "target modifier"; break;
        case LEFT_OP:           str = "<<"; break;
        case RIGHT_OP:          str = ">>"; break;
        case SIGNDIV:           str = "//"; break;
        case SIGNMOD:           str = "%%"; break;
        case START_SECTION_ID:  str = "$$"; break;
        case ID:                str = "identifier"; break;
        case LOCAL_ID:          str = ".identifier"; break;
        case SPECIAL_ID:        str = "..identifier"; break;
        case LINE:              str = "%line"; break;
        default:
            strch[1] = token;
            str = strch;
            break;
    }
    yasm_error_set(YASM_ERROR_PARSE, "expected %s", str);
    destroy_curtok();
    return 0;
}
#define expect(token) expect_(parser_nasm, token)

void
nasm_parser_parse(yasm_parser_nasm *parser_nasm)
{
    while (get_next_token() != 0) {
        yasm_bytecode *bc = NULL, *temp_bc;
        
        if (!is_eol()) {
            bc = parse_line(parser_nasm);
            demand_eol();
        }

        if (parser_nasm->abspos) {
            /* If we're inside an absolute section, just add to the absolute
             * position rather than appending bytecodes to a section.
             * Only RES* are allowed in an absolute section, so this is easy.
             */
            if (bc) {
                /*@null@*/ const yasm_expr *numitems, *multiple;
                unsigned int itemsize;
                numitems = yasm_bc_reserve_numitems(bc, &itemsize);
                if (numitems) {
                    yasm_expr *e;
                    e = yasm_expr_create(YASM_EXPR_MUL,
                        yasm_expr_expr(yasm_expr_copy(numitems)),
                        yasm_expr_int(yasm_intnum_create_uint(itemsize)),
                        cur_line);
                    multiple = yasm_bc_get_multiple_expr(bc);
                    if (multiple)
                        e = yasm_expr_create_tree(e, YASM_EXPR_MUL,
                                                  yasm_expr_copy(multiple),
                                                  cur_line);
                    parser_nasm->abspos = yasm_expr_create_tree(
                        parser_nasm->abspos, YASM_EXPR_ADD, e, cur_line);
                } else
                    yasm_error_set(YASM_ERROR_SYNTAX,
                        N_("only RES* allowed within absolute section"));
                yasm_bc_destroy(bc);
            }
            temp_bc = NULL;
        } else {
            temp_bc = yasm_section_bcs_append(cursect, bc);
            if (temp_bc)
                parser_nasm->prev_bc = temp_bc;
        }
        yasm_errwarn_propagate(parser_nasm->errwarns, cur_line);

        if (parser_nasm->save_input)
            yasm_linemap_add_source(parser_nasm->linemap,
                temp_bc,
                (char *)parser_nasm->save_line[parser_nasm->save_last ^ 1]);
        yasm_linemap_goto_next(parser_nasm->linemap);
    }
}

/* All parse_* functions expect to be called with curtok being their first
 * token.  They should return with curtok being the token *after* their
 * information.
 */

static yasm_bytecode *
parse_line(yasm_parser_nasm *parser_nasm)
{
    yasm_bytecode *bc;

    bc = parse_exp(parser_nasm);
    if (bc)
        return bc;

    switch (curtok) {
        case LINE: /* LINE INTNUM '+' INTNUM FILENAME */
        {
            yasm_intnum *line, *incr;
            char *filename;

            get_next_token();

            if (!expect(INTNUM)) return NULL;
            line = INTNUM_val;
            get_next_token();

            if (!expect('+')) return NULL;
            get_next_token();

            if (!expect(INTNUM)) return NULL;
            incr = INTNUM_val;
            get_next_token();

            if (!expect(FILENAME)) return NULL;
            filename = FILENAME_val;
            get_next_token();

            /* %line indicates the line number of the *next* line, so subtract
             * out the increment when setting the line number.
             */
            yasm_linemap_set(parser_nasm->linemap, filename,
                yasm_intnum_get_uint(line) - yasm_intnum_get_uint(incr),
                yasm_intnum_get_uint(incr));
            yasm_intnum_destroy(line);
            yasm_intnum_destroy(incr);
            yasm_xfree(filename);
            return NULL;
        }
        case '[': /* [ directive ] */
        {
            char *dirname;
            yasm_valparamhead dir_vps;
            int have_vps = 1;

            parser_nasm->state = DIRECTIVE;
            get_next_token();

            if (!expect(DIRECTIVE_NAME))
                return NULL;
            dirname = DIRECTIVE_NAME_val;
            get_next_token();

            if (curtok == ']' || curtok == ':')
                have_vps = 0;
            else if (!parse_directive_valparams(parser_nasm, &dir_vps)) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("invalid arguments to [%s]"), dirname);
                yasm_xfree(dirname);
                return NULL;
            }
            if (curtok == ':') {
                yasm_valparamhead ext_vps;
                get_next_token();
                if (!parse_directive_valparams(parser_nasm, &ext_vps)) {
                    yasm_error_set(YASM_ERROR_SYNTAX,
                                   N_("invalid arguments to [%s]"), dirname);
                    yasm_xfree(dirname);
                    return NULL;
                }
                nasm_parser_directive(parser_nasm, dirname,
                                      have_vps ? &dir_vps : NULL, &ext_vps);
            } else
                nasm_parser_directive(parser_nasm, dirname,
                                      have_vps ? &dir_vps : NULL, NULL);
            yasm_xfree(dirname);
            expect(']');
            get_next_token();
            return NULL;
        }
        case TIMES: /* TIMES expr exp */
            get_next_token();
            return parse_times(parser_nasm);
        case ID:
        case SPECIAL_ID:
        case LOCAL_ID:
        {
            char *name = ID_val;
            int local = (curtok != ID);

            get_next_token();
            if (is_eol()) {
                /* label alone on the line */
                yasm_warn_set(YASM_WARN_ORPHAN_LABEL,
                    N_("label alone on a line without a colon might be in error"));
                define_label(parser_nasm, name, local);
                return NULL;
            }
            if (curtok == ':')
                get_next_token();

            if (curtok == EQU) {
                /* label EQU expr */
                yasm_expr *e;
                get_next_token();
                e = parse_expr(parser_nasm, NORM_EXPR);
                if (!e) {
                    yasm_error_set(YASM_ERROR_SYNTAX,
                                   N_("expression expected after %s"), "EQU");
                    yasm_xfree(name);
                    return NULL;
                }
                yasm_symtab_define_equ(p_symtab, name, e, cur_line);
                yasm_xfree(name);
                return NULL;
            }

            define_label(parser_nasm, name, local);
            if (is_eol())
                return NULL;
            if (curtok == TIMES) {
                get_next_token();
                return parse_times(parser_nasm);
            }
            bc = parse_exp(parser_nasm);
            if (!bc)
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("instruction expected after label"));
            return bc;
        }
        default:
            yasm_error_set(YASM_ERROR_SYNTAX,
                N_("label or instruction expected at start of line"));
            return NULL;
    }
}

static int
parse_directive_valparams(yasm_parser_nasm *parser_nasm,
                          /*@out@*/ yasm_valparamhead *vps)
{
    yasm_vps_initialize(vps);
    for (;;) {
        yasm_valparam *vp;
        yasm_expr *e;
        char *id = NULL;

        /* Look for value first */
        if (curtok == ID) {
            get_peek_token(parser_nasm);
            if (parser_nasm->peek_token == '=') {
                id = ID_val;
                get_next_token(); /* id */
                get_next_token(); /* '=' */
            }
        }

        /* Look for parameter */
        switch (curtok) {
            case STRING:
                vp = yasm_vp_create_string(id, STRING_val.contents);
                get_next_token();
                break;
            case ID:
                /* We need a peek token, but avoid error if we have one
                 * already; we need to work whether or not we hit the
                 * "value=" if test above.
                 *
                 * We cheat and peek ahead to see if this is just an ID or
                 * the ID is part of an expression.  We assume a + or - means
                 * that it's part of an expression (e.g. "x+y" is parsed as
                 * the expression "x+y" and not as "x", "+y").
                 */
                if (parser_nasm->peek_token == NONE)
                    get_peek_token(parser_nasm);
                switch (parser_nasm->peek_token) {
                    case '|': case '^': case '&': case LEFT_OP: case RIGHT_OP:
                    case '+': case '-':
                    case '*': case '/': case '%': case SIGNDIV: case SIGNMOD:
                        break;
                    default:
                        /* Just an id */
                        vp = yasm_vp_create_id(id, ID_val, '$');
                        get_next_token();
                        goto next;
                }
                /*@fallthrough@*/
            default:
                e = parse_expr(parser_nasm, DIR_EXPR);
                if (!e) {
                    yasm_vps_delete(vps);
                    return 0;
                }
                vp = yasm_vp_create_expr(id, e);
                break;
        }
next:
        yasm_vps_append(vps, vp);
        if (curtok == ',')
            get_next_token();
        if (curtok == ']' || curtok == ':' || is_eol())
            return 1;
    }
}

static yasm_bytecode *
parse_times(yasm_parser_nasm *parser_nasm)
{
    yasm_expr *multiple;
    yasm_bytecode *bc;

    multiple = parse_expr(parser_nasm, DV_EXPR);
    if (!multiple) {
        yasm_error_set(YASM_ERROR_SYNTAX, N_("expression expected after %s"),
                       "TIMES");
        return NULL;
    }
    bc = parse_exp(parser_nasm);
    if (!bc) {
        yasm_error_set(YASM_ERROR_SYNTAX,
                       N_("instruction expected after TIMES expression"));
        yasm_expr_destroy(multiple);
        return NULL;
    }
    yasm_bc_set_multiple(bc, multiple);
    return bc;
}

static yasm_bytecode *
parse_exp(yasm_parser_nasm *parser_nasm)
{
    yasm_bytecode *bc;

    bc = parse_instr(parser_nasm);
    if (bc)
        return bc;

    switch (curtok) {
        case DECLARE_DATA:
        {
            unsigned int size = DECLARE_DATA_val/8;
            yasm_datavalhead dvs;
            yasm_dataval *dv;
            yasm_expr *e;

            get_next_token();

            yasm_dvs_initialize(&dvs);
            for (;;) {
                if (curtok == STRING) {
                    /* Peek ahead to see if we're in an expr.  If we're not,
                     * then generate a real string dataval.
                     */
                    get_peek_token(parser_nasm);
                    if (parser_nasm->peek_token == ','
                        || is_eol_tok(parser_nasm->peek_token)) {
                        dv = yasm_dv_create_string(STRING_val.contents,
                                                   STRING_val.len);
                        get_next_token();
                        goto dv_done;
                    }
                }
                if ((e = parse_expr(parser_nasm, DV_EXPR)))
                    dv = yasm_dv_create_expr(e);
                else {
                    yasm_error_set(YASM_ERROR_SYNTAX,
                                   N_("expression or string expected"));
                    yasm_dvs_delete(&dvs);
                    return NULL;
                }
dv_done:
                yasm_dvs_append(&dvs, dv);
                if (is_eol())
                    break;
                if (!expect(',')) {
                    yasm_dvs_delete(&dvs);
                    return NULL;
                }
                get_next_token();
                if (is_eol())   /* allow trailing , on list */
                    break;
            }
            return yasm_bc_create_data(&dvs, size, 0, p_object->arch,
                                       cur_line);
        }
        case RESERVE_SPACE:
        {
            unsigned int size = RESERVE_SPACE_val/8;
            yasm_expr *e;
            get_next_token();
            e = parse_expr(parser_nasm, DV_EXPR);
            if (!e) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("expression expected after %s"), "RESx");
                return NULL;
            }
            return yasm_bc_create_reserve(e, size, cur_line);
        }
        case INCBIN:
        {
            char *filename;
            yasm_expr *start = NULL, *maxlen = NULL;

            get_next_token();

            if (!expect(STRING)) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("filename string expected after INCBIN"));
                return NULL;
            }
            filename = STRING_val.contents;
            get_next_token();

            /* optional start expression */
            if (curtok == ',')
                get_next_token();
            if (is_eol())
                goto incbin_done;
            start = parse_expr(parser_nasm, DV_EXPR);
            if (!start) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("expression expected for INCBIN start"));
                return NULL;
            }

            /* optional maxlen expression */
            if (curtok == ',')
                get_next_token();
            if (is_eol())
                goto incbin_done;
            maxlen = parse_expr(parser_nasm, DV_EXPR);
            if (!maxlen) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                    N_("expression expected for INCBIN maximum length"));
                return NULL;
            }

incbin_done:
            return yasm_bc_create_incbin(filename, start, maxlen,
                                         parser_nasm->linemap, cur_line);
        }
        default:
            return NULL;
    }
}

static yasm_bytecode *
parse_instr(yasm_parser_nasm *parser_nasm)
{
    yasm_bytecode *bc;

    switch (curtok) {
        case INSN:
        {
            yasm_insn *insn;
            bc = INSN_val;
            insn = yasm_bc_get_insn(bc);

            get_next_token();
            if (is_eol())
                return bc;      /* no operands */

            /* parse operands */
            for (;;) {
                yasm_insn_operand *op = parse_operand(parser_nasm);
                if (!op) {
                    yasm_error_set(YASM_ERROR_SYNTAX,
                                   N_("expression syntax error"));
                    yasm_bc_destroy(bc);
                    return NULL;
                }
                yasm_insn_ops_append(insn, op);

                if (is_eol())
                    break;
                if (!expect(',')) {
                    yasm_bc_destroy(bc);
                    return NULL;
                }
                get_next_token();
            }
            return bc;
        }
        case PREFIX:
        {
            uintptr_t prefix = PREFIX_val;
            get_next_token();
            bc = parse_instr(parser_nasm);
            if (bc)
                yasm_insn_add_prefix(yasm_bc_get_insn(bc), prefix);
            return bc;
        }
        case SEGREG:
        {
            uintptr_t segreg = SEGREG_val;
            get_next_token();
            bc = parse_instr(parser_nasm);
            if (bc)
                yasm_insn_add_seg_prefix(yasm_bc_get_insn(bc), segreg);
            return bc;
        }
        default:
            return NULL;
    }
}

static yasm_insn_operand *
parse_operand(yasm_parser_nasm *parser_nasm)
{
    yasm_insn_operand *op;
    switch (curtok) {
        case '[':
        {
            yasm_effaddr *ea;
            get_next_token();
            ea = parse_memaddr(parser_nasm);

            expect(']');
            get_next_token();

            if (!ea) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("memory address expected"));
                return NULL;
            }
            return yasm_operand_create_mem(ea);
        }
        case SEGREG:
            op = yasm_operand_create_segreg(SEGREG_val);
            get_next_token();
            return op;
        case REG:
            op = yasm_operand_create_reg(REG_val);
            get_next_token();
            return op;
        case STRICT:
            get_next_token();
            op = parse_operand(parser_nasm);
            if (op)
                op->strict = 1;
            return op;
        case SIZE_OVERRIDE:
        {
            unsigned int size = SIZE_OVERRIDE_val;
            get_next_token();
            op = parse_operand(parser_nasm);
            if (!op)
                return NULL;
            if (op->type == YASM_INSN__OPERAND_REG &&
                yasm_arch_get_reg_size(p_object->arch, op->data.reg) != size)
                yasm_error_set(YASM_ERROR_TYPE,
                               N_("cannot override register size"));
            else {
                /* Silently override others unless a warning is turned on.
                 * This is to allow overrides such as:
                 *   %define arg1 dword [bp+4]
                 *   cmp word arg1, 2
                 * Which expands to:
                 *   cmp word dword [bp+4], 2
                 */
                if (op->size != 0) {
                    if (op->size != size)
                        yasm_warn_set(YASM_WARN_SIZE_OVERRIDE,
                            N_("overriding operand size from %u-bit to %u-bit"),
                            op->size, size);
                    else
                        yasm_warn_set(YASM_WARN_SIZE_OVERRIDE,
                                      N_("double operand size override"));
                }
                op->size = size;
            }
            return op;
        }
        case TARGETMOD:
        {
            uintptr_t tmod = TARGETMOD_val;
            get_next_token();
            op = parse_operand(parser_nasm);
            if (op)
                op->targetmod = tmod;
            return op;
        }
        default:
        {
            yasm_expr *e = parse_expr(parser_nasm, NORM_EXPR);
            if (!e)
                return NULL;
            return yasm_operand_create_imm(e);
        }
    }
}

/* memory addresses */
static yasm_effaddr *
parse_memaddr(yasm_parser_nasm *parser_nasm)
{
    yasm_effaddr *ea;
    switch (curtok) {
        case SEGREG:
        {
            uintptr_t segreg = SEGREG_val;
            get_next_token();
            if (!expect(':')) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("`:' required after segment register"));
                return NULL;
            }
            get_next_token();
            ea = parse_memaddr(parser_nasm);
            if (ea)
                yasm_ea_set_segreg(ea, segreg);
            return ea;
        }
        case SIZE_OVERRIDE:
        {
            unsigned int size = SIZE_OVERRIDE_val;
            get_next_token();
            ea = parse_memaddr(parser_nasm);
            if (ea)
                ea->disp.size = size;
            return ea;
        }
        case NOSPLIT:
            get_next_token();
            ea = parse_memaddr(parser_nasm);
            if (ea)
                ea->nosplit = 1;
            return ea;
        case REL:
            get_next_token();
            ea = parse_memaddr(parser_nasm);
            if (ea) {
                ea->pc_rel = 1;
                ea->not_pc_rel = 0;
            }
            return ea;
        case ABS:
            get_next_token();
            ea = parse_memaddr(parser_nasm);
            if (ea) {
                ea->pc_rel = 0;
                ea->not_pc_rel = 1;
            }
            return ea;
        default:
        {
            yasm_expr *e = parse_expr(parser_nasm, NORM_EXPR);
            if (!e)
                return NULL;
            return yasm_arch_ea_create(p_object->arch, e);
        }
    }
}

/* Expression grammar parsed is:
 *
 * expr  : bexpr [ : bexpr ]
 * bexpr : expr0 [ WRT expr6 ]
 * expr0 : expr1 [ {|} expr1...]
 * expr1 : expr2 [ {^} expr2...]
 * expr2 : expr3 [ {&} expr3...]
 * expr3 : expr4 [ {<<,>>} expr4...]
 * expr4 : expr5 [ {+,-} expr5...]
 * expr5 : expr6 [ {*,/,%,//,%%} expr6...]
 * expr6 : { ~,+,-,SEG } expr6
 *       | (expr)
 *       | symbol
 *       | $
 *       | number
 */

#define parse_expr_common(leftfunc, tok, rightfunc, op) \
    do {                                                \
        yasm_expr *e, *f;                               \
        e = leftfunc(parser_nasm, type);                \
        if (!e)                                         \
            return NULL;                                \
                                                        \
        while (curtok == tok) {                         \
            get_next_token();                           \
            f = rightfunc(parser_nasm, type);           \
            if (!f) {                                   \
                yasm_expr_destroy(e);                   \
                return NULL;                            \
            }                                           \
            e = p_expr_new_tree(e, op, f);              \
        }                                               \
        return e;                                       \
    } while(0)

static yasm_expr *
parse_expr(yasm_parser_nasm *parser_nasm, expr_type type)
{
    switch (type) {
        case NORM_EXPR:
            parse_expr_common(parse_bexpr, ':', parse_bexpr, YASM_EXPR_SEGOFF);
        case DV_EXPR:
            /* dataval expressions can't handle seg:off */
            return parse_bexpr(parser_nasm, type);
        case DIR_EXPR:
            /* directive expressions can't handle seg:off or WRT */
            return parse_expr0(parser_nasm, type);
    }
    /*@notreached@*/
    return NULL;
}

static yasm_expr *
parse_bexpr(yasm_parser_nasm *parser_nasm, expr_type type)
{
    parse_expr_common(parse_expr0, WRT, parse_expr6, YASM_EXPR_WRT);
}

static yasm_expr *
parse_expr0(yasm_parser_nasm *parser_nasm, expr_type type)
{
    parse_expr_common(parse_expr1, '|', parse_expr1, YASM_EXPR_OR);
}

static yasm_expr *
parse_expr1(yasm_parser_nasm *parser_nasm, expr_type type)
{
    parse_expr_common(parse_expr2, '^', parse_expr2, YASM_EXPR_XOR);
}

static yasm_expr *
parse_expr2(yasm_parser_nasm *parser_nasm, expr_type type)
{
    parse_expr_common(parse_expr3, '&', parse_expr3, YASM_EXPR_AND);
}

static yasm_expr *
parse_expr3(yasm_parser_nasm *parser_nasm, expr_type type)
{
    yasm_expr *e, *f;
    e = parse_expr4(parser_nasm, type);
    if (!e)
        return NULL;

    while (curtok == LEFT_OP || curtok == RIGHT_OP) {
        int op = curtok;
        get_next_token();
        f = parse_expr4(parser_nasm, type);
        if (!f) {
            yasm_expr_destroy(e);
            return NULL;
        }

        switch (op) {
            case LEFT_OP: e = p_expr_new_tree(e, YASM_EXPR_SHL, f); break;
            case RIGHT_OP: e = p_expr_new_tree(e, YASM_EXPR_SHR, f); break;
        }
    }
    return e;
}

static yasm_expr *
parse_expr4(yasm_parser_nasm *parser_nasm, expr_type type)
{
    yasm_expr *e, *f;
    e = parse_expr5(parser_nasm, type);
    if (!e)
        return NULL;

    while (curtok == '+' || curtok == '-') {
        int op = curtok;
        get_next_token();
        f = parse_expr5(parser_nasm, type);
        if (!f) {
            yasm_expr_destroy(e);
            return NULL;
        }

        switch (op) {
            case '+': e = p_expr_new_tree(e, YASM_EXPR_ADD, f); break;
            case '-': e = p_expr_new_tree(e, YASM_EXPR_SUB, f); break;
        }
    }
    return e;
}

static yasm_expr *
parse_expr5(yasm_parser_nasm *parser_nasm, expr_type type)
{
    yasm_expr *e, *f;
    e = parse_expr6(parser_nasm, type);
    if (!e)
        return NULL;

    while (curtok == '*' || curtok == '/' || curtok == '%'
           || curtok == SIGNDIV || curtok == SIGNMOD) {
        int op = curtok;
        get_next_token();
        f = parse_expr6(parser_nasm, type);
        if (!f) {
            yasm_expr_destroy(e);
            return NULL;
        }

        switch (op) {
            case '*': e = p_expr_new_tree(e, YASM_EXPR_MUL, f); break;
            case '/': e = p_expr_new_tree(e, YASM_EXPR_DIV, f); break;
            case '%': e = p_expr_new_tree(e, YASM_EXPR_MOD, f); break;
            case SIGNDIV: e = p_expr_new_tree(e, YASM_EXPR_SIGNDIV, f); break;
            case SIGNMOD: e = p_expr_new_tree(e, YASM_EXPR_SIGNMOD, f); break;
        }
    }
    return e;
}

static yasm_expr *
parse_expr6(yasm_parser_nasm *parser_nasm, expr_type type)
{
    yasm_expr *e;
    yasm_symrec *sym;

    /* directives allow very little and handle IDs specially */
    if (type == DIR_EXPR) {
        switch (curtok) {
        case '~':
            get_next_token();
            e = parse_expr6(parser_nasm, type);
            if (!e)
                return NULL;
            return p_expr_new_branch(YASM_EXPR_NOT, e);
        case '(':
            get_next_token();
            e = parse_expr(parser_nasm, type);
            if (!e)
                return NULL;
            if (!expect(')')) {
                yasm_error_set(YASM_ERROR_SYNTAX, N_("missing parenthesis"));
                return NULL;
            }
            get_next_token();
            return e;
        case INTNUM:
            e = p_expr_new_ident(yasm_expr_int(INTNUM_val));
            break;
        case REG:
            e = p_expr_new_ident(yasm_expr_reg(REG_val));
            break;
        case ID:
            sym = yasm_symtab_use(p_symtab, ID_val, cur_line);
            e = p_expr_new_ident(yasm_expr_sym(sym));
            yasm_xfree(ID_val);
            break;
        default:
            return NULL;
        }
    } else switch (curtok) {
        case '+':
            get_next_token();
            return parse_expr6(parser_nasm, type);
        case '-':
            get_next_token();
            e = parse_expr6(parser_nasm, type);
            if (!e)
                return NULL;
            return p_expr_new_branch(YASM_EXPR_NEG, e);
        case '~':
            get_next_token();
            e = parse_expr6(parser_nasm, type);
            if (!e)
                return NULL;
            return p_expr_new_branch(YASM_EXPR_NOT, e);
        case SEG:
            get_next_token();
            e = parse_expr6(parser_nasm, type);
            if (!e)
                return NULL;
            return p_expr_new_branch(YASM_EXPR_SEG, e);
        case '(':
            get_next_token();
            e = parse_expr(parser_nasm, type);
            if (!e)
                return NULL;
            if (!expect(')')) {
                yasm_error_set(YASM_ERROR_SYNTAX, N_("missing parenthesis"));
                return NULL;
            }
            get_next_token();
            return e;
        case INTNUM:
            e = p_expr_new_ident(yasm_expr_int(INTNUM_val));
            break;
        case FLTNUM:
            e = p_expr_new_ident(yasm_expr_float(FLTNUM_val));
            break;
        case REG:
            if (type == DV_EXPR) {
                yasm_error_set(YASM_ERROR_SYNTAX,
                               N_("data values can't have registers"));
                return NULL;
            }
            e = p_expr_new_ident(yasm_expr_reg(REG_val));
            break;
        case STRING:
            e = p_expr_new_ident(yasm_expr_int(
                yasm_intnum_create_charconst_nasm(STRING_val.contents)));
            yasm_xfree(STRING_val.contents);
            break;
        case ID:
        case SPECIAL_ID:
        case LOCAL_ID:
            sym = yasm_symtab_use(p_symtab, ID_val, cur_line);
            e = p_expr_new_ident(yasm_expr_sym(sym));
            yasm_xfree(ID_val);
            break;
        case '$':
            /* "$" references the current assembly position */
            if (parser_nasm->abspos)
                e = yasm_expr_copy(parser_nasm->abspos);
            else {
                sym = yasm_symtab_define_curpos(p_symtab, "$",
                    parser_nasm->prev_bc, cur_line);
                e = p_expr_new_ident(yasm_expr_sym(sym));
            }
            break;
        case START_SECTION_ID:
            /* "$$" references the start of the current section */
            if (parser_nasm->absstart)
                e = yasm_expr_copy(parser_nasm->absstart);
            else {
                sym = yasm_symtab_define_label(p_symtab, "$$",
                    yasm_section_bcs_first(cursect), 0, cur_line);
                e = p_expr_new_ident(yasm_expr_sym(sym));
            }
            break;
        default:
            return NULL;
    }
    get_next_token();
    return e;
}

static void
define_label(yasm_parser_nasm *parser_nasm, char *name, int local)
{
    if (!local) {
        if (parser_nasm->locallabel_base)
            yasm_xfree(parser_nasm->locallabel_base);
        parser_nasm->locallabel_base_len = strlen(name);
        parser_nasm->locallabel_base =
            yasm_xmalloc(parser_nasm->locallabel_base_len+1);
        strcpy(parser_nasm->locallabel_base, name);
    }

    if (parser_nasm->abspos)
        yasm_symtab_define_equ(p_symtab, name,
                               yasm_expr_copy(parser_nasm->abspos), cur_line);
    else
        yasm_symtab_define_label(p_symtab, name, parser_nasm->prev_bc, 1,
                                 cur_line);
    yasm_xfree(name);
}

static void
dir_align(yasm_object *object, yasm_valparamhead *valparams,
          yasm_valparamhead *objext_valparams, unsigned long line)
{
    yasm_valparam *vp = yasm_vps_first(valparams);
    yasm_expr *boundval = yasm_vp_expr(vp, object->symtab, line);
    /*@depedent@*/ yasm_intnum *boundintn;

    /* Largest .align in the section specifies section alignment.
     * Note: this doesn't match NASM behavior, but is a lot more
     * intelligent!
     */
    if (boundval && (boundintn = yasm_expr_get_intnum(&boundval, 0))) {
        unsigned long boundint = yasm_intnum_get_uint(boundintn);

        /* Alignments must be a power of two. */
        if (is_exp2(boundint)) {
            if (boundint > yasm_section_get_align(object->cur_section))
                yasm_section_set_align(object->cur_section, boundint, line);
        }
    }

    /* As this directive is called only when nop is used as fill, always
     * use arch (nop) fill.
     */
    yasm_section_bcs_append(object->cur_section,
        yasm_bc_create_align(boundval, NULL, NULL,
            /*yasm_section_is_code(object->cur_section) ?*/
            yasm_arch_get_fill(object->arch)/* : NULL*/,
            line));
}

static void
nasm_parser_directive(yasm_parser_nasm *parser_nasm, const char *name,
                      yasm_valparamhead *valparams,
                      yasm_valparamhead *objext_valparams)
{
    unsigned long line = cur_line;
    yasm_valparam *vp;

    if (!yasm_object_directive(p_object, name, "nasm", valparams,
                               objext_valparams, line))
        ;
    else if (yasm__strcasecmp(name, "absolute") == 0) {
        if (!valparams) {
            yasm_error_set(YASM_ERROR_SYNTAX,
                           N_("directive `%s' requires an argument"),
                           "absolute");
        } else {
            vp = yasm_vps_first(valparams);
            if (parser_nasm->absstart)
                yasm_expr_destroy(parser_nasm->absstart);
            if (parser_nasm->abspos)
                yasm_expr_destroy(parser_nasm->abspos);
            parser_nasm->absstart = yasm_vp_expr(vp, p_object->symtab, line);
            parser_nasm->abspos = yasm_expr_copy(parser_nasm->absstart);
            cursect = NULL;
            parser_nasm->prev_bc = NULL;
        }
    } else if (yasm__strcasecmp(name, "align") == 0) {
        /* Really, we shouldn't end up with an align directive in an absolute
         * section (as it's supposed to be only used for nop fill), but handle
         * it gracefully anyway.
         */
        if (parser_nasm->abspos) {
            yasm_expr *boundval, *e;
            vp = yasm_vps_first(valparams);
            boundval = yasm_vp_expr(vp, p_object->symtab, line);
            e = yasm_expr_create_tree(
                yasm_expr_create_tree(yasm_expr_copy(parser_nasm->absstart),
                                      YASM_EXPR_SUB,
                                      yasm_expr_copy(parser_nasm->abspos),
                                      cur_line),
                YASM_EXPR_AND,
                yasm_expr_create(YASM_EXPR_SUB, yasm_expr_expr(boundval),
                                 yasm_expr_int(yasm_intnum_create_uint(1)),
                                 cur_line),
                cur_line);
            parser_nasm->abspos = yasm_expr_create_tree(
                parser_nasm->abspos, YASM_EXPR_ADD, e, cur_line);
        } else if (!valparams) {
            yasm_error_set(YASM_ERROR_SYNTAX,
                           N_("directive `%s' requires an argument"), "align");
        } else
            dir_align(p_object, valparams, objext_valparams, line);
    } else if (yasm__strcasecmp(name, "default") == 0) {
        if (!valparams)
            ;
        else {
            vp = yasm_vps_first(valparams);
            while (vp) {
                const char *id = yasm_vp_id(vp);
                if (id) {
                    if (yasm__strcasecmp(id, "rel") == 0)
                        yasm_arch_set_var(p_object->arch, "default_rel", 1);
                    else if (yasm__strcasecmp(id, "abs") == 0)
                        yasm_arch_set_var(p_object->arch, "default_rel", 0);
                    else
                        yasm_error_set(YASM_ERROR_SYNTAX,
                                       N_("unrecognized default `%s'"), id);
                } else
                    yasm_error_set(YASM_ERROR_SYNTAX,
                                   N_("unrecognized default value"));
                vp = yasm_vps_next(vp);
            }
        }
    } else
        yasm_error_set(YASM_ERROR_SYNTAX, N_("unrecognized directive `%s'"),
                       name);

    if (parser_nasm->absstart && cursect) {
        /* We switched to a new section.  Get out of absolute section mode. */
        yasm_expr_destroy(parser_nasm->absstart);
        parser_nasm->absstart = NULL;
        if (parser_nasm->abspos) {
            yasm_expr_destroy(parser_nasm->abspos);
            parser_nasm->abspos = NULL;
        }
    }

    if (cursect) {
        /* In case cursect changed or a bytecode was added, update prev_bc. */
        parser_nasm->prev_bc = yasm_section_bcs_last(cursect);
    }

    if (valparams)
        yasm_vps_delete(valparams);
    if (objext_valparams)
        yasm_vps_delete(objext_valparams);
}
