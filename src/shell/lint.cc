/**
 * @file lint.cc
 * @brief What parses now but would only fail once it runs
 *
 * The only C++ in QDOS: it reads the Quadrate front-end's syntax tree, which
 * has no C interface. Kept to this one file, behind the C header.
 */

#include "lint.h"

#include <quadrate/qc/ast.h>
#include <quadrate/qc/ast_node_for.h>
#include <quadrate/qc/ast_node_function.h>
#include <quadrate/qc/ast_node_identifier.h>
#include <quadrate/qc/ast_node_instruction.h>
#include <quadrate/qc/ast_node_local.h>
#include <quadrate/qc/ast_node_parameter.h>
#include <quadrate/qc/ast_node_scoped.h>

#include <cstdio>
#include <string>
#include <unordered_set>

namespace {

	using Type = Qd::IAstNode::Type;
	using vocabulary = std::unordered_set<std::string>;

	/** @brief Names a body binds for itself: parameters, `-> x`, a `for` iterator */
	using names = std::unordered_set<std::string>;

	bool collect(const char* name, void* userdata) {
		static_cast<vocabulary*>(userdata)->insert(name);
		return true;
	}

	/**
	 * A short name for a construct the interpreter cannot run.
	 *
	 * Compiled Quadrate has all of these. What the interpreter answers for
	 * grows, so this list is the one to shorten when it does -- naming a
	 * construct it now runs costs the user a program that would have worked.
	 * See lib/interp/README.md in Quadrate for what it covers.
	 */
	const char* construct_name(Type type) {
		switch (type) {
		case Type::DEFER_STATEMENT:
			return "DEFER";

		// The language has `while` again; the interpreter runs `loop` only
		case Type::WHILE_STATEMENT:
			return "WHILE";

		// Methods come through here too: a receiver needs the struct it binds
		// to, which is the thing above
		case Type::STRUCT_DECLARATION:
		case Type::STRUCT_FIELD:
		case Type::STRUCT_CONSTRUCTION:
		case Type::FIELD_ACCESS:
		case Type::FIELD_SET:
			return "STRUCTS";

		case Type::GLOBAL_VAR_DECLARATION:
			return "GLOBALS";
		case Type::TYPE_ALIAS_DECLARATION:
			return "TYPES";
		case Type::USE_STATEMENT:
		case Type::IMPORT_STATEMENT:
			return "IMPORTS";
		case Type::ANONYMOUS_FUNCTION:
		case Type::FUNCTION_POINTER_REFERENCE:
			return "FN VALUES";

		// Interpolation is the one that fails quietly: the holes are left
		// in the string rather than filled
		case Type::STRING_INTERPOLATION:
			return "TEXT HOLES";

		case Type::TEST_DECLARATION:
			return "TESTS";
		default:
			return "THIS";
		}
	}

	struct finding {
		std::string message;
		bool found = false;
	};

	void check_name(const std::string& name, size_t line, const vocabulary& known, const names& bound, finding& out) {
		if (known.count(name) != 0 || bound.count(name) != 0) {
			return;
		}

		// The wording the interpreter itself would use, with the line it is on
		char text[128];
		std::snprintf(text, sizeof(text), "L%zu: '%.12s' NOT DEFINED", line, name.c_str());
		out.message = text;
		out.found = true;
	}

