/* beginnings of a PDF parser in hammer
 * pesco 2019
 */

#include <string.h>	/* strncmp() */

#include <hammer/hammer.h>
#include <hammer/glue.h>

/* convenience macros */
#define SEQ(...)	h_sequence(__VA_ARGS__, NULL)
#define CHX(...)	h_choice(__VA_ARGS__, NULL)
#define REP(P,N)	h_repeat_n(P, N)
#define IGN(P)		h_ignore(P)
#define LIT(S)		h_ignore(h_literal(S))
#define IN(STR)		h_in(STR, sizeof(STR) - 1)
#define NOT_IN(STR)	h_not_in(STR, sizeof STR - 1)


/*
 * some helpers
 */

/* a combinator to parse a given character but return a different value */

HParsedToken *
act_mapch(const HParseResult *p, void *u)
{
	return H_MAKE_UINT((uint8_t)u);
}

HParser *
p_mapch(uint8_t c, uint8_t v)
{
	return h_action(h_ch(c), act_mapch, (void *)(uintptr_t)v);
}

/* a parser that just returns a given token */

HParsedToken *
act_return(const HParseResult *p, void *u)
{
	return u;
}

HParser *
p_return__m(HAllocator *mm__, const HParsedToken *tok)
{
	HParser *eps  = h_epsilon_p__m(mm__);

	return h_action__m(mm__, eps, act_return, (void *)tok);
}

