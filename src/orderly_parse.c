/*
 * Copyright 2009, Lloyd Hilaiel.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 * 
 *  3. Neither the name of Lloyd Hilaiel nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */ 

#include "orderly_parse.h"
#include "orderly_lex.h"
#include "orderly_alloc.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <math.h>

#define BUF_STRDUP(dst, a, ob, ol)               \
    (dst) = OR_MALLOC((a), (ol) + 1);            \
    memcpy((void *)(dst), (void *) (ob), (ol));  \
    ((char *) (dst))[(ol)] = 0;


static orderly_parse_status
orderly_parse_definition_suffix(orderly_alloc_funcs * alloc,
                                const unsigned char * schemaText,
                                const unsigned int schemaTextLen,
                                orderly_lexer lxr,
                                unsigned int * offset,
                                orderly_node * n)
{
    orderly_tok t;
    const unsigned char * outBuf = NULL;
    unsigned int outLen = 0;

    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);    

    /* optional_enum_values? */
    if (t == orderly_tok_json_array) {
        BUF_STRDUP(n->values, alloc, outBuf, outLen);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);    
    }

    /* optional_default_value? */
    if (t == orderly_tok_default_value) {
        BUF_STRDUP(n->default_value, alloc, outBuf, outLen);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);    
    }

    /* optional_requires? */
    if (t == orderly_tok_lt) {
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);            
        if (t != orderly_tok_property_name) {
            return orderly_parse_s_prop_name_expected;
        }
        BUF_STRDUP(n->requires, alloc, outBuf, outLen);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);            
        if (t != orderly_tok_gt) {
            return orderly_parse_s_gt_expected;
        }
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);            
    }

    /* optional_default_value? */    
    if (t == orderly_tok_optional_marker) {    
        n->optional = 1;
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);            
    }

    /* "unlex" the last token.  let higher level code deal with it */
    *offset -= outLen;
    
    return orderly_parse_s_ok;
}

static orderly_parse_status
orderly_parse_string_suffix(orderly_alloc_funcs * alloc,
                            const unsigned char * schemaText,
                            const unsigned int schemaTextLen,
                            orderly_lexer lxr,
                            unsigned int * offset,
                            orderly_node * n)
{
    if (orderly_lex_peek(lxr, schemaText, schemaTextLen, *offset) == orderly_tok_regex) 
    {
        const unsigned char * outBuf = NULL;
        unsigned int outLen = 0;
        (void) orderly_lex_lex(lxr, schemaText, schemaTextLen, offset,
                               &outBuf, &outLen);
        /* chomp off leading and trailing slashes. the lexer MUST return
         * a string of at least length two */
        assert(outLen >= 2);
        BUF_STRDUP(n->regex, alloc, outBuf + 1, outLen - 2);        
    }

    return orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen, lxr, offset, n);
}


/* in-place unescaping of a json string */
static char *
unescapeJsonString(orderly_alloc_funcs * alloc,
                   const char * json,
                   unsigned int len)
{
#define FREE_AND_BAIL { free(str); str = NULL; return str; }
#define CHECK_LEN if ((json - orig) >= len) FREE_AND_BAIL;

    char * str = NULL;
    char * p = NULL;
    const char * orig = json;

    if (json && len > 0) {
        p = str = OR_MALLOC(alloc, len);
        if (*json++ != '"') FREE_AND_BAIL;
        /* now for string content that we'll copy over as we go */
        do
        {
            CHECK_LEN;
            switch(*json++) {
                /* the end, my friend */  
                case '"': goto i_want_to_break_free;
                case '\\':
                    CHECK_LEN;
                    switch(*json++) {
                        case 'f': *p++ = '\f'; break;
                        case 'r': *p++ = '\r'; break;
                        case 'n': *p++ = '\n'; break;
                        case 't': *p++ = '\t'; break;
                        case 'b': *p++ = '\b'; break;
                        /* TODO: support decoding of \u escaping in *property* names? */
                        default: *p++ = *(json-1);
                    }
                    break;
                default: *p++ = *(json-1);
            }
        } while (1);
      i_want_to_break_free:
        *p = 0;
    }
    
    return str;
}


static int
decodeJsonInteger(const unsigned char * json, unsigned int len,
                  long int * i)
{
    char numBuf[64];
    if (!json || !len || sizeof(numBuf) <= len) return 0;
    memcpy(numBuf, json, len);
    numBuf[len] = 0;
    *i = strtol(numBuf, NULL, 10);    
    if ((*i == LONG_MIN || *i == LONG_MAX) && errno == ERANGE) {
        return 0;
    }
    return 1;
}


