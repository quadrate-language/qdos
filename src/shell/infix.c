/**
 * @file infix.c
 * @brief A Y= body written as a TI takes it, turned into the RPN Quadrate runs
 */

#include "infix.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What a word takes off the stack and leaves on it */
typedef struct {
	const char* name;
	int takes;
	int leaves;
} word_effect;

static const word_effect EFFECTS[] = {
		{"x", 0, 1},
		{"y", 0, 1},
		{"t", 0, 1},
		{"theta", 0, 1},
		{"pi", 0, 1},
		{"e", 0, 1},
		{"i", 0, 1},
		{"sin", 1, 1},
		{"cos", 1, 1},
		{"tan", 1, 1},
		{"asin", 1, 1},
		{"acos", 1, 1},
		{"atan", 1, 1},
		{"sinh", 1, 1},
		{"cosh", 1, 1},
		{"tanh", 1, 1},
		{"asinh", 1, 1},
		{"acosh", 1, 1},
		{"atanh", 1, 1},
		{"ln", 1, 1},
		{"log", 1, 1},
		{"log10", 1, 1},
		{"log2", 1, 1},
		{"exp", 1, 1},
		{"exp2", 1, 1},
		{"alog", 1, 1},
		{"sqrt", 1, 1},
		{"cbrt", 1, 1},
		{"sq", 1, 1},
		{"cb", 1, 1},
		{"abs", 1, 1},
		{"inv", 1, 1},
		{"neg", 1, 1},
		{"fac", 1, 1},
		{"floor", 1, 1},
		{"ceil", 1, 1},
		{"round", 1, 1},
		{"trunc", 1, 1},
		{"real", 1, 1},
		{"imag", 1, 1},
		{"conj", 1, 1},
		{"angle", 1, 1},
		{"+", 2, 1},
		{"-", 2, 1},
		{"*", 2, 1},
		{"/", 2, 1},
		{"%", 2, 1},
		{"plus", 2, 1},
		{"minus", 2, 1},
		{"times", 2, 1},
		{"divide", 2, 1},
		{"pow", 2, 1},
		{"mod", 2, 1},
		{"modulo", 2, 1},
		{"fmod", 2, 1},
		{"min", 2, 1},
		{"max", 2, 1},
		{"fmin", 2, 1},
		{"fmax", 2, 1},
		{"hypot", 2, 1},
		{"atan2", 2, 1},
		{"dup", 1, 2},
		{"drop", 1, 0},
		{"swap", 2, 2},
		{"over", 2, 3},
		{"rot", 3, 3},
		{"nip", 2, 1},
		{"dup2", 2, 4},
		{"percent", 2, 2},
		{"lastx", 0, 1},
		{"print", 1, 0},
		{"nl", 0, 0},
		{"within", 3, 1},
		{"inc", 1, 1},
		{"dec", 1, 1},
		{"<", 2, 1},
		{">", 2, 1},
		{"<=", 2, 1},
		{">=", 2, 1},
		{"==", 2, 1},
		{"!=", 2, 1},
		{"and", 2, 1},
		{"or", 2, 1},
		{"not", 1, 1},
};

static const word_effect* effect_of(const char* name, size_t len) {
	for (size_t i = 0; i < sizeof(EFFECTS) / sizeof(*EFFECTS); i++) {
		if (strlen(EFFECTS[i].name) == len && strncmp(EFFECTS[i].name, name, len) == 0) {
			return &EFFECTS[i];
		}
	}
	return NULL;
}

bool qdos_word_effect(const char* name, int* takes, int* leaves) {
	const word_effect* effect = effect_of(name, strlen(name));
	if (effect == NULL) {
		return false;
	}
	*takes = effect->takes;
	*leaves = effect->leaves;
	return true;
}

static bool name_start(char ch) {
	return isalpha((unsigned char)ch) || ch == '_';
}

static bool name_char(char ch) {
	return isalnum((unsigned char)ch) || ch == '_' || ch == ':';
}

/* A number the way Quadrate reads one, the whole of the token */
static bool is_number(const char* token, size_t len) {
	size_t i = (token[0] == '-') ? 1 : 0;
	if (i >= len || !(isdigit((unsigned char)token[i]) || token[i] == '.')) {
		return false;
	}
	char buf[64];
	if (len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, token, len);
	buf[len] = '\0';
	char* end;
	strtod(buf, &end);
	return *end == '\0';
}

