/* beginnings of a PDF parser in hammer */

#include <string.h>	/* strncmp() */

#include <hammer/hammer.h>
#include <hammer/glue.h>

/* convenience macros */
#define SEQ(...)	h_sequence(__VA_ARGS__, NULL)
#define CHX(...)	h_choice(__VA_ARGS__, NULL)
#define REP(P,N)	h_repeat_n(P, N)
#define IGN(P)		h_ignore(P)
#define LIT(S)		h_ignore(h_literal(S))
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
	uint64_t x = 0;
	HCountedArray *seq = H_CAST_SEQ(p->ast);

	for (size_t i = 0; i < seq->used; i++)
		x = x*10 + H_CAST_UINT(seq->elements[i]);

	return H_MAKE_UINT(x);
}
#define act_xrnat act_nat
#define act_xroff act_nat
#define act_xrgen act_nat

HParsedToken *
act_pnat(const HParseResult *p, void *u)
{
	uint64_t x = H_FIELD_UINT(0);
	HCountedArray *seq = H_FIELD_SEQ(1);

	for (size_t i = 0; i < seq->used; i++)
		x = x*10 + H_CAST_UINT(seq->elements[i]);
	
	return H_MAKE_UINT(x);
}

HParsedToken *
act_intg(const HParseResult *p, void *u)
{
	int64_t x = 0;
	HCountedArray *seq = H_FIELD_SEQ(1);

	for (size_t i = 0; i < seq->used; i++)
		x = x*10 + H_CAST_UINT(seq->elements[i]);

	HParsedToken *sgn = H_INDEX_TOKEN(p->ast, 0);
	if (sgn->token_type == TT_BYTES &&
	    sgn->bytes.token[0] == '-')
		x = -x;
	
	return H_MAKE_SINT(x);
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

#define act_stream act_token
#define act_xrefs h_act_last


/*
 * input grammar
 */

/* continuation for h_bind() */
HParser *kstream(HAllocator *, const HParsedToken *, void *);

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
	H_RULE(nl,	IGN(eol));
	H_RULE(line,	h_many(NOT_IN("\r\n")));

	/* character classes */
#define LWCHARS	"\0\t\f "
#define WCHARS	LWCHARS "\n\r"
#define DCHARS	"()<>[]{}/%"
	H_RULE(wchar,	IN(WCHARS));			/* white-space */
	H_RULE(lwchar,	IN(LWCHARS));			/* "line" whitespace */
	//H_RULE(dchar,	IN(DCHARS));			/* delimiter */
	H_RULE(rchar,	NOT_IN(WCHARS DCHARS));		/* regular */
	H_RULE(nchar,	NOT_IN(WCHARS DCHARS "#"));	/* name */
	H_RULE(schar,	NOT_IN("()\n\\"));		/* string literal */
	H_ARULE(digit,	h_ch_range('0', '9'));
	H_ARULE(pdigit,	h_ch_range('1', '9'));
	H_ARULE(hlower,	h_ch_range('a', 'f'));
	H_ARULE(hupper,	h_ch_range('A', 'F'));
	H_RULE(hdigit,	CHX(digit, hlower, hupper));
	H_ARULE(odigit,	h_ch_range('0', '7'));
	H_RULE(sign,	IN("+-"));

	H_RULE(sp,	h_ch(' '));
	H_RULE(percent,	h_ch('%'));
	H_RULE(period,	h_ch('.'));
	H_RULE(slash,	h_ch('/'));
	H_RULE(hash,	h_ch('#'));
	H_RULE(bslash,	h_ch('\\'));
	H_RULE(lparen,	h_ch('('));
	H_RULE(rparen,	h_ch(')'));
	H_RULE(langle,	h_ch('<'));
	H_RULE(rangle,	h_ch('>'));
	H_RULE(lbrack,	h_ch('['));
	H_RULE(rbrack,	h_ch(']'));

	/* whitespace */
	H_RULE(comment,	SEQ(percent, line));
	H_RULE(ws,	IGN(h_many(CHX(wchar, comment))));
	H_RULE(lws,	IGN(h_many(lwchar)));

#define TOK(X)	h_middle(ws, X, h_not(rchar))
#define KW(S)	TOK(LIT(S))
#define TOKD(X)	h_right(ws, X)	/* for tokens that end on delimiters */
// XXX this allows, for instance, "<<<<" to be parsed as "<< <<". ok?

	/* misc */
	H_RULE(end,	h_end_p());
	H_RULE(epsilon,	h_epsilon_p());
	H_RULE(empty,	SEQ(epsilon));
	H_ARULE(nat,	TOK(h_many1(digit)));
	H_ARULE(pnat,	TOK(SEQ(pdigit, h_many(digit))));

#define OPT(X)	CHX(X, epsilon)

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
	H_RULE(sesc,	h_right(bslash, CHX(escape, octal, nl, epsilon)));
		/* NB: lone backslashes and escaped newlines are ignored */
	H_ARULE(schars,	h_many(CHX(schar, snest, sesc, eol)));
	H_RULE(snest_,	SEQ(lparen, schars, rparen));
	H_ARULE(litstr,	h_middle(TOKD(lparen), schars, rparen));
	H_RULE(hexchr,	h_right(ws, hdigit));
	H_RULE(hexstr,	h_middle(TOKD(langle), h_many(hexchr), TOKD(rangle)));
	H_RULE(string,	CHX(litstr, hexstr));
	h_bind_indirect(snest, snest_);

	/* arrays and dictionaries */
	H_RULE(dopen,	LIT("<<"));
	H_RULE(dclose,	LIT(">>"));
	H_RULE(obj,	h_indirect());
	H_RULE(k_v,	SEQ(name, obj));
	H_RULE(dict,	h_middle(TOKD(dopen), h_many(k_v), TOKD(dclose)));
	H_RULE(array,	h_middle(TOKD(lbrack), h_many(obj), TOKD(rbrack)));
		// XXX validate: dict keys must be unique

	/* streams */
	H_RULE(stmbeg,	SEQ(dict, KW("stream"), OPT(cr), lf));
	H_RULE(stmend,	SEQ(OPT(eol), LIT("endstream")));
	H_ARULE(stream,	h_left(h_bind(stmbeg, kstream, NULL), stmend));
		// XXX is whitespace allowed between the eol and "endstream"?

	H_RULE(obj_,	CHX(ref, null, boole, real, intg, name, string,
			    array, dict));
	h_bind_indirect(obj, obj_);

	/*
	 * file structure
	 */

	/* header */
	H_RULE(version,	SEQ(pdigit, IGN(period), pdigit));
	H_RULE(header,	h_middle(LIT("%PDF-"), version, eol));

	/* body */
	H_RULE(indobj,	CHX(stream, obj));
	H_RULE(objdef,	SEQ(pnat, nat, KW("obj"), indobj, KW("endobj")));
	H_RULE(body,	h_many(objdef));	// XXX object streams

	/* cross-reference table */
	H_RULE(xreol,	CHX(SEQ(sp, cr), SEQ(sp, lf), crlf));
		// ^ XXX does the real world follow this rule?! cf. loop.pdf
	H_RULE(xrtyp,	CHX(h_ch('n'), h_ch('f')));
	H_ARULE(xroff,	REP(digit, 10));
	H_ARULE(xrgen,	REP(digit, 5));
	H_RULE(xrent,	SEQ(xroff, IGN(sp), xrgen, IGN(sp), xrtyp, IGN(xreol)));
	H_ARULE(xrnat,	h_many1(digit));
	H_RULE(xrhead,	SEQ(xrnat, IGN(sp), xrnat, nl));
	H_RULE(xrsub,	SEQ(xrhead, h_many(xrent)));
	H_ARULE(xrefs,	SEQ(KW("xref"), eol, h_many(xrsub)));
		// XXX whitespace allowed between "xref" and eol?
		// XXX cross-reference streams

	/* trailer */
	H_RULE(tdict,	SEQ(KW("trailer"), dict, lws, nl));
	H_RULE(startxr,	SEQ(KW("startxref"), lws, nl,	// XXX KW() ok? lws ok?
			    lws, xrnat, lws, nl));	// XXX trailing lws ok?
		// NB: lws before xref offset is allowed, cf. p.48 (example 4)
	H_RULE(eofmark,	SEQ(LIT("%%EOF"), CHX(eol, end)));
		// XXX should lws be allowed around EOF marker?
	H_RULE(txrefs,	SEQ(xrefs, tdict));
	H_RULE(trailer,	SEQ(h_optional(txrefs), startxr, eofmark));

	H_RULE(tail,	SEQ(body, trailer));
	//H_RULE(pdf,	SEQ(header, h_many(tail), OPT(body), epsilon));	// XXX debug
	H_RULE(pdf,	SEQ(header, h_many1(tail), end));

	return p = pdf;
}

