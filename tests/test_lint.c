/**
 * @file test_lint.c
 * @brief Finding what a declaration puts off until it is called
 */

#include "check.h"

#include "../src/shell/lint.h"

static int native_sto(qd_context* ctx, void* userdata) {
	(void)ctx;
	(void)userdata;
	return 0;
}

/** An interpreter with a vocabulary, the way the shell hands one over */
static qd_interp* fresh(void) {
	qd_interp* interp = qd_interp_create(4096);
	qd_interp_register(interp, "sto", "(value:i64 slot:i64 -- )", native_sto, NULL);
	qd_interp_eval(interp, "fn isqrt(n:i64 -- r:i64) {\n\t0\n}");
	return interp;
}

/** Declare, then lint, as check does: a word may call itself */
static bool lint(qd_interp* interp, const char* source, char* out, size_t cap) {
	out[0] = '\0';
	CHECK(qd_interp_eval(interp, source));
	return qdos_lint_program(interp, source, out, cap);
}

/** The programs that ship say nothing, or the check would cry wolf on boot. */
static void test_the_shipped_programs_are_clean(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!lint(interp,
			"// Integer square root.\n"
			"fn isqrt(n:i64 -- r:i64) {\n"
			"\t0\n"
			"\tloop {\n"
			"\t\tdup 1 + dup *\n"
			"\t\tn > if { break }\n"
			"\t\t1 +\n"
			"\t}\n"
			"}",
			message, sizeof(message)));
	CHECK_STR(message, "");

	CHECK(!lint(interp,
			"// Pythagoras.\n"
			"fn hyp(a:i64 b:i64 -- r:i64) {\n"
			"\ta a * b b * + isqrt\n"
			"}",
			message, sizeof(message)));

	CHECK(!lint(interp, "fn hello( -- r:str) {\n\t\"Hello, world!\"\n}", message, sizeof(message)));

	qd_interp_destroy(interp);
}

/** The case that started this: a body naming something that is not a word. */
static void test_an_undefined_word_in_a_body_is_found(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn hello() {\n\n\t\tok\n}", message, sizeof(message)));
	CHECK_STR(message, "L3: 'ok' NOT DEFINED");

	qd_interp_destroy(interp);
}

/** The wording the interpreter would have used, so both agree. */
static void test_the_message_matches_what_running_says(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn t() { dupp }", message, sizeof(message)));
	CHECK(strstr(message, "'dupp'") != NULL);

	// Declared, so calling it is what reports the name
	CHECK(!qd_interp_eval(interp, "t"));
	CHECK(strstr(qd_interp_error(interp), "'dupp'") != NULL);
	CHECK(strstr(qd_interp_error(interp), "not defined") != NULL);

	qd_interp_destroy(interp);
}

/** Only the first one, because there is one message line to say it on. */
static void test_it_stops_at_the_first_find(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn t() {\n\tfirst\n\tsecond\n}", message, sizeof(message)));
	CHECK_STR(message, "L2: 'first' NOT DEFINED");

	qd_interp_destroy(interp);
}

/** A name longer than the line holds is cut, not left to overrun. */
static void test_a_long_name_is_cut(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn t() { abcdefghijklmnopqrst }", message, sizeof(message)));
	CHECK_STR(message, "L1: 'abcdefghijkl' NOT DEFINED");

	qd_interp_destroy(interp);
}

/** Everything the vocabulary holds passes: builtins, natives, other programs. */
static void test_the_whole_vocabulary_counts(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!lint(interp, "fn a() { dup drop swap over rot pick }", message, sizeof(message)));
	CHECK(!lint(interp, "fn b() { 1 2 sto }", message, sizeof(message)));			  // a registered native
	CHECK(!lint(interp, "fn c(n:i64 -- r:i64) { isqrt }", message, sizeof(message))); // another program

	qd_interp_destroy(interp);
}

/** A word that calls itself is fine: check declares it before looking. */
static void test_recursion_is_not_a_find(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!lint(interp, "fn down(n:i64 -- ) { dup 0 > if { 1 - down } }", message, sizeof(message)));

	qd_interp_destroy(interp);
}

/** Two words in one program, the first calling the second. */
static void test_a_program_can_declare_its_own_helper(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!lint(interp, "fn outer() { inner }\nfn inner() { 1 }", message, sizeof(message)));

	qd_interp_destroy(interp);
}

/** A branch that did not run this time is still read. */
static void test_both_arms_of_an_if_are_read(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn t(c:i64 -- ) {\n\tif { 1 } else {\n\t\tnope\n\t}\n}", message, sizeof(message)));
	CHECK_STR(message, "L3: 'nope' NOT DEFINED");

	qd_interp_destroy(interp);
}