int qdos_rpn_results(const char* body) {
	int depth = 0;
	for (const char* p = body; *p;) {
		while (*p && isspace((unsigned char)*p)) {
			p++;
		}
		const char* token = p;
		while (*p && !isspace((unsigned char)*p)) {
			p++;
		}
		const size_t len = (size_t)(p - token);
		if (len == 0) {
			break;
		}

		if (is_number(token, len)) {
			depth++;
			continue;
		}
		const word_effect* effect = effect_of(token, len);
		if (effect != NULL) {
			if (depth < effect->takes) {
				return QDOS_RPN_NOT;
			}
			depth += effect->leaves - effect->takes;
			continue;
		}

		// A name is a word of the program's, which could do anything
		bool name = name_start(token[0]);
		for (size_t i = 1; name && i < len; i++) {
			name = name_char(token[i]);
		}
		if (name) {
			return QDOS_RPN_UNKNOWN;
		}

		// What only a formula has: brackets, ^, a comma, an operator glued on, x+2, or 2x
		if (len == 2 && (strncmp(token, "->", 2) == 0 || strncmp(token, "--", 2) == 0)) {
			return QDOS_RPN_UNKNOWN; // a local being named
		}
		for (size_t i = 0; i < len; i++) {
			if (strchr("()^,+-*/", token[i]) != NULL) {
				return QDOS_RPN_NOT;
			}
		}
		if (isdigit((unsigned char)token[0]) || token[0] == '.' || token[0] == '-' || token[0] == '+') {
			return QDOS_RPN_NOT;
		}

		// The rest is Quadrate's own syntax, braces and arrows
		return QDOS_RPN_UNKNOWN;
	}
	return depth;
}

/* ---------------------------------------------------------------------------
 * The formula, read by recursive descent and written out as it is read
 * ------------------------------------------------------------------------- */

typedef enum {
	TOK_END,
	TOK_NUMBER,
	TOK_NAME,
	TOK_OP ///< One of + - * / ^ ( ) , !
} tok_kind;

typedef struct {
	const char* p; ///< Where the next token starts
	tok_kind kind;
	const char* text;
	size_t len;
	char op;
	char last_op; ///< The operator before this token, to say what went without a value

	char* out;
	size_t cap;
	size_t used;
	char* error;
	size_t error_cap;
	bool failed;
	int depth; ///< Brackets deep, so a runaway formula cannot run the C stack out
} parser;

#define INFIX_DEPTH_MAX 32

static bool fail(parser* ps, const char* why) {
	if (!ps->failed) {
		snprintf(ps->error, ps->error_cap, "%s", why);
		ps->failed = true;
	}
	return false;
}

static bool emit(parser* ps, const char* text, size_t len) {
	if (ps->used + len + 2 > ps->cap) {
		return fail(ps, "TOO LONG");
	}
	if (ps->used > 0) {
		ps->out[ps->used++] = ' ';
	}
	memcpy(ps->out + ps->used, text, len);
	ps->used += len;
	ps->out[ps->used] = '\0';
	return true;
}

static bool emit_word(parser* ps, const char* word) {
	return emit(ps, word, strlen(word));
}

/* The keypad's words for the operators, when they stand between two values */
static char word_operator(const char* text, size_t len) {
	static const struct {
		const char* word;
		char op;
	} WORDS[] = {{"plus", '+'}, {"minus", '-'}, {"times", '*'}, {"divide", '/'}, {"pow", '^'}};

	for (size_t i = 0; i < sizeof(WORDS) / sizeof(*WORDS); i++) {
		if (strlen(WORDS[i].word) == len && strncmp(WORDS[i].word, text, len) == 0) {
			return WORDS[i].op;
		}
	}
	return '\0';
}

static const char* skip_space(const char* p) {
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	return p;
}

