/**
 * @file lint.cc
 * @brief What parses now but would only fail once it runs
 *
 * The only C++ in QDOS: it reads the Quadrate front-end's syntax tree, which
 * has no C interface. Kept to this one file, behind the C header.
 */

#include "lint.h"

#include <quadrate/qc/ast.h>
#include <quadrate/qc/ast_node_identifier.h>
#include <quadrate/qc/ast_node_instruction.h>
#include <quadrate/qc/ast_node_scoped.h>

#include <cstdio>
#include <string>
#include <unordered_set>

namespace {

	using Type = Qd::IAstNode::Type;
	using vocabulary = std::unordered_set<std::string>;

	bool collect(const char* name, void* userdata) {
		static_cast<vocabulary*>(userdata)->insert(name);
		return true;
	}

	/**
	 * A short name for a construct the interpreter cannot run. Compiled
	 * Quadrate has all of these; the shell runs the interpreter, which walks
	 * literals, words, if and loop and refuses the rest.
	 */
	const char* construct_name(Type type) {
		switch (type) {
			case Type::LOCAL: return "LOCALS";
			case Type::FOR_STATEMENT: return "FOR";
			case Type::SWITCH_STATEMENT:
			case Type::CASE_STATEMENT: return "SWITCH";
			case Type::RETURN_STATEMENT: return "RETURN";
			case Type::DEFER_STATEMENT: return "DEFER";
			case Type::STRUCT_DECLARATION:
			case Type::STRUCT_FIELD:
			case Type::STRUCT_CONSTRUCTION:
			case Type::FIELD_ACCESS:
			case Type::FIELD_SET: return "STRUCTS";
			case Type::ARRAY_LITERAL:
			case Type::ARRAY_INDEX: return "ARRAYS";
			case Type::CONSTANT_DECLARATION: return "CONST";
			case Type::GLOBAL_VAR_DECLARATION: return "GLOBALS";
			case Type::ENUM_DECLARATION: return "ENUMS";
			case Type::TYPE_ALIAS_DECLARATION: return "TYPES";
			case Type::USE_STATEMENT:
			case Type::IMPORT_STATEMENT: return "IMPORTS";
			case Type::ANONYMOUS_FUNCTION:
			case Type::FUNCTION_POINTER_REFERENCE: return "FN VALUES";
			case Type::STRING_INTERPOLATION: return "TEXT HOLES";
			case Type::AS_CAST: return "CASTS";
			case Type::TEST_DECLARATION: return "TESTS";
			default: return "THIS";
		}
	}

	struct finding {
		std::string message;
		bool found = false;
	};

	void check_name(const std::string& name, size_t line, const vocabulary& known, finding& out) {
		if (known.count(name) != 0)
			return;

		// The wording the interpreter itself would use, with the line it is on
		char text[128];
		std::snprintf(text, sizeof(text), "L%zu: '%.12s' NOT DEFINED", line, name.c_str());
		out.message = text;
		out.found = true;
	}

	/** One switch per node type, following evalNode in the interpreter */
	void walk(const Qd::IAstNode* node, const vocabulary& known, finding& out) {
		if (node == nullptr || out.found)
			return;

		switch (node->type()) {
			// Nothing to look up, and nothing underneath that is run
			case Type::LITERAL:
			case Type::COMMENT:
			case Type::VARIABLE_DECLARATION: // a parameter, recorded by the declaration
			case Type::BREAK_STATEMENT:
			case Type::CONTINUE_STATEMENT: return;

			case Type::INSTRUCTION:
				check_name(static_cast<const Qd::AstNodeInstruction*>(node)->name(), node->line(), known, out);
				return;

			case Type::IDENTIFIER:
				check_name(static_cast<const Qd::AstNodeIdentifier*>(node)->name(), node->line(), known, out);
				return;

			case Type::SCOPED_IDENTIFIER: {
				const auto* scoped = static_cast<const Qd::AstNodeScopedIdentifier*>(node);
				check_name(scoped->scope() + "::" + scoped->name(), node->line(), known, out);
				return;
			}

			// Only the body: a signature is read when the word is declared,
			// and its nodes are never walked
			case Type::FUNCTION_DECLARATION:
				for (size_t i = 0; i < node->childCount(); i++) {
					if (node->child(i)->type() == Type::BLOCK)
						walk(node->child(i), known, out);
				}
				return;

			// Walked below. An else branch is looked at even though only one
			// arm of an if ever runs -- the point is to find it before it does.
			case Type::PROGRAM:
			case Type::BLOCK:
			case Type::IF_STATEMENT:
			case Type::LOOP_STATEMENT: break;

			default: {
				char text[128];
				std::snprintf(
						text, sizeof(text), "L%zu: %s NOT SUPPORTED", node->line(), construct_name(node->type()));
				out.message = text;
				out.found = true;
				return;
			}
		}

		for (size_t i = 0; i < node->childCount() && !out.found; i++)
			walk(node->child(i), known, out);
	}

} // namespace

bool qdos_lint_program(qd_interp* interp, const char* source, char* out, size_t cap) {
	if (interp == nullptr || source == nullptr || out == nullptr || cap == 0)
		return false;

	// The text as written. Anything that is not a declaration has already run,
	// so it has reported whatever it would have tripped over.
	Qd::Ast ast;
	const Qd::IAstNode* root = ast.generate(source, false, "<check>");
	if (root == nullptr || ast.hasErrors())
		return false;

	vocabulary known;
	qd_interp_visit_words(interp, collect, &known);

	finding result;
	walk(root, known, result);
	if (!result.found)
		return false;

	std::snprintf(out, cap, "%s", result.message.c_str());
	return true;
}