/** Only the body: a signature names types and parameters, which are not words. */
static void test_a_signature_is_not_read_as_words(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!lint(interp, "fn t(alpha:i64 beta:str -- gamma:i64) { drop drop 0 }", message, sizeof(message)));

	qd_interp_destroy(interp);
}

/**
 * Quadrate the language has more than the interpreter runs. What the shell
 * cannot run is worth saying at a check rather than at a call.
 */
static void test_what_the_interpreter_cannot_run_is_reported(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(lint(interp, "fn t( -- ) {\n\tdefer {\n\t\t1 drop\n\t}\n}", message, sizeof(message)));
	CHECK_STR(message, "L2: DEFER NOT SUPPORTED");

	// And it really is only a check-time find: declaring it succeeded above
	CHECK(!qd_interp_eval(interp, "t"));

	// `while` parses again since the compiler readded it, and still does not run
	CHECK(lint(interp, "fn w( -- ) {\n\t0 -> i\n\ti 5 < while {\n\t\ti 1 + -> i\n\t}\n}", message, sizeof(message)));
	CHECK_STR(message, "L3: WHILE NOT SUPPORTED");
	CHECK(!qd_interp_eval(interp, "w"));

	qd_interp_destroy(interp);
}

/**
 * What the interpreter learns, the check has to stop refusing.
 *
 * A stale list here is worse than no list: it turns a program that runs into
 * one the machine says no to, and there is no way past it from the keypad.
 */
static void test_what_the_interpreter_now_runs_is_not_a_find(void) {
	qd_interp* interp = fresh();
	char message[80];

	// A named parameter, which is how a word reads its arguments
	CHECK(!lint(interp, "fn p(a:i64 -- r:i64) { a a + }", message, sizeof(message)));

	CHECK(!lint(interp, "fn l(a:i64 -- r:i64) { a -> x x x + }", message, sizeof(message)));
	CHECK(!lint(interp, "fn fo( -- r:i64) { 0 0 5 1 for i { i + } }", message, sizeof(message)));
	CHECK(!lint(interp, "fn sw(a:i64 -- r:i64) { a switch { 1 { 10 } _ { 20 } } }", message, sizeof(message)));
	CHECK(!lint(interp, "fn re(a:i64 -- r:i64) { a 0 > if { 1 return } 2 }", message, sizeof(message)));
	CHECK(!lint(interp, "const K = 7\nfn co( -- r:i64) { K }", message, sizeof(message)));
	CHECK(!lint(interp, "enum C { Red, Blue }\nfn en( -- r:i64) { C::Blue }", message, sizeof(message)));
	CHECK(!lint(interp, "fn ca( -- r:f64) { 3 cast<f64> }", message, sizeof(message)));
	CHECK(!lint(interp, "fn ar( -- r:i64) { [1 2 3] -> a a 1 nth }", message, sizeof(message)));

	qd_interp_destroy(interp);
}

/** A syntax error belongs to eval, which already has a message for it. */
static void test_unparsable_text_is_left_alone(void) {
	qd_interp* interp = fresh();
	char message[80];

	message[0] = '\0';
	CHECK(!qdos_lint_program(interp, "fn t( {{{", message, sizeof(message)));
	CHECK_STR(message, "");

	qd_interp_destroy(interp);
}

/** Nothing to look at is not a find. */
static void test_the_empty_cases(void) {
	qd_interp* interp = fresh();
	char message[80];

	CHECK(!qdos_lint_program(interp, "", message, sizeof(message)));
	CHECK(!qdos_lint_program(interp, "// just a comment\n", message, sizeof(message)));
	CHECK(!qdos_lint_program(NULL, "fn t() { ok }", message, sizeof(message)));
	CHECK(!qdos_lint_program(interp, NULL, message, sizeof(message)));
	CHECK(!qdos_lint_program(interp, "fn t() { ok }", NULL, sizeof(message)));
	CHECK(!qdos_lint_program(interp, "fn t() { ok }", message, 0));

	qd_interp_destroy(interp);
}

int main(void) {
	test_the_shipped_programs_are_clean();
	test_an_undefined_word_in_a_body_is_found();
	test_the_message_matches_what_running_says();
	test_it_stops_at_the_first_find();
	test_a_long_name_is_cut();
	test_the_whole_vocabulary_counts();
	test_recursion_is_not_a_find();
	test_a_program_can_declare_its_own_helper();
	test_both_arms_of_an_if_are_read();
	test_a_signature_is_not_read_as_words();
	test_what_the_interpreter_cannot_run_is_reported();
	test_what_the_interpreter_now_runs_is_not_a_find();
	test_unparsable_text_is_left_alone();
	test_the_empty_cases();
	return check_report("lint");
}