/* a helper to look up a value in a dictionary */
const HParsedToken *
dictentry(const HCountedArray *dict, const char *key)
{
	HParsedToken *ent;
	HBytes k;
	size_t len;

	len = strlen(key);
	for (size_t i = 0; i < dict->used; i++) {
		ent = dict->elements[i];
		k = H_INDEX_BYTES(ent, 0);

		if (k.len == len && strncmp(key, k.token, k.len) == 0)
			return H_INDEX_TOKEN(ent, 1);
	}

	return NULL;
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
#define act_string act_token

HParsedToken *
act_octal(const HParseResult *p, void *u)
{
	uint64_t x = 0;
	HCountedArray *seq = H_CAST_SEQ(p->ast);

	for (size_t i = 0; i < seq->used; i++)
		x = x*8 + H_CAST_UINT(seq->elements[i]);

	return H_MAKE_UINT(x);
}

#define act_xrefs h_act_last

/* stream semantics (defined further below) */
bool validate_xrstm(HParseResult *, void *);
HParsedToken *act_xrstm(const HParseResult *, void *);


/*
 * input grammar
 */

HParser *p_pdf;
HParser *p_pdfdbg;
HParser *p_startxref;
HParser *p_xref;

/* continuation for h_bind() */
HParser *kstream(HAllocator *, const HParsedToken *, void *);

void
init_parser(const char *input)
{
	/* lines */
	H_RULE(cr,	p_mapch('\r', '\n'));	/* semantic value: \n */
	H_RULE(lf,	h_ch('\n'));		/* semantic value: \n */
	H_RULE(crlf,	h_right(cr, lf));	/* semantic value: \n */
	H_RULE(eol,	CHX(crlf, cr, lf));
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
	H_RULE(schar,	NOT_IN("()\n\r\\"));		/* string literal */
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
	H_RULE(nl,	IGN(h_right(lws, eol)));
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
	H_RULE(bsn,	p_mapch('n', 0x0a));	/* LF */
	H_RULE(bsr,	p_mapch('r', 0x0d));	/* CR */
	H_RULE(bst,	p_mapch('t', 0x09));	/* HT */
	H_RULE(bsb,	p_mapch('b', 0x08));	/* BS (backspace) */
	H_RULE(bsf,	p_mapch('f', 0x0c));	/* FF */
	H_RULE(escape,	CHX(bsn, bsr, bst, bsb, bsf, lparen, rparen, bslash));
	H_ARULE(octal,	CHX(REP(odigit,3), REP(odigit,2), REP(odigit,1)));
	H_RULE(wrap,	IGN(eol));
	H_RULE(sesc,	h_right(bslash, CHX(escape, octal, wrap, epsilon)));
		/* NB: lone backslashes and escaped newlines are ignored */
	H_ARULE(schars,	h_many(CHX(schar, snest, sesc, eol)));
	H_RULE(snest_,	SEQ(lparen, schars, rparen));
	H_RULE(litstr,	h_middle(TOKD(lparen), schars, rparen));
	H_RULE(hexchr,	h_right(ws, hdigit));
	H_RULE(hexstr,	h_middle(TOKD(langle), h_many(hexchr), TOKD(rangle)));
	H_ARULE(string,	CHX(litstr, hexstr));
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
	H_RULE(stream,	h_left(h_bind(stmbeg, kstream, (void *)input), stmend));
		// XXX is whitespace allowed between the eol and "endstream"?

	H_RULE(obj_,	CHX(ref, null, boole, real, intg, name, string,
			    array, dict));
	h_bind_indirect(obj, obj_);

	/*
	 * file structure
	 */

	/* header */
	H_RULE(version,	SEQ(pdigit, IGN(period), pdigit));
	H_RULE(header,	h_middle(LIT("%PDF-"), version, nl));

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
	H_ARULE(xrefs,	SEQ(KW("xref"), nl, h_many(xrsub)));
	H_AVRULE(xrstm,	SEQ(pnat, nat, KW("obj"), stream, KW("endobj")));

	/* trailer */
	H_RULE(startxr,	SEQ(nl, KW("startxref"), nl,
			    lws, xrnat, nl,
			    LIT("%%EOF"), CHX(nl, end)));
		// XXX should lws be allowed before EOF marker?
		// NB: lws before xref offset is allowed, cf. p.48 (example 4)
	H_RULE(xr_td,	CHX(SEQ(xrefs, KW("trailer"), dict), xrstm));

	H_RULE(tail,	SEQ(body, h_optional(xr_td), startxr));
	H_RULE(pdf,	SEQ(header, h_many1(tail), end));

	/* debug parser to consume as much as possible */
	H_RULE(pdfdbg,	SEQ(header, h_many(tail), body, OPT(xr_td), OPT(startxr)));

	p_pdf = pdf;
	p_pdfdbg = pdfdbg;
	p_startxref = startxr;
	p_xref = xr_td;
}

/* combine current position combined with env=(input,sz) into HBytes */
HParsedToken *
act_ks_bytes(const HParseResult *p, void *env)
{
	const HBytes *bs = env;
	size_t offset = H_CAST_UINT(p->ast) / 8;

	/*
	 * NB: we must allocate a new HBytes struct here because the old one is
	 * allocated only temporarily for the lifetime of the continuation
	 * below.
	 */
	return H_MAKE_BYTES(bs->token + offset, bs->len);
}

/*
 * This continuation takes the stream dictionary (as first element of x) and
 * should return a parser that consumes exactly the bytes that make up the
 * stream data.
 */
HParser *
kstream(HAllocator *mm__, const HParsedToken *x, void *env)
{
	const char *input = env;
	const HParsedToken *dict_t = H_INDEX_TOKEN(x, 0);
	const HCountedArray *dict = H_CAST_SEQ(dict_t);
	const HParsedToken *v = NULL;
	size_t sz;

	/* look for the Length entry */
	v = dictentry(dict, "Length");
	if (v == NULL || v->token_type != TT_SINT || v->sint < 0)
		goto fail;
	sz = (size_t)v->sint;
		// XXX support indirect objects for the Length value!

	//fprintf(stderr, "parsing stream object, length %zu.\n", sz);	// XXX debug

	/* dummy struct to hold the pair (input,sz) */
	HBytes *bytes = h_alloc(mm__, sizeof(HBytes));
	bytes->token = input;
	bytes->len = sz;

	HParser *tell = h_tell__m(mm__);
	HParser *skip = h_skip__m(mm__, sz * 8);

	HParser *bytes_p = h_action__m(mm__, tell, act_ks_bytes, bytes);
	HParser *dict_p  = p_return__m(mm__, dict_t);
	return h_sequence__m(mm__, dict_p, bytes_p, skip, NULL);
fail:
#if 0
	if (v == NULL)
		fprintf(stderr, "stream /Length missing\n");
	else if (v -> token_type != TT_SINT)
		fprintf(stderr, "stream /Length not an integer\n");
	else if (v < 0)
		fprintf(stderr, "stream /Length negative\n");
#endif
	//h_pprintln(stderr, p);	// XXX debug
	return h_nothing_p__m(mm__);
}

/*
 * validate the /Type field on a cross-reference stream.
 *
 * p = pnat nat (dict offs offs)
 */
bool
validate_xrstm(HParseResult *p, void *u)
{
	const HCountedArray *tdict = H_FIELD_SEQ(2, 0);
	const HParsedToken *v = dictentry(tdict, "Type");

#if 0
	if (v == NULL)
		fprintf(stderr, "stream dict has not /Type\n");
	else if (v->token_type != TT_BYTES)
		fprintf(stderr, "stream /Type is no name object\n");
	else if (v->bytes.len == 4 && strncmp("XRef", v->bytes.token, v->bytes.len) == 0)
		return true;
	return false;
#endif

	return (v != NULL && v->token_type == TT_BYTES && v->bytes.len == 4 &&
	    strncmp("XRef", v->bytes.token, v->bytes.len) == 0);
}

/*
 * interpret a cross-reference stream and return it in the same form as other
 * cross-reference sections:
 *
 * p = pnat nat (dict bytes)
 * result = (xrefs dict)
 */
HParsedToken *
act_xrstm(const HParseResult *p, void *u)
{
	HParsedToken *bytes, *dict, *result;

	dict = H_INDEX_TOKEN(p->ast, 2, 0);
	bytes = H_INDEX_TOKEN(p->ast, 2, 1);

	// XXX decode XRefStm

	result = H_MAKE_SEQN(2);
	result->seq->elements[0] = bytes;
	result->seq->elements[1] = dict;
	result->seq->used = 2;
	return result;
}

/*
 * main program
 */

#include <stdio.h>
#include <inttypes.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>	/* realloc() */
#include <fcntl.h>	/* open() */
#include <unistd.h>	/* lseek() */
#include <sys/mman.h>	/* mmap() */

const char *infile = NULL;

/*
 * This helper implements the standard backwards parsing strategy to read all
 * cross-reference sections and trailer dictionaries, starting from the
 * 'startxref' offset found at the very end of the input.
 *
 * Allocates and returns an array of HParsedTokens, each containing the result
 * of a successful 'p_xref' parse. Sets the output parameter 'nxrefs' to the
 * number of elements.
 *
 * A return value of NULL indicates an empty result.
 */
const HParsedToken **
parse_xrefs(const char *input, size_t sz, size_t *nxrefs)
{
	HParseResult *res = NULL;
	const HParsedToken **xrefs = NULL;	/* empty result */
	const HParsedToken *tok = NULL;
	size_t n = 0;
	size_t offset = 0;

	// XXX try formulating this as a parser using h_seek()

	/* search for the "startxref" section from the back of the file */
	HParser *p = h_left(p_startxref, h_end_p());
	for (size_t i = 0; i < sz; i++) {
		res = h_parse(p, input + sz - i, i);
		if (res != NULL)
			break;
	}
	if (res == NULL) {
		fprintf(stderr, "%s: startxref not found\n", infile);
		goto end;
	}
	offset = H_INDEX_UINT(res->ast, 0);

	for (;;) {
		res = h_parse(p_xref, input + offset, sz - offset);
		if (res == NULL) {
			fprintf(stderr, "%s: error parsing xref section at "
			    "position %zu (0x%zx)\n", infile, offset, offset);
			break;
		}

		/* save this section in xrefs */
		if (n >= SIZE_MAX / sizeof(HParsedToken *))
			errc(1, EOVERFLOW, "overflow");
		xrefs = realloc(xrefs, (n + 1) * sizeof(HParsedToken *));
		if (xrefs == NULL)
			err(1, "realloc");
		xrefs[n++] = res->ast;

		/* look up the next offset (to the previous xref section) */
		tok = dictentry(H_INDEX_SEQ(res->ast, 1), "Prev");
		if (tok == NULL)
			break;
		if (tok->token_type != TT_SINT) {
			fprintf(stderr, "%s: /Prev not an integer\n", infile);
			break;
		}

		/*
		 * validate the new offset. we don't want to get caught in a
		 * loop. the offsets should strictly decrease, unless the file
		 * is a "linearized" PDF. in that case there should be exactly
		 * two xref sections in the reverse order, so we allow the
		 * first section to point forward.
		 */
		if (n > 1 && tok->sint >= offset) {
			fprintf(stderr, "%s: /Prev pointer of xref section at "
			    "%zu (0x%zx) points forward\n", infile, offset,
			    offset);
			break;
		}

		offset = (size_t)tok->sint;
	}
	// XXX debug
	//fprintf(stderr, "%s: %zu xref sections parsed\n", infile, n);
	//for (size_t i = 0; i < n; i++)
	//	h_pprintln(stderr, xrefs[i]);

end:
	*nxrefs = n;
	return xrefs;
}

int
main(int argc, char *argv[])
{
	HParseResult *res = NULL;
	const HParsedToken **xrefs;
	const uint8_t *input;
	size_t sz, nxrefs;
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

	/* build parsers */
	init_parser(input);

	/* parse all cross-reference sections and trailer dictionaries */
	xrefs = parse_xrefs(input, sz, &nxrefs);

	/* run the main parser */
	res = h_parse(p_pdf, input, sz);
	if (!res) {
		fprintf(stderr, "%s: no parse\n", infile);

		/* help us find the error */
		res = h_parse(p_pdfdbg, input, sz);
		if (res) {
			int64_t pos = res->bit_length / 8;
			fprintf(stderr, "%s: error after position"
			    " %" PRId64 " (0x%" PRIx64 ")\n",
			    infile, pos, pos);
			//h_pprintln(stderr, res->ast);	// XXX debug
		}

		return 1;
	}

	/* print result */
	h_pprintln(stdout, res->ast);

	return 0;
}