static int
decodeJsonDouble(unsigned const char * json, unsigned int len,
                 double * d)
{
    char numBuf[64];
    if (!json || !len || sizeof(numBuf) <= len) return 0;
    memcpy(numBuf, json, len);
    numBuf[len] = 0;
    *d = strtod(numBuf, NULL);    
    if ((*d == HUGE_VAL || *d == -HUGE_VAL) && errno == ERANGE) {
        return 0;
    }
    return 1;
}

                                 
static orderly_parse_status
orderly_parse_range(orderly_alloc_funcs * alloc,
                    const unsigned char * schemaText,
                    const unsigned int schemaTextLen,
                    orderly_lexer lxr,
                    unsigned int * offset,
                    orderly_node * n)
{
    const unsigned char * outBuf = NULL;
    unsigned int outLen = 0;
    orderly_tok t;

    assert(n != NULL);
    
    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    if (t != orderly_tok_left_curly) {
        return orderly_parse_s_malformed_range;
    }

    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    /* optional starting range */
    if (t == orderly_tok_json_integer) {
        n->range.info |= ORDERLY_RANGE_LHS_INT;
        if (!decodeJsonInteger(outBuf, outLen, &(n->range.lhs.i))) {
            return orderly_parse_s_integer_overflow;
        }
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    } else if (t == orderly_tok_json_number) {
        n->range.info |= ORDERLY_RANGE_LHS_DOUBLE;
        if (!decodeJsonDouble(outBuf, outLen, &(n->range.lhs.d))) {
            return orderly_parse_s_numeric_parse_error;
        }
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    }

    if (t != orderly_tok_comma) {
        return orderly_parse_s_malformed_range;
    }
    
    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    /* optional ending range */
    if (t == orderly_tok_json_integer) {
        n->range.info |= ORDERLY_RANGE_RHS_INT;
        if (!decodeJsonInteger(outBuf, outLen, &(n->range.rhs.i))) {
            return orderly_parse_s_integer_overflow;
        }
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    } else if (t == orderly_tok_json_number) {
        n->range.info |= ORDERLY_RANGE_RHS_DOUBLE;
        if (!decodeJsonDouble(outBuf, outLen, &(n->range.rhs.d))) {
            return orderly_parse_s_numeric_parse_error;
        }
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    }

    if (t != orderly_tok_right_curly) {
        return orderly_parse_s_malformed_range;
    }
    
    return orderly_parse_s_ok;
}


static orderly_parse_status
orderly_parse_property_name(orderly_alloc_funcs * alloc,
                            const unsigned char * schemaText,
                            const unsigned int schemaTextLen,
                            orderly_lexer lxr,
                            unsigned int * offset,
                            orderly_node * n)
{
    const unsigned char * outBuf = NULL;
    unsigned int outLen = 0;
    orderly_tok t;

    assert(n != NULL);
    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, &outBuf, &outLen);
    
    if (t == orderly_tok_property_name) {
        BUF_STRDUP(n->name, alloc, outBuf, outLen);
    } else if (t == orderly_tok_json_string) {
        n->name = unescapeJsonString(alloc, (const char *) outBuf, outLen);        
        if (n->name == NULL) {
            return orderly_parse_s_prop_name_syntax_error;
        }
    } else {
        return orderly_parse_s_prop_name_expected;
    }

    return orderly_parse_s_ok;
}


static orderly_parse_status
orderly_parse_optional_range(orderly_alloc_funcs * alloc,
                            const unsigned char * schemaText,
                            const unsigned int schemaTextLen,
                            orderly_lexer lxr,
                            unsigned int * offset,
                            orderly_node * n)
{
    orderly_parse_status s = orderly_parse_s_ok;
    if (orderly_lex_peek(lxr, schemaText, schemaTextLen, *offset) == orderly_tok_left_curly)
    {
        s = orderly_parse_range(alloc, schemaText, schemaTextLen, lxr, offset, n);
    }
    return s;
}

/* forward decl neccesitated by mutual recursion */
static orderly_parse_status
orderly_parse_entry(orderly_alloc_funcs * alloc,
                    const unsigned char * schemaText,
                    const unsigned int schemaTextLen,
                    orderly_lexer lxr,
                    unsigned int * offset,
                    orderly_node ** n,
                    int named);


