/* beginnings of a PDF parser in hammer */

#include <hammer/hammer.h>
#include <hammer/glue.h>

/* convenience macros */
#define SEQ(...)	h_sequence(__VA_ARGS__, NULL)
#define CHX(...)	h_choice(__VA_ARGS__, NULL)
#define REP(P,N)	h_repeat_n(P, N)
#define IN(STR)		h_in(STR, sizeof(STR))
#define NOT_IN(STR)	h_not_in(STR, sizeof(STR))


/* a combinator to parse a given character but return a different value */

HParsedToken *
act_mapch(const HParseResult *p, void *u)
{
	return H_MAKE_UINT((uint8_t)u);
}

HParser *
mapch(uint8_t c, uint8_t v)
{
	return h_action(h_ch(c), act_mapch, (void *)(uintptr_t)v);
}

/*
 * semantic actions
 */

HParsedToken *
act_digit(const HParseResult *p, void *u)
{
	return H_MAKE_UINT(H_CAST_UINT(p->ast) - '0');
}
#define act_pdigit act_digit
#define act_odigit act_digit

HParsedToken *
act_hlower(const HParseResult *p, void *u)
{
	return H_MAKE_UINT(H_CAST_UINT(p->ast) - 'a');
}

HParsedToken *
act_hupper(const HParseResult *p, void *u)
{
	return H_MAKE_UINT(H_CAST_UINT(p->ast) - 'A');
}

HParsedToken *
act_nat(const HParseResult *p, void *u)
{
	uint64_t x = H_FIELD_UINT(0);
	HCountedArray *seq = H_FIELD_SEQ(1);

	for (size_t i = 0; i < seq->used; i++)
		x = x*10 + H_CAST_UINT(seq->elements[i]);
	
	return H_MAKE_UINT(x);
}
#define act_pnat act_nat

HParsedToken *
act_intg(const HParseResult *p, void *u)
{
	uint64_t x = 0;
	HCountedArray *seq = H_FIELD_SEQ(1);

	for (size_t i = 0; i < seq->used; i++)
		x = x*10 + H_CAST_UINT(seq->elements[i]);

	HParsedToken *sgn = H_INDEX_TOKEN(p->ast, 0);
	if (sgn->token_type == TT_BYTES &&
	    sgn->bytes.token[0] == '-')
		x = -x;
	
	return H_MAKE_UINT(x);
}

HParsedToken *
act_real(const HParseResult *p, void *u)
{
	double x = 0;
	double f = 0;
	HCountedArray *whole = H_FIELD_SEQ(1, 0);
	HCountedArray *fract = H_FIELD_SEQ(1, 2);

	for (size_t i = 0; i < whole->used; i++)
		x = x*10 + H_CAST_UINT(whole->elements[i]);
	for (size_t i = 0; i < fract->used; i++)
		f = (f + H_CAST_UINT(fract->elements[fract->used - 1 - i])) / 10;

	HParsedToken *sgn = H_INDEX_TOKEN(p->ast, 0);
	if (sgn->token_type == TT_BYTES &&
	    sgn->bytes.token[0] == '-')
		x = -x;
	
	return H_MAKE_UINT(x + f);	// XXX H_MAKE_DOUBLE (-> pprint)
}

HParsedToken *
act_token(const HParseResult *p, void *u)
{
	HCountedArray *seq = H_CAST_SEQ(p->ast);
	uint8_t *bytes;

	bytes = h_arena_malloc(p->arena, seq->used);
	for (size_t i = 0; i < seq->used; i++)
		bytes[i] = H_CAST_UINT(seq->elements[i]);

	return H_MAKE_BYTES(bytes, seq->used);
}
#define act_nstr act_token

HParsedToken *
act_nesc(const HParseResult *p, void *u)
{
	return H_MAKE_UINT(H_FIELD_UINT(1)*16 + H_FIELD_UINT(2));
}

#define act_schars h_act_flatten
#define act_litstr act_token