static void next(parser* ps) {
	ps->last_op = (ps->kind == TOK_OP) ? ps->op : '\0';
	const char* p = skip_space(ps->p);
	ps->text = p;

	if (*p == '\0') {
		ps->kind = TOK_END;
		ps->len = 0;
		ps->p = p;
		return;
	}

	if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
		while (isdigit((unsigned char)*p)) {
			p++;
		}
		if (*p == '.') {
			p++;
			while (isdigit((unsigned char)*p)) {
				p++;
			}
		}
		// An exponent only where digits follow: 2e is two e's
		if ((*p == 'e' || *p == 'E') &&
				(isdigit((unsigned char)p[1]) || ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
			p += 2;
			while (isdigit((unsigned char)*p)) {
				p++;
			}
		}
		ps->kind = TOK_NUMBER;
		ps->len = (size_t)(p - ps->text);
		ps->p = p;
		return;
	}

	if (name_start(*p)) {
		while (name_char(*p)) {
			p++;
		}
		ps->len = (size_t)(p - ps->text);
		ps->p = p;
		// pow(x, 2) is a call; x pow 2 is the operator
		ps->op = (*skip_space(p) == '(') ? '\0' : word_operator(ps->text, ps->len);
		ps->kind = ps->op ? TOK_OP : TOK_NAME;
		return;
	}

	if (strchr("+-*/^(),!", *p) != NULL) {
		ps->kind = TOK_OP;
		ps->op = *p;
		ps->len = 1;
		ps->p = p + 1;
		return;
	}

	char why[32];
	snprintf(why, sizeof(why), "CANNOT READ '%c'", *p);
	fail(ps, why);
	ps->kind = TOK_END;
	ps->len = 0;
}

static bool is_op(const parser* ps, char op) {
	return ps->kind == TOK_OP && ps->op == op;
}

/* Where a value begins, so two side by side are multiplied */
static bool starts_value(const parser* ps) {
	return ps->kind == TOK_NUMBER || ps->kind == TOK_NAME || is_op(ps, '(');
}

static bool name_is(const parser* ps, const char* const* names, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (strlen(names[i]) == ps->len && strncmp(names[i], ps->text, ps->len) == 0) {
			return true;
		}
	}
	return false;
}

/* The TI's x², x³, x⁻¹ and !, which follow what they apply to */
static bool is_postfix(const parser* ps) {
	static const char* const POSTFIX[] = {"sq", "cb", "inv", "fac"};
	if (is_op(ps, '!')) {
		return true;
	}
	return ps->kind == TOK_NAME && *skip_space(ps->p) != '(' &&
		   name_is(ps, POSTFIX, sizeof(POSTFIX) / sizeof(*POSTFIX));
}

static bool sum(parser* ps);
static bool unary(parser* ps);

static bool number(parser* ps) {
	char text[64];
	size_t len = 0;
	const char* p = ps->text;
	const char* end = ps->text + ps->len;
	if (ps->len + 4 > sizeof(text)) {
		return fail(ps, "NUMBER TOO LONG");
	}
	if (*p == '.') {
		text[len++] = '0';
	}
	bool point = false, exponent = false;
	for (; p < end; p++) {
		if (*p == 'e' || *p == 'E') {
			// 5.e3 needs its nought as much as 5. does
			if (len > 0 && text[len - 1] == '.') {
				text[len++] = '0';
			}
			exponent = true;
		}
		point = point || *p == '.';
		text[len++] = *p;
	}
	if (len > 0 && text[len - 1] == '.') {
		text[len++] = '0';
	}
	// A float, so 1/2 is a half and not Quadrate's integer nought
	if (!point && !exponent) {
		text[len++] = '.';
		text[len++] = '0';
	}
	next(ps);
	return emit(ps, text, len);
}

/* name(a, b) or, for a function of one, name followed by what it takes */
static bool call(parser* ps) {
	char name[64];
	if (ps->len >= sizeof(name)) {
		return fail(ps, "NAME TOO LONG");
	}
	memcpy(name, ps->text, ps->len);
	name[ps->len] = '\0';
	next(ps);

	if (is_op(ps, '(')) {
		next(ps);
		if (!is_op(ps, ')')) {
			if (!sum(ps)) {
				return false;
			}
			while (is_op(ps, ',')) {
				next(ps);
				if (!sum(ps)) {
					return false;
				}
			}
		}
		if (!is_op(ps, ')')) {
			return fail(ps, "MISSING )");
		}
		next(ps);
		return emit_word(ps, name);
	}

	// sin x, as sin(x); only for a function known to take one
	const word_effect* effect = effect_of(name, strlen(name));
	if (effect != NULL && effect->takes > 0) {
		if (effect->takes != 1 || effect->leaves != 1 || !starts_value(ps)) {
			char why[80];
			snprintf(why, sizeof(why), "%s NEEDS (...)", name);
			return fail(ps, why);
		}
		if (!unary(ps)) {
			return false;
		}
	}
	return emit_word(ps, name);
}