static orderly_parse_status
orderly_parse_entries(orderly_alloc_funcs * alloc,
                            const unsigned char * schemaText,
                            const unsigned int schemaTextLen,
                            orderly_lexer lxr,
                            unsigned int * offset,
                            orderly_node ** n,
                            int named)
{
    /*
     * (un)named_entries
     *     (un)named_entry ';' (un)named_entries
     *     (un)named_entry 
     *     # nothing
     *
     */
    orderly_parse_status s = orderly_parse_s_ok;
    orderly_tok t = orderly_lex_peek(lxr, schemaText, schemaTextLen, *offset);

    if (t == orderly_tok_kw_string ||
        t == orderly_tok_kw_integer ||
        t == orderly_tok_kw_number ||
        t == orderly_tok_kw_boolean ||
        t == orderly_tok_kw_null ||
        t == orderly_tok_kw_any ||
        t == orderly_tok_kw_array ||
        t == orderly_tok_kw_object ||
        t == orderly_tok_kw_union)
    {
        /* looks like we got a named entry! */
        s = orderly_parse_entry(alloc, schemaText, schemaTextLen, lxr, offset, n, named);
        if (s == orderly_parse_s_ok) {
            /* optionally consume a semicolon */
            if (orderly_tok_semicolon == orderly_lex_peek(lxr, schemaText, schemaTextLen, *offset))
            {
                (void) orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
                /* if a semicolon is present, we'll try to lex another named entry, recursively */ 
                s = orderly_parse_entries(alloc, schemaText, schemaTextLen,
                                          lxr, offset, &((*n)->sibling), named);
                if (orderly_parse_s_ok != s)
                {
                    orderly_free_node(alloc, n);            
                }
            }
        }
    }

    return s;
}

static orderly_parse_status
orderly_parse_entry(orderly_alloc_funcs * alloc,
                    const unsigned char * schemaText,
                    const unsigned int schemaTextLen,
                    orderly_lexer lxr,
                    unsigned int * offset,
                    orderly_node ** n,
                    int named)
{
    orderly_tok t;
    orderly_parse_status s;

    t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
    
    if (t == orderly_tok_kw_string)
    {
        *n = orderly_alloc_node(alloc, orderly_node_string);
        if ((s = orderly_parse_optional_range(alloc, schemaText, schemaTextLen,
                                             lxr, offset, *n)) || 
            (named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                       lxr, offset, *n))) || 
            (s = orderly_parse_string_suffix(alloc, schemaText, schemaTextLen,
                                             lxr, offset, *n)))
        {
            orderly_free_node(alloc, n);
        }
    }
    else if (t == orderly_tok_kw_null || t == orderly_tok_kw_boolean ||
             t == orderly_tok_kw_any)
    {
        if (t == orderly_tok_kw_null) *n = orderly_alloc_node(alloc, orderly_node_null);
        else if (t == orderly_tok_kw_boolean) *n = orderly_alloc_node(alloc, orderly_node_boolean);
        else if (t == orderly_tok_kw_any) *n = orderly_alloc_node(alloc, orderly_node_any);

        if ((named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                       lxr, offset, *n))) || 
            (s = orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen,
                                                 lxr, offset, *n)))
        {
            orderly_free_node(alloc, n);
        }
    }
    else if (t == orderly_tok_kw_integer || t == orderly_tok_kw_number)
    {
        if (t == orderly_tok_kw_integer) *n = orderly_alloc_node(alloc, orderly_node_integer);
        else if (t == orderly_tok_kw_number) *n = orderly_alloc_node(alloc, orderly_node_number);

        if ((s = orderly_parse_optional_range(alloc, schemaText, schemaTextLen,
                                             lxr, offset, *n)) ||
            (named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                       lxr, offset, *n))) || 
            (s = orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen,
                                                 lxr, offset, *n)))
        {
            orderly_free_node(alloc, n);
        }
    }
    else if (t == orderly_tok_kw_object)
    {
        *n = orderly_alloc_node(alloc, orderly_node_object);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
        if (t != orderly_tok_left_curly) {
            orderly_free_node(alloc, n);            
            return orderly_parse_s_left_curly_expected;
        }

        /* now parse named entries (a.k.a. kiddies) */
        s = orderly_parse_entries(alloc, schemaText, schemaTextLen,
                                  lxr, offset, &((*n)->child), 1);
        if (orderly_parse_s_ok != s)
        {
            orderly_free_node(alloc, n);            
        }
        else
        {
            t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
            if (t != orderly_tok_right_curly) {
                orderly_free_node(alloc, n);            
                s = orderly_parse_s_right_curly_expected;
            } else if ((named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                                  lxr, offset, *n))) || 
                       (s = orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen,
                                                            lxr, offset, *n)))
            {
                orderly_free_node(alloc, n);                            
            }
        }
    }
    else if (t == orderly_tok_kw_array)
    {
        *n = orderly_alloc_node(alloc, orderly_node_array);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
        if (t != orderly_tok_left_curly) {
            orderly_free_node(alloc, n);            
            return orderly_parse_s_left_curly_expected;
        }

        /* now parse unnamed entries (a.k.a. kiddies) */
        s = orderly_parse_entries(alloc, schemaText, schemaTextLen,
                                  lxr, offset, &((*n)->child), 0);
        if (orderly_parse_s_ok != s)
        {
            orderly_free_node(alloc, n);            
        }
        else
        {
            t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
            if (t != orderly_tok_right_curly) {
                orderly_free_node(alloc, n);            
                s = orderly_parse_s_right_curly_expected;
            } else if ((s = orderly_parse_optional_range(alloc, schemaText, schemaTextLen,
                                             lxr, offset, *n)) ||
                       (named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                                  lxr, offset, *n))) || 
                       (s = orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen,
                                                            lxr, offset, *n)))
            {
                orderly_free_node(alloc, n);                            
            }
        }
    }
    else if (t == orderly_tok_kw_union)
    {
        *n = orderly_alloc_node(alloc, orderly_node_union);
        t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
        if (t != orderly_tok_left_curly) {
            orderly_free_node(alloc, n);            
            return orderly_parse_s_left_curly_expected;
        }

        /* now parse unnamed entries (a.k.a. kiddies) */
        s = orderly_parse_entries(alloc, schemaText, schemaTextLen,
                                  lxr, offset, &((*n)->child), 0);
        if (orderly_parse_s_ok != s)
        {
            orderly_free_node(alloc, n);            
        }
        else
        {
            t = orderly_lex_lex(lxr, schemaText, schemaTextLen, offset, NULL, NULL);
            if (t != orderly_tok_right_curly) {
                orderly_free_node(alloc, n);            
                s = orderly_parse_s_right_curly_expected;
            } else if ((named && (s = orderly_parse_property_name(alloc, schemaText, schemaTextLen,
                                                                  lxr, offset, *n))) || 
                       (s = orderly_parse_definition_suffix(alloc, schemaText, schemaTextLen,
                                                            lxr, offset, *n)))
            {
                orderly_free_node(alloc, n);                            
            }
        }
    }
    else
    {
        s = orderly_parse_s_expected_schema_entry; /* a.k.a. named_entry */
    }

    return s;
}