HParsedToken *
act_octal(const HParseResult *p, void *u)
{
	uint64_t x = 0;
	HCountedArray *seq = H_CAST_SEQ(p->ast);

	for (size_t i = 0; i < seq->used; i++)
		x = x*8 + H_CAST_UINT(seq->elements[i]);

	return H_MAKE_UINT(x);
}

HParser *
pdf_parser(void)
{
	static HParser *p = NULL;
	if(p) return p;

	/* lines */
	H_RULE(cr,	mapch('\r', '\n'));	/* semantic value: \n */
	H_RULE(lf,	h_ch('\n'));		/* semantic value: \n */
	H_RULE(crlf,	h_right(cr, lf));	/* semantic value: \n */
	H_RULE(eol,	CHX(crlf, cr, lf));
	H_RULE(line,	h_many(NOT_IN("\r\n")));

	/* character classes */
#define WCHARS "\0\t\n\f\r "
#define DCHARS "()<>[]{}/%"
	H_RULE(wchar,	IN(WCHARS));			/* white-space */
	//H_RULE(dchar,	IN(DCHARS));			/* delimiter */
	//H_RULE(rchar,	NOT_IN(WCHARS DCHARS));		/* regular */
	H_RULE(nchar,	NOT_IN(WCHARS DCHARS "#"));	/* name */
	H_RULE(schar,	NOT_IN("()\n\\"));		/* string literal */
	H_ARULE(digit,	h_ch_range('0', '9'));
	H_ARULE(pdigit,	h_ch_range('1', '9'));
	H_ARULE(hlower,	h_ch_range('a', 'f'));
	H_ARULE(hupper,	h_ch_range('A', 'F'));
	H_RULE(hdigit,	CHX(digit, hlower, hupper));
	H_ARULE(odigit,	h_ch_range('0', '7'));

	H_RULE(sign,	IN("+-"));
	H_RULE(period,	h_ch('.'));
	H_RULE(slash,	h_ch('/'));
	H_RULE(hash,	h_ch('#'));
	H_RULE(bslash,	h_ch('\\'));
	H_RULE(lparen,	h_ch('('));
	H_RULE(rparen,	h_ch(')'));

	/* whitespace */
	H_RULE(comment,	h_right(h_ch('%'), line));
	H_RULE(ws,	h_many(CHX(wchar, comment)));

#define TOK(X)	h_right(ws, X)
#define KW(S)	TOK(h_ignore(h_literal(S)))
// XXX this allows, for instance, "<<<<" to be parsed as "<< <<". ok?

	/* misc */
	H_RULE(epsilon,	h_epsilon_p());
	H_RULE(empty,	SEQ(epsilon));
	H_ARULE(nat,	TOK(SEQ(digit,  h_many(digit))));
	H_ARULE(pnat,	TOK(SEQ(pdigit, h_many(digit))));

	/*
	 * objects
	 */
	
	H_RULE(ref,	SEQ(pnat, nat, KW("R")));
	H_RULE(null,	KW("null"));
	H_RULE(boole,	CHX(KW("true"), KW("false")));

	/* numbers */
	H_RULE(digits,	h_many1(digit));
	H_ARULE(intg,	TOK(SEQ(h_optional(sign), digits)));
	H_RULE(realnn,	CHX(SEQ(digits, period, digits),	/* 12.3 */
			    SEQ(digits, period, empty),		/* 123. */
			    SEQ(empty, period, digits)));	/* .123 */
	H_ARULE(real,	TOK(SEQ(h_optional(sign), realnn)));

	/* names */
	H_ARULE(nesc,	SEQ(hash, hdigit, hdigit));
	H_ARULE(nstr,	h_many(CHX(nchar, nesc)));	/* '/' is valid */
	H_RULE(name,	TOK(h_right(slash, nstr)));

	/* strings */
	H_RULE(snest,	h_indirect());
	H_RULE(bsn,	mapch('n', 0x0a));	/* LF */
	H_RULE(bsr,	mapch('r', 0x0d));	/* CR */
	H_RULE(bst,	mapch('t', 0x09));	/* HT */
	H_RULE(bsb,	mapch('b', 0x08));	/* BS (backspace) */
	H_RULE(bsf,	mapch('f', 0x0c));	/* FF */
	H_RULE(escape,	CHX(bsn, bsr, bst, bsb, bsf, lparen, rparen, bslash));
	H_ARULE(octal,	CHX(REP(odigit,3), REP(odigit,2), REP(odigit,1)));
	H_RULE(wrap,	h_ignore(eol));
	H_RULE(sesc,	h_right(bslash, CHX(escape, octal, wrap, epsilon)));
						/* NB: lone '\' is ignored */
	H_ARULE(schars,	h_many(CHX(schar, snest, sesc, eol)));
	H_RULE(snest_,	SEQ(lparen, schars, rparen));
	H_ARULE(litstr,	TOK(h_middle(lparen, schars, rparen)));
	H_RULE(hexstr,	h_middle(KW("<"), h_many(TOK(hdigit)), KW(">")));
	H_RULE(string,	CHX(litstr, hexstr));
	h_bind_indirect(snest, snest_);

	/* arrays and dictionaries */
	H_RULE(obj,	h_indirect());
	H_RULE(k_v,	SEQ(name, obj));
	H_RULE(dict,	h_middle(KW("<<"), h_many(k_v), KW(">>")));
	H_RULE(array,	h_middle(KW("["), h_many(obj), KW("]")));

	/* streams */
	H_RULE(stream,	h_nothing_p());	// XXX

	H_RULE(obj_,	CHX(ref, null, boole, intg, real, name, string,
			    array, dict, stream));
	h_bind_indirect(obj, obj_);

	/*
	 * file structure
	 */

	H_RULE(version,	SEQ(pdigit, h_ignore(h_ch('.')), pdigit));
	H_RULE(header,	SEQ(h_literal("%PDF-"), version, eol));

	H_RULE(objdef,	SEQ(pnat, nat, KW("obj"), obj, KW("endobj")));
	H_RULE(body,	h_many(objdef));	// XXX object streams

	H_RULE(xrefs,	epsilon);

	H_RULE(trailer,	epsilon);

	H_RULE(end,	epsilon);	// XXX
	H_RULE(tail,	SEQ(body, xrefs, trailer));
	H_RULE(pdf,	SEQ(header, SEQ/*XXX h_many1*/(tail), end));

	return p = pdf;
}


