/**
 * @file complete.c
 * @brief Completing a partly typed word against the interpreter's vocabulary
 */

#include "complete.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ':' among them: `foo::triple` is one name, not two */
static bool word_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':';
}

size_t qdos_complete_prefix_len(const char* input, size_t len) {
	size_t n = 0;
	while (n < len && word_char(input[len - 1 - n])) {
		n++;
	}
	// A run starting with a digit is a number, not a word
	if (n > 0 && input[len - n] >= '0' && input[len - n] <= '9') {
		return 0;
	}
	return n < QDOS_WORD_MAX ? n : 0;
}

typedef struct {
	const char* prefix;
	size_t prefix_len;
	qdos_completion* out;
} walk;

/** @brief Shorten the common prefix to what this match also shares */
static void narrow_common(qdos_completion* out, const char* name) {
	size_t i = 0;
	while (out->common[i] != '\0' && name[i] != '\0' && out->common[i] == name[i]) {
		i++;
	}
	out->common[i] = '\0';
}

static void append_listing(qdos_completion* out, const char* name) {
	const size_t used = strlen(out->listing);
	const size_t need = strlen(name) + (used > 0 ? 1 : 0);

	if (used + need + 1 >= sizeof(out->listing)) {
		// No room: mark the list as cut rather than silently dropping names
		if (used + 4 < sizeof(out->listing)) {
			snprintf(out->listing + used, sizeof(out->listing) - used, " ...");
		}
		return;
	}
	snprintf(out->listing + used, sizeof(out->listing) - used, "%s%s", used > 0 ? " " : "", name);
}

/** @brief Is name already in the space separated listing, as a whole word? */
static bool listing_has(const char* listing, const char* name) {
	const size_t len = strlen(name);

	for (const char* p = listing; *p != '\0';) {
		const char* end = strchr(p, ' ');
		const size_t span = (end != NULL) ? (size_t)(end - p) : strlen(p);
		if (span == len && strncmp(p, name, len) == 0) {
			return true;
		}
		if (end == NULL) {
			break;
		}
		p = end + 1;
	}
	return false;
}

static bool consider(const char* name, void* userdata) {
	walk* w = (walk*)userdata;

	if (strncmp(name, w->prefix, w->prefix_len) != 0) {
		return true;
	}
	// A declaration shadowing a builtin is one word, not two
	if (listing_has(w->out->listing, name)) {
		return true;
	}

	if (w->out->matches == 0) {
		snprintf(w->out->common, sizeof(w->out->common), "%s", name);
	} else {
		narrow_common(w->out, name);
	}
	append_listing(w->out, name);
	w->out->matches++;
	return true;
}

void qdos_complete(const qd_interp* interp, const char* prefix, qdos_completion* out) {
	memset(out, 0, sizeof(*out));
	if (!interp || !prefix || *prefix == '\0') {
		return;
	}

	walk w = {.prefix = prefix, .prefix_len = strlen(prefix), .out = out};
	qd_interp_visit_words(interp, consider, &w);
}