/*
 * This continuation takes the stream dictionary (as first element of x) and
 * should return a parser that consumes exactly the bytes that make up the
 * stream data.
 */
HParser *
kstream(HAllocator *mm__, const HParsedToken *x, void *env)
{
	HCountedArray *dict = H_INDEX_SEQ(x, 0);
	HParsedToken *ent, *v = NULL;
	HBytes k;
	size_t sz;

	/* look for the Length entry */
	for (size_t i = 0; i < dict->used; i++) {
		ent = dict->elements[i];
		k = H_INDEX_BYTES(ent, 0);

		if (strncmp("Length", k.token, k.len) == 0) {	// XXX strncasecmp?
			v = H_INDEX_TOKEN(ent, 1);
			break;
		}
	}
	if (v == NULL || v->token_type != TT_SINT || v->sint < 0)
		goto fail;
	sz = (size_t)v->sint;
		// XXX support indirect objects for the Length value!

	return h_repeat_n__m(mm__, h_uint8__m(mm__), sz);
fail:
	if (v == NULL)
		fprintf(stderr, "stream /Length missing\n");
	else if (v -> token_type != TT_SINT)
		fprintf(stderr, "stream /Length not an integer\n");
	else if (v < 0)
		fprintf(stderr, "stream /Length negative\n");
	h_pprint(stderr, x, 0, 2);	// XXX debug
	return h_nothing_p__m(mm__);
}


/*
 * minimal main program
 */

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