static orderly_parse_status
orderly_parse_named_entry(orderly_alloc_funcs * alloc,
                          const unsigned char * schemaText,
                          const unsigned int schemaTextLen,
                          orderly_lexer lxr,
                          unsigned int * offset,
                          orderly_node ** n)
{
    return orderly_parse_entry(alloc, schemaText, schemaTextLen, lxr,
                               offset, n, 1);
}


orderly_parse_status
orderly_parse(orderly_alloc_funcs * alloc,
              const unsigned char * schemaText,
              const unsigned int schemaTextLen,
              orderly_node ** n,
              unsigned int * final_offset)
{
    unsigned int offset = 0;
    orderly_parse_status s = orderly_parse_s_ok;
    orderly_lexer lxr;

    /* initialize the output offset (how much was successfully parsed?)
     * to zero */
    if (final_offset) *final_offset = 0;

    {
        static orderly_alloc_funcs orderlyAllocFuncBuffer;
        static orderly_alloc_funcs * orderlyAllocFuncBufferPtr = NULL;

        if (orderlyAllocFuncBufferPtr == NULL) {
            orderly_set_default_alloc_funcs(&orderlyAllocFuncBuffer);
            orderlyAllocFuncBufferPtr = &orderlyAllocFuncBuffer;
        }
        if (alloc == NULL) alloc = orderlyAllocFuncBufferPtr;
    }

    *n = NULL;
    
    lxr = orderly_lex_alloc(alloc);
    s = orderly_parse_named_entry(alloc, schemaText, schemaTextLen, lxr,
                                  &offset, n);

    /* if we've parsed ok so far, let's ensure we consumed the entire schema. */
    if (s == orderly_parse_s_ok) {
        orderly_tok t = orderly_lex_lex(lxr, schemaText, schemaTextLen, &offset,
                                        NULL, NULL);
        if (t == orderly_tok_semicolon) {
            t = orderly_lex_lex(lxr, schemaText, schemaTextLen, &offset, NULL, NULL);            
        }
        
        if (t != orderly_tok_eof) {
            s = orderly_parse_s_junk_at_end_of_input;
            if (*n) orderly_free_node(alloc, n);
        }
    }

    /* in the case our parse broke, we should now return offset information */
    if (final_offset) *final_offset = offset;

    orderly_lex_free(lxr);
    
    return s;
}
