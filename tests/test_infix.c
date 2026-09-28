/**
 * @file test_infix.c
 * @brief Formulas as a TI takes them, written out as the RPN that works them out
 */

#include "check.h"

#include "../src/shell/infix.h"

#include <string.h>

static char g_out[256];
static char g_error[64];
static int g_results;

static bool translate(const char* text) {
	return qdos_infix_to_rpn(text, g_out, sizeof(g_out), &g_results, g_error, sizeof(g_error));
}

/* Translates, to exactly this, leaving one value */
static void check_rpn(const char* text, const char* rpn) {
	CHECK(translate(text));
	CHECK_STR(g_out, rpn);
	CHECK(g_results == 1);
}

static void check_error(const char* text, const char* error) {
	CHECK(!translate(text));
	CHECK_STR(g_error, error);
}

static void test_precedence(void) {
	check_rpn("x^2", "x 2.0 pow");
	check_rpn("x^2 + 3*x - 1", "x 2.0 pow 3.0 x * + 1.0 -");
	check_rpn("2^3^2", "2.0 3.0 2.0 pow pow"); // to the right
	check_rpn("-x^2", "x 2.0 pow neg");		   // as a TI has it
	check_rpn("2^-1", "2.0 1.0 neg pow");
	check_rpn("(x+1)*(x-1)", "x 1.0 + x 1.0 - *");
	check_rpn("1/2*x", "1.0 2.0 divide x *"); // a half, not nought
	check_rpn("x - -1", "x 1.0 neg -");
}

static void test_juxtaposition_multiplies(void) {
	check_rpn("2x", "2.0 x *");
	check_rpn("3 sin(x)", "3.0 x sin *");
	check_rpn("(x+1)(x-1)", "x 1.0 + x 1.0 - *");
	check_rpn("2pi x", "2.0 pi * x *");
	check_rpn("x -1", "x 1.0 -"); // a gap is no sign
	check_rpn("2e", "2.0 e *");	  // no digits after e, so no exponent
}

static void test_numbers(void) {
	check_rpn(".5x", "0.5 x *");
	check_rpn("5.", "5.0");
	check_rpn("1.5e-3", "1.5e-3");
	check_rpn("2e3", "2e3");
}

static void test_functions(void) {
	check_rpn("sin(x)", "x sin");
	check_rpn("sin x", "x sin");
	check_rpn("sin x^2", "x 2.0 pow sin");
	check_rpn("sqrt(x) + 1", "x sqrt 1.0 +");
	check_rpn("max(x, 0)", "x 0.0 max");
	check_rpn("pow(x, 2)", "x 2.0 pow");
	check_rpn("Y1(x) + 1", "x Y1 1.0 +"); // a word of the machine's own
	check_rpn("neg x", "x neg");
	check_error("max x", "max NEEDS (...)");
	check_error("sin + 1", "sin NEEDS (...)");
}

static void test_postfix(void) {
	check_rpn("x sq + 1", "x sq 1.0 +");
	check_rpn("5!", "5.0 fac");
	check_rpn("x inv", "x inv");
	check_rpn("sq(x)", "x sq");
}

/* What the keypad types into a slot */
static void test_keypad_words(void) {
	check_rpn("x pow 2", "x 2.0 pow");
	check_rpn("x divide 2", "x 2.0 divide");
	check_rpn("x plus 1 times 2", "x 1.0 2.0 * +");
}

static void test_a_comma_leaves_values_side_by_side(void) {
	CHECK(translate("cos(t), sin(t)"));
	CHECK_STR(g_out, "t cos t sin");
	CHECK(g_results == 2);
}

static void test_errors(void) {
	check_error("x +", "NOTHING AFTER +");
	check_error("(x + 1", "MISSING )");
	check_error("*x", "NOTHING BEFORE *");
	check_error("x )", "UNEXPECTED )");
	check_error("x $ 1", "CANNOT READ '$'");
	check_error("x 2 +", "UNEXPECTED 2");
	check_error("2 3", "UNEXPECTED 3"); // two numbers are not multiplied

	// Nesting has a floor, not a crash
	char deep[200];
	memset(deep, '(', 100);
	strcpy(deep + 100, "x");
	check_error(deep, "TOO DEEP");
}

static void test_rpn_is_recognised(void) {
	CHECK(qdos_rpn_results("x sq") == 1);
	CHECK(qdos_rpn_results("x 2 pow 3 x * +") == 1);
	CHECK(qdos_rpn_results("t cos t sin") == 2);
	CHECK(qdos_rpn_results("x -1 *") == 1);
	CHECK(qdos_rpn_results("x dup *") == 1);
	CHECK(qdos_rpn_results("x 2") == 2);

	// What only a formula can be
	CHECK(qdos_rpn_results("x - 1") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("x^2") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("sin(x)") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("-x") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("2x") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("x+2") == QDOS_RPN_NOT); // glued, with nothing else to give it away
	CHECK(qdos_rpn_results("x*2") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("x/2") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("x-1") == QDOS_RPN_NOT);
	CHECK(qdos_rpn_results("x -> a a") == QDOS_RPN_UNKNOWN);

	// A word of the program's, or control flow, is taken on trust
	CHECK(qdos_rpn_results("x myword") == QDOS_RPN_UNKNOWN);
	CHECK(qdos_rpn_results("x 0 < if { x neg } else { x }") == QDOS_RPN_UNKNOWN);
}

int main(void) {
	test_precedence();
	test_juxtaposition_multiplies();
	test_numbers();
	test_functions();
	test_postfix();
	test_keypad_words();
	test_a_comma_leaves_values_side_by_side();
	test_errors();
	test_rpn_is_recognised();
	return check_report("infix");
}