#include <stdio.h>
#include <err.h>
#include <assert.h>
#include <fcntl.h>	/* open() */
#include <unistd.h>	/* lseek() */
#include <sys/mman.h>	/* mmap() */

int main(int argc, char *argv[])
{
	HParser *p;
	HParseResult *res;
	const char *infile = NULL;
	const uint8_t *input;
	size_t sz;
	int fd;

	/* command line handling */
	if (argc != 2) {
		fprintf(stderr, "usage: %s file\n", argv[0]);
		return 1;
	}
	infile = argv[1];

	/* mmap the input file */
	fd = open(infile, O_RDONLY);
	if (fd == -1)
		err(1, "%s", infile);
	sz = lseek(fd, 0, SEEK_END);
	if (sz == -1)
		err(1, "lseek");
	input = mmap(NULL, sz?sz:1, PROT_READ, MAP_PRIVATE, fd, 0);
	if (input == MAP_FAILED)
		err(1, "mmap");

	/* build and run parser */
	p = pdf_parser();
	assert(p != NULL);
	res = h_parse(p, input, sz);
	if (!res) {
		fprintf(stderr, "%s: no parse\n", infile);
		return 1;
	}

	/* print result */
	if (!res->ast)
		printf("null\n");
	else
		h_pprint(stdout, res->ast, 0, 2);

	return 0;
}