	/** One switch per node type, following evalNode in the interpreter */
	void walk(const Qd::IAstNode* node, const vocabulary& known, names& bound, finding& out) {
		if (node == nullptr || out.found) {
			return;
		}

		switch (node->type()) {
		// Nothing to look up, and nothing underneath that is run
		case Type::LITERAL:
		case Type::COMMENT:
		case Type::VARIABLE_DECLARATION: // a parameter, recorded by the declaration
		case Type::BREAK_STATEMENT:
		case Type::CONTINUE_STATEMENT:
			return;

		// Declarations the interpreter takes at declare time. Their names
		// are in the vocabulary by the time this runs, and what is under
		// them is variant names rather than words.
		case Type::CONSTANT_DECLARATION:
		case Type::ENUM_DECLARATION:
			return;

		// `cast<f64>` names a type, which is not a word either. What it
		// converts was pushed before it and is read on its own.
		case Type::AS_CAST:
			return;

		case Type::INSTRUCTION: {
			const auto& name = static_cast<const Qd::AstNodeInstruction*>(node)->name();

			// `cast` carries its type rather than spelling it in the name,
			// so it is syntax and not a word to look up -- evalInstruction
			// resolves it before the builtin table too
			if (name == "cast") {
				return;
			}

			check_name(name, node->line(), known, bound, out);
			return;
		}

		case Type::IDENTIFIER:
			check_name(static_cast<const Qd::AstNodeIdentifier*>(node)->name(), node->line(), known, bound, out);
			return;

		case Type::SCOPED_IDENTIFIER: {
			const auto* scoped = static_cast<const Qd::AstNodeScopedIdentifier*>(node);
			check_name(scoped->scope() + "::" + scoped->name(), node->line(), known, bound, out);
			return;
		}

		// Only the body: a signature is read when the word is declared,
		// and its nodes are never walked. Its parameters are named in the
		// body, though, so the body starts knowing them -- locals are
		// function-scoped here as they are in the interpreter.
		case Type::FUNCTION_DECLARATION: {
			const auto* function = static_cast<const Qd::AstNodeFunctionDeclaration*>(node);
			names scope;

			// Inputs bind as locals; `stack fn` is the one that leaves them on
			// the stack, where the body reads them positionally and their names
			// are documentation. The rule bindParameters follows.
			if (!function->hasReceiver() && !function->isStack()) {
				for (const auto& input : function->inputParameters()) {
					const auto* parameter = static_cast<const Qd::AstNodeParameter*>(input.get());
					if (parameter->hasName()) {
						scope.insert(parameter->name());
					}
				}
			}

			for (size_t i = 0; i < node->childCount(); i++) {
				if (node->child(i)->type() == Type::BLOCK) {
					walk(node->child(i), known, scope, out);
				}
			}
			return;
		}

		// `a -> x` names what it took off the stack
		case Type::LOCAL:
			for (const auto& name : static_cast<const Qd::AstNodeLocal*>(node)->names()) {
				bound.insert(name);
			}
			return;

		case Type::FOR_STATEMENT:
			bound.insert(static_cast<const Qd::AstNodeForStatement*>(node)->iteratorName());
			break;

		// Walked below. An else branch is looked at even though only one
		// arm of an if ever runs -- the point is to find it before it does.
		case Type::PROGRAM:
		case Type::BLOCK:
		case Type::IF_STATEMENT:
		case Type::LOOP_STATEMENT:
		case Type::SWITCH_STATEMENT:
		case Type::CASE_STATEMENT:
		case Type::RETURN_STATEMENT:
		case Type::ARRAY_LITERAL:
			break;

		default: {
			char text[128];
			std::snprintf(text, sizeof(text), "L%zu: %s NOT SUPPORTED", node->line(), construct_name(node->type()));
			out.message = text;
			out.found = true;
			return;
		}
		}

		for (size_t i = 0; i < node->childCount() && !out.found; i++) {
			walk(node->child(i), known, bound, out);
		}
	}

} // namespace

bool qdos_lint_program(qd_interp* interp, const char* source, char* out, size_t cap) {
	if (interp == nullptr || source == nullptr || out == nullptr || cap == 0) {
		return false;
	}

	// The text as written. Anything that is not a declaration has already run,
	// so it has reported whatever it would have tripped over.
	Qd::Ast ast;
	const Qd::IAstNode* root = ast.generate(source, false, "<check>");
	if (root == nullptr || ast.hasErrors()) {
		return false;
	}

	vocabulary known;
	qd_interp_visit_words(interp, collect, &known);

	finding result;
	names bound;
	walk(root, known, bound, result);
	if (!result.found) {
		return false;
	}

	std::snprintf(out, cap, "%s", result.message.c_str());
	return true;
}
