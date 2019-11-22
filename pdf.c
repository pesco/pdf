/* beginnings of a PDF parser in hammer */

#include <hammer/hammer.h>
#include <hammer/glue.h>

/* convenience macros */
#define SEQ(...)	h_sequence(__VA_ARGS__, NULL)
#define CHX(...)	h_choice(__VA_ARGS__, NULL)
#define IN(STR)		h_in(STR, sizeof(STR))
#define NOT_IN(STR)	h_not_in(STR, sizeof(STR))

#include <assert.h>

HParsedToken *
act_digit(const HParseResult *p, void *u)
{
	return H_MAKE_UINT(H_CAST_UINT(p->ast) - '0');
}
#define act_pdigit act_digit

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


HParser *
pdf_parser(void)
{
	static HParser *p = NULL;
	if(p) return p;

	/* lines */
	H_RULE(crlf,	h_literal("\r\n"));
	H_RULE(cr,	h_ch('\r'));
	H_RULE(lf,	h_ch('\n'));
	H_RULE(eol,	CHX(crlf, cr, lf));
	H_RULE(line,	h_many(h_not_in("\r\n", 2)));

	/* character classes */
#define WCHARS "\0\t\n\f\r "
#define DCHARS "()<>[]{}/%"
	H_RULE(wchar,	IN(WCHARS));			/* white-space */
	//H_RULE(dchar,	IN(DCHARS));			/* delimiter */
	//H_RULE(rchar,	NOT_IN(WCHARS DCHARS));		/* regular */
	H_RULE(nchar,	NOT_IN(WCHARS DCHARS "#"));	/* name */
	H_ARULE(digit,	h_ch_range('0', '9'));
	H_ARULE(pdigit,	h_ch_range('1', '9'));
	H_ARULE(hlower,	h_ch_range('a', 'f'));
	H_ARULE(hupper,	h_ch_range('A', 'F'));
	H_RULE(hdigit,	CHX(digit, hlower, hupper));

	/* whitespace */
	H_RULE(comment,	h_right(h_ch('%'), line));
	H_RULE(ws,	h_many(CHX(wchar, comment)));

#define TOK(X)	h_right(ws, X)
#define KW(S)	TOK(h_ignore(h_literal(S)))

	/* misc */
	H_ARULE(nat,	TOK(SEQ(digit,  h_many(digit))));
	H_ARULE(pnat,	TOK(SEQ(pdigit, h_many(digit))));

	/*
	 * objects
	 */
	
	H_RULE(ref,	SEQ(pnat, nat, KW("R")));
	H_RULE(null,	KW("null"));
	H_RULE(boole,	CHX(KW("true"), KW("false")));

	/* numbers */
	H_RULE(sign,	h_in("+-", 2));
	H_RULE(period,	h_ch('.'));
	H_RULE(digits,	h_many1(digit));
	H_ARULE(intg,	TOK(SEQ(h_optional(sign), digits)));
	H_RULE(empty,	SEQ(h_epsilon_p()));
	H_RULE(realnn,	CHX(SEQ(digits, period, digits),	/* 12.3 */
			    SEQ(digits, period, empty),		/* 123. */
			    SEQ(empty, period, digits)));	/* .123 */
	H_ARULE(real,	TOK(SEQ(h_optional(sign), realnn)));

	/* names */
	H_RULE(slash,	h_ch('/'));
	H_RULE(hash,	h_ch('#'));
	H_ARULE(nesc,	SEQ(hash, hdigit, hdigit));
	H_ARULE(nstr,	h_many(CHX(nchar, nesc)));	/* '/' is valid */
	H_RULE(name,	TOK(h_right(slash, nstr)));

	/* strings */
	H_RULE(string,	h_nothing_p());	// XXX

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

	H_RULE(xrefs,	h_epsilon_p());

	H_RULE(trailer,	h_epsilon_p());

	H_RULE(end,	h_epsilon_p());	// XXX
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