static bool primary(parser* ps) {
	if (ps->failed) {
		return false;
	}
	if (ps->kind == TOK_NUMBER) {
		return number(ps);
	}
	if (ps->kind == TOK_NAME) {
		return call(ps);
	}
	if (is_op(ps, '(')) {
		if (++ps->depth > INFIX_DEPTH_MAX) {
			return fail(ps, "TOO DEEP");
		}
		next(ps);
		if (!sum(ps)) {
			return false;
		}
		if (!is_op(ps, ')')) {
			return fail(ps, "MISSING )");
		}
		ps->depth--;
		next(ps);
		return true;
	}
	char why[32];
	if (ps->last_op != '\0' && ps->last_op != '(' && ps->last_op != ',') {
		snprintf(why, sizeof(why), "NOTHING AFTER %c", ps->last_op);
	} else if (ps->kind == TOK_END) {
		snprintf(why, sizeof(why), "NOTHING TO WORK OUT");
	} else {
		snprintf(why, sizeof(why), "NOTHING BEFORE %c", ps->op);
	}
	return fail(ps, why);
}

static bool postfix(parser* ps) {
	if (!primary(ps)) {
		return false;
	}
	while (is_postfix(ps)) {
		const bool bang = is_op(ps, '!');
		char name[8];
		snprintf(name, sizeof(name), "%.*s", (int)ps->len, ps->text);
		next(ps);
		if (!emit_word(ps, bang ? "fac" : name)) {
			return false;
		}
	}
	return true;
}

/* ^ to the right, and over a sign: 2^-1 is a half */
static bool power(parser* ps) {
	if (!postfix(ps)) {
		return false;
	}
	if (!is_op(ps, '^')) {
		return true;
	}
	next(ps);
	return unary(ps) && emit_word(ps, "pow");
}

static bool unary(parser* ps) {
	if (++ps->depth > INFIX_DEPTH_MAX) {
		return fail(ps, "TOO DEEP");
	}
	bool ok;
	if (is_op(ps, '-')) {
		next(ps);
		ok = unary(ps) && emit_word(ps, "neg");
	} else if (is_op(ps, '+')) {
		next(ps);
		ok = unary(ps);
	} else {
		ok = power(ps);
	}
	ps->depth--;
	return ok;
}

static bool product(parser* ps) {
	if (!unary(ps)) {
		return false;
	}
	for (;;) {
		if (is_op(ps, '*') || is_op(ps, '/')) {
			const char op = ps->op;
			next(ps);
			if (!unary(ps) || !emit_word(ps, op == '*' ? "*" : "divide")) {
				return false;
			}
		} else if (ps->kind == TOK_NAME || is_op(ps, '(')) {
			// 2x, 3 sin(x), (x+1)(x-1); never two numbers, which is RPN short of a word
			if (!power(ps) || !emit_word(ps, "*")) {
				return false;
			}
		} else {
			return true;
		}
	}
}

static bool sum(parser* ps) {
	if (!product(ps)) {
		return false;
	}
	while (is_op(ps, '+') || is_op(ps, '-')) {
		const char op = ps->op;
		next(ps);
		if (!product(ps) || !emit_word(ps, op == '+' ? "+" : "-")) {
			return false;
		}
	}
	return true;
}

bool qdos_infix_to_rpn(const char* text, char* out, size_t cap, int* results, char* error, size_t error_cap) {
	parser ps = {.p = text, .out = out, .cap = cap, .error = error, .error_cap = error_cap};
	if (cap > 0) {
		out[0] = '\0';
	}
	if (error_cap > 0) {
		error[0] = '\0';
	}

	next(&ps);
	int count = 0;
	for (;;) {
		if (!sum(&ps)) {
			return false;
		}
		count++;
		if (!is_op(&ps, ',')) {
			break;
		}
		next(&ps);
	}

	if (ps.kind != TOK_END) {
		char why[40];
		snprintf(why, sizeof(why), "UNEXPECTED %.*s", (int)(ps.len > 12 ? 12 : ps.len), ps.text);
		return fail(&ps, why);
	}
	if (ps.failed) {
		return false;
	}
	*results = count;
	return true;
}
