/**
 * @file shell.c
 * @brief The calculator's user-facing loop
 */

#include <qdos/shell.h>

#include <quadrate/interp/interp.h>
#include <quadrate/rt/array.h>

#include "../ui/console.h"
#include "complete.h"
#include "editor.h"
#include "graph.h"
#include "guarded.h"
#include "lint.h"
#include "mathwords.h"
#include "native.h"
#include "numeric.h"
#include "stats.h"
#include "statwords.h"
#include "surface.h"

#include "qdos_version.h"
#include "storage.h"
#include "wordlist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum characters in the input line. */
#define INPUT_MAX 256

/** Maximum characters in a pending numeric entry. */
#define ENTRY_MAX 64

/** Evaluator stack capacity, in elements. */
#define STACK_SIZE 4096

/*
 * Screen layout, in rows. A rule is one pixel on the bottom edge of a row, and
 * no glyph reaches that far, so it underlines a row of content rather than
 * taking a row of its own -- the panel has ten and cannot spare two for lines.
 */
/* The clock and the battery, on a black band above everything else */
#define ROW_STATUS 0

/* The list and the editor caption themselves; the calculator does not need to */
#define ROW_HEADER 1
#define ROW_CONTENT_FIRST 2

/* Numbered rows already say how deep the stack is, so it starts under the band */
#define ROW_STACK_FIRST 1
#define ROW_CONTENT_LAST (QDOS_ROWS - 3)
#define ROW_INPUT (QDOS_ROWS - 2)

/*
 * A message borrows the input line rather than keeping a row of its own. The
 * panel has ten rows and a message is on screen for one keypress in twenty, so
 * a row reserved for it is a row wasted nineteen times over. The next key takes
 * the line back -- see handle_key().
 */
#define ROW_MESSAGE ROW_INPUT

/* An error is set in the small font: errors are the long ones, and a diagnostic
 * cut at 24 columns rarely still says where the problem is */
#define MESSAGE_COLS (QDOS_SCREEN_W / QDOS_SMALL_FONT_W)

/* How often the band looks at the battery when there is no clock to follow */
#define STATUS_POLL_MS 60000

/* Last row, so the labels sit against the edge the function keys are under */
#define ROW_SOFT (QDOS_ROWS - 1)

#define SOFT_KEYS 5
#define SOFT_WIDTH (QDOS_COLS / SOFT_KEYS)

#define STACK_ROWS (ROW_CONTENT_LAST - ROW_STACK_FIRST + 1)

/*
 * Half a blink period, so the cursor comes and goes once a second. The panel
 * is a Sharp Memory LCD: it holds its image unpowered and costs only what is
 * clocked into it, and the driver is already sending it a VCOM message every
 * second, so two frames a second sit alongside traffic the machine has anyway.
 */
#define CURSOR_BLINK_MS 500

/*
 * How long the cursor keeps blinking after the last key. Past this the machine
 * has been left alone rather than thought about, so the cursor goes solid --
 * still saying where you are -- and the loop stops waking to toggle it. That is
 * what lets qdos_shell_run() wait on the keypad with no timer at all.
 */
#define CURSOR_SETTLE_MS 10000

/*
 * Minutes of inactivity before the machine turns itself off, nought being
 * never. Calculators normally use five; this starts longer because no boot has
 * been timed on the hardware yet, and a machine that switches off more eagerly
 * than it comes back is worse than one that simply stays on.
 */
static const int AUTO_OFF_MINUTES[] = {0, 5, 10, 30, 60};
#define AUTO_OFF_COUNT (sizeof(AUTO_OFF_MINUTES) / sizeof(AUTO_OFF_MINUTES[0]))
#define AUTO_OFF_DEFAULT 2

/** Long enough to read the warning and reach for a key. */
#define AUTO_OFF_WARN_MS 10000

/** Prompts. The character says which mode the keypad is in. */
#define PROMPT "> "
#define LINE_PROMPT ": "
#define CONT_PROMPT ".."
#define PROMPT_LEN 2

/** @brief What the keypad is doing */
typedef enum {
	QDOS_MODE_CALC,	 ///< Digits build a number; an operator applies immediately
	QDOS_MODE_LINE,	 ///< Whole lines of Quadrate, evaluated on Enter
	QDOS_MODE_LIST,	 ///< Browsing the vocabulary
	QDOS_MODE_EDIT,	 ///< Editing a program in the stack area
	QDOS_MODE_ABOUT, ///< What this firmware is
	QDOS_MODE_SETTINGS,
	QDOS_MODE_DEBUG,	 ///< What the machine has been saying
	QDOS_MODE_GRAPH,	 ///< A word plotted as y = f(x)
	QDOS_MODE_GRAPH3,	 ///< A word plotted as z = f(x, y)
	QDOS_MODE_PLOT,		 ///< The Y= slots, each a function of x
	QDOS_MODE_PLOT_EDIT, ///< Typing one of them
	QDOS_MODE_MENU,		 ///< A numbered list to pick from: ZOOM, CALC and the rest
	QDOS_MODE_WINDOW,	 ///< The plot's edges and tick spacing, typed
	QDOS_MODE_TABLE,	 ///< The curves' values down a column of x
	QDOS_MODE_STAT,		 ///< The lists, L1 to L6
	QDOS_MODE_RESULT,	 ///< What a statistics calculation found
	QDOS_MODE_OUTPUT,	 ///< What the last evaluation printed, when it was more than a line
	QDOS_MODE__COUNT
} qdos_mode;

#define LOG_LINES 48

/** @brief What a setting can be changed to */
typedef enum {
	SETTING_ANGLE = 0,
	SETTING_DECIMALS,
	SETTING_AUTO_OFF,
	SETTING_USB,	 ///< Only where the backend has a gadget to offer
	SETTING_MODULES, ///< Only when something is blocked, there being nothing else to say
	SETTING__COUNT
} qdos_setting;

#define DECIMALS_AUTO -1
#define DECIMALS_MAX 9

/** @brief As many apps as the card may offer at once */
#define QDOS_APPS_MAX 32

/** @brief As many words as one session may declare at the prompt */
#define QDOS_LINE_WORDS 64

/* The Y= slots, Y1 to Y6: one screen of them, and as many curves as one plot draws */
#define PLOT_SLOTS 6
#define PLOT_BODY_MAX 96

/* The lists, L1 to L6, and as many values as each holds */
#define STAT_LISTS 6
#define STAT_LIST_MAX 99

/* What a menu page is listing */
typedef enum {
	MENU_ZOOM = 0,
	MENU_CALC,
	MENU_GRAPH_TOOLS, ///< MENU on the graph
	MENU_PLOT_TOOLS,  ///< MENU on Y=
	MENU_FORMAT,
	MENU_STAT_CALC,
	MENU_STAT_PLOT,
	MENU__COUNT
} menu_id;

/* What the graph's arrows are doing */
typedef enum {
	GRAPH_PAN = 0,
	GRAPH_TRACE, ///< Following a curve
	GRAPH_FREE,	 ///< A cursor anywhere on the plot
	GRAPH_ASK	 ///< CALC or zoom box, waiting for a point or a curve
} graph_state;

/* The CALC menu */
typedef enum {
	CALC_VALUE = 0,
	CALC_ZERO,
	CALC_MINIMUM,
	CALC_MAXIMUM,
	CALC_INTERSECT,
	CALC_DERIVATIVE,
	CALC_INTEGRAL,
	CALC_TANGENT,
	CALC_BOX ///< Not on the menu: ZOOM BOX asks for its corners the same way
} calc_kind;

/* What a number typed on the input row is for */
typedef enum {
	FIELD_NONE = 0,
	FIELD_WINDOW,
	FIELD_TRACE_X,
	FIELD_CALC,
	FIELD_TABLE_START,
	FIELD_TABLE_STEP,
	FIELD_STAT
} field_target;

/* A stat plot's kind */
typedef enum {
	STATPLOT_OFF = 0,
	STATPLOT_SCATTER,
	STATPLOT_XYLINE,
	STATPLOT_HISTOGRAM,
	STATPLOT_BOX,
	STATPLOT__COUNT
} statplot_type;

/* The ranges a parametric and a polar curve are taken over */
typedef struct {
	double t0, t1, tstep;
	double th0, th1, thstep;
} plot_ranges;

/* A row of the results page: a name and the value it can push */
#define RESULT_ROWS 16

typedef struct {
	char body[PLOT_BODY_MAX]; ///< Quadrate in x, and y for a surface; empty for an unused slot
	bool on;				  ///< Drawn by GRAPH
	bool broken;			  ///< Would not declare, so there is no word to plot
	qdos_graph_shape shape;
} plot_slot;

/** @brief L1 to L6 as words, each knowing which it is */
typedef struct {
	struct qdos_shell* sh;
	int index;
} list_word;

/** @brief One registered app word, and what running it needs to know */
typedef struct {
	char name[QDOS_PROGRAM_NAME_MAX];
	struct qdos_shell* sh;
} app_word;

struct qdos_shell {
	qdos_hal* hal;	   ///< Borrowed, not owned
	qd_interp* interp; ///< Owned
	qdos_console con;

	qdos_mode mode;

	char entry[ENTRY_MAX]; ///< Number being typed, not yet on the stack
	size_t entry_len;

	char input[INPUT_MAX]; ///< Line being typed in QDOS_MODE_LINE

	qdos_wordlist list;
	qdos_program_entry apps[QDOS_WORDLIST_MAX];
	size_t app_count;

	/** @brief What each app word was registered with; the interpreter keeps
	 * the pointer, so it has to outlive the registration */
	app_word app_word[QDOS_APPS_MAX];
	size_t app_word_count;

	/** @brief Words declared at the prompt rather than read off the card.
	 * They are in memory only, so `forget` has nothing to erase for them. */
	char line_word[QDOS_LINE_WORDS][QDOS_PROGRAM_NAME_MAX];
	size_t line_word_count;
	bool list_all; ///< Every word, rather than just the installed programs
	size_t list_sel;
	size_t list_top;
	qdos_mode list_from;
	qdos_mode about_from; ///< Mode to return to
	qdos_mode page_from;  ///< Where settings and debug were opened from

	char log[LOG_LINES][QDOS_COLS + 1];
	size_t log_count; ///< Lines ever written, so the ring can be read in order
	size_t log_top;	  ///< First line shown on the debug page

	size_t setting_sel;
	int decimals; ///< DECIMALS_AUTO, or how many to show after the point

	qdos_value undo[QDOS_REGISTER_MAX];
	size_t undo_depth;
	bool undo_ready;
	bool delete_armed; ///< One press of backspace has already asked
	bool drop_armed;   ///< One press of ESC has already asked, in the editor
	bool powering_off; ///< Set by the power key, acted on by the run loop
	bool usb_exported; ///< The inbox is currently a PC's to write to

	enum {
		REGISTER_IDLE = 0,
		REGISTER_STORING,
		REGISTER_RECALLING
	} register_wait;

	qdos_natives natives; ///< Uploaded code, and its words in the vocabulary

	qdos_editor ed;
	size_t ed_top; ///< First visible line
	size_t input_len;
	size_t input_cursor;

	char message[MESSAGE_COLS + 1]; ///< Error or status under the stack
	bool message_is_error;
	char status[16]; ///< What the status band last showed, to know when it is stale

	/* What is being plotted: one word, or every Y= slot switched on */
	char graph_words[PLOT_SLOTS][QDOS_PROGRAM_NAME_MAX];
	qdos_graph_shape graph_shape[PLOT_SLOTS]; ///< A curve in x, or a parametric or polar one
	int graph_count;
	int graph_curve; ///< Which of them trace is on
	qdos_graph_view graph_view;
	qdos_graph_samples graph_samples[PLOT_SLOTS];
	qdos_graph_points graph_points[PLOT_SLOTS]; ///< For the parametric and polar ones
	bool graph_stale;							///< The window moved in x, so the samples are old
	bool graph_points_stale;					///< The t or theta range changed
	bool graph_fit_pending;						///< Scale y to the first samples taken
	graph_state graph_state;
	double graph_x;						///< Where trace is, on a curve in x
	int graph_index;					///< Where trace is, on a parametric or polar one
	int graph_free_col, graph_free_row; ///< The free cursor, in pixels of the plot
	char graph_error[MESSAGE_COLS + 1]; ///< Why the last sample had no value
	bool graph_statplot;				///< The stat plot is drawn with the curves

	/* CALC and ZOOM BOX, part-way through asking */
	calc_kind calc;
	int calc_step;
	int calc_curves[2]; ///< The curves an intersection is between
	double calc_bound[2];
	char graph_result[MESSAGE_COLS + 1]; ///< What the last CALC found, in the readout until the next key
	bool shade_on;						 ///< The area the last integral found
	int shade_curve;
	double shade_a, shade_b;
	bool tangent_on; ///< The line the last TANGENT drew
	double tangent_x, tangent_y, tangent_slope;

	/* The window GRAPH on Y= draws in, kept across a restart; the graph's own
	 * is a copy of it while it is open */
	qdos_graph_view plot_view;
	plot_ranges ranges;
	bool grid_on;
	bool axes_off;

	/* A number being typed on the input row, and what it is for */
	field_target field;
	char field_text[ENTRY_MAX];
	size_t field_len;
	char field_prompt[24];
	qdos_mode field_mode; ///< Where it was opened, which it is cancelled back to

	menu_id menu;
	size_t menu_sel;
	size_t menu_top;
	qdos_mode menu_from;

	qdos_mode window_from;
	size_t window_sel;
	size_t window_top;

	qdos_mode table_from;
	char table_words[PLOT_SLOTS][QDOS_PROGRAM_NAME_MAX];
	int table_count;
	int table_first; ///< First curve shown, when there are more than fit across
	double table_start, table_step;
	int table_sel; ///< Selected row, from the top of the page

	double lists[STAT_LISTS][STAT_LIST_MAX];
	size_t list_len[STAT_LISTS];
	list_word list_word[STAT_LISTS];
	int stat_col;	 ///< Which list the cursor is in
	size_t stat_row; ///< Which element; at list_len it is the empty one after the last
	int stat_left;	 ///< First list on screen
	size_t stat_top; ///< First element on screen
	bool clear_armed;

	statplot_type statplot;
	int statplot_x, statplot_y; ///< Which lists it draws
	qdos_mode stat_from;

	char result_title[QDOS_COLS + 1];
	char result_name[RESULT_ROWS][12];
	double result_value[RESULT_ROWS];
	bool result_pushable[RESULT_ROWS];
	int result_count;
	size_t result_sel;
	size_t result_top;
	size_t output_first; ///< Log line the last evaluation's output starts at, counted since boot
	size_t output_count;
	size_t output_top;
	qdos_mode output_from;

	int result_model; ///< The regression found, for TO Y; -1 for none
	double result_coef[3];
	qdos_surface graph_surface; ///< Sampled once; turning it only redraws
	qdos_surface_view graph3_view;
	qdos_mode graph_return; ///< Where ESC from a graph goes: Y=, if it came from there

	plot_slot slots[PLOT_SLOTS];
	size_t slot_sel;

	bool cursor_on; ///< Which half of the blink the cursor is in

	size_t auto_off; ///< Index into AUTO_OFF_MINUTES
	bool off_warned; ///< The warning is already on screen
};

/** @brief How long the machine may sit idle, in ms; nought means never */
static uint32_t auto_off_ms(const qdos_shell* sh) {
	// Nothing to come back to a half-finished transfer for, so a shared card
	// keeps the machine awake however long it is left.
	if (sh->usb_exported) {
		return 0;
	}

	return (uint32_t)AUTO_OFF_MINUTES[sh->auto_off] * 60u * 1000u;
}

/** @brief A keypad with one face reports none, and costs no space */
static char modifier_char(const qdos_shell* sh) {
	if (sh->hal->modifier == NULL) {
		return '\0';
	}

	switch (sh->hal->modifier(sh->hal)) {
	case QDOS_MOD_ALPHA:
		return 'A';
	case QDOS_MOD_SYMBOL:
		return '#';
	default:
		return '\0';
	}
}

/** @brief Is there a cursor on screen, and therefore something to blink? */
static bool has_cursor(const qdos_shell* sh) {
	if (sh->field != FIELD_NONE) {
		return sh->message[0] == '\0';
	}
	if (sh->mode == QDOS_MODE_EDIT) {
		return true;
	}

	// Everywhere else the caret is on the input row, which a message takes
	// over. The pages -- list, settings, debug, about -- have no caret at all.
	if (sh->mode == QDOS_MODE_CALC || sh->mode == QDOS_MODE_LINE || sh->mode == QDOS_MODE_PLOT_EDIT) {
		return sh->message[0] == '\0';
	}

	return false;
}

/** @brief Append text to the input line, silently ignoring overflow */
static void input_append(qdos_shell* sh, const char* text) {
	const size_t len = strlen(text);
	if (sh->input_len + len >= INPUT_MAX) {
		return;
	}

	memmove(sh->input + sh->input_cursor + len, sh->input + sh->input_cursor, sh->input_len - sh->input_cursor);
	memcpy(sh->input + sh->input_cursor, text, len);
	sh->input_len += len;
	sh->input_cursor += len;
	sh->input[sh->input_len] = '\0';
}

static void input_backspace(qdos_shell* sh) {
	if (sh->input_cursor == 0) {
		return;
	}

	memmove(sh->input + sh->input_cursor - 1, sh->input + sh->input_cursor, sh->input_len - sh->input_cursor);
	sh->input_cursor--;
	sh->input_len--;
	sh->input[sh->input_len] = '\0';
}

static void input_clear(qdos_shell* sh) {
	sh->input_len = 0;
	sh->input_cursor = 0;
	sh->input[0] = '\0';
}

/*
 * Every evaluation goes through here. A type error in lib/rt -- `1.5 2.5 and`,
 * a string where a number was wanted -- is fatal and would end the process, so
 * recovery is armed and the runtime longjmps back instead. The stack may have
 * been left part-way through the failed word, which is the documented price.
 */

/** @brief Append one line, wrapped to the panel, to the ring */
static void log_line(qdos_shell* sh, const char* text, size_t len) {
	while (len > 0) {
		const size_t take = (len > QDOS_COLS) ? (size_t)QDOS_COLS : len;
		char* slot = sh->log[sh->log_count % LOG_LINES];
		memcpy(slot, text, take);
		slot[take] = '\0';
		sh->log_count++;
		text += take;
		len -= take;
	}
}

/** @brief Record whatever a program printed, or anything worth reading later */
static void log_add(qdos_shell* sh, const char* text) {
	if (text == NULL || text[0] == '\0') {
		return;
	}

	const char* start = text;
	for (const char* p = text;; p++) {
		if (*p != '\n' && *p != '\0') {
			continue;
		}
		if (p > start) {
			log_line(sh, start, (size_t)(p - start));
		}
		if (*p == '\0') {
			break;
		}
		start = p + 1;
	}
}

/** @brief The i-th line still in the ring, oldest first, or NULL */
static const char* log_at(const qdos_shell* sh, size_t index) {
	const size_t held = (sh->log_count < LOG_LINES) ? sh->log_count : LOG_LINES;
	if (index >= held) {
		return NULL;
	}
	const size_t first = sh->log_count - held;
	return sh->log[(first + index) % LOG_LINES];
}

static size_t log_held(const qdos_shell* sh) {
	return (sh->log_count < LOG_LINES) ? sh->log_count : LOG_LINES;
}

/**
 * @brief A value as the settings say to show it, in @p room columns
 *
 * Too wide for the row, a number goes to exponent form: there is no end of one
 * that is safe to drop.
 */
static void format_value(const qdos_shell* sh, const qd_interp_value* value, char* out, size_t cap, size_t room) {
	const bool number = value->type == QD_INTERP_VALUE_INT || value->type == QD_INTERP_VALUE_FLOAT;
	const double shown = (value->type == QD_INTERP_VALUE_INT) ? (double)value->i : value->f;

	if (sh->decimals == DECIMALS_AUTO || !number) {
		snprintf(out, cap, "%s", value->text);
	} else {
		// A fixed number of decimals is a column to read down, so whole numbers
		// get them too rather than jumping about
		snprintf(out, cap, "%.*f", sh->decimals, shown);
	}

	if (!number || strlen(out) <= room) {
		return;
	}

	for (int digits = 9; digits >= 0; digits--) {
		snprintf(out, cap, "%.*e", digits, shown);
		if (strlen(out) <= room) {
			return;
		}
	}
}

static const char* array_type_name(qd_array_type type) {
	switch (type) {
	case QD_ARRAY_TYPE_INT:
		return "i64";
	case QD_ARRAY_TYPE_FLOAT:
		return "f64";
	case QD_ARRAY_TYPE_STR:
		return "str";
	case QD_ARRAY_TYPE_PTR:
		return "ptr";
	case QD_ARRAY_TYPE_ANY:
		return "any";
	}
	return "?";
}

/**
 * @brief An array written out, rather than as the address it is
 *
 * qd_interp_peek renders a pointer as its address, which says nothing on a
 * calculator and is a different number every run. The raw element is the only
 * way to the array behind it.
 *
 * @param index Depth from the top, counted the way qd_interp_peek counts
 * @param room Columns the row has left
 * @return false where that element is not an array, leaving @p out untouched
 */
static bool format_array(const qdos_shell* sh, size_t index, char* out, size_t cap, size_t room) {
	const qd_stack* st = qd_interp_context(sh->interp)->st;
	if (cap < 8 || index >= (size_t)st->size) {
		return false;
	}

	const qd_stack_element_t* element = &st->data[st->size - 1 - index];
	if (element->type != QD_STACK_TYPE_PTR || !qd_array_is_valid(element->value.p)) {
		return false;
	}

	const qd_array_t* array = (const qd_array_t*)element->value.p;
	const size_t length = qd_array_length(array);

	size_t used = 0;
	out[used++] = '[';

	for (size_t i = 0; i < length; i++) {
		char item[32];
		int64_t whole = 0;
		double real = 0.0;

		if (array->elemType == QD_ARRAY_TYPE_INT && qd_array_get_int(array, i, &whole) == 0) {
			snprintf(item, sizeof(item), "%lld", (long long)whole);
		} else if (array->elemType == QD_ARRAY_TYPE_FLOAT && qd_array_get_float(array, i, &real) == 0) {
			snprintf(item, sizeof(item), "%g", real);
		} else {
			// A string, or an array of its own: a structure needing a row it
			// is not going to get here
			snprintf(item, sizeof(item), "..");
		}

		const size_t width = strlen(item) + ((i > 0) ? 1 : 0);
		if (used + width + 2 > cap) {
			break;
		}
		if (i > 0) {
			out[used++] = ' ';
		}
		memcpy(out + used, item, strlen(item));
		used += strlen(item);
	}

	out[used++] = ']';
	out[used] = '\0';

	// Wider than the row: what shape it is, which is what a row this size can
	// usefully say about it
	if (used > room) {
		snprintf(out, cap, "[%zu %s]", length, array_type_name(array->elemType));
	}
	return true;
}

static void set_message(qdos_shell* sh, const char* text, bool is_error) {
	snprintf(sh->message, sizeof(sh->message), "%s", text ? text : "");
	sh->message_is_error = is_error;
	if (is_error) {
		log_add(sh, sh->message);
	}
}

/** @brief Is the line ready to evaluate, or still open? */
static bool input_is_complete(const qdos_shell* sh) {
	int depth = 0;
	bool in_string = false;

	for (size_t i = 0; i < sh->input_len; i++) {
		const char ch = sh->input[i];

		if (in_string) {
			if (ch == '\\' && i + 1 < sh->input_len) {
				i++; // an escaped character is never a delimiter
			} else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		switch (ch) {
		case '"':
			in_string = true;
			break;
		case '{':
		case '(':
		case '[':
			depth++;
			break;
		case '}':
		case ')':
		case ']':
			depth--;
			break;
		default:
			break;
		}
	}

	return depth <= 0;
}

/**
 * @brief Say what the last eval declared
 *
 * A word written on the line lives in memory only. The card holds programs,
 * which are put there by `edit` or by uploading them, and scratch work at the
 * prompt is not that -- it would otherwise fill the store with every `sq` ever
 * tried, each one a row in APPS.
 */
static void report_declaration(qdos_shell* sh) {
	const char* name = qd_interp_last_declared(sh->interp);
	if (!name) {
		set_message(sh, "", false);
		return;
	}

	bool known = false;
	for (size_t i = 0; i < sh->line_word_count; i++) {
		known = known || strcmp(sh->line_word[i], name) == 0;
	}

	if (!known && sh->line_word_count < QDOS_LINE_WORDS) {
		snprintf(sh->line_word[sh->line_word_count++], QDOS_PROGRAM_NAME_MAX, "%s", name);
	}

	char message[80];
	snprintf(message, sizeof(message), "DECLARED '%.20s'", name);
	set_message(sh, message, false);
}

/** @brief Whether this word was written at the prompt, and so is not on the card */
static bool declared_here(qdos_shell* sh, const char* name) {
	for (size_t i = 0; i < sh->line_word_count; i++) {
		if (strcmp(sh->line_word[i], name) == 0) {
			return true;
		}
	}
	return false;
}

static void forget_here(qdos_shell* sh, const char* name) {
	for (size_t i = 0; i < sh->line_word_count; i++) {
		if (strcmp(sh->line_word[i], name) != 0) {
			continue;
		}
		memcpy(sh->line_word[i], sh->line_word[--sh->line_word_count], QDOS_PROGRAM_NAME_MAX);
		return;
	}
}

/** A soft key's label, and the key it stands for in this mode */
typedef struct {
	const char* label;
	qdos_key key;
} soft_key;

static const soft_key SOFT[QDOS_MODE__COUNT][SOFT_KEYS] = {
		// Turning off is the PWR key's job: the one action on the row that cannot be
		// undone by pressing it again. The last slot's label is the setting itself.
		// PLOT where CLR was: that did what the ESC key under it does already.
		// Four letters, like the rest, or it runs into APPS beside it.
		[QDOS_MODE_CALC] = {{"PLOT", QDOS_KEY_GRAPH}, {"APPS", QDOS_KEY_LIST}, {"CAT", QDOS_KEY_CATALOG},
				{"INFO", QDOS_KEY_ABOUT}, {"", QDOS_KEY_ANGLE}},
		[QDOS_MODE_LINE] = {{"ESC", QDOS_KEY_CLEAR}, {"APPS", QDOS_KEY_LIST}, {"COMP", QDOS_KEY_TAB},
				{"CAT", QDOS_KEY_CATALOG}, {"", QDOS_KEY_ANGLE}},
		// No arrows here or below: the keypad has its own
		[QDOS_MODE_LIST] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}, {"PICK", QDOS_KEY_ENTER},
				{"EDIT", QDOS_KEY_OPEN}},
		// ESC, as everywhere else: DROP is a keypad word that empties the stack
		[QDOS_MODE_EDIT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"CHECK", QDOS_KEY_CHECK},
				{"", QDOS_KEY_NONE}, {"SAVE", QDOS_KEY_SAVE}},
		[QDOS_MODE_ABOUT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"SET", QDOS_KEY_SETTINGS},
				{"LOG", QDOS_KEY_DEBUG}, {"", QDOS_KEY_NONE}},
		[QDOS_MODE_SETTINGS] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE},
				{"CHG", QDOS_KEY_ENTER}, {"LOG", QDOS_KEY_DEBUG}},
		// FIT and STD are on ZOOM, with the rest of the windows. A five-letter
		// label fills its slot, so it goes last or beside a short one: TRACE
		// and GRAPH on the right, where a TI has them.
		[QDOS_MODE_GRAPH] = {{"ESC", QDOS_KEY_CLEAR}, {"ZOOM", QDOS_KEY_ZOOM}, {"CALC", QDOS_KEY_CALC},
				{"MENU", QDOS_KEY_MENU}, {"TRACE", QDOS_KEY_TRACE}},
		[QDOS_MODE_PLOT] = {{"ESC", QDOS_KEY_CLEAR}, {"EDIT", QDOS_KEY_OPEN}, {"ON", QDOS_KEY_TOGGLE},
				{"MENU", QDOS_KEY_MENU}, {"GRAPH", QDOS_KEY_GRAPH}},
		// The variables on keys of their own: the letters are otherwise ALPHA
		// away, and a body is mostly them. TAB and ENTER have keys already.
		[QDOS_MODE_PLOT_EDIT] = {{"ESC", QDOS_KEY_CLEAR}, {"x", QDOS_KEY_VAR_X}, {"y", QDOS_KEY_VAR_Y},
				{"t", QDOS_KEY_VAR_T}, {"theta", QDOS_KEY_VAR_THETA}},
		[QDOS_MODE_MENU] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}, {"PICK", QDOS_KEY_ENTER},
				{"", QDOS_KEY_NONE}},
		[QDOS_MODE_WINDOW] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE},
				{"GRAPH", QDOS_KEY_GRAPH}},
		[QDOS_MODE_TABLE] = {{"ESC", QDOS_KEY_CLEAR}, {"START", QDOS_KEY_TBL_START}, {"", QDOS_KEY_NONE},
				{"STEP", QDOS_KEY_TBL_STEP}, {"GRAPH", QDOS_KEY_GRAPH}},
		[QDOS_MODE_STAT] = {{"ESC", QDOS_KEY_CLEAR}, {"CALC", QDOS_KEY_CALC}, {"PLOT", QDOS_KEY_STATPLOT},
				{"CLR", QDOS_KEY_LIST_CLEAR}, {"GRAPH", QDOS_KEY_GRAPH}},
		[QDOS_MODE_OUTPUT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE},
				{"", QDOS_KEY_NONE}},
		[QDOS_MODE_RESULT] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"PUSH", QDOS_KEY_ENTER},
				{"TO Y", QDOS_KEY_TO_Y}, {"", QDOS_KEY_NONE}},
		[QDOS_MODE_GRAPH3] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE}, {"STD", QDOS_KEY_STD},
				{"", QDOS_KEY_NONE}},
		[QDOS_MODE_DEBUG] = {{"ESC", QDOS_KEY_CLEAR}, {"", QDOS_KEY_NONE}, {"", QDOS_KEY_NONE},
				{"CLR", QDOS_KEY_BACKSPACE}, {"SET", QDOS_KEY_SETTINGS}},
};

static const char* angle_text(void);
static const qdos_native_entry* list_module(const qdos_shell* sh, size_t i);
static void register_apps(qdos_shell* sh, qd_interp* interp);

static void render_soft(qdos_shell* sh, qdos_console* con) {
	for (int i = 0; i < SOFT_KEYS; i++) {
		const char* label = SOFT[sh->mode][i].label;

		if (SOFT[sh->mode][i].key == QDOS_KEY_ANGLE) {
			label = angle_text();
		}

		// Two jobs, so it names whichever is next
		if (sh->mode == QDOS_MODE_LINE && SOFT[sh->mode][i].key == QDOS_KEY_CLEAR) {
			label = (sh->input_len > 0) ? "CLR" : "ESC";
		}

		if (label[0] == '\0') {
			continue;
		}

		// Only a program can be edited
		if (sh->mode == QDOS_MODE_LIST && SOFT[sh->mode][i].key == QDOS_KEY_OPEN &&
				(sh->list_all || list_module(sh, sh->list_sel) != NULL)) {
			continue;
		}

		const int width = (int)strlen(label);
		qdos_console_puts(con, i * SOFT_WIDTH + (SOFT_WIDTH - width) / 2, ROW_SOFT, label);
	}
}

static int push_value(qd_context* ctx, const qdos_value* value);

/** @brief Take what an evaluation printed and put it where it can be seen */
static void absorb_output(qdos_shell* sh) {
	const char* printed = qdos_guarded_output();
	if (printed[0] == '\0') {
		return;
	}

	const size_t before = sh->log_count;
	log_add(sh, printed);
	const size_t held = log_held(sh);
	if (sh->log_count - before > 1) {
		sh->output_first = before;
		sh->output_count = sh->log_count - before;
		sh->output_top = 0;
		sh->output_from = sh->mode;
		sh->mode = QDOS_MODE_OUTPUT;
	} else if (held > 0) {
		set_message(sh, log_at(sh, held - 1), false);
	}
}

/* One step back, which is all a calculator ever offers */
static void undo_snapshot(qdos_shell* sh) {
	const size_t depth = qd_interp_depth(sh->interp);
	sh->undo_depth = (depth > QDOS_REGISTER_MAX) ? (size_t)QDOS_REGISTER_MAX : depth;

	for (size_t i = 0; i < sh->undo_depth; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value)) {
			sh->undo_depth = i;
			break;
		}

		qdos_value* slot = &sh->undo[i];
		if (value.type == QD_INTERP_VALUE_INT) {
			slot->type = QDOS_VALUE_INT;
			slot->i = value.i;
		} else if (value.type == QD_INTERP_VALUE_FLOAT) {
			slot->type = QDOS_VALUE_FLOAT;
			slot->f = value.f;
		} else {
			slot->type = QDOS_VALUE_STRING;
			snprintf(slot->s, sizeof(slot->s), "%s", value.text);
		}
	}
	sh->undo_ready = true;
}

static void undo_restore(qdos_shell* sh) {
	if (!sh->undo_ready) {
		set_message(sh, "NOTHING TO UNDO", false);
		return;
	}

	qd_context* ctx = qd_interp_context(sh->interp);
	qdos_guarded_eval(sh->interp, "clear");

	// Snapshots run top-first, so put them back the other way round
	for (size_t i = sh->undo_depth; i > 0; i--) {
		push_value(ctx, &sh->undo[i - 1]);
	}

	sh->undo_ready = false;
	set_message(sh, "UNDONE", false);
}

static bool infix_operator(char ch) {
	return ch == '+' || ch == '-' || ch == '*' || ch == '/';
}

/** @brief Is the whole of this the way a number is written? */
static bool infix_number(const char* text, size_t len) {
	bool digit = false;
	bool point = false;

	for (size_t i = 0; i < len; i++) {
		if (text[i] >= '0' && text[i] <= '9') {
			digit = true;
		} else if (text[i] == '.' && !point) {
			point = true;
		} else if (text[i] != '-' || i != 0) {
			return false;
		}
	}
	return digit;
}

/**
 * @brief Catch a line written the way it is said aloud
 *
 * `5 - 3` and `5-3` are both valid Quadrate and neither is two. See
 * docs/design.md.
 *
 * @return true when @p out holds what to say instead
 */
static bool infix_hint(const char* line, size_t len, char* out, size_t cap) {
	const char* token[4];
	size_t token_len[4];
	size_t count = 0;

	for (size_t i = 0; i < len;) {
		while (i < len && line[i] == ' ') {
			i++;
		}
		if (i >= len) {
			break;
		}
		if (count == 4) {
			return false; // more on the line than this mistake is made of
		}
		token[count] = line + i;
		while (i < len && line[i] != ' ') {
			i++;
		}
		token_len[count] = (size_t)(line + i - token[count]);
		count++;
	}

	const char* left = NULL;
	const char* right = NULL;
	size_t left_len = 0;
	size_t right_len = 0;
	char op = 0;

	if (count == 3 && token_len[1] == 1 && infix_operator(token[1][0])) {
		left = token[0];
		left_len = token_len[0];
		right = token[2];
		right_len = token_len[2];
		op = token[1][0];
	} else if (count == 1) {
		// Never the first character, which is a sign rather than an operator
		for (size_t i = 1; i + 1 < token_len[0]; i++) {
			if (!infix_operator(token[0][i])) {
				continue;
			}
			left = token[0];
			left_len = i;
			right = token[0] + i + 1;
			right_len = token_len[0] - i - 1;
			op = token[0][i];
			break;
		}
	}

	if (op == 0 || !infix_number(left, left_len) || !infix_number(right, right_len)) {
		return false;
	}

	const int shown = 6;
	snprintf(out, cap, "RPN: TRY %.*s %.*s %c", (int)(left_len < (size_t)shown ? left_len : (size_t)shown), left,
			(int)(right_len < (size_t)shown ? right_len : (size_t)shown), right, op);
	return true;
}

/** @brief Evaluate the input line and report the outcome */
static void submit(qdos_shell* sh) {
	if (sh->input_len == 0) {
		return;
	}

	// Caught before anything on the stack moves
	char hint[QDOS_COLS + 1];
	if (infix_hint(sh->input, sh->input_len, hint, sizeof(hint))) {
		set_message(sh, hint, true);
		return;
	}

	// A word may take over the screen, and then the line and the message it
	// left are its business, not ours
	const qdos_mode before = sh->mode;

	undo_snapshot(sh);
	if (qdos_guarded_eval(sh->interp, sh->input)) {
		if (sh->mode == before) {
			report_declaration(sh);
		}
	} else {
		set_message(sh, qdos_guarded_error(sh->interp), true);

		// The line stays, to be corrected rather than typed again
		if (sh->mode == before) {
			return;
		}
	}

	// What a program printed outranks whatever the shell had to say
	if (sh->mode == before) {
		absorb_output(sh);
	}

	input_clear(sh);
}

/** @brief Complete the word being typed, or report why nothing happened */
static void complete_word(qdos_shell* sh) {
	const size_t n = qdos_complete_prefix_len(sh->input, sh->input_len);
	if (n == 0) {
		return;
	}

	char prefix[QDOS_WORD_MAX];
	memcpy(prefix, sh->input + sh->input_len - n, n);
	prefix[n] = '\0';

	qdos_completion done;
	qdos_complete(sh->interp, prefix, &done);

	if (done.matches == 0) {
		set_message(sh, "NO MATCH", false);
		return;
	}

	// The shared prefix is always safe to type for the user, however many
	// words matched. Only when it adds nothing is the list worth showing.
	if (strlen(done.common) > n) {
		input_append(sh, done.common + n);
	}

	if (done.matches == 1) {
		input_append(sh, " ");
		set_message(sh, "", false);
	} else {
		set_message(sh, done.listing, false);
	}
}

/** @brief Translate a key press into an edit or an action */
/** Append to the pending number, ignoring overflow. */
static void entry_append(qdos_shell* sh, char ch) {
	if (sh->entry_len + 1 >= ENTRY_MAX) {
		return;
	}
	sh->entry[sh->entry_len++] = ch;
	sh->entry[sh->entry_len] = '\0';
}

static void entry_clear(qdos_shell* sh) {
	sh->entry_len = 0;
	sh->entry[0] = '\0';
}

/**
 * @brief Put the pending number on the stack
 * @return false if the entry would not parse
 */
static bool entry_commit(qdos_shell* sh) {
	if (sh->entry_len == 0) {
		return true;
	}

	// Quadrate wants a digit on each side of the point; a keypad does not
	char text[ENTRY_MAX + 2];
	const char* p = sh->entry;
	size_t len = 0;
	if (*p == '-') {
		text[len++] = *p++;
	}
	if (*p == '.') {
		text[len++] = '0';
	}
	memcpy(text + len, p, strlen(p) + 1);
	len += strlen(p);
	if (text[len - 1] == '.') {
		text[len++] = '0';
		text[len] = '\0';
	}

	undo_snapshot(sh);
	if (!qdos_guarded_eval(sh->interp, text)) {
		set_message(sh, qdos_guarded_error(sh->interp), true);
		return false;
	}
	entry_clear(sh);
	return true;
}

/** @brief Apply a word to the stack, committing any pending number first */
static void apply_word(qdos_shell* sh, const char* word) {
	if (!entry_commit(sh)) {
		return;
	}
	undo_snapshot(sh);
	if (qdos_guarded_eval(sh->interp, word)) {
		set_message(sh, "", false);
		absorb_output(sh);
	} else {
		set_message(sh, qdos_guarded_error(sh->interp), true);
	}
}

static void enter_line_mode(qdos_shell* sh) {
	// Commit first, or the digits already typed are lost.
	entry_commit(sh);
	sh->mode = QDOS_MODE_LINE;
	input_clear(sh);

	// No announcement: it would sit on the input line and hide the ':' prompt,
	// which is what says the mode changed. The soft row already labels ESC.
	set_message(sh, "", false);
}

static void leave_line_mode(qdos_shell* sh) {
	sh->mode = QDOS_MODE_CALC;
	input_clear(sh);
	set_message(sh, "", false);
}

/** Keys while the keypad is a calculator. */
/* In QDOS_KEY_FN_FIRST..QDOS_KEY_FN_LAST order */
static const char* const FUNCTION_WORD[] = {
		"sin",
		"cos",
		"tan",
		"ln",
		"log",
		"sqrt",
		"sq",
		"pow",
		"inv",
		"abs",
		"floor",
		"ceil",
		"round",
		"mod",
		"rot",
		"over",
};

/* In QDOS_KEY_FN2_FIRST..QDOS_KEY_FN2_LAST order, the run added at the end */
static const char* const FUNCTION_WORD2[] = {
		"asin",
		"acos",
		"atan",
		"exp",
};

static const char* function_word(qdos_key key) {
	if (key >= QDOS_KEY_FN_FIRST && key <= QDOS_KEY_FN_LAST) {
		return FUNCTION_WORD[key - QDOS_KEY_FN_FIRST];
	}
	if (key >= QDOS_KEY_FN2_FIRST && key <= QDOS_KEY_FN2_LAST) {
		return FUNCTION_WORD2[key - QDOS_KEY_FN2_FIRST];
	}
	return NULL;
}

/* Flips the sign of the number being typed, or negates x when none is */
static void entry_negate(qdos_shell* sh) {
	if (sh->entry_len == 0) {
		apply_word(sh, "neg");
		return;
	}

	if (sh->entry[0] == '-') {
		memmove(sh->entry, sh->entry + 1, sh->entry_len);
		sh->entry_len--;
	} else if (sh->entry_len + 1 < ENTRY_MAX) {
		memmove(sh->entry + 1, sh->entry, sh->entry_len + 1);
		sh->entry[0] = '-';
		sh->entry_len++;
	}
}

/** @brief Nought to nine; the other ninety need `sto` and `rcl` written out */
static bool register_digit(qdos_shell* sh, const qdos_key_event* ev) {
	if (sh->register_wait == REGISTER_IDLE) {
		return false;
	}

	const bool storing = (sh->register_wait == REGISTER_STORING);
	sh->register_wait = REGISTER_IDLE;

	if (ev->key < QDOS_KEY_0 || ev->key > QDOS_KEY_9) {
		set_message(sh, "CANCELLED", false);
		return true;
	}

	char line[16];
	snprintf(line, sizeof(line), "%d %s", (int)(ev->key - QDOS_KEY_0), storing ? "sto" : "rcl");

	undo_snapshot(sh);
	if (!qdos_guarded_eval(sh->interp, line)) {
		set_message(sh, qdos_guarded_error(sh->interp), true);
		return true;
	}

	char message[QDOS_COLS + 1];
	snprintf(message, sizeof(message), "%s %d", storing ? "STORED" : "RECALLED", (int)(ev->key - QDOS_KEY_0));
	set_message(sh, message, false);
	return true;
}

static void handle_calc_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (register_digit(sh, ev)) {
		return;
	}

	const char* word = function_word(ev->key);
	if (word != NULL) {
		apply_word(sh, word);
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_0:
		entry_append(sh, '0');
		break;
	case QDOS_KEY_1:
		entry_append(sh, '1');
		break;
	case QDOS_KEY_2:
		entry_append(sh, '2');
		break;
	case QDOS_KEY_3:
		entry_append(sh, '3');
		break;
	case QDOS_KEY_4:
		entry_append(sh, '4');
		break;
	case QDOS_KEY_5:
		entry_append(sh, '5');
		break;
	case QDOS_KEY_6:
		entry_append(sh, '6');
		break;
	case QDOS_KEY_7:
		entry_append(sh, '7');
		break;
	case QDOS_KEY_8:
		entry_append(sh, '8');
		break;
	case QDOS_KEY_9:
		entry_append(sh, '9');
		break;
	case QDOS_KEY_DOT:
		entry_append(sh, '.');
		break;

	case QDOS_KEY_ADD:
		apply_word(sh, "+");
		break;
	case QDOS_KEY_SUB:
		apply_word(sh, "-");
		break;
	case QDOS_KEY_MUL:
		apply_word(sh, "*");
		break;
	// A calculator divides rather than truncating; the language keeps "/"
	case QDOS_KEY_DIV:
		apply_word(sh, "divide");
		break;
	case QDOS_KEY_DUP:
		apply_word(sh, "dup");
		break;
	case QDOS_KEY_DROP:
		apply_word(sh, "drop");
		break;
	case QDOS_KEY_SWAP:
		apply_word(sh, "swap");
		break;
	case QDOS_KEY_NEG:
		entry_negate(sh);
		break;
	case QDOS_KEY_UNDO:
		undo_restore(sh);
		break;

	// Commit first, or the register gets what was under the entry
	case QDOS_KEY_STO:
	case QDOS_KEY_RCL:
		if (!entry_commit(sh)) {
			break;
		}
		sh->register_wait = (ev->key == QDOS_KEY_STO) ? REGISTER_STORING : REGISTER_RECALLING;
		set_message(sh, ev->key == QDOS_KEY_STO ? "STO: WHICH? 0-9" : "RCL: WHICH? 0-9", false);
		break;

	// Bare Enter duplicates: how an RPN calculator squares a number.
	case QDOS_KEY_ENTER:
		if (sh->entry_len > 0) {
			if (entry_commit(sh)) {
				set_message(sh, "", false);
			}
		} else {
			apply_word(sh, "dup");
		}
		break;

	case QDOS_KEY_BACKSPACE:
		if (sh->entry_len > 0) {
			sh->entry[--sh->entry_len] = '\0';
		}
		break;

	case QDOS_KEY_CLEAR:
		if (sh->entry_len > 0) {
			entry_clear(sh);
		} else {
			undo_snapshot(sh);
			qdos_guarded_eval(sh->interp, "clear");
			set_message(sh, "STACK CLEARED", false);
		}
		break;

	case QDOS_KEY_CHAR:
		if (ev->ch == ':') {
			enter_line_mode(sh);
		} else if (ev->ch != ' ') {
			set_message(sh, "PRESS : TO TYPE A LINE", false);
		}
		break;

	default:
		break;
	}
}

/** Keys while whole lines of Quadrate are being typed. */
static void handle_line_key(qdos_shell* sh, const qdos_key_event* ev) {
	const char* word = function_word(ev->key);
	if (word != NULL) {
		input_append(sh, " ");
		input_append(sh, word);
		input_append(sh, " ");
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_0:
		input_append(sh, "0");
		break;
	case QDOS_KEY_1:
		input_append(sh, "1");
		break;
	case QDOS_KEY_2:
		input_append(sh, "2");
		break;
	case QDOS_KEY_3:
		input_append(sh, "3");
		break;
	case QDOS_KEY_4:
		input_append(sh, "4");
		break;
	case QDOS_KEY_5:
		input_append(sh, "5");
		break;
	case QDOS_KEY_6:
		input_append(sh, "6");
		break;
	case QDOS_KEY_7:
		input_append(sh, "7");
		break;
	case QDOS_KEY_8:
		input_append(sh, "8");
		break;
	case QDOS_KEY_9:
		input_append(sh, "9");
		break;
	case QDOS_KEY_DOT:
		input_append(sh, ".");
		break;

	case QDOS_KEY_ADD:
		input_append(sh, "+");
		break;
	case QDOS_KEY_SUB:
		input_append(sh, "-");
		break;
	case QDOS_KEY_MUL:
		input_append(sh, "*");
		break;
	// divide, as the calculator's key does; Quadrate's own / truncates
	case QDOS_KEY_DIV:
		input_append(sh, " divide ");
		break;
	case QDOS_KEY_DUP:
		input_append(sh, "dup");
		break;
	case QDOS_KEY_DROP:
		input_append(sh, "drop");
		break;
	case QDOS_KEY_SWAP:
		input_append(sh, "swap");
		break;
	case QDOS_KEY_NEG:
		input_append(sh, " neg ");
		break;

	case QDOS_KEY_STO:
		input_append(sh, " sto ");
		break;
	case QDOS_KEY_RCL:
		input_append(sh, " rcl ");
		break;

	case QDOS_KEY_CHAR:
		if (ev->ch) {
			const char text[2] = {ev->ch, '\0'};
			input_append(sh, text);
		}
		break;

	case QDOS_KEY_ENTER:
		if (input_is_complete(sh)) {
			submit(sh);
		} else {
			input_append(sh, "\n");
		}
		break;

	case QDOS_KEY_BACKSPACE:
		input_backspace(sh);
		break;

	case QDOS_KEY_TAB:
		complete_word(sh);
		break;

	case QDOS_KEY_LEFT:
		if (sh->input_cursor > 0) {
			sh->input_cursor--;
		}
		break;

	case QDOS_KEY_RIGHT:
		if (sh->input_cursor < sh->input_len) {
			sh->input_cursor++;
		}
		break;

	// The line first, the mode second, as CLR does in the calculator. A line
	// that survives a failed eval needs somewhere to be thrown away.
	case QDOS_KEY_CLEAR:
		if (sh->input_len > 0) {
			input_clear(sh);
		} else {
			leave_line_mode(sh);
		}
		break;

	default:
		break;
	}
}

static void edit_open(qdos_shell* sh, const char* name, const char* source);

#define LIST_ROWS (ROW_CONTENT_LAST - ROW_CONTENT_FIRST + 1)

/**
 * @brief Shared libraries, being what the programs under them are liable to call
 *
 * A module inside an app is that app's own half rather than something the card
 * offers, so it is loaded and callable but never listed: the row that matters
 * is the app.
 */
static size_t module_count(const qdos_shell* sh) {
	size_t n = 0;
	for (size_t i = 0; i < sh->natives.count; i++) {
		if (sh->natives.entry[i].app[0] == '\0') {
			n++;
		}
	}
	return n;
}

static size_t list_count(const qdos_shell* sh) {
	if (sh->list_all) {
		return sh->list.count;
	}
	return module_count(sh) + sh->app_count;
}

/** @brief The module a row shows, or NULL where the row is a program */
static const qdos_native_entry* list_module(const qdos_shell* sh, size_t i) {
	if (sh->list_all) {
		return NULL;
	}

	size_t at = 0;
	for (size_t n = 0; n < sh->natives.count; n++) {
		if (sh->natives.entry[n].app[0] != '\0') {
			continue;
		}
		if (at++ == i) {
			return &sh->natives.entry[n];
		}
	}
	return NULL;
}

/** @brief The program a row shows, or NULL where the row is a module */
static const qdos_program_entry* list_program(const qdos_shell* sh, size_t i) {
	if (sh->list_all || i < module_count(sh)) {
		return NULL;
	}

	const size_t at = i - module_count(sh);
	return (at < sh->app_count) ? &sh->apps[at] : NULL;
}

static const char* list_name(const qdos_shell* sh, size_t i) {
	if (sh->list_all) {
		return sh->list.name[i];
	}

	const qdos_native_entry* module = list_module(sh, i);
	if (module != NULL) {
		return module->name;
	}

	const qdos_program_entry* program = list_program(sh, i);
	return (program != NULL) ? program->name : "";
}

/** @brief A star is an override with a read-only copy underneath */
static const char* origin_text(bool system, bool inbox, bool user) {
	if (user) {
		return (system || inbox) ? "USER*" : "USER";
	}
	return inbox ? "CARD" : "SYS";
}

/** @brief Read a program from wherever it lives, nearest scope first */
static bool load_program_anywhere(qdos_shell* sh, const char* name, char* out, size_t cap) {
	static const qdos_store_scope ORDER[] = {QDOS_SCOPE_USER, QDOS_SCOPE_INBOX, QDOS_SCOPE_SYSTEM};

	for (size_t i = 0; i < sizeof(ORDER) / sizeof(*ORDER); i++) {
		if (qdos_program_load(sh->hal, ORDER[i], name, out, cap) == QDOS_STORE_OK) {
			return true;
		}
	}
	return false;
}

static void list_load(qdos_shell* sh) {
	if (sh->list_all) {
		qdos_wordlist_gather(sh->interp, &sh->list);
	} else {
		sh->app_count = qdos_programs_gather(sh->hal, sh->apps, QDOS_WORDLIST_MAX);
	}

	sh->list_sel = 0;
	sh->list_top = 0;
}

static void list_open_scoped(qdos_shell* sh, bool all) {
	sh->list_all = all;
	list_load(sh);
	sh->list_from = sh->mode;
	sh->mode = QDOS_MODE_LIST;
	set_message(sh, "", false);
}

static void list_open(qdos_shell* sh) {
	list_open_scoped(sh, false);
}

/* Every word, the way a TI-83 reaches the ones with no key of their own */
static void catalog_open(qdos_shell* sh) {
	list_open_scoped(sh, true);
}

static void list_scroll_into_view(qdos_shell* sh) {
	if (sh->list_sel < sh->list_top) {
		sh->list_top = sh->list_sel;
	} else if (sh->list_sel >= sh->list_top + LIST_ROWS) {
		sh->list_top = sh->list_sel - (LIST_ROWS - 1);
	}
}

static void handle_list_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (ev->key != QDOS_KEY_BACKSPACE) {
		sh->delete_armed = false;
	}

	switch (ev->key) {
	case QDOS_KEY_UP:
		if (sh->list_sel > 0) {
			sh->list_sel--;
		}
		list_scroll_into_view(sh);
		break;

	case QDOS_KEY_DOWN:
		if (sh->list_sel + 1 < list_count(sh)) {
			sh->list_sel++;
		}
		list_scroll_into_view(sh);
		break;

	case QDOS_KEY_LEFT:
		sh->list_sel = (sh->list_sel > LIST_ROWS) ? sh->list_sel - LIST_ROWS : 0;
		list_scroll_into_view(sh);
		break;

	case QDOS_KEY_RIGHT:
		sh->list_sel += LIST_ROWS;
		if (sh->list_sel >= list_count(sh)) {
			sh->list_sel = list_count(sh) ? list_count(sh) - 1 : 0;
		}
		list_scroll_into_view(sh);
		break;

	case QDOS_KEY_TAB:
		sh->list_all = !sh->list_all;
		list_load(sh);
		break;

	// Typing a letter jumps to it, which is how a catalog of 100 is usable
	case QDOS_KEY_CHAR: {
		const char want = (ev->ch >= 'A' && ev->ch <= 'Z') ? (char)(ev->ch + 32) : ev->ch;
		for (size_t i = 0; i < list_count(sh); i++) {
			if (list_name(sh, i)[0] == want) {
				sh->list_sel = i;
				sh->list_top = i; // the match at the top, with its neighbours under it
				list_scroll_into_view(sh);
				break;
			}
		}
		break;
	}

	case QDOS_KEY_ENTER:
		// Picking types the name, leaving the user to run or edit it
		sh->mode = QDOS_MODE_LINE;
		if (list_count(sh) > 0) {
			input_append(sh, list_name(sh, sh->list_sel));

			// A library is a scope; Tab does the rest. A program is a word.
			const qdos_native_entry* picked = list_module(sh, sh->list_sel);
			input_append(sh, (picked != NULL && !qdos_native_is_app(picked)) ? "::" : " ");
		}
		break;

	case QDOS_KEY_OPEN: {
		// A module is not text and there is nothing to open
		if (list_count(sh) == 0 || sh->list_all || list_module(sh, sh->list_sel) != NULL) {
			break;
		}

		const char* name = list_name(sh, sh->list_sel);
		char source[QDOS_PROGRAM_MAX];
		if (load_program_anywhere(sh, name, source, sizeof(source))) {
			edit_open(sh, name, source);
		}
		break;
	}

	case QDOS_KEY_BACKSPACE: {
		if (sh->list_all || list_count(sh) == 0) {
			break;
		}

		// Nothing here writes a module, so there is no copy to drop
		if (list_module(sh, sh->list_sel) != NULL) {
			set_message(sh, "TAKE IT OFF THE CARD", false);
			break;
		}

		const char* name = list_name(sh, sh->list_sel);
		if (!qdos_program_is_user(sh->hal, name)) {
			set_message(
					sh, qdos_program_is_inbox(sh->hal, name) ? "TAKE IT OFF THE CARD" : "THAT ONE IS SHIPPED", false);
			break;
		}

		// Asking once, because there is no way back from this
		char confirm[QDOS_COLS + 8];
		snprintf(confirm, sizeof(confirm), "BKS AGAIN: DROP '%.8s'", name);
		if (!sh->delete_armed) {
			sh->delete_armed = true;
			set_message(sh, confirm, false);
			break;
		}

		char message[QDOS_COLS + 8];
		qdos_program_erase(sh->hal, name);
		snprintf(message, sizeof(message), "DROPPED '%.10s'", name);
		sh->delete_armed = false;
		list_load(sh);
		if (sh->list_sel >= list_count(sh) && sh->list_sel > 0) {
			sh->list_sel--;
		}
		set_message(sh, message, false);
		break;
	}

	case QDOS_KEY_LIST:
	case QDOS_KEY_CLEAR:
		sh->mode = sh->list_from;
		break;

	default:
		break;
	}
}

/*
 * The program is drawn in the small font, between the rule under the header and
 * the one closing the pane: 10 lines of 50 rather than 7 of 24. The header,
 * messages and soft labels stay at reading size, being read at a glance.
 */
#define EDIT_PANE_TOP (ROW_CONTENT_FIRST * QDOS_CELL_H)
#define EDIT_PANE_H ((ROW_CONTENT_LAST + 1) * QDOS_CELL_H - 1 - EDIT_PANE_TOP)
#define EDIT_ROWS (EDIT_PANE_H / QDOS_SMALL_FONT_H)
#define EDIT_COLS (QDOS_SCREEN_W / QDOS_SMALL_FONT_W)
#define EDIT_Y0 (EDIT_PANE_TOP + (EDIT_PANE_H - EDIT_ROWS * QDOS_SMALL_FONT_H) / 2)

static void edit_scroll_into_view(qdos_shell* sh) {
	size_t line, col;
	qdos_editor_where(&sh->ed, &line, &col);

	if (line < sh->ed_top) {
		sh->ed_top = line;
	} else if (line >= sh->ed_top + EDIT_ROWS) {
		sh->ed_top = line - (EDIT_ROWS - 1);
	}
}

static void edit_open(qdos_shell* sh, const char* name, const char* source) {
	qdos_editor_open(&sh->ed, name, source);
	sh->ed_top = 0;
	sh->mode = QDOS_MODE_EDIT;
	edit_scroll_into_view(sh);
	set_message(sh, "", false);
}

static void register_natives(qdos_shell* sh, qd_interp* interp);

/**
 * @brief Compile the editor text without letting it reach the session
 *
 * A scratch interpreter, because eval also runs whatever is at top level and
 * declares what parses -- neither belongs in the session until a save.
 *
 * Parsing alone is not much of an answer: declaring a word does not resolve
 * the names in its body, so the lint reads the body afterwards and says what
 * calling it would have said. See src/shell/lint.h.
 */
static void check_program(qdos_shell* sh) {
	qd_interp* scratch = qd_interp_create(STACK_SIZE);
	if (!scratch) {
		set_message(sh, "CANNOT CHECK", true);
		return;
	}

	// The same vocabulary the real interpreter has, or a program that calls
	// another would fail to compile here and nowhere else
	register_natives(sh, scratch);
	qdos_natives_register(&sh->natives, scratch);
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++) {
		qdos_programs_restore(sh->hal, (qdos_store_scope)scope, scratch);
	}
	register_apps(sh, scratch);

	char message[80];
	bool bad = !qdos_guarded_eval(scratch, sh->ed.text);
	if (bad) {
		snprintf(message, sizeof(message), "%s", qdos_guarded_error(scratch));
	} else if (qdos_lint_program(scratch, sh->ed.text, message, sizeof(message))) {
		bad = true; // the lint wrote the message
	} else {
		snprintf(message, sizeof(message), "'%.12s' COMPILES", sh->ed.name);
	}

	qd_interp_destroy(scratch);
	set_message(sh, message, bad);
}

/**
 * @brief Run an app, which is a folder on the card holding main.qd
 *
 * In an interpreter of its own, so that every app may call its entry point
 * `main` and name its helpers whatever suits it without two of them ever
 * meeting. An app holds the screen until `main` returns, and leaves nothing
 * behind in the vocabulary when it does.
 */
static int run_app(qd_context* ctx, void* userdata) {
	qdos_shell* sh = ((app_word*)userdata)->sh;
	const char* name = ((app_word*)userdata)->name;

	char source[QDOS_PROGRAM_MAX];
	if (!load_program_anywhere(sh, name, source, sizeof(source))) {
		set_message(sh, "GONE FROM THE CARD", true);
		return 0;
	}

	qd_interp* app = qd_interp_create(STACK_SIZE);
	if (!app) {
		set_message(sh, "CANNOT RUN IT", true);
		return 0;
	}

	register_natives(sh, app);
	qdos_natives_register(&sh->natives, app);

	if (!qdos_guarded_eval(app, source)) {
		set_message(sh, qdos_guarded_error(app), true);
	} else if (!qdos_guarded_eval(app, QDOS_APP_ENTRY)) {
		set_message(sh, qdos_guarded_error(app), true);
	}

	qd_interp_destroy(app);

	// Stopped rather than finished, so whatever ran the app stops too
	if (qdos_natives_broken()) {
		qd_set_error_msg(ctx, "BREAK");
		return 1;
	}
	return 0;
}

/**
 * @brief Make every app on the card a word, so typing its name runs it
 *
 * A slot is found by name and never moved, because the interpreter keeps the
 * pointer: reusing one for a different app would make a name that is still
 * registered run something else. An app taken off the card keeps its slot and
 * says so when it cannot be loaded.
 */
static void register_apps(qdos_shell* sh, qd_interp* interp) {
	qdos_program_entry found[QDOS_WORDLIST_MAX];
	const size_t count = qdos_programs_gather(sh->hal, found, QDOS_WORDLIST_MAX);

	for (size_t i = 0; i < count; i++) {
		if (!found[i].app) {
			continue;
		}

		app_word* slot = NULL;
		for (size_t s = 0; s < sh->app_word_count && slot == NULL; s++) {
			if (strcmp(sh->app_word[s].name, found[i].name) == 0) {
				slot = &sh->app_word[s];
			}
		}

		if (slot == NULL) {
			if (sh->app_word_count >= QDOS_APPS_MAX) {
				break;
			}
			slot = &sh->app_word[sh->app_word_count];
			snprintf(slot->name, sizeof(slot->name), "%s", found[i].name);
			slot->sh = sh;
			sh->app_word_count++;
		}

		qd_interp_register(interp, slot->name, "( -- )", run_app, slot);
	}
}

static void edit_insert_word(qdos_shell* sh, const char* word) {
	qdos_editor_insert(&sh->ed, ' ');
	for (const char* c = word; *c; c++) {
		qdos_editor_insert(&sh->ed, *c);
	}
	qdos_editor_insert(&sh->ed, ' ');
	edit_scroll_into_view(sh);
}

static void handle_edit_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (ev->key != QDOS_KEY_CLEAR) {
		sh->drop_armed = false;
	}

	const char* word = function_word(ev->key);
	if (word != NULL) {
		edit_insert_word(sh, word);
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_STO:
		edit_insert_word(sh, "sto");
		return;
	case QDOS_KEY_RCL:
		edit_insert_word(sh, "rcl");
		return;
	default:
		break;
	}

	switch (ev->key) {
	case QDOS_KEY_UP:
		qdos_editor_move(&sh->ed, 0, -1);
		break;
	case QDOS_KEY_DOWN:
		qdos_editor_move(&sh->ed, 0, 1);
		break;
	case QDOS_KEY_LEFT:
		qdos_editor_move(&sh->ed, -1, 0);
		break;
	case QDOS_KEY_RIGHT:
		qdos_editor_move(&sh->ed, 1, 0);
		break;

	case QDOS_KEY_ENTER:
		qdos_editor_insert(&sh->ed, '\n');
		break;
	case QDOS_KEY_TAB:
		qdos_editor_insert(&sh->ed, '\t');
		break;

	case QDOS_KEY_NEG:
		for (const char* c = " neg "; *c; c++) {
			qdos_editor_insert(&sh->ed, *c);
		}
		break;

	case QDOS_KEY_BACKSPACE:
		qdos_editor_backspace(&sh->ed);
		break;

	case QDOS_KEY_CHAR:
		if (ev->ch) {
			qdos_editor_insert(&sh->ed, ev->ch);
		}
		break;

	case QDOS_KEY_CHECK:
		check_program(sh);
		break;

	case QDOS_KEY_SAVE: {
		char message[80];
		if (!qdos_guarded_eval(sh->interp, sh->ed.text)) {
			// Stay in the editor: the text is still the only copy
			set_message(sh, qdos_guarded_error(sh->interp), true);
			break;
		}
		qdos_program_save(sh->hal, sh->ed.name, sh->ed.text);
		snprintf(message, sizeof(message), "SAVED '%.12s'", sh->ed.name);
		sh->mode = QDOS_MODE_CALC;
		set_message(sh, message, false);
		break;
	}

	case QDOS_KEY_CLEAR:
		// The editor holds the only copy; untouched text has nothing to lose
		if (sh->ed.dirty && !sh->drop_armed) {
			sh->drop_armed = true;
			set_message(sh, "ESC AGAIN: LOSE EDITS", false);
			break;
		}
		sh->drop_armed = false;
		sh->mode = QDOS_MODE_CALC;
		set_message(sh, sh->ed.dirty ? "NOT SAVED" : "", false);
		break;

	default:
		break;
	}

	edit_scroll_into_view(sh);
}

static void handle_mode_key(qdos_shell* sh, const qdos_key_event* ev);

/** @brief Translate a soft key press into the key it stands for */
static bool expand_soft(qdos_shell* sh, const qdos_key_event* in, qdos_key_event* out) {
	if (in->key < QDOS_KEY_SOFT1 || in->key > QDOS_KEY_SOFT5) {
		return false;
	}

	const soft_key* sk = &SOFT[sh->mode][in->key - QDOS_KEY_SOFT1];
	if (sk->key == QDOS_KEY_NONE) {
		return false;
	}

	out->key = sk->key;
	out->ch = 0;
	return true;
}

static void handle_key(qdos_shell* sh, const qdos_key_event* ev) {
	// A message is holding the input line, so this press takes it back. Set
	// first, so whatever this key has to say replaces it rather than being
	// wiped by it.
	sh->message[0] = '\0';
	sh->message_is_error = false;

	// One keypress, one session write at most
	qdos_natives_rearm();

	qdos_key_event expanded;
	if (expand_soft(sh, ev, &expanded)) {
		handle_mode_key(sh, &expanded);
		return;
	}

	handle_mode_key(sh, ev);
}

/**
 * @brief The settings this machine actually has, in the order they are listed
 *
 * Two are conditional, so the page is built rather than numbered and the
 * selection indexes what is on screen.
 */
static size_t settings_visible(const qdos_shell* sh, qdos_setting* out) {
	size_t n = 0;
	out[n++] = SETTING_ANGLE;
	out[n++] = SETTING_DECIMALS;
	out[n++] = SETTING_AUTO_OFF;

	if (sh->hal->usb_export) {
		out[n++] = SETTING_USB;
	}
	if (qdos_natives_blocked_count(&sh->natives) > 0) {
		out[n++] = SETTING_MODULES;
	}

	return n;
}

static size_t setting_count(const qdos_shell* sh) {
	qdos_setting shown[SETTING__COUNT];
	return settings_visible(sh, shown);
}

/** @brief Which setting the selection is sitting on */
static qdos_setting setting_at(const qdos_shell* sh, size_t index) {
	qdos_setting shown[SETTING__COUNT];
	const size_t count = settings_visible(sh, shown);
	return (index < count) ? shown[index] : shown[0];
}

/**
 * @brief Hand the inbox to a PC, or take it back and read what landed
 *
 * Taking it back is the moment an upload becomes a word, so the programs are
 * declared again there rather than on the next boot.
 */
static void toggle_usb(qdos_shell* sh) {
	const bool want = !sh->usb_exported;

	if (sh->hal->usb_export(sh->hal, want) != 0) {
		set_message(sh, "USB WOULD NOT SWITCH", true);
		return;
	}
	sh->usb_exported = want;

	if (want) {
		set_message(sh, "PLUG INTO A PC", false);
		return;
	}

	const int found = qdos_programs_restore(sh->hal, QDOS_SCOPE_INBOX, sh->interp);
	char message[QDOS_COLS + 1];
	snprintf(message, sizeof(message), "%d FROM THE CARD", found > 0 ? found : 0);
	set_message(sh, message, false);
}

/*
 * One entry per setting rather than one record holding all of them, so adding a
 * setting cannot make the others unreadable: a key nobody writes reads as absent
 * and keeps its default. They carry no ".qd", so the vocabulary never lists them.
 */
#define SETTINGS_KEY_ANGLE "settings.angle"
#define SETTINGS_KEY_DECIMALS "settings.decimals"
#define SETTINGS_KEY_AUTO_OFF "settings.autooff"
#define SETTINGS_KEY_GRID "settings.grid"
#define SETTINGS_KEY_AXES "settings.axes"
#define SETTINGS_KEY_STATPLOT "stat.plot"
#define SETTINGS_KEY_XLIST "stat.xlist"
#define SETTINGS_KEY_YLIST "stat.ylist"

static void save_setting(qdos_shell* sh, const char* key, int64_t number) {
	qdos_value value;
	memset(&value, 0, sizeof(value));
	value.type = QDOS_VALUE_INT;
	value.i = number;

	// Nowhere to report a failed write to, and refusing to change the setting on
	// screen because of it would be worse than forgetting it on the next boot.
	qdos_storage_save(sh->hal, key, &value);
}

/** @brief Read one saved setting, leaving @p out alone when there is none */
static void load_setting(qdos_shell* sh, const char* key, int64_t* out) {
	qdos_value value;
	if (qdos_storage_load(sh->hal, key, &value) == QDOS_STORE_OK && value.type == QDOS_VALUE_INT) {
		*out = value.i;
	}
}

/** @brief Write the settings out, which is done the moment one changes */
static void save_settings(qdos_shell* sh) {
	save_setting(sh, SETTINGS_KEY_ANGLE, qdos_math_degrees() ? 1 : 0);
	save_setting(sh, SETTINGS_KEY_DECIMALS, sh->decimals);
	save_setting(sh, SETTINGS_KEY_AUTO_OFF, (int64_t)sh->auto_off);
	save_setting(sh, SETTINGS_KEY_GRID, sh->grid_on ? 1 : 0);
	save_setting(sh, SETTINGS_KEY_AXES, sh->axes_off ? 0 : 1);
	save_setting(sh, SETTINGS_KEY_STATPLOT, (int64_t)sh->statplot);
	save_setting(sh, SETTINGS_KEY_XLIST, sh->statplot_x);
	save_setting(sh, SETTINGS_KEY_YLIST, sh->statplot_y);
}

/**
 * @brief Put back what was saved, keeping the default where nothing was
 *
 * Every value is checked against what this build accepts. A store written by
 * other firmware must not be able to leave the machine unreadable, or turning
 * off at a timeout this build has no name for.
 */
static void restore_settings(qdos_shell* sh) {
	int64_t degrees = qdos_math_degrees() ? 1 : 0;
	load_setting(sh, SETTINGS_KEY_ANGLE, &degrees);
	qdos_math_set_degrees(degrees != 0);

	int64_t decimals = sh->decimals;
	load_setting(sh, SETTINGS_KEY_DECIMALS, &decimals);
	if (decimals >= DECIMALS_AUTO && decimals <= DECIMALS_MAX) {
		sh->decimals = (int)decimals;
	}

	int64_t auto_off = (int64_t)sh->auto_off;
	load_setting(sh, SETTINGS_KEY_AUTO_OFF, &auto_off);
	if (auto_off >= 0 && auto_off < (int64_t)AUTO_OFF_COUNT) {
		sh->auto_off = (size_t)auto_off;
	}

	int64_t grid = 0, axes = 1, plot = STATPLOT_OFF, xlist = 0, ylist = 1;
	load_setting(sh, SETTINGS_KEY_GRID, &grid);
	load_setting(sh, SETTINGS_KEY_AXES, &axes);
	load_setting(sh, SETTINGS_KEY_STATPLOT, &plot);
	load_setting(sh, SETTINGS_KEY_XLIST, &xlist);
	load_setting(sh, SETTINGS_KEY_YLIST, &ylist);
	sh->grid_on = grid != 0;
	sh->axes_off = axes == 0;
	sh->statplot = (plot >= 0 && plot < STATPLOT__COUNT) ? (statplot_type)plot : STATPLOT_OFF;
	sh->statplot_x = (xlist >= 0 && xlist < STAT_LISTS) ? (int)xlist : 0;
	sh->statplot_y = (ylist >= 0 && ylist < STAT_LISTS) ? (int)ylist : 1;
}

/**
 * @brief Change the selected setting
 * @param dir +1 to step forwards, -1 back. The two toggles read the same either
 *            way, which is why both arrows reach them.
 */
static void setting_step(qdos_shell* sh, int dir) {
	const qdos_setting setting = setting_at(sh, sh->setting_sel);

	switch (setting) {
	case SETTING_ANGLE:
		qdos_math_set_degrees(!qdos_math_degrees());
		break;

	case SETTING_USB:
		toggle_usb(sh);
		break;

	case SETTING_MODULES:
		qdos_natives_unblock(&sh->natives, sh->hal);
		set_message(sh, "ON AT NEXT START", false);
		break;

	case SETTING_AUTO_OFF:
		sh->auto_off = (sh->auto_off + (dir > 0 ? 1 : AUTO_OFF_COUNT - 1)) % AUTO_OFF_COUNT;
		break;

	default:
		if (dir > 0) {
			sh->decimals = (sh->decimals >= DECIMALS_MAX) ? DECIMALS_AUTO : sh->decimals + 1;
		} else {
			sh->decimals = (sh->decimals <= DECIMALS_AUTO) ? DECIMALS_MAX : sh->decimals - 1;
		}
		break;
	}

	// Written now: pulling the battery is a normal way to turn a calculator off.
	// The other two are live state rather than a preference.
	if (setting != SETTING_USB && setting != SETTING_MODULES) {
		save_settings(sh);
	}

	// Unblocking takes the row away
	if (sh->setting_sel >= setting_count(sh) && sh->setting_sel > 0) {
		sh->setting_sel = setting_count(sh) - 1;
	}
}

static void handle_settings_key(qdos_shell* sh, const qdos_key_event* ev) {
	switch (ev->key) {
	case QDOS_KEY_UP:
		if (sh->setting_sel > 0) {
			sh->setting_sel--;
		}
		break;

	case QDOS_KEY_DOWN:
		if (sh->setting_sel + 1 < setting_count(sh)) {
			sh->setting_sel++;
		}
		break;

	case QDOS_KEY_ENTER:
	case QDOS_KEY_RIGHT:
		setting_step(sh, +1);
		break;

	case QDOS_KEY_LEFT:
		setting_step(sh, -1);
		break;

	case QDOS_KEY_CLEAR:
		sh->mode = sh->page_from;
		break;

	default:
		break;
	}
}

static void handle_debug_key(qdos_shell* sh, const qdos_key_event* ev) {
	const size_t held = log_held(sh);
	const size_t last = (held > (size_t)LIST_ROWS) ? held - (size_t)LIST_ROWS : 0;

	switch (ev->key) {
	case QDOS_KEY_UP:
		if (sh->log_top > 0) {
			sh->log_top--;
		}
		break;

	case QDOS_KEY_DOWN:
		if (sh->log_top < last) {
			sh->log_top++;
		}
		break;

	case QDOS_KEY_BACKSPACE:
		sh->log_count = 0;
		sh->log_top = 0;
		break;

	case QDOS_KEY_CLEAR:
		sh->mode = sh->page_from;
		break;

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Graph
 *
 * `"f" graph` plots one word taking x and leaving y; GRAPH on the Y= page plots
 * every slot switched on, on the same axes. The samples are taken outside the
 * evaluation that asked for them, at the next repaint, and again only when the
 * window moves along x: moving it up or down is just a redraw.
 * ------------------------------------------------------------------------- */

/* The plot runs from under the status band to the rule over the readout */
#define GRAPH_TOP (ROW_STACK_FIRST * QDOS_CELL_H)
#define GRAPH_HEIGHT ((ROW_CONTENT_LAST + 1) * QDOS_CELL_H - 1 - GRAPH_TOP)

/* How far one press of an arrow moves the window, and one of + or - zooms it */
#define GRAPH_PAN 0.25
#define GRAPH_ZOOM 0.5

/* Where the small-font readout sits, in the row a message would take */
#define READOUT_Y (ROW_MESSAGE * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2)

#define PI 3.14159265358979323846

/**
 * @brief @p word run on @p count inputs, leaving @p outs values, the calculator's stack left as it was
 * @param shape What the word must take and leave, for the message when it does not
 */
static bool graph_run(
		qdos_shell* sh, const char* word, const double* in, size_t count, double* out, size_t outs, const char* shape) {
	qd_context* ctx = qd_interp_context(sh->interp);
	const size_t before = qd_interp_depth(sh->interp);
	for (size_t k = 0; k < count; k++) {
		if (qd_push_f(ctx, in[k]) != 0) {
			while (qd_interp_depth(sh->interp) > before && qdos_guarded_eval(sh->interp, "drop")) {
			}
			return false;
		}
	}

	bool got = false;
	if (!qdos_guarded_eval(sh->interp, word)) {
		snprintf(sh->graph_error, sizeof(sh->graph_error), "%s", qdos_guarded_error(sh->interp));
	} else if (qd_interp_depth(sh->interp) != before + outs) {
		snprintf(sh->graph_error, sizeof(sh->graph_error), "'%.24s' MUST %s", word, shape);
	} else {
		got = true;
		for (size_t k = 0; k < outs && got; k++) {
			qd_interp_value v;
			if (qd_interp_peek(sh->interp, outs - 1 - k, &v) &&
					(v.type == QD_INTERP_VALUE_INT || v.type == QD_INTERP_VALUE_FLOAT)) {
				out[k] = (v.type == QD_INTERP_VALUE_INT) ? (double)v.i : v.f;
			} else {
				snprintf(sh->graph_error, sizeof(sh->graph_error), "'%.24s' MUST LEAVE A NUMBER", word);
				got = false;
			}
		}
	}

	// Whatever the word left, or dropped halfway through failing, goes: `drop`
	// rather than a pop, so a string left behind is freed the way it should be
	while (qd_interp_depth(sh->interp) > before && qdos_guarded_eval(sh->interp, "drop")) {
	}
	return got;
}

/* Which word a sampling pass is running, and for an intersection the other */
typedef struct {
	qdos_shell* sh;
	const char* word;
	const char* other;
} graph_call;

static bool graph_eval(void* user, double x, double* y) {
	const graph_call* call = user;
	return graph_run(call->sh, call->word, &x, 1, y, 1, "TAKE X, LEAVE Y");
}

static bool surface_eval(void* user, double x, double y, double* z) {
	const graph_call* call = user;
	const double in[2] = {x, y};
	return graph_run(call->sh, call->word, in, 2, z, 1, "TAKE X Y, LEAVE Z");
}

static bool param_eval(void* user, double t, double* x, double* y) {
	const graph_call* call = user;
	double xy[2];
	if (!graph_run(call->sh, call->word, &t, 1, xy, 2, "TAKE T, LEAVE X Y")) {
		return false;
	}
	*x = xy[0];
	*y = xy[1];
	return true;
}

/* r at theta, placed on the plane; theta is in the angle the calculator is set to */
static bool polar_eval(void* user, double theta, double* x, double* y) {
	const graph_call* call = user;
	double r;
	if (!graph_run(call->sh, call->word, &theta, 1, &r, 1, "TAKE THETA, LEAVE R")) {
		return false;
	}
	const double a = qdos_math_degrees() ? theta * PI / 180.0 : theta;
	*x = r * cos(a);
	*y = r * sin(a);
	return true;
}

/* One curve less the other, whose roots are where they cross */
static bool difference_eval(void* user, double x, double* y) {
	const graph_call* call = user;
	double f, g;
	if (!graph_run(call->sh, call->word, &x, 1, &f, 1, "TAKE X, LEAVE Y") ||
			!graph_run(call->sh, call->other, &x, 1, &g, 1, "TAKE X, LEAVE Y")) {
		return false;
	}
	*y = f - g;
	return true;
}

static bool is_function(const qdos_shell* sh, int i) {
	return i >= 0 && i < sh->graph_count && sh->graph_shape[i] == QDOS_GRAPH_CURVE;
}

static bool on_points(const qdos_shell* sh) {
	return sh->graph_count > 0 && !is_function(sh, sh->graph_curve);
}

/* The range a parametric or polar curve is taken over */
static void curve_range(const qdos_shell* sh, int i, double* t0, double* t1, double* step) {
	const plot_ranges* r = &sh->ranges;
	const bool polar = sh->graph_shape[i] == QDOS_GRAPH_POLAR;
	*t0 = polar ? r->th0 : r->t0;
	*t1 = polar ? r->th1 : r->t1;
	*step = polar ? r->thstep : r->tstep;
}

static void graph_clear_results(qdos_shell* sh) {
	sh->graph_result[0] = '\0';
	sh->shade_on = false;
	sh->tangent_on = false;
}

/** @brief Plot @p count words on one pair of axes, each a curve in x or a parametric or polar one */
static void graph_open_many(
		qdos_shell* sh, const char words[][QDOS_PROGRAM_NAME_MAX], const qdos_graph_shape* shapes, int count) {
	sh->graph_count = count;
	for (int i = 0; i < count; i++) {
		snprintf(sh->graph_words[i], sizeof(sh->graph_words[i]), "%s", words[i]);
		sh->graph_shape[i] = shapes[i];
	}
	qdos_graph_standard(&sh->graph_view);
	sh->graph_stale = true;
	sh->graph_points_stale = true;
	sh->graph_fit_pending = true;
	sh->graph_state = GRAPH_PAN;
	sh->graph_curve = 0;
	sh->graph_statplot = false;
	graph_clear_results(sh);
	sh->graph_return = QDOS_MODE_CALC;
	sh->mode = QDOS_MODE_GRAPH;
	set_message(sh, "", false);
}

static void graph_open(qdos_shell* sh, const char* word) {
	char words[1][QDOS_PROGRAM_NAME_MAX];
	const qdos_graph_shape shapes[1] = {QDOS_GRAPH_CURVE};
	snprintf(words[0], sizeof(words[0]), "%s", word);
	graph_open_many(sh, words, shapes, 1);
}

static void graph3_open(qdos_shell* sh, const char* word) {
	sh->graph_count = 1;
	snprintf(sh->graph_words[0], sizeof(sh->graph_words[0]), "%s", word);
	qdos_surface_standard(&sh->graph3_view);
	sh->graph_stale = true;
	sh->graph_return = QDOS_MODE_CALC;
	sh->mode = QDOS_MODE_GRAPH3;
	set_message(sh, "", false);
}

/* The y range that shows every curve at once */
static bool graph_fit_all(qdos_shell* sh) {
	bool any = false;
	qdos_graph_view all = sh->graph_view;
	for (int i = 0; i < sh->graph_count; i++) {
		qdos_graph_view one = sh->graph_view;
		const bool fits = is_function(sh, i) ? qdos_graph_fit(&sh->graph_samples[i], &one)
											 : qdos_graph_fit_points(&sh->graph_points[i], &one);
		if (!fits) {
			continue;
		}
		all.y0 = (!any || one.y0 < all.y0) ? one.y0 : all.y0;
		all.y1 = (!any || one.y1 > all.y1) ? one.y1 : all.y1;
		any = true;
	}
	if (any) {
		sh->graph_view = all;
	}
	return any;
}

/** @brief Take the samples if the window has moved, and leave if none had a value */
static void graph_refresh(qdos_shell* sh) {
	if (sh->mode == QDOS_MODE_GRAPH3 && sh->graph_stale) {
		sh->graph_stale = false;
		sh->graph_error[0] = '\0';
		qdos_graph_view w;
		qdos_graph_standard(&w);
		graph_call call = {sh, sh->graph_words[0], NULL};
		if (qdos_surface_sample(
					&sh->graph_surface, QDOS_SURFACE_DEFAULT, w.x0, w.x1, w.y0, w.y1, surface_eval, &call) == 0) {
			sh->mode = sh->graph_return;
			set_message(sh, sh->graph_error[0] ? sh->graph_error : "NOTHING TO PLOT", true);
		}
		return;
	}

	if (sh->mode != QDOS_MODE_GRAPH || (!sh->graph_stale && !sh->graph_points_stale)) {
		return;
	}

	sh->graph_error[0] = '\0';
	const bool opening = sh->graph_fit_pending;
	int good = 0;
	for (int i = 0; i < sh->graph_count; i++) {
		graph_call call = {sh, sh->graph_words[i], NULL};
		if (is_function(sh, i)) {
			if (sh->graph_stale) {
				good += qdos_graph_sample(&sh->graph_samples[i], &sh->graph_view, QDOS_SCREEN_W, graph_eval, &call);
			} else {
				good++;
			}
			continue;
		}

		// Taken along t, so moving the window does not need them again
		if (sh->graph_points_stale) {
			double t0, t1, step;
			curve_range(sh, i, &t0, &t1, &step);
			good += qdos_graph_sample_points(&sh->graph_points[i], t0, t1, step,
					sh->graph_shape[i] == QDOS_GRAPH_POLAR ? polar_eval : param_eval, &call);
		} else {
			good++;
		}
	}
	sh->graph_stale = false;
	sh->graph_points_stale = false;

	if (sh->graph_fit_pending) {
		sh->graph_fit_pending = false;
		graph_fit_all(sh);
	}

	// Nothing to show on opening means the words are wrong, not the window.
	// A stat plot alone is something to show.
	if (good == 0 && opening && sh->graph_count > 0 && !sh->graph_statplot) {
		sh->mode = sh->graph_return;
		set_message(sh, sh->graph_error[0] ? sh->graph_error : "NOTHING TO PLOT", true);
	}
}

static qdos_graph_area graph_area(qdos_console* con) {
	const qdos_graph_area area = {
			.fb = con->fb,
			.stride = QDOS_SCREEN_W,
			.left = 0,
			.top = GRAPH_TOP,
			.width = QDOS_SCREEN_W,
			.height = GRAPH_HEIGHT,
			.ink = con->ink,
			.paper = con->paper,
	};
	return area;
}

/* Solid, dashed and dotted, round again: three is as many as can be told apart */
static qdos_graph_style curve_style(int i) {
	static const qdos_graph_style STYLES[] = {QDOS_GRAPH_SOLID, QDOS_GRAPH_DASHED, QDOS_GRAPH_DOTTED};
	return STYLES[i % 3];
}

/* y on curve @p i at x, run afresh rather than read off a column */
static bool curve_at(qdos_shell* sh, int i, double x, double* y) {
	graph_call call = {sh, sh->graph_words[i], NULL};
	return graph_eval(&call, x, y);
}

/* Where trace is on a parametric or polar curve */
static bool point_at(const qdos_shell* sh, double* x, double* y, double* t) {
	const qdos_graph_points* p = &sh->graph_points[sh->graph_curve];
	const int k = sh->graph_index;
	if (k < 0 || k >= p->count) {
		return false;
	}
	*t = p->t0 + k * p->step;
	*x = p->x[k];
	*y = p->y[k];
	return p->ok[k];
}

static const char* statplot_name(statplot_type type);
static void stat_draw(qdos_shell* sh, const qdos_graph_area* area);

/* What the arrows are on, for the readout: the curve's name when there is more than one */
static void which_curve(const qdos_shell* sh, char* out, size_t cap) {
	out[0] = '\0';
	if (sh->graph_count > 1) {
		snprintf(out, cap, "%s ", sh->graph_words[sh->graph_curve]);
	}
}

/* The prompt CALC or ZOOM BOX is showing */
static const char* calc_prompt(const qdos_shell* sh) {
	if (sh->calc == CALC_BOX) {
		return sh->calc_step == 0 ? "FIRST CORNER?" : "SECOND CORNER?";
	}
	int step = sh->calc_step;
	if (sh->calc == CALC_INTERSECT) {
		if (step < 2) {
			return step == 0 ? "FIRST CURVE?" : "SECOND CURVE?";
		}
		step -= 2;
	}
	if (sh->calc == CALC_DERIVATIVE || sh->calc == CALC_TANGENT) {
		return "X?";
	}
	return step == 0 ? "LEFT BOUND?" : "RIGHT BOUND?";
}

static bool asking_for_curve(const qdos_shell* sh) {
	return sh->graph_state == GRAPH_ASK && sh->calc == CALC_INTERSECT && sh->calc_step < 2;
}

static bool uses_free_cursor(const qdos_shell* sh) {
	return sh->graph_state == GRAPH_FREE || (sh->graph_state == GRAPH_ASK && sh->calc == CALC_BOX);
}

static void render_graph(qdos_shell* sh, qdos_console* con) {
	const qdos_graph_area area = graph_area(con);
	const qdos_graph_view* v = &sh->graph_view;
	if (sh->grid_on) {
		qdos_graph_draw_grid(&area, v);
	}
	if (!sh->axes_off) {
		qdos_graph_draw_axes(&area, v);
	}
	if (sh->shade_on && is_function(sh, sh->shade_curve)) {
		qdos_graph_shade(&area, v, &sh->graph_samples[sh->shade_curve], sh->shade_a, sh->shade_b);
	}
	for (int i = 0; i < sh->graph_count; i++) {
		if (is_function(sh, i)) {
			qdos_graph_draw_curve(&area, v, &sh->graph_samples[i], curve_style(i));
		} else {
			qdos_graph_draw_points(&area, v, &sh->graph_points[i], curve_style(i));
		}
	}
	if (sh->graph_statplot) {
		stat_draw(sh, &area);
	}
	if (sh->tangent_on) {
		const double w = v->x1 - v->x0;
		qdos_graph_draw_segment(&area, v, v->x0 - w, sh->tangent_y + sh->tangent_slope * (v->x0 - w - sh->tangent_x),
				v->x1 + w, sh->tangent_y + sh->tangent_slope * (v->x1 + w - sh->tangent_x), QDOS_GRAPH_SOLID);
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);

	// The readout, small, where messages go: a message says more when there is one.
	// Room for the longest numbers; what does not fit the row is clipped there.
	char line[160];
	char which[QDOS_PROGRAM_NAME_MAX + 2];
	which_curve(sh, which, sizeof(which));

	if (uses_free_cursor(sh)) {
		const double x = qdos_graph_x(v, sh->graph_free_col, QDOS_SCREEN_W);
		const double y = qdos_graph_y(v, sh->graph_free_row, GRAPH_HEIGHT);
		qdos_graph_draw_cursor_at(&area, v, x, y);
		if (sh->graph_state == GRAPH_ASK && sh->calc_step == 1) {
			// The box as it stands, from the first corner to the cursor
			qdos_graph_draw_rect(&area, v, sh->calc_bound[0], sh->calc_bound[1], x, y);
		}
		char prompt[32] = "";
		if (sh->graph_state == GRAPH_ASK) {
			snprintf(prompt, sizeof(prompt), "%s ", calc_prompt(sh));
		}
		snprintf(line, sizeof(line), "%sX=%.8g  Y=%.8g", prompt, x, y);
	} else if (sh->graph_state == GRAPH_ASK && asking_for_curve(sh)) {
		snprintf(line, sizeof(line), "%s %s", calc_prompt(sh), sh->graph_words[sh->graph_curve]);
	} else if ((sh->graph_state == GRAPH_TRACE || sh->graph_state == GRAPH_ASK) && sh->graph_count > 0) {
		char prompt[32] = "";
		if (sh->graph_state == GRAPH_ASK) {
			snprintf(prompt, sizeof(prompt), "%s ", calc_prompt(sh));
		}
		double x, y, t;
		if (sh->graph_result[0]) {
			snprintf(line, sizeof(line), "%s", sh->graph_result);
			if (!on_points(sh) && curve_at(sh, sh->graph_curve, sh->graph_x, &y)) {
				qdos_graph_draw_cursor_at(&area, v, sh->graph_x, y);
			}
		} else if (on_points(sh)) {
			const bool polar = sh->graph_shape[sh->graph_curve] == QDOS_GRAPH_POLAR;
			if (point_at(sh, &x, &y, &t)) {
				qdos_graph_draw_cursor_at(&area, v, x, y);
				if (polar) {
					snprintf(line, sizeof(line), "%sTH=%.6g  R=%.6g  X=%.6g  Y=%.6g", which, t, hypot(x, y), x, y);
				} else {
					snprintf(line, sizeof(line), "%sT=%.6g  X=%.6g  Y=%.6g", which, t, x, y);
				}
			} else {
				snprintf(line, sizeof(line), "%s%s=%.6g  UNDEFINED", which, polar ? "TH" : "T", t);
			}
		} else if (curve_at(sh, sh->graph_curve, sh->graph_x, &y)) {
			qdos_graph_draw_cursor_at(&area, v, sh->graph_x, y);
			snprintf(line, sizeof(line), "%s%sX=%.8g  Y=%.8g", prompt, which, sh->graph_x, y);
		} else {
			snprintf(line, sizeof(line), "%s%sX=%.8g  Y UNDEFINED", prompt, which, sh->graph_x);
		}
	} else if (sh->graph_result[0]) {
		snprintf(line, sizeof(line), "%s", sh->graph_result);
	} else {
		char names[64] = "";
		for (int i = 0; i < sh->graph_count; i++) {
			const size_t used = strlen(names);
			snprintf(names + used, sizeof(names) - used, "%s%.12s", i ? " " : "", sh->graph_words[i]);
		}
		if (sh->graph_count == 0 && sh->graph_statplot) {
			snprintf(names, sizeof(names), "%s", statplot_name(sh->statplot));
		}
		snprintf(line, sizeof(line), "%s  X %.4g:%.4g  Y %.4g:%.4g", names, v->x0, v->x1, v->y0, v->y1);
	}
	if (sh->field == FIELD_NONE) {
		qdos_console_puts_small(con, 0, READOUT_Y, line);
	}
}

static void render_graph3(qdos_shell* sh, qdos_console* con) {
	const qdos_graph_area area = graph_area(con);
	qdos_surface_draw(&area, &sh->graph_surface, &sh->graph3_view);
	qdos_console_rule(con, ROW_CONTENT_LAST);

	char line[96];
	const qdos_surface_view* v = &sh->graph3_view;
	snprintf(line, sizeof(line), "%.12s  AZ %d EL %d  Z %.4g:%.4g", sh->graph_words[0], (int)v->azimuth,
			(int)v->elevation, sh->graph_surface.z0, sh->graph_surface.z1);
	qdos_console_puts_small(con, 0, READOUT_Y, line);
}

/* One press of an arrow turns or tilts the surface this far, in degrees */
#define GRAPH3_TURN 15.0
#define GRAPH3_TILT 10.0
#define GRAPH3_TILT_MAX 80.0
#define GRAPH3_ZOOM 1.25

static void handle_graph3_key(qdos_shell* sh, const qdos_key_event* ev) {
	qdos_surface_view* v = &sh->graph3_view;

	switch (ev->key) {
	case QDOS_KEY_CLEAR:
		sh->mode = sh->graph_return;
		break;

	case QDOS_KEY_STD:
		qdos_surface_standard(v);
		break;

	// Kept to a whole turn, so the readout says 30 rather than 390
	case QDOS_KEY_LEFT:
	case QDOS_KEY_RIGHT:
		v->azimuth = fmod(v->azimuth + ((ev->key == QDOS_KEY_RIGHT) ? GRAPH3_TURN : -GRAPH3_TURN) + 360.0, 360.0);
		break;

	case QDOS_KEY_UP:
	case QDOS_KEY_DOWN:
		v->elevation += (ev->key == QDOS_KEY_UP) ? GRAPH3_TILT : -GRAPH3_TILT;
		v->elevation = fmax(-GRAPH3_TILT_MAX, fmin(GRAPH3_TILT_MAX, v->elevation));
		break;

	case QDOS_KEY_ADD:
		v->zoom = fmin(v->zoom * GRAPH3_ZOOM, 8.0);
		break;

	case QDOS_KEY_SUB:
		v->zoom = fmax(v->zoom / GRAPH3_ZOOM, 0.25);
		break;

	default:
		break;
	}
}

static void field_open(qdos_shell* sh, field_target target, const char* prompt, const char* initial);
static bool field_starts(const qdos_key_event* ev);
static void field_key(qdos_shell* sh, const qdos_key_event* ev);
static void menu_open(qdos_shell* sh, menu_id id);
static void window_save(qdos_shell* sh);
static void stat_zoom(qdos_shell* sh, qdos_graph_view* v);

/* The window moved: new samples, and the old answers no longer where they were drawn */
static void graph_moved(qdos_shell* sh) {
	sh->graph_stale = true;
	sh->shade_on = false;
}

/* Trace put on the middle of the plot, on a curve that is there */
static void trace_start(qdos_shell* sh) {
	sh->graph_state = GRAPH_TRACE;
	sh->graph_x = qdos_graph_x(&sh->graph_view, QDOS_SCREEN_W / 2, QDOS_SCREEN_W);
	if (on_points(sh)) {
		sh->graph_index = sh->graph_points[sh->graph_curve].count / 2;
	}
}

/* Trace to x, bringing it into view if it is off the plot */
static void trace_to(qdos_shell* sh, double x) {
	qdos_graph_view* v = &sh->graph_view;
	sh->graph_x = x;
	if (x < v->x0 || x > v->x1) {
		const double shift = x - (v->x0 + v->x1) / 2.0;
		v->x0 += shift;
		v->x1 += shift;
		graph_moved(sh);
	}
}

/* One column along the curve, or one step of t; off the edge brings more of it in */
static void trace_step(qdos_shell* sh, int dir) {
	if (on_points(sh)) {
		const int count = sh->graph_points[sh->graph_curve].count;
		sh->graph_index = (sh->graph_index + dir + count) % (count ? count : 1);
		return;
	}
	qdos_graph_view* v = &sh->graph_view;
	const int col = qdos_graph_col(v, sh->graph_x, QDOS_SCREEN_W) + dir;
	sh->graph_x = qdos_graph_x(v, col, QDOS_SCREEN_W);
	if (col < 0 || col >= QDOS_SCREEN_W) {
		qdos_graph_pan(v, dir * GRAPH_PAN, 0.0);
		graph_moved(sh);
	}
}

/* The next curve up or down that @p want accepts, from the one trace is on */
static void curve_step(qdos_shell* sh, int dir, bool functions_only) {
	const int n = sh->graph_count;
	for (int k = 1; k <= n; k++) {
		const int i = ((sh->graph_curve + dir * k) % n + n) % n;
		if (!functions_only || is_function(sh, i)) {
			const bool was_points = on_points(sh);
			double x, y, t;
			if (was_points && point_at(sh, &x, &y, &t)) {
				sh->graph_x = x;
			}
			sh->graph_curve = i;
			if (on_points(sh) && (sh->graph_index < 0 || sh->graph_index >= sh->graph_points[i].count)) {
				sh->graph_index = sh->graph_points[i].count / 2;
			}
			return;
		}
	}
}

/* Put an answer on the calculator's stack, where RPN wants it */
static void graph_push(qdos_shell* sh, const double* values, int count) {
	undo_snapshot(sh);
	qd_context* ctx = qd_interp_context(sh->interp);
	for (int i = 0; i < count; i++) {
		qd_push_f(ctx, values[i]);
	}
}

static void calc_start(qdos_shell* sh, calc_kind kind) {
	graph_clear_results(sh);
	if (kind == CALC_BOX) {
		sh->calc = kind;
		sh->calc_step = 0;
		sh->graph_state = GRAPH_ASK;
		sh->graph_free_col = QDOS_SCREEN_W / 2;
		sh->graph_free_row = GRAPH_HEIGHT / 2;
		return;
	}

	int functions = 0;
	for (int i = 0; i < sh->graph_count; i++) {
		functions += is_function(sh, i) ? 1 : 0;
	}
	if (functions == 0) {
		set_message(sh, "CALC NEEDS A FUNCTION", true);
		return;
	}
	if (kind == CALC_INTERSECT && functions < 2) {
		set_message(sh, "INTERSECT NEEDS TWO CURVES", true);
		return;
	}
	if (!is_function(sh, sh->graph_curve)) {
		curve_step(sh, 1, true);
	}

	// On the curve, where trace would be, unless it is already somewhere
	if (sh->graph_state != GRAPH_TRACE) {
		trace_start(sh);
	}
	sh->calc = kind;
	sh->calc_step = 0;
	sh->graph_state = GRAPH_ASK;
	if (kind == CALC_VALUE) {
		sh->graph_state = GRAPH_TRACE;
		field_open(sh, FIELD_CALC, "X=", "");
	}
}

/* The answer, readable, and the cursor on it */
static void calc_answer(qdos_shell* sh, const char* text, double x) {
	snprintf(sh->graph_result, sizeof(sh->graph_result), "%s", text);
	sh->graph_state = GRAPH_TRACE;
	trace_to(sh, x);
}

static void calc_finish(qdos_shell* sh) {
	const char* word = sh->graph_words[sh->graph_curve];
	graph_call call = {sh, word, NULL};
	char text[MESSAGE_COLS + 1];
	double x = sh->graph_x, y = 0.0;
	double lo = fmin(sh->calc_bound[0], sh->calc_bound[1]), hi = fmax(sh->calc_bound[0], sh->calc_bound[1]);
	sh->graph_error[0] = '\0';

	switch (sh->calc) {
	case CALC_ZERO:
		if (!qdos_num_root(graph_eval, &call, lo, hi, &x)) {
			break;
		}
		graph_push(sh, &x, 1);
		snprintf(text, sizeof(text), "ZERO X=%.10g", x);
		calc_answer(sh, text, x);
		return;

	case CALC_MINIMUM:
	case CALC_MAXIMUM: {
		const bool low = sh->calc == CALC_MINIMUM;
		if (!(low ? qdos_num_minimum : qdos_num_maximum)(graph_eval, &call, lo, hi, &x, &y)) {
			break;
		}
		const double both[2] = {x, y};
		graph_push(sh, both, 2);
		snprintf(text, sizeof(text), "%s X=%.10g Y=%.10g", low ? "MINIMUM" : "MAXIMUM", x, y);
		calc_answer(sh, text, x);
		return;
	}

	case CALC_INTERSECT: {
		graph_call pair = {sh, sh->graph_words[sh->calc_curves[0]], sh->graph_words[sh->calc_curves[1]]};
		if (!qdos_num_root(difference_eval, &pair, lo, hi, &x) || !graph_eval(&pair, x, &y)) {
			break;
		}
		const double both[2] = {x, y};
		graph_push(sh, both, 2);
		sh->graph_curve = sh->calc_curves[0];
		snprintf(text, sizeof(text), "INTERSECTION X=%.10g Y=%.10g", x, y);
		calc_answer(sh, text, x);
		return;
	}

	case CALC_DERIVATIVE:
	case CALC_TANGENT: {
		double d;
		if (!qdos_num_derivative(graph_eval, &call, x, &d) || !graph_eval(&call, x, &y)) {
			break;
		}
		if (sh->calc == CALC_DERIVATIVE) {
			graph_push(sh, &d, 1);
			snprintf(text, sizeof(text), "DY/DX=%.10g AT X=%.8g", d, x);
		} else {
			const double line[2] = {d, y - d * x};
			graph_push(sh, line, 2);
			snprintf(text, sizeof(text), "TANGENT Y=%.8gX%+.8g", d, y - d * x);
			sh->tangent_on = true;
			sh->tangent_x = x;
			sh->tangent_y = y;
			sh->tangent_slope = d;
		}
		calc_answer(sh, text, x);
		return;
	}

	case CALC_INTEGRAL: {
		double area;
		if (!qdos_num_integral(graph_eval, &call, sh->calc_bound[0], sh->calc_bound[1], &area)) {
			break;
		}
		graph_push(sh, &area, 1);
		snprintf(text, sizeof(text), "INTEGRAL=%.10g", area);
		calc_answer(sh, text, sh->calc_bound[1]);
		sh->shade_on = true;
		sh->shade_curve = sh->graph_curve;
		sh->shade_a = lo;
		sh->shade_b = hi;
		return;
	}

	default:
		break;
	}

	sh->graph_state = GRAPH_TRACE;
	set_message(sh, sh->graph_error[0] ? sh->graph_error : "NO ANSWER IN BOUNDS", true);
}

/* How many steps a kind asks for before it can answer */
static int calc_steps(calc_kind kind) {
	switch (kind) {
	case CALC_INTERSECT:
		return 4;
	case CALC_DERIVATIVE:
	case CALC_TANGENT:
		return 1;
	default:
		return 2;
	}
}

/* The step's answer is x, from the cursor or typed */
static void calc_accept_x(qdos_shell* sh, double x) {
	int bound = sh->calc_step - (sh->calc == CALC_INTERSECT ? 2 : 0);
	if (bound >= 0 && bound < 2) {
		sh->calc_bound[bound] = x;
	}
	trace_to(sh, x);
	sh->graph_x = x;
	if (++sh->calc_step >= calc_steps(sh->calc)) {
		calc_finish(sh);
	}
}

static void box_accept(qdos_shell* sh) {
	qdos_graph_view* v = &sh->graph_view;
	const double x = qdos_graph_x(v, sh->graph_free_col, QDOS_SCREEN_W);
	const double y = qdos_graph_y(v, sh->graph_free_row, GRAPH_HEIGHT);
	if (sh->calc_step == 0) {
		sh->calc_bound[0] = x;
		sh->calc_bound[1] = y;
		sh->calc_step = 1;
		return;
	}
	if (x == sh->calc_bound[0] || y == sh->calc_bound[1]) {
		set_message(sh, "A BOX NEEDS TWO CORNERS", true);
		return;
	}
	v->x0 = fmin(x, sh->calc_bound[0]);
	v->x1 = fmax(x, sh->calc_bound[0]);
	v->y0 = fmin(y, sh->calc_bound[1]);
	v->y1 = fmax(y, sh->calc_bound[1]);
	graph_moved(sh);
	sh->graph_state = GRAPH_PAN;
}

static void handle_ask_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (sh->calc == CALC_BOX) {
		switch (ev->key) {
		case QDOS_KEY_LEFT:
			sh->graph_free_col = (sh->graph_free_col > 0) ? sh->graph_free_col - 1 : 0;
			break;
		case QDOS_KEY_RIGHT:
			sh->graph_free_col = (sh->graph_free_col < QDOS_SCREEN_W - 1) ? sh->graph_free_col + 1 : QDOS_SCREEN_W - 1;
			break;
		case QDOS_KEY_UP:
			sh->graph_free_row = (sh->graph_free_row > 0) ? sh->graph_free_row - 1 : 0;
			break;
		case QDOS_KEY_DOWN:
			sh->graph_free_row = (sh->graph_free_row < GRAPH_HEIGHT - 1) ? sh->graph_free_row + 1 : GRAPH_HEIGHT - 1;
			break;
		case QDOS_KEY_ENTER:
			box_accept(sh);
			break;
		default:
			break;
		}
		return;
	}

	if (asking_for_curve(sh)) {
		switch (ev->key) {
		case QDOS_KEY_UP:
		case QDOS_KEY_DOWN:
			curve_step(sh, ev->key == QDOS_KEY_DOWN ? 1 : -1, true);
			break;
		case QDOS_KEY_ENTER:
			if (sh->calc_step == 1 && sh->graph_curve == sh->calc_curves[0]) {
				set_message(sh, "PICK ANOTHER CURVE", true);
				break;
			}
			sh->calc_curves[sh->calc_step++] = sh->graph_curve;
			if (sh->calc_step == 1) {
				curve_step(sh, 1, true);
			}
			break;
		default:
			break;
		}
		return;
	}

	if (field_starts(ev)) {
		field_open(sh, FIELD_CALC, "X=", "");
		field_key(sh, ev);
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_LEFT:
	case QDOS_KEY_RIGHT:
		trace_step(sh, ev->key == QDOS_KEY_RIGHT ? 1 : -1);
		break;
	case QDOS_KEY_UP:
	case QDOS_KEY_DOWN:
		// The curve is chosen at the first step; after that it is the one asked about
		if (sh->calc_step == 0 && sh->calc != CALC_INTERSECT) {
			curve_step(sh, ev->key == QDOS_KEY_DOWN ? 1 : -1, true);
		}
		break;
	case QDOS_KEY_ENTER:
		calc_accept_x(sh, sh->graph_x);
		break;
	default:
		break;
	}
}

/* The ZOOM menu, in the order a TI numbers it */
typedef enum {
	ZOOM_BOX = 0,
	ZOOM_IN,
	ZOOM_OUT,
	ZOOM_STANDARD,
	ZOOM_FIT,
	ZOOM_DECIMAL,
	ZOOM_INTEGER,
	ZOOM_SQUARE,
	ZOOM_TRIG,
	ZOOM_STAT
} zoom_kind;

static void graph_zoom(qdos_shell* sh, zoom_kind kind) {
	qdos_graph_view* v = &sh->graph_view;
	graph_clear_results(sh);
	switch (kind) {
	case ZOOM_BOX:
		calc_start(sh, CALC_BOX);
		return;
	case ZOOM_IN:
	case ZOOM_OUT:
		qdos_graph_zoom(
				v, (v->x0 + v->x1) / 2.0, (v->y0 + v->y1) / 2.0, kind == ZOOM_IN ? GRAPH_ZOOM : 1.0 / GRAPH_ZOOM);
		break;
	case ZOOM_STANDARD:
		qdos_graph_standard(v);
		break;
	case ZOOM_FIT:
		// Fitted to samples, which a window just opened has not taken yet
		if (sh->graph_stale || sh->graph_points_stale) {
			sh->graph_fit_pending = true;
		} else if (!graph_fit_all(sh)) {
			set_message(sh, "NOTHING TO FIT", true);
		}
		break;
	case ZOOM_DECIMAL:
		qdos_graph_decimal(v, QDOS_SCREEN_W, GRAPH_HEIGHT);
		break;
	case ZOOM_INTEGER:
		qdos_graph_integer(v, QDOS_SCREEN_W, GRAPH_HEIGHT);
		break;
	case ZOOM_SQUARE:
		qdos_graph_square(v, QDOS_SCREEN_W, GRAPH_HEIGHT);
		break;
	case ZOOM_TRIG:
		qdos_graph_trig(v, QDOS_SCREEN_W, GRAPH_HEIGHT, qdos_math_degrees());
		break;
	case ZOOM_STAT:
		stat_zoom(sh, v);
		break;
	}
	graph_moved(sh);
	if (sh->graph_state == GRAPH_TRACE) {
		trace_start(sh);
	}
}

static void handle_graph_key(qdos_shell* sh, const qdos_key_event* ev) {
	qdos_graph_view* v = &sh->graph_view;

	// An answer stays in the readout until the next key
	sh->graph_result[0] = '\0';

	if (ev->key == QDOS_KEY_CLEAR) {
		// Asking is backed out of; otherwise ESC leaves, as it always has
		if (sh->graph_state == GRAPH_ASK) {
			sh->graph_state = (sh->calc == CALC_BOX) ? GRAPH_PAN : GRAPH_TRACE;
			return;
		}
		if (sh->graph_return != QDOS_MODE_CALC) {
			sh->plot_view = sh->graph_view;
			window_save(sh);
		}
		sh->mode = sh->graph_return;
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_ZOOM:
		menu_open(sh, MENU_ZOOM);
		return;
	case QDOS_KEY_CALC:
		menu_open(sh, MENU_CALC);
		return;
	case QDOS_KEY_MENU:
		menu_open(sh, MENU_GRAPH_TOOLS);
		return;
	case QDOS_KEY_FIT:
		graph_zoom(sh, ZOOM_FIT);
		return;
	case QDOS_KEY_STD:
		graph_zoom(sh, ZOOM_STANDARD);
		return;
	default:
		break;
	}

	if (sh->graph_state == GRAPH_ASK) {
		handle_ask_key(sh, ev);
		return;
	}

	if (ev->key == QDOS_KEY_TRACE) {
		if (sh->graph_state == GRAPH_TRACE) {
			sh->graph_state = GRAPH_PAN;
		} else if (sh->graph_count == 0) {
			set_message(sh, "NOTHING TO TRACE", true);
		} else {
			trace_start(sh);
		}
		return;
	}

	// A number typed while tracing is where to go
	if (sh->graph_state == GRAPH_TRACE && field_starts(ev)) {
		field_open(sh, FIELD_TRACE_X, on_points(sh) ? "T=" : "X=", "");
		field_key(sh, ev);
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_LEFT:
	case QDOS_KEY_RIGHT: {
		const int dir = (ev->key == QDOS_KEY_RIGHT) ? 1 : -1;
		if (sh->graph_state == GRAPH_TRACE) {
			trace_step(sh, dir);
		} else if (sh->graph_state == GRAPH_FREE) {
			sh->graph_free_col = (sh->graph_free_col + dir + QDOS_SCREEN_W) % QDOS_SCREEN_W;
		} else {
			qdos_graph_pan(v, dir * GRAPH_PAN, 0.0);
			graph_moved(sh);
		}
		break;
	}

	case QDOS_KEY_UP:
	case QDOS_KEY_DOWN:
		// Tracing several, up and down go from one curve to the next, as on a
		// TI; otherwise they move the window
		if (sh->graph_state == GRAPH_TRACE && sh->graph_count > 1) {
			curve_step(sh, ev->key == QDOS_KEY_DOWN ? 1 : -1, false);
			break;
		}
		if (sh->graph_state == GRAPH_FREE) {
			const int dir = (ev->key == QDOS_KEY_DOWN) ? 1 : -1;
			sh->graph_free_row = (sh->graph_free_row + dir + GRAPH_HEIGHT) % GRAPH_HEIGHT;
			break;
		}
		qdos_graph_pan(v, 0.0, (ev->key == QDOS_KEY_UP) ? GRAPH_PAN : -GRAPH_PAN);
		sh->shade_on = false;
		break;

	case QDOS_KEY_ADD:
	case QDOS_KEY_SUB: {
		// About the traced point when there is one, so it stays under the cursor
		double cx = (v->x0 + v->x1) / 2.0, cy = (v->y0 + v->y1) / 2.0;
		if (sh->graph_state == GRAPH_FREE) {
			cx = qdos_graph_x(v, sh->graph_free_col, QDOS_SCREEN_W);
			cy = qdos_graph_y(v, sh->graph_free_row, GRAPH_HEIGHT);
		} else if (sh->graph_state == GRAPH_TRACE) {
			double y, t;
			if (on_points(sh)) {
				if (point_at(sh, &cx, &cy, &t)) {
					qdos_graph_zoom(v, cx, cy, (ev->key == QDOS_KEY_ADD) ? GRAPH_ZOOM : 1.0 / GRAPH_ZOOM);
					graph_moved(sh);
				}
				break;
			}
			cx = sh->graph_x;
			if (curve_at(sh, sh->graph_curve, cx, &y)) {
				cy = y;
			}
		}
		qdos_graph_zoom(v, cx, cy, (ev->key == QDOS_KEY_ADD) ? GRAPH_ZOOM : 1.0 / GRAPH_ZOOM);
		if (sh->graph_state == GRAPH_TRACE) {
			// The traced x on the middle column again, so stepping lands on columns
			qdos_graph_pan(v, (cx - qdos_graph_x(v, QDOS_SCREEN_W / 2, QDOS_SCREEN_W)) / (v->x1 - v->x0), 0.0);
			sh->graph_x = qdos_graph_x(v, QDOS_SCREEN_W / 2, QDOS_SCREEN_W);
		} else if (sh->graph_state == GRAPH_FREE) {
			sh->graph_free_col = QDOS_SCREEN_W / 2;
			sh->graph_free_row = GRAPH_HEIGHT / 2;
			v->x0 += cx - qdos_graph_x(v, sh->graph_free_col, QDOS_SCREEN_W);
			v->x1 += cx - qdos_graph_x(v, sh->graph_free_col, QDOS_SCREEN_W);
		}
		graph_moved(sh);
		break;
	}

	default:
		break;
	}
}

/* ---------------------------------------------------------------------------
 * Y=
 *
 * Six slots, Y1 to Y6, each the body of a function typed the way a line is.
 * A slot is declared as a word of its own name, so Y1 is usable anywhere a
 * word is, and kept in the store, so the slots are still there after a
 * restart. A body that uses y is a surface, in x and y; one that does not is
 * a curve in x.
 * ------------------------------------------------------------------------- */

static void slot_name(size_t i, char* out, size_t cap) {
	snprintf(out, cap, "Y%zu", i + 1);
}

/** @brief Declare slot @p i as the word it names; false, and the word gone, if it would not */
static bool slot_declare(qdos_shell* sh, size_t i) {
	plot_slot* slot = &sh->slots[i];
	char name[8];
	slot_name(i, name, sizeof(name));

	// Out with the old first: a body that fails must not leave the last one
	// plotting under its name
	qd_interp_undeclare(sh->interp, name);
	slot->shape = qdos_graph_body_shape(slot->body);
	slot->broken = false;
	if (slot->shape == QDOS_GRAPH_NONE) {
		return true;
	}

	static const char* const TAKES[] = {"", "x:f64", "x:f64 y:f64", "t:f64", "theta:f64"};
	char source[PLOT_BODY_MAX + 64];
	snprintf(source, sizeof(source), "fn %s(%s -- %s) { %s }", name, TAKES[slot->shape],
			slot->shape == QDOS_GRAPH_PARAM ? "x:f64 y:f64" : "r:f64", slot->body);
	slot->broken = !qdos_guarded_eval(sh->interp, source);
	return !slot->broken;
}

static void slot_save(qdos_shell* sh, size_t i) {
	char key[16];
	snprintf(key, sizeof(key), "plot.y%zu", i + 1);

	qdos_value value;
	memset(&value, 0, sizeof(value));
	value.type = QDOS_VALUE_STRING;
	snprintf(value.s, sizeof(value.s), "%c%s", sh->slots[i].on ? '1' : '0', sh->slots[i].body);
	qdos_storage_save(sh->hal, key, &value);
}

/** @brief The slots as they were left, declared; after the programs, which a body may use */
static void slots_restore(qdos_shell* sh) {
	for (size_t i = 0; i < PLOT_SLOTS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "plot.y%zu", i + 1);

		qdos_value value;
		if (qdos_storage_load(sh->hal, key, &value) != QDOS_STORE_OK || value.type != QDOS_VALUE_STRING ||
				value.s[0] == '\0') {
			continue;
		}
		sh->slots[i].on = (value.s[0] == '1');
		snprintf(sh->slots[i].body, sizeof(sh->slots[i].body), "%s", value.s + 1);
		slot_declare(sh, i);
	}
}

static void plot_open(qdos_shell* sh) {
	sh->mode = QDOS_MODE_PLOT;
	set_message(sh, "", false);
}

/**
 * @brief GRAPH: the selected slot alone if it is a surface, every curve switched on otherwise
 *
 * In the window kept for Y=, which WINDOW and ZOOM change, with the stat plot
 * over the curves when there is one.
 */
static void plot_graph(qdos_shell* sh) {
	const plot_slot* selected = &sh->slots[sh->slot_sel];
	char name[8];
	const qdos_mode from = (sh->mode == QDOS_MODE_STAT) ? QDOS_MODE_STAT : QDOS_MODE_PLOT;

	if (sh->mode == QDOS_MODE_PLOT && selected->shape == QDOS_GRAPH_SURFACE && !selected->broken) {
		slot_name(sh->slot_sel, name, sizeof(name));
		graph3_open(sh, name);
		sh->graph_return = QDOS_MODE_PLOT;
		return;
	}

	char words[PLOT_SLOTS][QDOS_PROGRAM_NAME_MAX];
	qdos_graph_shape shapes[PLOT_SLOTS];
	int count = 0;
	for (size_t i = 0; i < PLOT_SLOTS; i++) {
		const plot_slot* slot = &sh->slots[i];
		if (slot->on && slot->shape != QDOS_GRAPH_NONE && slot->shape != QDOS_GRAPH_SURFACE && !slot->broken) {
			shapes[count] = slot->shape;
			slot_name(i, words[count++], sizeof(words[0]));
		}
	}
	if (count == 0 && sh->statplot == STATPLOT_OFF) {
		set_message(sh, "NO CURVE IS ON", false);
		return;
	}
	graph_open_many(sh, words, shapes, count);
	sh->graph_view = sh->plot_view;
	sh->graph_fit_pending = false;
	sh->graph_statplot = sh->statplot != STATPLOT_OFF;
	sh->graph_return = from;
}

static void render_plot(qdos_shell* sh, qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "Y=");
	qdos_console_rule(con, ROW_HEADER);

	// The name at reading size, the body small so a whole one fits, and what
	// it plots as at the end
	const int body_x = 4 * QDOS_CELL_W;
	const int body_room = (QDOS_SCREEN_W - body_x - 4 * QDOS_CELL_W) / QDOS_SMALL_FONT_W;
	for (size_t i = 0; i < PLOT_SLOTS; i++) {
		const plot_slot* slot = &sh->slots[i];
		const int row = ROW_CONTENT_FIRST + (int)i;

		// '=' lit on the ones GRAPH draws, as a TI marks them
		char label[12];
		char name[8];
		slot_name(i, name, sizeof(name));
		snprintf(label, sizeof(label), "%s%c", name, slot->on ? '=' : ' ');
		qdos_console_puts(con, 0, row, label);

		char body[PLOT_BODY_MAX + 1];
		snprintf(body, sizeof(body), "%s", slot->body);
		if ((int)strlen(body) > body_room) {
			body[body_room - 1] = QDOS_ELIDED;
			body[body_room] = '\0';
		}
		qdos_console_puts_small(con, body_x, row * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2, body);

		if (slot->shape != QDOS_GRAPH_NONE) {
			static const char* const SHAPE[] = {"", "2D", "3D", "PAR", "POL"};
			qdos_console_puts_right(con, row, slot->broken ? "ERR" : SHAPE[slot->shape]);
		}
		if (i == sh->slot_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);
}

/* The slot being typed, on the input row, after its name */
static void render_plot_edit(qdos_shell* sh, qdos_console* con) {
	char prompt[8];
	slot_name(sh->slot_sel, prompt, sizeof(prompt));
	strcat(prompt, "=");
	const int prompt_len = (int)strlen(prompt);
	qdos_console_puts(con, 0, ROW_INPUT, prompt);

	const int room = QDOS_COLS - prompt_len - 1; // a cell for the cursor
	const size_t caret = sh->input_cursor;
	const size_t start = (caret > (size_t)room) ? caret - (size_t)room : 0;
	qdos_console_puts(con, prompt_len, ROW_INPUT, sh->input + start);
	if (sh->cursor_on) {
		qdos_console_invert(con, prompt_len + (int)(caret - start), ROW_INPUT, 1);
	}
}

static void handle_plot_key(qdos_shell* sh, const qdos_key_event* ev) {
	plot_slot* slot = &sh->slots[sh->slot_sel];

	switch (ev->key) {
	case QDOS_KEY_UP:
		sh->slot_sel = (sh->slot_sel + PLOT_SLOTS - 1) % PLOT_SLOTS;
		break;

	case QDOS_KEY_DOWN:
		sh->slot_sel = (sh->slot_sel + 1) % PLOT_SLOTS;
		break;

	case QDOS_KEY_OPEN:
	case QDOS_KEY_ENTER:
		input_clear(sh);
		input_append(sh, slot->body);
		sh->mode = QDOS_MODE_PLOT_EDIT;
		break;

	case QDOS_KEY_TOGGLE:
		if (slot->shape != QDOS_GRAPH_NONE) {
			slot->on = !slot->on;
			slot_save(sh, sh->slot_sel);
		}
		break;

	// Emptied, the way CLEAR empties one on a TI
	case QDOS_KEY_BACKSPACE:
		slot->body[0] = '\0';
		slot->on = false;
		slot_declare(sh, sh->slot_sel);
		slot_save(sh, sh->slot_sel);
		break;

	case QDOS_KEY_GRAPH:
		plot_graph(sh);
		break;

	case QDOS_KEY_MENU:
		menu_open(sh, MENU_PLOT_TOOLS);
		break;

	case QDOS_KEY_CLEAR:
		sh->mode = QDOS_MODE_CALC;
		break;

	default:
		break;
	}
}

/* A variable as a word of its own: spaced from what is before it and after */
static void plot_edit_variable(qdos_shell* sh, const char* name) {
	if (sh->input_cursor > 0 && sh->input[sh->input_cursor - 1] != ' ') {
		input_append(sh, " ");
	}
	input_append(sh, name);
	input_append(sh, " ");
}

/* The body without the spaces round it, which the keys leave behind */
static void trim(char* text) {
	size_t len = strlen(text);
	while (len > 0 && text[len - 1] == ' ') {
		text[--len] = '\0';
	}
	size_t lead = 0;
	while (text[lead] == ' ') {
		lead++;
	}
	memmove(text, text + lead, len - lead + 1);
}

static void handle_plot_edit_key(qdos_shell* sh, const qdos_key_event* ev) {
	plot_slot* slot = &sh->slots[sh->slot_sel];

	switch (ev->key) {
	case QDOS_KEY_VAR_X:
		plot_edit_variable(sh, "x");
		break;

	case QDOS_KEY_VAR_Y:
		plot_edit_variable(sh, "y");
		break;

	case QDOS_KEY_VAR_T:
		plot_edit_variable(sh, "t");
		break;

	case QDOS_KEY_VAR_THETA:
		plot_edit_variable(sh, "theta");
		break;

	case QDOS_KEY_ENTER: {
		// Refused rather than cut: a body missing its end would declare as
		// something else, or not at all
		if (sh->input_len >= sizeof(slot->body)) {
			set_message(sh, "TOO LONG FOR A SLOT", true);
			break;
		}

		// Switched on when it gets a body it did not have, as a TI does
		const bool was_empty = slot->shape == QDOS_GRAPH_NONE;
		memcpy(slot->body, sh->input, sh->input_len + 1);
		trim(slot->body);
		if (!slot_declare(sh, sh->slot_sel)) {
			set_message(sh, qdos_guarded_error(sh->interp), true);
		}
		if (was_empty && slot->shape != QDOS_GRAPH_NONE) {
			slot->on = true;
		}
		if (slot->shape == QDOS_GRAPH_NONE) {
			slot->on = false;
		}
		slot_save(sh, sh->slot_sel);
		input_clear(sh);
		sh->mode = QDOS_MODE_PLOT;
		break;
	}

	case QDOS_KEY_CLEAR:
		input_clear(sh);
		sh->mode = QDOS_MODE_PLOT;
		break;

	// Everything else edits the line the way it does at the prompt
	default:
		handle_line_key(sh, ev);
		break;
	}
}

/* ---------------------------------------------------------------------------
 * A number typed on the input row
 *
 * WINDOW, TABLE, trace, CALC and the lists all want one number. What is typed
 * is evaluated as a line, so `pi 2 /` is as good as `1.5708`, and it has to
 * leave exactly one number.
 * ------------------------------------------------------------------------- */

static void field_open(qdos_shell* sh, field_target target, const char* prompt, const char* initial) {
	sh->field = target;
	sh->field_mode = sh->mode;
	snprintf(sh->field_prompt, sizeof(sh->field_prompt), "%s", prompt);
	snprintf(sh->field_text, sizeof(sh->field_text), "%s", initial);
	sh->field_len = strlen(sh->field_text);
}

/* The keys that begin a number, and so open a field where one is wanted */
static bool field_starts(const qdos_key_event* ev) {
	switch (ev->key) {
	case QDOS_KEY_0:
	case QDOS_KEY_1:
	case QDOS_KEY_2:
	case QDOS_KEY_3:
	case QDOS_KEY_4:
	case QDOS_KEY_5:
	case QDOS_KEY_6:
	case QDOS_KEY_7:
	case QDOS_KEY_8:
	case QDOS_KEY_9:
	case QDOS_KEY_DOT:
	case QDOS_KEY_NEG:
		return true;
	case QDOS_KEY_CHAR:
		return ev->ch > ' ' && ev->ch < 0x7F;
	default:
		return false;
	}
}

static void field_append(qdos_shell* sh, const char* text) {
	const size_t len = strlen(text);
	if (sh->field_len + len >= sizeof(sh->field_text)) {
		return;
	}
	memcpy(sh->field_text + sh->field_len, text, len + 1);
	sh->field_len += len;
}

/* What was typed, as the one number it must come to */
static bool field_number(qdos_shell* sh, double* out) {
	const size_t before = qd_interp_depth(sh->interp);
	bool got = false;
	if (!qdos_guarded_eval(sh->interp, sh->field_text)) {
		set_message(sh, qdos_guarded_error(sh->interp), true);
	} else if (qd_interp_depth(sh->interp) != before + 1) {
		set_message(sh, "NEED ONE NUMBER", true);
	} else {
		qd_interp_value v;
		got = qd_interp_peek(sh->interp, 0, &v) && (v.type == QD_INTERP_VALUE_INT || v.type == QD_INTERP_VALUE_FLOAT);
		if (got) {
			*out = (v.type == QD_INTERP_VALUE_INT) ? (double)v.i : v.f;
			got = isfinite(*out);
		}
		if (!got) {
			set_message(sh, "NEED ONE NUMBER", true);
		}
	}
	while (qd_interp_depth(sh->interp) > before && qdos_guarded_eval(sh->interp, "drop")) {
	}
	return got;
}

static void field_commit(qdos_shell* sh, double value);

static void field_key(qdos_shell* sh, const qdos_key_event* ev) {
	const char* word = function_word(ev->key);
	if (word != NULL) {
		field_append(sh, " ");
		field_append(sh, word);
		field_append(sh, " ");
		return;
	}

	static const char DIGITS[] = "0123456789";
	if (ev->key >= QDOS_KEY_0 && ev->key <= QDOS_KEY_9) {
		const char digit[2] = {DIGITS[ev->key - QDOS_KEY_0], '\0'};
		field_append(sh, digit);
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_DOT:
		field_append(sh, ".");
		break;
	case QDOS_KEY_ADD:
		field_append(sh, "+");
		break;
	case QDOS_KEY_SUB:
		field_append(sh, "-");
		break;
	case QDOS_KEY_MUL:
		field_append(sh, "*");
		break;
	case QDOS_KEY_DIV:
		field_append(sh, " divide ");
		break;

	// A sign, not an operator: on the number where there is only one
	case QDOS_KEY_NEG:
		if (sh->field_text[0] == '-') {
			memmove(sh->field_text, sh->field_text + 1, sh->field_len--);
		} else if (sh->field_len + 1 < sizeof(sh->field_text)) {
			memmove(sh->field_text + 1, sh->field_text, ++sh->field_len);
			sh->field_text[0] = '-';
		}
		break;

	case QDOS_KEY_CHAR:
		if (ev->ch >= ' ' && ev->ch < 0x7F) {
			const char text[2] = {ev->ch, '\0'};
			field_append(sh, text);
		}
		break;

	case QDOS_KEY_BACKSPACE:
		if (sh->field_len > 0) {
			sh->field_text[--sh->field_len] = '\0';
		}
		break;

	case QDOS_KEY_ENTER: {
		double value;
		if (sh->field_len == 0) {
			sh->field = FIELD_NONE;
			break;
		}
		if (field_number(sh, &value)) {
			field_commit(sh, value);
		}
		break;
	}

	case QDOS_KEY_CLEAR:
		sh->field = FIELD_NONE;
		break;

	default:
		break;
	}
}

static void render_field(qdos_shell* sh, qdos_console* con) {
	if (sh->field == FIELD_NONE || sh->message[0]) {
		return;
	}
	const int prompt_len = qdos_console_puts(con, 0, ROW_INPUT, sh->field_prompt);
	const int room = QDOS_COLS - prompt_len - 1;
	const size_t start = (sh->field_len > (size_t)room) ? sh->field_len - (size_t)room : 0;
	qdos_console_puts(con, prompt_len, ROW_INPUT, sh->field_text + start);
	if (sh->cursor_on) {
		qdos_console_invert(con, prompt_len + (int)(sh->field_len - start), ROW_INPUT, 1);
	}
}

/* A number at most @p width characters wide, losing digits before it loses its size */
static void fit_number(double v, char* out, size_t cap, size_t width) {
	for (int digits = 10; digits >= 1; digits--) {
		snprintf(out, cap, "%.*g", digits, v);
		if (strlen(out) <= width) {
			return;
		}
	}
}

/* ---------------------------------------------------------------------------
 * What is kept across a restart: the window, the ranges, the table, the lists
 * ------------------------------------------------------------------------- */

#define WINDOW_KEY "plot.window"
#define WINDOW_VALUES 14

static void window_defaults(qdos_shell* sh) {
	qdos_graph_standard(&sh->plot_view);
	const bool deg = qdos_math_degrees();
	sh->ranges.t0 = 0.0;
	sh->ranges.t1 = deg ? 360.0 : 2.0 * PI;
	sh->ranges.tstep = deg ? 7.5 : PI / 24.0;
	sh->ranges.th0 = sh->ranges.t0;
	sh->ranges.th1 = sh->ranges.t1;
	sh->ranges.thstep = sh->ranges.tstep;
	sh->table_start = 0.0;
	sh->table_step = 1.0;
}

static void window_save(qdos_shell* sh) {
	const qdos_graph_view* v = &sh->plot_view;
	const plot_ranges* r = &sh->ranges;
	char text[WINDOW_VALUES * 26];
	snprintf(text, sizeof(text), "%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g",
			v->x0, v->x1, v->xscl, v->y0, v->y1, v->yscl, r->t0, r->t1, r->tstep, r->th0, r->th1, r->thstep,
			sh->table_start, sh->table_step);
	sh->hal->store_write(sh->hal, WINDOW_KEY, text, strlen(text));
}

/* Anything unreadable or out of shape leaves the defaults */
static void window_restore(qdos_shell* sh) {
	window_defaults(sh);
	char text[WINDOW_VALUES * 26 + 1];
	size_t len = 0;
	if (sh->hal->store_read(sh->hal, QDOS_SCOPE_USER, WINDOW_KEY, text, sizeof(text) - 1, &len) != QDOS_STORE_OK) {
		return;
	}
	text[len] = '\0';
	double d[WINDOW_VALUES];
	if (sscanf(text, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &d[0], &d[1], &d[2], &d[3], &d[4],
				&d[5], &d[6], &d[7], &d[8], &d[9], &d[10], &d[11], &d[12], &d[13]) != WINDOW_VALUES) {
		return;
	}
	if (!(d[0] < d[1]) || !(d[3] < d[4]) || d[2] < 0.0 || d[5] < 0.0 || !(d[8] > 0.0) || !(d[11] > 0.0) ||
			d[13] == 0.0) {
		return;
	}
	const qdos_graph_view v = {d[0], d[1], d[3], d[4], d[2], d[5]};
	sh->plot_view = v;
	const plot_ranges r = {d[6], d[7], d[8], d[9], d[10], d[11]};
	sh->ranges = r;
	sh->table_start = d[12];
	sh->table_step = d[13];
}

static void list_save(qdos_shell* sh, int i) {
	char key[16];
	snprintf(key, sizeof(key), "stat.l%d", (i + 1) % 10);
	char text[STAT_LIST_MAX * 25 + 1] = "";
	size_t used = 0;
	for (size_t k = 0; k < sh->list_len[i]; k++) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%.17g", k ? " " : "", sh->lists[i][k]);
	}
	sh->hal->store_write(sh->hal, key, text, used);
}

static void lists_restore(qdos_shell* sh) {
	for (int i = 0; i < STAT_LISTS; i++) {
		char key[16];
		snprintf(key, sizeof(key), "stat.l%d", (i + 1) % 10);
		char text[STAT_LIST_MAX * 25 + 1];
		size_t len = 0;
		sh->list_len[i] = 0;
		if (sh->hal->store_read(sh->hal, QDOS_SCOPE_USER, key, text, sizeof(text) - 1, &len) != QDOS_STORE_OK) {
			continue;
		}
		text[len] = '\0';
		char* p = text;
		while (sh->list_len[i] < STAT_LIST_MAX) {
			char* end;
			const double v = strtod(p, &end);
			if (end == p) {
				break;
			}
			sh->lists[i][sh->list_len[i]++] = v;
			p = end;
		}
	}
}

/* ---------------------------------------------------------------------------
 * Menus
 *
 * A page of numbered choices, as a TI's ZOOM and CALC are: the arrows and
 * PICK, or the digit. An item may carry a setting, shown on its right, which
 * PICK and the side arrows turn over without leaving the page.
 * ------------------------------------------------------------------------- */

typedef struct {
	const char* label;
	int action;
} menu_item;

enum {
	TOOL_WINDOW,
	TOOL_ZOOM,
	TOOL_TABLE,
	TOOL_FORMAT,
	TOOL_STAT,
	TOOL_STATPLOT,
	TOOL_FREE
};

enum {
	FORMAT_GRID,
	FORMAT_AXES
};

enum {
	STATCALC_ONE = -2,
	STATCALC_TWO = -1
	// The regressions are their own qdos_regression
};

enum {
	STATPLOT_TYPE,
	STATPLOT_XLIST,
	STATPLOT_YLIST,
	STATPLOT_ZOOM
};

static const menu_item ZOOM_ITEMS[] = {
		{"BOX", ZOOM_BOX},
		{"IN", ZOOM_IN},
		{"OUT", ZOOM_OUT},
		{"STANDARD", ZOOM_STANDARD},
		{"FIT", ZOOM_FIT},
		{"DECIMAL", ZOOM_DECIMAL},
		{"INTEGER", ZOOM_INTEGER},
		{"SQUARE", ZOOM_SQUARE},
		{"TRIG", ZOOM_TRIG},
		{"STAT", ZOOM_STAT},
};

static const menu_item CALC_ITEMS[] = {
		{"VALUE", CALC_VALUE},
		{"ZERO", CALC_ZERO},
		{"MINIMUM", CALC_MINIMUM},
		{"MAXIMUM", CALC_MAXIMUM},
		{"INTERSECT", CALC_INTERSECT},
		{"DY/DX", CALC_DERIVATIVE},
		{"INTEGRAL", CALC_INTEGRAL},
		{"TANGENT", CALC_TANGENT},
};

static const menu_item GRAPH_TOOL_ITEMS[] = {
		{"WINDOW", TOOL_WINDOW},
		{"TABLE", TOOL_TABLE},
		{"FORMAT", TOOL_FORMAT},
		{"FREE CURSOR", TOOL_FREE},
		{"STAT PLOT", TOOL_STATPLOT},
};

static const menu_item PLOT_TOOL_ITEMS[] = {
		{"WINDOW", TOOL_WINDOW},
		{"ZOOM", TOOL_ZOOM},
		{"TABLE", TOOL_TABLE},
		{"FORMAT", TOOL_FORMAT},
		{"STAT", TOOL_STAT},
		{"STAT PLOT", TOOL_STATPLOT},
};

static const menu_item FORMAT_ITEMS[] = {
		{"GRID", FORMAT_GRID},
		{"AXES", FORMAT_AXES},
};

static const menu_item STATCALC_ITEMS[] = {
		{"1-VAR STATS", STATCALC_ONE},
		{"2-VAR STATS", STATCALC_TWO},
		{"LINREG", QDOS_REG_LINEAR},
		{"QUADREG", QDOS_REG_QUADRATIC},
		{"EXPREG", QDOS_REG_EXPONENTIAL},
		{"PWRREG", QDOS_REG_POWER},
		{"LNREG", QDOS_REG_LOG},
};

static const menu_item STATPLOT_ITEMS[] = {
		{"TYPE", STATPLOT_TYPE},
		{"XLIST", STATPLOT_XLIST},
		{"YLIST", STATPLOT_YLIST},
		{"ZOOM STAT", STATPLOT_ZOOM},
};

typedef struct {
	const char* title;
	const menu_item* items;
	size_t count;
} menu_def;

#define ITEMS(a) (a), sizeof(a) / sizeof(*(a))

static const menu_def MENUS[MENU__COUNT] = {
		[MENU_ZOOM] = {"ZOOM", ITEMS(ZOOM_ITEMS)},
		[MENU_CALC] = {"CALC", ITEMS(CALC_ITEMS)},
		[MENU_GRAPH_TOOLS] = {"GRAPH", ITEMS(GRAPH_TOOL_ITEMS)},
		[MENU_PLOT_TOOLS] = {"Y=", ITEMS(PLOT_TOOL_ITEMS)},
		[MENU_FORMAT] = {"FORMAT", ITEMS(FORMAT_ITEMS)},
		[MENU_STAT_CALC] = {"STAT CALC", ITEMS(STATCALC_ITEMS)},
		[MENU_STAT_PLOT] = {"STAT PLOT", ITEMS(STATPLOT_ITEMS)},
};

#undef ITEMS

static const char* statplot_name(statplot_type type) {
	static const char* const NAMES[STATPLOT__COUNT] = {"OFF", "SCATTER", "XYLINE", "HISTOGRAM", "BOXPLOT"};
	return NAMES[type];
}

/* Nested menus go back where the first was opened from */
static void menu_open(qdos_shell* sh, menu_id id) {
	if (sh->mode != QDOS_MODE_MENU) {
		sh->menu_from = sh->mode;
	}
	sh->menu = id;
	sh->menu_sel = 0;
	sh->menu_top = 0;
	sh->mode = QDOS_MODE_MENU;
	set_message(sh, "", false);
}

/* The setting an item carries, or NULL for one that only does something */
static const char* menu_value(const qdos_shell* sh, menu_id id, int action, char* buf, size_t cap) {
	if (id == MENU_FORMAT) {
		return (action == FORMAT_GRID ? sh->grid_on : !sh->axes_off) ? "ON" : "OFF";
	}
	if (id == MENU_STAT_PLOT) {
		switch (action) {
		case STATPLOT_TYPE:
			return statplot_name(sh->statplot);
		case STATPLOT_XLIST:
			snprintf(buf, cap, "L%d", sh->statplot_x + 1);
			return buf;
		case STATPLOT_YLIST:
			snprintf(buf, cap, "L%d", sh->statplot_y + 1);
			return buf;
		default:
			return NULL;
		}
	}
	return NULL;
}

static void window_open(qdos_shell* sh, qdos_mode from);
static void table_open(qdos_shell* sh, qdos_mode from);
static void stat_open(qdos_shell* sh, qdos_mode from);
static void stat_calc(qdos_shell* sh, int action);

/* Graph the Y= slots from wherever a menu was opened, if not from the graph */
static bool menu_to_graph(qdos_shell* sh) {
	if (sh->menu_from == QDOS_MODE_GRAPH) {
		sh->mode = QDOS_MODE_GRAPH;
		return true;
	}
	sh->mode = (sh->menu_from == QDOS_MODE_STAT) ? QDOS_MODE_STAT : QDOS_MODE_MENU;
	plot_graph(sh);
	if (sh->mode != QDOS_MODE_GRAPH) {
		if (sh->mode != QDOS_MODE_GRAPH3) {
			sh->mode = sh->menu_from;
		}
		return false;
	}
	return true;
}

static void save_settings(qdos_shell* sh);

static void menu_act(qdos_shell* sh, int dir) {
	const menu_def* m = &MENUS[sh->menu];
	const int action = m->items[sh->menu_sel].action;

	switch (sh->menu) {
	case MENU_ZOOM:
		if (dir > 0 && menu_to_graph(sh)) {
			graph_zoom(sh, (zoom_kind)action);
		}
		return;

	case MENU_CALC:
		if (dir > 0) {
			sh->mode = QDOS_MODE_GRAPH;
			calc_start(sh, (calc_kind)action);
		}
		return;

	case MENU_GRAPH_TOOLS:
	case MENU_PLOT_TOOLS:
		if (dir <= 0) {
			return;
		}
		switch (action) {
		case TOOL_WINDOW:
			window_open(sh, sh->menu_from);
			break;
		case TOOL_ZOOM:
			menu_open(sh, MENU_ZOOM);
			break;
		case TOOL_TABLE:
			table_open(sh, sh->menu_from);
			break;
		case TOOL_FORMAT:
			menu_open(sh, MENU_FORMAT);
			break;
		case TOOL_STAT:
			stat_open(sh, sh->menu_from);
			break;
		case TOOL_STATPLOT:
			menu_open(sh, MENU_STAT_PLOT);
			break;
		case TOOL_FREE:
			sh->mode = QDOS_MODE_GRAPH;
			sh->graph_state = GRAPH_FREE;
			sh->graph_free_col = QDOS_SCREEN_W / 2;
			sh->graph_free_row = GRAPH_HEIGHT / 2;
			break;
		}
		return;

	case MENU_FORMAT:
		if (action == FORMAT_GRID) {
			sh->grid_on = !sh->grid_on;
		} else {
			sh->axes_off = !sh->axes_off;
		}
		save_settings(sh);
		return;

	case MENU_STAT_CALC:
		if (dir > 0) {
			stat_calc(sh, action);
		}
		return;

	case MENU_STAT_PLOT:
		switch (action) {
		case STATPLOT_TYPE:
			sh->statplot = (statplot_type)((sh->statplot + STATPLOT__COUNT + dir) % STATPLOT__COUNT);
			break;
		case STATPLOT_XLIST:
			sh->statplot_x = (sh->statplot_x + STAT_LISTS + dir) % STAT_LISTS;
			break;
		case STATPLOT_YLIST:
			sh->statplot_y = (sh->statplot_y + STAT_LISTS + dir) % STAT_LISTS;
			break;
		case STATPLOT_ZOOM:
			if (dir > 0 && menu_to_graph(sh)) {
				graph_zoom(sh, ZOOM_STAT);
			}
			break;
		}
		// A graph of the slots, open under the menu, follows the setting
		if (sh->graph_return != QDOS_MODE_CALC) {
			sh->graph_statplot = sh->statplot != STATPLOT_OFF;
		}
		save_settings(sh);
		return;

	default:
		return;
	}
}

static void menu_scroll_into_view(qdos_shell* sh) {
	if (sh->menu_sel < sh->menu_top) {
		sh->menu_top = sh->menu_sel;
	} else if (sh->menu_sel >= sh->menu_top + LIST_ROWS) {
		sh->menu_top = sh->menu_sel - (LIST_ROWS - 1);
	}
}

static void handle_menu_key(qdos_shell* sh, const qdos_key_event* ev) {
	const menu_def* m = &MENUS[sh->menu];

	// A digit is the item it numbers, 0 the tenth
	if (ev->key >= QDOS_KEY_0 && ev->key <= QDOS_KEY_9) {
		const size_t item = (ev->key == QDOS_KEY_0) ? 9 : (size_t)(ev->key - QDOS_KEY_1);
		if (item < m->count) {
			sh->menu_sel = item;
			menu_scroll_into_view(sh);
			menu_act(sh, 1);
		}
		return;
	}

	switch (ev->key) {
	case QDOS_KEY_UP:
		sh->menu_sel = (sh->menu_sel + m->count - 1) % m->count;
		break;
	case QDOS_KEY_DOWN:
		sh->menu_sel = (sh->menu_sel + 1) % m->count;
		break;
	case QDOS_KEY_ENTER:
		menu_act(sh, 1);
		return;
	case QDOS_KEY_LEFT:
	case QDOS_KEY_RIGHT: {
		char buf[8];
		if (menu_value(sh, sh->menu, m->items[sh->menu_sel].action, buf, sizeof(buf)) != NULL) {
			menu_act(sh, ev->key == QDOS_KEY_RIGHT ? 1 : -1);
		}
		return;
	}
	case QDOS_KEY_CLEAR:
		sh->mode = sh->menu_from;
		return;
	default:
		return;
	}
	menu_scroll_into_view(sh);
}

static void render_menu(qdos_shell* sh, qdos_console* con) {
	const menu_def* m = &MENUS[sh->menu];
	qdos_console_puts(con, 0, ROW_HEADER, m->title);
	qdos_console_rule(con, ROW_HEADER);

	for (size_t i = 0; i < LIST_ROWS; i++) {
		const size_t item = sh->menu_top + i;
		if (item >= m->count) {
			break;
		}
		const int row = ROW_CONTENT_FIRST + (int)i;
		char label[QDOS_COLS + 1];
		snprintf(label, sizeof(label), "%zu %s", (item + 1) % 10, m->items[item].label);
		qdos_console_puts(con, 0, row, label);

		char buf[8];
		const char* value = menu_value(sh, sh->menu, m->items[item].action, buf, sizeof(buf));
		if (value != NULL) {
			qdos_console_puts_right(con, row, value);
		}
		if (item == sh->menu_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);
}

/* ---------------------------------------------------------------------------
 * WINDOW
 *
 * The edges and tick spacing of the plot, and the ranges a parametric and a
 * polar curve are taken over, each typed. From the graph it changes the one
 * on screen; from Y= the one GRAPH will open with.
 * ------------------------------------------------------------------------- */

#define WINDOW_ROWS 12

static const char* const WINDOW_NAMES[WINDOW_ROWS] = {
		"XMIN", "XMAX", "XSCL", "YMIN", "YMAX", "YSCL", "TMIN", "TMAX", "TSTEP", "THETAMIN", "THETAMAX", "THETASTEP"};

static qdos_graph_view* window_view(qdos_shell* sh) {
	return (sh->window_from == QDOS_MODE_GRAPH) ? &sh->graph_view : &sh->plot_view;
}

/* Where row @p i of the page is kept, in @p v and @p r */
static double* window_value(qdos_graph_view* v, plot_ranges* r, size_t i) {
	double* const at[WINDOW_ROWS] = {&v->x0, &v->x1, &v->xscl, &v->y0, &v->y1, &v->yscl, &r->t0, &r->t1, &r->tstep,
			&r->th0, &r->th1, &r->thstep};
	return at[i];
}

static void window_open(qdos_shell* sh, qdos_mode from) {
	sh->window_from = from;
	sh->window_sel = 0;
	sh->window_top = 0;
	sh->mode = QDOS_MODE_WINDOW;
	set_message(sh, "", false);
}

static void window_set(qdos_shell* sh, size_t row, double value) {
	qdos_graph_view v = *window_view(sh);
	plot_ranges r = sh->ranges;
	*window_value(&v, &r, row) = value;

	const char* wrong = NULL;
	if (!(v.x0 < v.x1)) {
		wrong = "XMIN MUST BE UNDER XMAX";
	} else if (!(v.y0 < v.y1)) {
		wrong = "YMIN MUST BE UNDER YMAX";
	} else if (v.xscl < 0.0 || v.yscl < 0.0) {
		wrong = "A SCALE CANNOT BE NEGATIVE";
	} else if (!(r.t0 <= r.t1) || !(r.th0 <= r.th1)) {
		wrong = "A RANGE MUST NOT RUN BACKWARDS";
	} else if (!(r.tstep > 0.0) || !(r.thstep > 0.0)) {
		wrong = "A STEP MUST BE OVER 0";
	}
	if (wrong != NULL) {
		set_message(sh, wrong, true);
		return;
	}

	*window_view(sh) = v;
	if (sh->window_from == QDOS_MODE_GRAPH) {
		graph_moved(sh);
		if (sh->graph_return != QDOS_MODE_CALC) {
			sh->plot_view = v;
		}
	}
	if (row >= 6) {
		sh->ranges = r;
		sh->graph_points_stale = true;
	}
	window_save(sh);
	sh->field = FIELD_NONE;
	sh->window_sel = (row + 1 < WINDOW_ROWS) ? row + 1 : row;
	if (sh->window_sel >= sh->window_top + LIST_ROWS) {
		sh->window_top = sh->window_sel - (LIST_ROWS - 1);
	}
}

static void window_edit(qdos_shell* sh, const qdos_key_event* first) {
	char prompt[16], value[32] = "";
	snprintf(prompt, sizeof(prompt), "%s=", WINDOW_NAMES[sh->window_sel]);
	if (first == NULL) {
		snprintf(value, sizeof(value), "%.10g", *window_value(window_view(sh), &sh->ranges, sh->window_sel));
	}
	field_open(sh, FIELD_WINDOW, prompt, value);
	if (first != NULL) {
		field_key(sh, first);
	}
}

/* GRAPH from a page: the graph it came from, or the slots drawn afresh */
static void page_graph(qdos_shell* sh, qdos_mode from) {
	if (from == QDOS_MODE_GRAPH) {
		sh->mode = QDOS_MODE_GRAPH;
		return;
	}
	sh->mode = (from == QDOS_MODE_STAT) ? QDOS_MODE_STAT : QDOS_MODE_MENU;
	plot_graph(sh);
	if (sh->mode != QDOS_MODE_GRAPH && sh->mode != QDOS_MODE_GRAPH3) {
		sh->mode = from;
	}
}

static void handle_window_key(qdos_shell* sh, const qdos_key_event* ev) {
	if (field_starts(ev)) {
		window_edit(sh, ev);
		return;
	}
	switch (ev->key) {
	case QDOS_KEY_UP:
		sh->window_sel = (sh->window_sel + WINDOW_ROWS - 1) % WINDOW_ROWS;
		break;
	case QDOS_KEY_DOWN:
		sh->window_sel = (sh->window_sel + 1) % WINDOW_ROWS;
		break;
	case QDOS_KEY_ENTER:
		window_edit(sh, NULL);
		return;
	case QDOS_KEY_GRAPH:
		page_graph(sh, sh->window_from);
		return;
	case QDOS_KEY_CLEAR:
		sh->mode = sh->window_from;
		return;
	default:
		return;
	}
	if (sh->window_sel < sh->window_top) {
		sh->window_top = sh->window_sel;
	} else if (sh->window_sel >= sh->window_top + LIST_ROWS) {
		sh->window_top = sh->window_sel - (LIST_ROWS - 1);
	}
}

static void render_window(qdos_shell* sh, qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "WINDOW");
	qdos_console_rule(con, ROW_HEADER);
	qdos_graph_view* v = window_view(sh);
	for (size_t i = 0; i < LIST_ROWS; i++) {
		const size_t item = sh->window_top + i;
		if (item >= WINDOW_ROWS) {
			break;
		}
		const int row = ROW_CONTENT_FIRST + (int)i;
		qdos_console_puts(con, 1, row, WINDOW_NAMES[item]);
		char value[24];
		fit_number(*window_value(v, &sh->ranges, item), value, sizeof(value), 13);
		qdos_console_puts_right(con, row, value);
		if (item == sh->window_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);
}

/* ---------------------------------------------------------------------------
 * TABLE
 *
 * The functions switched on, down a column of x from START in steps of STEP.
 * Set small, so four curves fit across beside x.
 * ------------------------------------------------------------------------- */

#define TABLE_ROWS 9
#define TABLE_COLS 4
#define TABLE_CELL 10 ///< Small-font characters a column is wide
#define TABLE_Y0 (ROW_CONTENT_FIRST * QDOS_CELL_H)

static void table_open(qdos_shell* sh, qdos_mode from) {
	sh->table_count = 0;
	if (from == QDOS_MODE_GRAPH) {
		for (int i = 0; i < sh->graph_count; i++) {
			if (is_function(sh, i)) {
				snprintf(sh->table_words[sh->table_count++], QDOS_PROGRAM_NAME_MAX, "%s", sh->graph_words[i]);
			}
		}
	} else {
		for (size_t i = 0; i < PLOT_SLOTS; i++) {
			const plot_slot* slot = &sh->slots[i];
			if (slot->on && slot->shape == QDOS_GRAPH_CURVE && !slot->broken) {
				slot_name(i, sh->table_words[sh->table_count++], QDOS_PROGRAM_NAME_MAX);
			}
		}
	}
	if (sh->table_count == 0) {
		sh->mode = from;
		set_message(sh, "NO FUNCTION IS ON", true);
		return;
	}
	sh->table_from = from;
	sh->table_first = 0;
	sh->table_sel = 0;
	sh->mode = QDOS_MODE_TABLE;
	set_message(sh, "", false);
}

static double table_x(const qdos_shell* sh, int row) {
	return sh->table_start + (double)row * sh->table_step;
}

static bool table_value(qdos_shell* sh, int col, double x, double* y) {
	graph_call call = {sh, sh->table_words[col], NULL};
	return graph_eval(&call, x, y);
}

static void handle_table_key(qdos_shell* sh, const qdos_key_event* ev) {
	char value[32];
	if (field_starts(ev)) {
		field_open(sh, FIELD_TABLE_START, "START=", "");
		field_key(sh, ev);
		return;
	}
	switch (ev->key) {
	case QDOS_KEY_UP:
		if (sh->table_sel > 0) {
			sh->table_sel--;
		} else {
			sh->table_start -= sh->table_step;
		}
		break;
	case QDOS_KEY_DOWN:
		if (sh->table_sel < TABLE_ROWS - 1) {
			sh->table_sel++;
		} else {
			sh->table_start += sh->table_step;
		}
		break;
	case QDOS_KEY_LEFT:
		sh->table_first = (sh->table_first > 0) ? sh->table_first - 1 : 0;
		break;
	case QDOS_KEY_RIGHT:
		if (sh->table_first + TABLE_COLS < sh->table_count) {
			sh->table_first++;
		}
		break;
	case QDOS_KEY_TBL_START:
		snprintf(value, sizeof(value), "%.10g", sh->table_start);
		field_open(sh, FIELD_TABLE_START, "START=", value);
		break;
	case QDOS_KEY_TBL_STEP:
		snprintf(value, sizeof(value), "%.10g", sh->table_step);
		field_open(sh, FIELD_TABLE_STEP, "STEP=", value);
		break;
	case QDOS_KEY_GRAPH:
		page_graph(sh, sh->table_from);
		break;
	case QDOS_KEY_CLEAR:
		sh->mode = sh->table_from;
		break;
	default:
		break;
	}
}

static void render_table(qdos_shell* sh, qdos_console* con) {
	const int header_y = ROW_HEADER * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2;
	const int shown = (sh->table_count - sh->table_first < TABLE_COLS) ? sh->table_count - sh->table_first : TABLE_COLS;
	qdos_console_puts_small(con, 0, header_y, "X");
	for (int c = 0; c < shown; c++) {
		char name[TABLE_CELL + 1];
		snprintf(name, sizeof(name), "%.*s", TABLE_CELL - 1, sh->table_words[sh->table_first + c]);
		qdos_console_puts_small(con, (c + 1) * TABLE_CELL * QDOS_SMALL_FONT_W, header_y, name);
	}
	qdos_console_rule(con, ROW_HEADER);

	for (int r = 0; r < TABLE_ROWS; r++) {
		const int y = TABLE_Y0 + r * QDOS_SMALL_FONT_H;
		const double x = table_x(sh, r);
		char cell[TABLE_CELL + 8];
		fit_number(x, cell, sizeof(cell), TABLE_CELL - 1);
		qdos_console_puts_small(con, 0, y, cell);
		for (int c = 0; c < shown; c++) {
			double v;
			if (table_value(sh, sh->table_first + c, x, &v)) {
				fit_number(v, cell, sizeof(cell), TABLE_CELL - 1);
			} else {
				snprintf(cell, sizeof(cell), "--");
			}
			qdos_console_puts_small(con, (c + 1) * TABLE_CELL * QDOS_SMALL_FONT_W, y, cell);
		}
		if (r == sh->table_sel) {
			qdos_console_invert_rect(con, 0, y, QDOS_SCREEN_W, QDOS_SMALL_FONT_H);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);

	// The selected row's first curve in full, where the input row is
	if (sh->field == FIELD_NONE && !sh->message[0]) {
		char line[MESSAGE_COLS + 1];
		const double x = table_x(sh, sh->table_sel);
		double v;
		if (table_value(sh, sh->table_first, x, &v)) {
			snprintf(line, sizeof(line), "%.12s(%.10g)=%.12g", sh->table_words[sh->table_first], x, v);
		} else {
			snprintf(line, sizeof(line), "%.12s(%.10g) UNDEFINED", sh->table_words[sh->table_first], x);
		}
		qdos_console_puts_small(con, 0, READOUT_Y, line);
	}
}

/* ---------------------------------------------------------------------------
 * STAT
 *
 * The lists, L1 to L6, three across, each a column to type values down. They
 * are words as well: `L1 mean`, `L1 L2 linreg`.
 * ------------------------------------------------------------------------- */

#define STAT_COLS 3
#define STAT_CELL 8
#define STAT_ROWS (ROW_CONTENT_LAST - ROW_CONTENT_FIRST + 1)

static void stat_open(qdos_shell* sh, qdos_mode from) {
	// Opened from a menu, back to where the menu was opened
	sh->stat_from = (from == QDOS_MODE_MENU) ? sh->menu_from : from;
	sh->clear_armed = false;
	sh->mode = QDOS_MODE_STAT;
	set_message(sh, "", false);
}

static void stat_into_view(qdos_shell* sh) {
	if (sh->stat_row > sh->list_len[sh->stat_col]) {
		sh->stat_row = sh->list_len[sh->stat_col];
	}
	if (sh->stat_col < sh->stat_left) {
		sh->stat_left = sh->stat_col;
	} else if (sh->stat_col >= sh->stat_left + STAT_COLS) {
		sh->stat_left = sh->stat_col - (STAT_COLS - 1);
	}
	if (sh->stat_row < sh->stat_top) {
		sh->stat_top = sh->stat_row;
	} else if (sh->stat_row >= sh->stat_top + STAT_ROWS) {
		sh->stat_top = sh->stat_row - (STAT_ROWS - 1);
	}
}

static void stat_prompt(const qdos_shell* sh, char* out, size_t cap) {
	snprintf(out, cap, "L%d(%zu)=", sh->stat_col + 1, sh->stat_row + 1);
}

static void stat_edit(qdos_shell* sh, const qdos_key_event* first) {
	char prompt[16], value[32] = "";
	stat_prompt(sh, prompt, sizeof(prompt));
	if (first == NULL && sh->stat_row < sh->list_len[sh->stat_col]) {
		snprintf(value, sizeof(value), "%.10g", sh->lists[sh->stat_col][sh->stat_row]);
	}
	field_open(sh, FIELD_STAT, prompt, value);
	if (first != NULL) {
		field_key(sh, first);
	}
}

static void stat_set(qdos_shell* sh, double value) {
	const int l = sh->stat_col;
	if (sh->stat_row >= sh->list_len[l]) {
		if (sh->list_len[l] >= STAT_LIST_MAX) {
			set_message(sh, "LIST IS FULL", true);
			return;
		}
		sh->stat_row = sh->list_len[l]++;
	}
	sh->lists[l][sh->stat_row] = value;
	list_save(sh, l);
	sh->field = FIELD_NONE;
	sh->stat_row++;
	stat_into_view(sh);
}

static void handle_stat_key(qdos_shell* sh, const qdos_key_event* ev) {
	const int l = sh->stat_col;
	const bool armed = sh->clear_armed;
	sh->clear_armed = false;

	if (field_starts(ev)) {
		stat_edit(sh, ev);
		return;
	}
	switch (ev->key) {
	case QDOS_KEY_LEFT:
		sh->stat_col = (sh->stat_col > 0) ? sh->stat_col - 1 : 0;
		break;
	case QDOS_KEY_RIGHT:
		sh->stat_col = (sh->stat_col < STAT_LISTS - 1) ? sh->stat_col + 1 : STAT_LISTS - 1;
		break;
	case QDOS_KEY_UP:
		sh->stat_row = (sh->stat_row > 0) ? sh->stat_row - 1 : 0;
		break;
	case QDOS_KEY_DOWN:
		sh->stat_row++;
		break;
	case QDOS_KEY_ENTER:
		stat_edit(sh, NULL);
		return;
	case QDOS_KEY_BACKSPACE:
		if (sh->stat_row < sh->list_len[l]) {
			memmove(&sh->lists[l][sh->stat_row], &sh->lists[l][sh->stat_row + 1],
					(sh->list_len[l] - sh->stat_row - 1) * sizeof(double));
			sh->list_len[l]--;
			list_save(sh, l);
		}
		break;
	case QDOS_KEY_LIST_CLEAR:
		if (!armed) {
			char text[32];
			snprintf(text, sizeof(text), "CLR AGAIN TO EMPTY L%d", l + 1);
			set_message(sh, text, false);
			sh->clear_armed = true;
			return;
		}
		sh->list_len[l] = 0;
		sh->stat_row = 0;
		list_save(sh, l);
		break;
	case QDOS_KEY_CALC:
		menu_open(sh, MENU_STAT_CALC);
		return;
	case QDOS_KEY_STATPLOT:
		menu_open(sh, MENU_STAT_PLOT);
		return;
	case QDOS_KEY_GRAPH:
		plot_graph(sh);
		return;
	case QDOS_KEY_CLEAR:
		sh->mode = sh->stat_from;
		return;
	default:
		break;
	}
	stat_into_view(sh);
}

static void render_stat(qdos_shell* sh, qdos_console* con) {
	for (int c = 0; c < STAT_COLS; c++) {
		char name[8];
		snprintf(name, sizeof(name), "L%d", (sh->stat_left + c + 1) % 10);
		qdos_console_puts(con, c * STAT_CELL + (STAT_CELL - 2) / 2, ROW_HEADER, name);
	}
	qdos_console_rule(con, ROW_HEADER);

	for (int r = 0; r < STAT_ROWS; r++) {
		const int row = ROW_CONTENT_FIRST + r;
		const size_t k = sh->stat_top + (size_t)r;
		for (int c = 0; c < STAT_COLS; c++) {
			const int l = sh->stat_left + c;
			if (k < sh->list_len[l]) {
				char cell[24];
				fit_number(sh->lists[l][k], cell, sizeof(cell), STAT_CELL - 1);
				qdos_console_puts(con, c * STAT_CELL + STAT_CELL - 1 - (int)strlen(cell), row, cell);
			}
			if (l == sh->stat_col && k == sh->stat_row) {
				qdos_console_invert(con, c * STAT_CELL, row, STAT_CELL);
			}
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);

	if (sh->field == FIELD_NONE && !sh->message[0]) {
		char line[48];
		stat_prompt(sh, line, sizeof(line));
		if (sh->stat_row < sh->list_len[sh->stat_col]) {
			const size_t used = strlen(line);
			snprintf(line + used, sizeof(line) - used, "%.12g", sh->lists[sh->stat_col][sh->stat_row]);
		}
		qdos_console_puts(con, 0, ROW_INPUT, line);
	}
}

/* The stat plot's lists, and how many points they make together */
static size_t stat_points(const qdos_shell* sh, const double** x, const double** y) {
	*x = sh->lists[sh->statplot_x];
	*y = sh->lists[sh->statplot_y];
	const size_t nx = sh->list_len[sh->statplot_x], ny = sh->list_len[sh->statplot_y];
	return (nx < ny) ? nx : ny;
}

/* A histogram's bar width: the x scale, or a round one for the window */
static double stat_bin(const qdos_graph_view* v) {
	return (v->xscl > 0.0) ? v->xscl : qdos_graph_tick_step(v->x1 - v->x0, 8);
}

/* Bars as far apart as there are pixels, at most */
#define STAT_BINS_MAX 400

static void stat_draw(qdos_shell* sh, const qdos_graph_area* area) {
	const qdos_graph_view* v = &sh->graph_view;
	const double *x, *y;
	const size_t nx = sh->list_len[sh->statplot_x];

	switch (sh->statplot) {
	case STATPLOT_SCATTER:
	case STATPLOT_XYLINE: {
		const size_t n = stat_points(sh, &x, &y);
		for (size_t i = 0; i < n; i++) {
			qdos_graph_draw_mark(area, v, x[i], y[i]);
			if (sh->statplot == STATPLOT_XYLINE && i + 1 < n) {
				qdos_graph_draw_segment(area, v, x[i], y[i], x[i + 1], y[i + 1], QDOS_GRAPH_SOLID);
			}
		}
		break;
	}

	case STATPLOT_HISTOGRAM: {
		x = sh->lists[sh->statplot_x];
		const double w = stat_bin(v);
		if (nx == 0 || !(w > 0.0)) {
			break;
		}
		qdos_stats1 s;
		qdos_stats_one(x, nx, &s);
		const double first = floor(s.min / w) * w;
		const double bins_needed = floor((s.max - first) / w) + 1.0;
		if (bins_needed > STAT_BINS_MAX) {
			break;
		}
		const int bins = (int)bins_needed;
		int count[STAT_BINS_MAX] = {0};
		for (size_t i = 0; i < nx; i++) {
			int b = (int)floor((x[i] - first) / w);
			b = (b < 0) ? 0 : (b >= bins ? bins - 1 : b);
			count[b]++;
		}
		for (int b = 0; b < bins; b++) {
			if (count[b] > 0) {
				qdos_graph_draw_rect(area, v, first + b * w, 0.0, first + (b + 1) * w, (double)count[b]);
			}
		}
		break;
	}

	case STATPLOT_BOX: {
		qdos_stats1 s;
		if (qdos_stats_one(sh->lists[sh->statplot_x], nx, &s)) {
			qdos_graph_draw_boxplot(area, v, s.min, s.q1, s.median, s.q3, s.max);
		}
		break;
	}

	default:
		break;
	}
}

/* lo and hi of @p n values, with a tenth either side, or one for a single value */
static void stat_range(const double* v, size_t n, double* lo, double* hi) {
	qdos_stats1 s;
	qdos_stats_one(v, n, &s);
	const double span = s.max - s.min;
	const double margin = (span > 0.0) ? span * 0.1 : 1.0;
	*lo = s.min - margin;
	*hi = s.max + margin;
}

/* ZOOM STAT: the window that shows the stat plot's data */
static void stat_zoom(qdos_shell* sh, qdos_graph_view* v) {
	const double *x, *y;
	const size_t nx = sh->list_len[sh->statplot_x];
	char text[32];
	if (nx == 0) {
		snprintf(text, sizeof(text), "L%d IS EMPTY", sh->statplot_x + 1);
		set_message(sh, text, true);
		return;
	}
	x = sh->lists[sh->statplot_x];

	if (sh->statplot == STATPLOT_HISTOGRAM) {
		qdos_stats1 s;
		qdos_stats_one(x, nx, &s);
		const double w = (s.max > s.min) ? qdos_graph_tick_step(s.max - s.min, 8) : 1.0;
		const double first = floor(s.min / w) * w;
		v->x0 = first;
		v->x1 = floor(s.max / w) * w + w;
		v->xscl = w;
		int most = 1;
		for (double edge = first; edge < v->x1 - w / 2.0; edge += w) {
			int c = 0;
			for (size_t i = 0; i < nx; i++) {
				c += (x[i] >= edge && x[i] < edge + w) ? 1 : 0;
			}
			most = (c > most) ? c : most;
		}
		v->y0 = -most / 4.0;
		v->y1 = most * 1.25;
		v->yscl = 0.0;
		return;
	}

	stat_range(x, nx, &v->x0, &v->x1);
	v->xscl = 0.0;
	if (sh->statplot == STATPLOT_BOX) {
		return;
	}
	const size_t n = stat_points(sh, &x, &y);
	if (n > 0) {
		stat_range(y, n, &v->y0, &v->y1);
		v->yscl = 0.0;
	}
}

/* ---------------------------------------------------------------------------
 * Results
 *
 * What STAT CALC found, a row each. PUSH puts the selected one on the stack;
 * TO Y puts a regression's equation in the first empty Y= slot.
 * ------------------------------------------------------------------------- */

static void result_add(qdos_shell* sh, const char* name, double value) {
	if (sh->result_count >= RESULT_ROWS) {
		return;
	}
	snprintf(sh->result_name[sh->result_count], sizeof(sh->result_name[0]), "%s", name);
	sh->result_value[sh->result_count] = value;
	sh->result_pushable[sh->result_count] = true;
	sh->result_count++;
}

static void result_open(qdos_shell* sh, const char* title) {
	snprintf(sh->result_title, sizeof(sh->result_title), "%s", title);
	sh->result_sel = 0;
	sh->result_top = 0;
	sh->mode = QDOS_MODE_RESULT;
	set_message(sh, "", false);
}

static void stat_calc(qdos_shell* sh, int action) {
	const int lx = sh->statplot_x, ly = sh->statplot_y;
	const double* x = sh->lists[lx];
	const double* y = sh->lists[ly];
	const size_t nx = sh->list_len[lx], ny = sh->list_len[ly];
	char text[48];
	sh->result_count = 0;
	sh->result_model = -1;
	sh->mode = QDOS_MODE_STAT;

	if (nx == 0) {
		snprintf(text, sizeof(text), "L%d IS EMPTY", lx + 1);
		set_message(sh, text, true);
		return;
	}

	if (action == STATCALC_ONE) {
		qdos_stats1 s;
		qdos_stats_one(x, nx, &s);
		result_add(sh, "N", (double)s.n);
		result_add(sh, "MEAN", s.mean);
		result_add(sh, "SUM", s.sum);
		result_add(sh, "SUMSQ", s.sum_sq);
		result_add(sh, "SX", s.sx);
		result_add(sh, "SIGMA", s.sigma);
		result_add(sh, "MIN", s.min);
		result_add(sh, "Q1", s.q1);
		result_add(sh, "MED", s.median);
		result_add(sh, "Q3", s.q3);
		result_add(sh, "MAX", s.max);
		snprintf(text, sizeof(text), "1-VAR L%d", lx + 1);
		result_open(sh, text);
		return;
	}

	if (nx != ny) {
		snprintf(text, sizeof(text), "L%d AND L%d DIFFER IN LENGTH", lx + 1, ly + 1);
		set_message(sh, text, true);
		return;
	}

	if (action == STATCALC_TWO) {
		qdos_stats2 s;
		qdos_stats_two(x, y, nx, &s);
		result_add(sh, "N", (double)s.n);
		result_add(sh, "MEANX", s.mean_x);
		result_add(sh, "MEANY", s.mean_y);
		result_add(sh, "SUMX", s.sum_x);
		result_add(sh, "SUMY", s.sum_y);
		result_add(sh, "SUMX2", s.sum_x2);
		result_add(sh, "SUMY2", s.sum_y2);
		result_add(sh, "SUMXY", s.sum_xy);
		result_add(sh, "SX", s.sx);
		result_add(sh, "SY", s.sy);
		result_add(sh, "R", s.r);
		snprintf(text, sizeof(text), "2-VAR L%d L%d", lx + 1, ly + 1);
		result_open(sh, text);
		return;
	}

	const qdos_regression model = (qdos_regression)action;
	double r2;
	if (!qdos_regress(model, x, y, nx, sh->result_coef, &r2)) {
		set_message(sh, "CANNOT FIT THESE POINTS", true);
		return;
	}
	static const char* const TITLE[QDOS_REG__COUNT] = {
			"LINREG Y=AX+B", "QUADREG Y=AX2+BX+C", "EXPREG Y=A*B^X", "PWRREG Y=A*X^B", "LNREG Y=A+BLNX"};
	static const char* const NAMES[3] = {"A", "B", "C"};
	for (int i = 0; i < qdos_regression_terms(model); i++) {
		result_add(sh, NAMES[i], sh->result_coef[i]);
	}
	result_add(sh, "R2", r2);
	if (model != QDOS_REG_QUADRATIC) {
		// The sign of the slope of the line the fit was made on
		const double* c = sh->result_coef;
		const double slope = (model == QDOS_REG_LINEAR) ? c[0] : (model == QDOS_REG_EXPONENTIAL) ? log(c[1]) : c[1];
		result_add(sh, "R", slope < 0.0 ? -sqrt(r2) : sqrt(r2));
	}
	sh->result_model = (int)model;
	result_open(sh, TITLE[model]);
}

/* The regression as a Y= body, in RPN */
static void regression_body(qdos_regression model, const double* c, char* out, size_t cap) {
	switch (model) {
	case QDOS_REG_LINEAR:
		snprintf(out, cap, "x %.10g * %.10g +", c[0], c[1]);
		break;
	case QDOS_REG_QUADRATIC:
		snprintf(out, cap, "x x * %.10g * x %.10g * + %.10g +", c[0], c[1], c[2]);
		break;
	case QDOS_REG_EXPONENTIAL:
		snprintf(out, cap, "%.10g x pow %.10g *", c[1], c[0]);
		break;
	case QDOS_REG_POWER:
		snprintf(out, cap, "x %.10g pow %.10g *", c[1], c[0]);
		break;
	default:
		snprintf(out, cap, "x ln %.10g * %.10g +", c[1], c[0]);
		break;
	}
}

static void result_to_slot(qdos_shell* sh) {
	if (sh->result_model < 0) {
		set_message(sh, "NOT A REGRESSION", true);
		return;
	}
	for (size_t i = 0; i < PLOT_SLOTS; i++) {
		plot_slot* slot = &sh->slots[i];
		if (slot->shape != QDOS_GRAPH_NONE) {
			continue;
		}
		regression_body((qdos_regression)sh->result_model, sh->result_coef, slot->body, sizeof(slot->body));
		slot->on = slot_declare(sh, i);
		slot_save(sh, i);
		char text[16];
		snprintf(text, sizeof(text), "IN Y%zu", i + 1);
		set_message(sh, text, false);
		return;
	}
	set_message(sh, "NO EMPTY SLOT", true);
}

static void handle_result_key(qdos_shell* sh, const qdos_key_event* ev) {
	const size_t n = (size_t)sh->result_count;
	switch (ev->key) {
	case QDOS_KEY_UP:
		sh->result_sel = (sh->result_sel + n - 1) % n;
		break;
	case QDOS_KEY_DOWN:
		sh->result_sel = (sh->result_sel + 1) % n;
		break;
	case QDOS_KEY_ENTER: {
		graph_push(sh, &sh->result_value[sh->result_sel], 1);
		char text[24];
		snprintf(text, sizeof(text), "PUSHED %s", sh->result_name[sh->result_sel]);
		set_message(sh, text, false);
		break;
	}
	case QDOS_KEY_TO_Y:
		result_to_slot(sh);
		break;
	case QDOS_KEY_CLEAR:
		sh->mode = QDOS_MODE_STAT;
		break;
	default:
		break;
	}
	if (sh->result_sel < sh->result_top) {
		sh->result_top = sh->result_sel;
	} else if (sh->result_sel >= sh->result_top + LIST_ROWS) {
		sh->result_top = sh->result_sel - (LIST_ROWS - 1);
	}
}

static void render_result(qdos_shell* sh, qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, sh->result_title);
	qdos_console_rule(con, ROW_HEADER);
	for (size_t i = 0; i < LIST_ROWS; i++) {
		const size_t item = sh->result_top + i;
		if (item >= (size_t)sh->result_count) {
			break;
		}
		const int row = ROW_CONTENT_FIRST + (int)i;
		qdos_console_puts(con, 1, row, sh->result_name[item]);
		char value[24];
		fit_number(sh->result_value[item], value, sizeof(value), 16);
		qdos_console_puts_right(con, row, value);
		if (item == sh->result_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);
}

/* Where a finished number goes */
static void field_commit(qdos_shell* sh, double value) {
	const field_target target = sh->field;
	sh->field = FIELD_NONE;

	switch (target) {
	case FIELD_WINDOW:
		sh->field = FIELD_WINDOW; // until it is taken, so a refusal can be corrected
		window_set(sh, sh->window_sel, value);
		break;

	case FIELD_TRACE_X:
		if (on_points(sh)) {
			const qdos_graph_points* p = &sh->graph_points[sh->graph_curve];
			const double k = round((value - p->t0) / p->step);
			sh->graph_index = (k < 0.0) ? 0 : (k >= p->count ? p->count - 1 : (int)k);
		} else {
			trace_to(sh, value);
		}
		break;

	case FIELD_CALC:
		if (sh->graph_state == GRAPH_ASK) {
			calc_accept_x(sh, value);
			break;
		}
		{
			// VALUE: y there, pushed
			double y;
			trace_to(sh, value);
			if (curve_at(sh, sh->graph_curve, value, &y)) {
				graph_push(sh, &y, 1);
				snprintf(sh->graph_result, sizeof(sh->graph_result), "%.12s X=%.10g Y=%.10g",
						sh->graph_words[sh->graph_curve], value, y);
			} else {
				set_message(sh, "UNDEFINED THERE", true);
			}
		}
		break;

	case FIELD_TABLE_START:
		sh->table_start = value;
		window_save(sh);
		break;

	case FIELD_TABLE_STEP:
		if (value == 0.0) {
			set_message(sh, "STEP CANNOT BE 0", true);
			break;
		}
		sh->table_step = value;
		window_save(sh);
		break;

	case FIELD_STAT:
		sh->field = FIELD_STAT;
		stat_set(sh, value);
		break;

	default:
		break;
	}
}

/* Line @p i of the last output, or NULL once the ring has moved past it */
static const char* output_line(const qdos_shell* sh, size_t i) {
	const size_t oldest = sh->log_count - log_held(sh);
	const size_t at = sh->output_first + i;
	return (at >= oldest && i < sh->output_count) ? log_at(sh, at - oldest) : NULL;
}

static void handle_output_key(qdos_shell* sh, const qdos_key_event* ev) {
	const size_t last = (sh->output_count > LIST_ROWS) ? sh->output_count - LIST_ROWS : 0;
	switch (ev->key) {
	case QDOS_KEY_UP:
		sh->output_top = (sh->output_top > 0) ? sh->output_top - 1 : 0;
		break;
	case QDOS_KEY_DOWN:
		sh->output_top = (sh->output_top < last) ? sh->output_top + 1 : last;
		break;
	case QDOS_KEY_CLEAR:
	case QDOS_KEY_ENTER:
		sh->mode = sh->output_from;
		break;
	default:
		break;
	}
}

static void render_output(qdos_shell* sh, qdos_console* con) {
	char header[QDOS_COLS + 1];
	snprintf(header, sizeof(header), "%zu LINES", sh->output_count);
	qdos_console_puts(con, 0, ROW_HEADER, "OUTPUT");
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);
	for (size_t i = 0; i < (size_t)LIST_ROWS; i++) {
		const char* text = output_line(sh, sh->output_top + i);
		if (text != NULL) {
			qdos_console_puts(con, 0, ROW_CONTENT_FIRST + (int)i, text);
		}
	}
	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void handle_mode_key(qdos_shell* sh, const qdos_key_event* ev) {
	// Only the calculator asks which register
	if (sh->mode != QDOS_MODE_CALC) {
		sh->register_wait = REGISTER_IDLE;
	}

	// A number being typed has every key but the one that turns the machine off
	if (sh->field != FIELD_NONE && ev->key != QDOS_KEY_POWER) {
		field_key(sh, ev);
		return;
	}

	if (ev->key == QDOS_KEY_LIST && sh->mode != QDOS_MODE_LIST) {
		list_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_CATALOG && sh->mode != QDOS_MODE_LIST) {
		catalog_open(sh);
		return;
	}

	if (ev->key == QDOS_KEY_POWER) {
		sh->powering_off = true;
		return;
	}

	// The pages that draw have GRAPH as their own key
	if (ev->key == QDOS_KEY_GRAPH && sh->mode != QDOS_MODE_PLOT && sh->mode != QDOS_MODE_WINDOW &&
			sh->mode != QDOS_MODE_TABLE && sh->mode != QDOS_MODE_STAT) {
		plot_open(sh);
		return;
	}

	// Wherever it is pressed from: the soft label reads back as the new setting,
	// so nothing else has to be said
	if (ev->key == QDOS_KEY_ANGLE) {
		qdos_math_set_degrees(!qdos_math_degrees());
		save_settings(sh);
		return;
	}

	if (ev->key == QDOS_KEY_SETTINGS && sh->mode != QDOS_MODE_SETTINGS) {
		sh->page_from = (sh->mode == QDOS_MODE_DEBUG) ? sh->page_from : sh->mode;
		sh->mode = QDOS_MODE_SETTINGS;
		set_message(sh, "", false);
		return;
	}

	if (ev->key == QDOS_KEY_DEBUG && sh->mode != QDOS_MODE_DEBUG) {
		sh->page_from = (sh->mode == QDOS_MODE_SETTINGS) ? sh->page_from : sh->mode;
		// Open at the end, where the newest lines are
		const size_t held = log_held(sh);
		sh->log_top = (held > (size_t)LIST_ROWS) ? held - (size_t)LIST_ROWS : 0;
		sh->mode = QDOS_MODE_DEBUG;
		set_message(sh, "", false);
		return;
	}

	if (ev->key == QDOS_KEY_ABOUT && sh->mode != QDOS_MODE_ABOUT) {
		sh->about_from = sh->mode;
		sh->mode = QDOS_MODE_ABOUT;
		set_message(sh, "", false);
		return;
	}

	if (sh->mode == QDOS_MODE_SETTINGS) {
		handle_settings_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_DEBUG) {
		handle_debug_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_ABOUT) {
		// Any way out will do
		if (ev->key == QDOS_KEY_CLEAR || ev->key == QDOS_KEY_ENTER || ev->key == QDOS_KEY_ABOUT) {
			sh->mode = sh->about_from;
		}
	} else if (sh->mode == QDOS_MODE_EDIT) {
		handle_edit_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_GRAPH) {
		handle_graph_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_GRAPH3) {
		handle_graph3_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_PLOT) {
		handle_plot_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_PLOT_EDIT) {
		handle_plot_edit_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_MENU) {
		handle_menu_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_WINDOW) {
		handle_window_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_TABLE) {
		handle_table_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_STAT) {
		handle_stat_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_RESULT) {
		handle_result_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_OUTPUT) {
		handle_output_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_LIST) {
		handle_list_key(sh, ev);
	} else if (sh->mode == QDOS_MODE_LINE) {
		handle_line_key(sh, ev);
	} else {
		handle_calc_key(sh, ev);
	}
}

static void render_edit(qdos_shell* sh, qdos_console* con) {
	size_t line, col;
	qdos_editor_where(&sh->ed, &line, &col);

	const char mod = modifier_char(sh);
	char header[QDOS_COLS + 1];
	if (mod != '\0') {
		snprintf(header, sizeof(header), "%c %zu:%zu", mod, line + 1, col + 1);
	} else {
		snprintf(header, sizeof(header), "%zu:%zu", line + 1, col + 1);
	}

	qdos_console_puts(con, 0, ROW_HEADER, sh->ed.name);
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	// One horizontal offset for the whole pane, so columns stay aligned
	const size_t width = EDIT_COLS - 1;
	const size_t left = (col >= width) ? col - width + 1 : 0;

	for (size_t i = 0; i < EDIT_ROWS; i++) {
		size_t len = 0;
		const char* text = qdos_editor_line(&sh->ed, sh->ed_top + i, &len);
		if (text == NULL) {
			break;
		}

		const int y = EDIT_Y0 + (int)i * QDOS_SMALL_FONT_H;
		for (size_t c = 0; left + c < len && c < width; c++) {
			const char ch = text[left + c];
			qdos_console_putc_small(con, (int)c * QDOS_SMALL_FONT_W, y, ch == '\t' ? ' ' : ch);
		}

		if (sh->ed_top + i == line && sh->cursor_on) {
			qdos_console_invert_rect(
					con, (int)(col - left) * QDOS_SMALL_FONT_W, y, QDOS_SMALL_FONT_W, QDOS_SMALL_FONT_H);
		}
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_about(qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "ABOUT");
	qdos_console_rule(con, ROW_HEADER);

	char line[QDOS_COLS + 1];
	int row = ROW_CONTENT_FIRST;

	snprintf(line, sizeof(line), "QDOS %s", QDOS_VERSION);
	qdos_console_puts(con, 0, row++, line);

	snprintf(line, sizeof(line), "BUILD %s", QDOS_COMMIT);
	qdos_console_puts(con, 0, row++, line);

	snprintf(line, sizeof(line), "PANEL %dX%d 1-BIT", QDOS_SCREEN_W, QDOS_SCREEN_H);
	qdos_console_puts(con, 0, row++, line);

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static const char* angle_text(void) {
	return qdos_math_degrees() ? "DEG" : "RAD";
}

static void decimals_text(const qdos_shell* sh, char* out, size_t cap) {
	if (sh->decimals == DECIMALS_AUTO) {
		snprintf(out, cap, "AUTO");
	} else {
		snprintf(out, cap, "%d", sh->decimals);
	}
}

static void auto_off_text(const qdos_shell* sh, char* out, size_t cap) {
	const int minutes = AUTO_OFF_MINUTES[sh->auto_off];
	if (minutes == 0) {
		snprintf(out, cap, "NEVER");
	} else {
		snprintf(out, cap, "%d MIN", minutes);
	}
}

static void render_settings(qdos_shell* sh, qdos_console* con) {
	qdos_console_puts(con, 0, ROW_HEADER, "SETTINGS");
	qdos_console_rule(con, ROW_HEADER);

	char value[16];
	decimals_text(sh, value, sizeof(value));

	char off[16];
	auto_off_text(sh, off, sizeof(off));

	char blocked[16];
	snprintf(blocked, sizeof(blocked), "%zu BLOCKED", qdos_natives_blocked_count(&sh->natives));

	static const char* const NAMES[SETTING__COUNT] = {"ANGLE", "DECIMALS", "AUTO OFF", "USB", "MODULES"};
	const char* values[SETTING__COUNT] = {angle_text(), value, off, sh->usb_exported ? "SHARED" : "OFF", blocked};

	qdos_setting shown[SETTING__COUNT];
	const size_t count = settings_visible(sh, shown);

	for (size_t i = 0; i < count; i++) {
		const int row = ROW_CONTENT_FIRST + (int)i;
		qdos_console_puts(con, 1, row, NAMES[shown[i]]);
		qdos_console_puts_right(con, row, values[shown[i]]);
		if (i == sh->setting_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_debug(qdos_shell* sh, qdos_console* con) {
	char header[QDOS_COLS + 1];
	snprintf(header, sizeof(header), "%zu", sh->log_count);
	qdos_console_puts(con, 0, ROW_HEADER, "DEBUG");
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	const size_t held = log_held(sh);
	if (held == 0) {
		qdos_console_puts(con, 1, ROW_CONTENT_FIRST, "NOTHING LOGGED");
	}

	for (size_t i = 0; i < (size_t)LIST_ROWS; i++) {
		const char* text = log_at(sh, sh->log_top + i);
		if (text == NULL) {
			break;
		}
		qdos_console_puts(con, 0, ROW_CONTENT_FIRST + (int)i, text);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

static void render_list(qdos_shell* sh, qdos_console* con) {
	const size_t total = list_count(sh);

	char header[QDOS_COLS + 1];
	snprintf(header, sizeof(header), "%zu/%zu", total ? sh->list_sel + 1 : 0, total);
	qdos_console_puts(con, 0, ROW_HEADER, sh->list_all ? "CATALOG" : "APPS");
	qdos_console_puts_right(con, ROW_HEADER, header);
	qdos_console_rule(con, ROW_HEADER);

	for (size_t i = 0; i < LIST_ROWS; i++) {
		const size_t item = sh->list_top + i;
		if (item >= total) {
			break;
		}

		const int row = ROW_CONTENT_FIRST + (int)i;

		const qdos_native_entry* module = list_module(sh, item);
		if (module != NULL) {
			char shown[QDOS_PROGRAM_NAME_MAX + 3];
			snprintf(shown, sizeof(shown), "%s%s", module->name, qdos_native_is_app(module) ? "" : "::");
			qdos_console_puts(con, 1, row, shown);

			// What went wrong outranks where it came from
			qdos_console_puts_right(con, row,
					module->error[0] ? module->error : origin_text(module->system, module->inbox, module->user));
		} else {
			qdos_console_puts(con, 1, row, list_name(sh, item));

			const qdos_program_entry* e = list_program(sh, item);
			if (e != NULL) {
				qdos_console_puts_right(con, row, origin_text(e->system, e->inbox, e->user));
			}
		}
		if (item == sh->list_sel) {
			qdos_console_invert(con, 0, row, QDOS_COLS);
		}
	}

	if (total == 0) {
		qdos_console_puts(con, 1, ROW_CONTENT_FIRST, "NONE INSTALLED");
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
}

/** @brief Whatever the shell last had to say, under the content */
static void render_message(qdos_shell* sh, qdos_console* con) {
	if (!sh->message[0]) {
		return;
	}

	if (!sh->message_is_error) {
		qdos_console_puts(con, 0, ROW_MESSAGE, sh->message);
		return;
	}

	// Centred in the row, on a band the full width of the panel, so an error
	// reads as one at a glance however short it is
	const int y = ROW_MESSAGE * QDOS_CELL_H;
	const int len = (int)strlen(sh->message);
	for (int i = 0; i < len; i++) {
		qdos_console_putc_small(con, i * QDOS_SMALL_FONT_W, y + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2, sh->message[i]);
	}
	qdos_console_invert_rect(con, 0, y, QDOS_SCREEN_W, QDOS_CELL_H);
}

/** @brief The time and the charge as the band would show them; either may be empty */
static void status_text(const qdos_shell* sh, char* clock, size_t clock_cap, char* charge, size_t charge_cap) {
	clock[0] = '\0';
	charge[0] = '\0';

	int seconds;
	if (sh->hal->time_of_day != NULL && sh->hal->time_of_day(sh->hal, &seconds)) {
		snprintf(clock, clock_cap, "%02d:%02d", seconds / 3600, seconds / 60 % 60);
	}

	const int percent = (sh->hal->battery != NULL) ? sh->hal->battery(sh->hal) : -1;
	if (percent >= 0) {
		snprintf(charge, charge_cap, "%d%%", percent);
	}
}

/* Whether the band says something other than it did at the last repaint */
static bool status_stale(const qdos_shell* sh) {
	char clock[8], charge[8], both[16];
	status_text(sh, clock, sizeof(clock), charge, sizeof(charge));
	snprintf(both, sizeof(both), "%s|%s", clock, charge);
	return strcmp(both, sh->status) != 0;
}

/*
 * The band is there on every screen whether or not it has anything to say, so
 * nothing below it moves when the clock is set or a battery turns up.
 */
static void render_status(qdos_shell* sh, qdos_console* con) {
	char clock[8], charge[8];
	status_text(sh, clock, sizeof(clock), charge, sizeof(charge));
	snprintf(sh->status, sizeof(sh->status), "%s|%s", clock, charge);

	const int y = ROW_STATUS * QDOS_CELL_H + (QDOS_CELL_H - QDOS_SMALL_FONT_H) / 2;
	const int margin = QDOS_SMALL_FONT_W;
	for (int i = 0; clock[i] != '\0'; i++) {
		qdos_console_putc_small(con, margin + i * QDOS_SMALL_FONT_W, y, clock[i]);
	}
	const int right = QDOS_SCREEN_W - margin - (int)strlen(charge) * QDOS_SMALL_FONT_W;
	for (int i = 0; charge[i] != '\0'; i++) {
		qdos_console_putc_small(con, right + i * QDOS_SMALL_FONT_W, y, charge[i]);
	}
	qdos_console_invert_rect(con, 0, ROW_STATUS * QDOS_CELL_H, QDOS_SCREEN_W, QDOS_CELL_H);
}

/** @brief Repaint the whole display */
static void render(qdos_shell* sh) {
	qdos_console* con = &sh->con;
	graph_refresh(sh);
	qdos_console_clear(con);
	render_status(sh, con);

	if (sh->mode == QDOS_MODE_PLOT || sh->mode == QDOS_MODE_PLOT_EDIT) {
		render_plot(sh, con);
		if (sh->mode == QDOS_MODE_PLOT_EDIT && !sh->message[0]) {
			render_plot_edit(sh, con);
		}
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_GRAPH || sh->mode == QDOS_MODE_GRAPH3) {
		if (sh->mode == QDOS_MODE_GRAPH) {
			render_graph(sh, con);
		} else {
			render_graph3(sh, con);
		}
		render_field(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_MENU || sh->mode == QDOS_MODE_WINDOW || sh->mode == QDOS_MODE_TABLE ||
			sh->mode == QDOS_MODE_STAT || sh->mode == QDOS_MODE_RESULT) {
		if (sh->mode == QDOS_MODE_MENU) {
			render_menu(sh, con);
		} else if (sh->mode == QDOS_MODE_WINDOW) {
			render_window(sh, con);
		} else if (sh->mode == QDOS_MODE_TABLE) {
			render_table(sh, con);
		} else if (sh->mode == QDOS_MODE_STAT) {
			render_stat(sh, con);
		} else {
			render_result(sh, con);
		}
		render_field(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_EDIT) {
		render_edit(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_LIST) {
		// The delete prompt lives here, so the list has to show messages too
		render_list(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_SETTINGS) {
		// Sharing the card reports from here, so this page needs one as well
		render_settings(sh, con);
		render_message(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_OUTPUT) {
		render_output(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_DEBUG) {
		render_debug(sh, con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	if (sh->mode == QDOS_MODE_ABOUT) {
		render_about(con);
		render_soft(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	const size_t depth = qd_interp_depth(sh->interp);

	// Top of stack nearest the input line; deeper than the rows hold, the top
	// one says so instead of holding a value.
	const size_t visible = (depth <= (size_t)STACK_ROWS) ? depth : (size_t)STACK_ROWS - 1;
	for (size_t i = 0; i < visible; i++) {
		qd_interp_value value;
		if (!qd_interp_peek(sh->interp, i, &value)) {
			continue;
		}

		const int row = ROW_CONTENT_LAST - (int)i;

		char label[16];
		snprintf(label, sizeof(label), "%zu:", i + 1);
		const int used = qdos_console_puts(con, 0, row, label);

		char shown[QD_INTERP_VALUE_TEXT_MAX];
		const size_t room = (size_t)(QDOS_COLS - used - 1);
		if (!format_array(sh, i, shown, sizeof(shown), room)) {
			format_value(sh, &value, shown, sizeof(shown), room);
		}
		qdos_console_puts_right_within(con, row, used + 1, shown);
	}

	// Its own row: sharing one with a value made the entry look like the marker
	if (depth > visible) {
		char hidden[QDOS_COLS + 1];
		snprintf(hidden, sizeof(hidden), "%zu MORE", depth - visible);
		qdos_console_puts(con, 0, ROW_STACK_FIRST, hidden);
	}

	qdos_console_rule(con, ROW_CONTENT_LAST);
	render_soft(sh, con);

	// The message and the input line are the same row, so only one is on it.
	// What was typed is still there underneath, and the next key brings it back.
	if (sh->message[0]) {
		render_message(sh, con);
		sh->hal->present(sh->hal, con->fb);
		return;
	}

	const bool line_mode = (sh->mode == QDOS_MODE_LINE);
	const char* prompt = line_mode ? LINE_PROMPT : PROMPT;
	const char* text = line_mode ? sh->input : sh->entry;
	size_t len = line_mode ? sh->input_len : sh->entry_len;

	if (line_mode) {
		const char* newline = strrchr(sh->input, '\n');
		if (newline != NULL) {
			prompt = CONT_PROMPT;
			text = newline + 1;
			len = sh->input_len - (size_t)(newline + 1 - sh->input);
		}
	}

	// After the prompt, with a space of its own: without one it reads as the
	// first letter of what is being typed
	const char mod = modifier_char(sh);
	char shown_prompt[PROMPT_LEN + 2] = {prompt[0], prompt[1], '\0', '\0'};
	int prompt_len = PROMPT_LEN;
	if (mod != '\0') {
		shown_prompt[1] = mod;
		shown_prompt[2] = ' ';
		prompt_len = PROMPT_LEN + 1;
	}

	qdos_console_puts(con, 0, ROW_INPUT, shown_prompt);

	const int room = QDOS_COLS - prompt_len - 1; // reserve a cell for the cursor

	// Scroll so the cursor stays on screen, rather than always showing the tail
	size_t caret = line_mode ? sh->input_cursor - (size_t)(text - sh->input) : len;
	if (caret > len) {
		caret = len;
	}
	const size_t start = (caret > (size_t)room) ? caret - (size_t)room : 0;

	qdos_console_puts(con, prompt_len, ROW_INPUT, text + start);
	if (sh->cursor_on) {
		qdos_console_invert(con, prompt_len + (int)(caret - start), ROW_INPUT, 1);
	}

	sh->hal->present(sh->hal, con->fb);
}

/* ------------------------------------------------------------------------ */
/* Native words                                                             */
/*                                                                          */
/* The machine's own capabilities, exposed as words in the language.         */
/* ------------------------------------------------------------------------ */

/** Take the top of stack as a storable value. */
static bool pop_value(qd_context* ctx, qdos_value* out) {
	const qd_stack* st = ctx->st;
	if (st->size == 0) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	switch (st->data[st->size - 1].type) {
	case QD_STACK_TYPE_FLOAT:
		out->type = QDOS_VALUE_FLOAT;
		return qd_pop_f(ctx, &out->f) == 0;
	case QD_STACK_TYPE_INT:
		out->type = QDOS_VALUE_INT;
		return qd_pop_i(ctx, &out->i) == 0;
	case QD_STACK_TYPE_STR:
		out->type = QDOS_VALUE_STRING;
		return qd_pop_s(ctx, out->s, sizeof(out->s)) == 0;
	default:
		return false;
	}
}

/** Push a stored value back. */
static int push_value(qd_context* ctx, const qdos_value* value) {
	switch (value->type) {
	case QDOS_VALUE_INT:
		return qd_push_i(ctx, value->i);
	case QDOS_VALUE_FLOAT:
		return qd_push_f(ctx, value->f);
	case QDOS_VALUE_STRING:
		return qd_push_s(ctx, value->s);
	case QDOS_VALUE_EMPTY:
		break;
	}
	qd_set_error_msg(ctx, "REGISTER IS EMPTY");
	return 1;
}

/** Pop a register number and name its storage entry. */
static bool pop_register_key(qd_context* ctx, const char* word, char* key, size_t cap) {
	int64_t slot = 0;
	if (qd_pop_i(ctx, &slot) != 0) {
		qd_set_error_msg(ctx, "REGISTER MUST BE A NUMBER");
		return false;
	}
	if (!qdos_register_key(slot, key, cap)) {
		char message[64];
		snprintf(message, sizeof(message), "%s: REGISTER 0 TO %d", word, QDOS_REGISTER_MAX);
		qd_set_error_msg(ctx, message);
		return false;
	}
	return true;
}

/** `sto` - (value slot -- ) store a value in a numbered register */
static int native_sto(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "sto", key, sizeof(key))) {
		return 1;
	}

	qdos_value value;
	if (!pop_value(ctx, &value)) {
		qd_set_error_msg(ctx, "sto: NOTHING TO STORE");
		return 1;
	}

	if (qdos_storage_save(sh->hal, key, &value) != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "sto: STORAGE WRITE FAILED");
		return 1;
	}
	return 0;
}

/** `rcl` - (slot -- value) recall a numbered register */
static int native_rcl(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "rcl", key, sizeof(key))) {
		return 1;
	}

	qdos_value value;
	const qdos_store_result result = qdos_storage_load(sh->hal, key, &value);
	if (result == QDOS_STORE_NOT_FOUND) {
		qd_set_error_msg(ctx, "rcl: REGISTER IS EMPTY");
		return 1;
	}
	if (result != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "rcl: STORAGE READ FAILED");
		return 1;
	}
	return push_value(ctx, &value);
}

/** `clr` - (slot -- ) empty a numbered register */
static int native_clr(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char key[32];
	if (!pop_register_key(ctx, "clr", key, sizeof(key))) {
		return 1;
	}

	if (qdos_storage_erase(sh->hal, key) != QDOS_STORE_OK) {
		qd_set_error_msg(ctx, "clr: STORAGE WRITE FAILED");
		return 1;
	}
	return 0;
}

/** `forget` - (name -- ) remove a Quadrate-defined word */
static int native_forget(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_VALUE_STRING_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "forget: NEED A STRING");
		return 1;
	}

	char message[80];

	// A read-only program cannot be removed, only overridden -- but a word
	// written at the prompt covering one is yours, and dropping it is what
	// brings the shipped version back
	if (!declared_here(sh, name) && qdos_program_is_readonly(sh->hal, name) && !qdos_program_is_user(sh->hal, name)) {
		const bool card = qdos_program_is_inbox(sh->hal, name);
		snprintf(message, sizeof(message), "'%.30s' IS %s", name, card ? "ON THE CARD" : "BUILT IN");
		qd_set_error_msg(ctx, message);
		return 1;
	}

	if (!qd_interp_undeclare(sh->interp, name)) {
		snprintf(message, sizeof(message), "'%.32s' IS NOT DECLARED", name);
		qd_set_error_msg(ctx, message);
		return 1;
	}

	forget_here(sh, name);
	qdos_program_erase(sh->hal, name);

	// Forgetting an override brings back whatever it was covering, card before
	// firmware, so the same copy wins as at startup
	char source[QDOS_PROGRAM_MAX];
	if (qdos_program_load(sh->hal, QDOS_SCOPE_INBOX, name, source, sizeof(source)) == QDOS_STORE_OK ||
			qdos_program_load(sh->hal, QDOS_SCOPE_SYSTEM, name, source, sizeof(source)) == QDOS_STORE_OK) {
		qdos_guarded_eval(sh->interp, source);
	}

	return 0;
}

/**
 * `edit` - (name -- ) load a program's source into the input line
 *
 * Flattened to one line with comments dropped: the input is a single line and
 * Quadrate does not care about the whitespace.
 */
static int native_edit(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_PROGRAM_NAME_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "edit: NEED A STRING");
		return 1;
	}

	char source[QDOS_PROGRAM_MAX];
	const bool found = load_program_anywhere(sh, name, source, sizeof(source));

	// An unknown name starts a new program rather than being an error
	edit_open(sh, name, found ? source : NULL);
	return 0;
}

/** `graph` - ( name:str -- ) plot a word taking x and leaving y */
static int native_graph(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_PROGRAM_NAME_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "graph: NEED A STRING");
		return 1;
	}

	graph_open(sh, name);
	return 0;
}

/** `graph3` - ( name:str -- ) plot a word taking x and y and leaving z */
static int native_graph3(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	char name[QDOS_PROGRAM_NAME_MAX];
	if (qd_pop_s(ctx, name, sizeof(name)) != 0) {
		qd_set_error_msg(ctx, "graph3: NEED A STRING");
		return 1;
	}

	graph3_open(sh, name);
	return 0;
}

/* Numbers off the top, the last of them first, then the name of a word under them */
static bool pop_word_and_numbers(qd_context* ctx, const char* word, int count, double* args, char* name, size_t cap) {
	for (int i = count - 1; i >= 0; i--) {
		qd_stack_element_t e;
		if (qd_stack_pop(ctx->st, &e) != QD_STACK_OK ||
				(e.type != QD_STACK_TYPE_INT && e.type != QD_STACK_TYPE_FLOAT)) {
			qdos_math_error(ctx, word, "NEEDS A NUMBER");
			return false;
		}
		args[i] = (e.type == QD_STACK_TYPE_INT) ? (double)e.value.i : e.value.f;
	}
	if (qd_pop_s(ctx, name, cap) != 0) {
		qdos_math_error(ctx, word, "NEEDS A WORD'S NAME");
		return false;
	}
	return true;
}

/* Why a word calling another found nothing: the other failing, or no answer */
static int numeric_fail(qd_context* ctx, qdos_shell* sh, const char* word, const char* none) {
	return qdos_math_error(ctx, word, sh->graph_error[0] ? sh->graph_error : none);
}

/** `root` - ( name:str a:f64 b:f64 -- x:f64 ) where the word is nought, between a and b */
static int native_root(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	char name[QDOS_PROGRAM_NAME_MAX];
	double a[2], x;
	if (!pop_word_and_numbers(ctx, "root", 2, a, name, sizeof(name))) {
		return 1;
	}
	graph_call call = {sh, name, NULL};
	sh->graph_error[0] = '\0';
	if (!qdos_num_root(graph_eval, &call, a[0], a[1], &x)) {
		return numeric_fail(ctx, sh, "root", "NO ROOT IN RANGE");
	}
	return qd_push_f(ctx, x);
}

static int extremum_word(qd_context* ctx, qdos_shell* sh, const char* word, bool low) {
	char name[QDOS_PROGRAM_NAME_MAX];
	double a[2], x, y;
	if (!pop_word_and_numbers(ctx, word, 2, a, name, sizeof(name))) {
		return 1;
	}
	graph_call call = {sh, name, NULL};
	sh->graph_error[0] = '\0';
	if (!(low ? qdos_num_minimum : qdos_num_maximum)(graph_eval, &call, a[0], a[1], &x, &y)) {
		return numeric_fail(ctx, sh, word, "NO VALUE IN RANGE");
	}
	return qd_push_f(ctx, x);
}

/** `fmin` - ( name:str a:f64 b:f64 -- x:f64 ) where the word is lowest between a and b */
static int native_fmin(qd_context* ctx, void* userdata) {
	return extremum_word(ctx, userdata, "fmin", true);
}

/** `fmax` - ( name:str a:f64 b:f64 -- x:f64 ) and where it is highest */
static int native_fmax(qd_context* ctx, void* userdata) {
	return extremum_word(ctx, userdata, "fmax", false);
}

/** `nderiv` - ( name:str x:f64 -- d:f64 ) the word's slope at x */
static int native_nderiv(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	char name[QDOS_PROGRAM_NAME_MAX];
	double a[1], d;
	if (!pop_word_and_numbers(ctx, "nderiv", 1, a, name, sizeof(name))) {
		return 1;
	}
	graph_call call = {sh, name, NULL};
	sh->graph_error[0] = '\0';
	if (!qdos_num_derivative(graph_eval, &call, a[0], &d)) {
		return numeric_fail(ctx, sh, "nderiv", "UNDEFINED THERE");
	}
	return qd_push_f(ctx, d);
}

/** `fnint` - ( name:str a:f64 b:f64 -- area:f64 ) the integral of the word from a to b */
static int native_fnint(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	char name[QDOS_PROGRAM_NAME_MAX];
	double a[2], area;
	if (!pop_word_and_numbers(ctx, "fnint", 2, a, name, sizeof(name))) {
		return 1;
	}
	graph_call call = {sh, name, NULL};
	sh->graph_error[0] = '\0';
	if (!qdos_num_integral(graph_eval, &call, a[0], a[1], &area)) {
		return numeric_fail(ctx, sh, "fnint", "UNDEFINED IN RANGE");
	}
	return qd_push_f(ctx, area);
}

/** `intersect` - ( f:str g:str a:f64 b:f64 -- x:f64 ) where two words are equal, between a and b */
static int native_intersect(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	char f[QDOS_PROGRAM_NAME_MAX], g[QDOS_PROGRAM_NAME_MAX];
	double a[2], x;
	if (!pop_word_and_numbers(ctx, "intersect", 2, a, g, sizeof(g))) {
		return 1;
	}
	if (qd_pop_s(ctx, f, sizeof(f)) != 0) {
		return qdos_math_error(ctx, "intersect", "NEEDS TWO WORDS' NAMES");
	}
	graph_call call = {sh, f, g};
	sh->graph_error[0] = '\0';
	if (!qdos_num_root(difference_eval, &call, a[0], a[1], &x)) {
		return numeric_fail(ctx, sh, "intersect", "NO CROSSING IN RANGE");
	}
	return qd_push_f(ctx, x);
}

/** `L1` to `L6` - ( -- xs:[]f64 ) a list, as the STAT page holds it */
static int native_list(qd_context* ctx, void* userdata) {
	const list_word* w = userdata;
	return qdos_push_numbers(ctx, w->sh->lists[w->index], w->sh->list_len[w->index]);
}

/** `lsto` - ( xs:[]f64 n:i64 -- ) store a list of numbers as Ln */
static int native_lsto(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	int64_t n;
	if (qd_pop_i(ctx, &n) != 0 || n < 1 || n > STAT_LISTS) {
		return qdos_math_error(ctx, "lsto", "NEEDS A LIST NUMBER, 1 TO 6");
	}
	double* values;
	size_t count;
	if (!qdos_pop_numbers(ctx, "lsto", &values, &count)) {
		return 1;
	}
	if (count > STAT_LIST_MAX) {
		free(values);
		return qdos_math_error(ctx, "lsto", "MORE THAN 99 VALUES");
	}
	memcpy(sh->lists[n - 1], values, count * sizeof(double));
	sh->list_len[n - 1] = count;
	free(values);
	list_save(sh, (int)(n - 1));
	return 0;
}

/**
 * `qdos::key` - ( -- key:i64 ch:i64 got:i64) the next keypress, if there is one
 *
 * For a program that has taken the screen. The shell is waiting inside the
 * call, so the keypad is the program's until it returns.
 */
static int native_key(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;

	qdos_key_event event;
	if (!qdos_natives_key(sh->hal, &event)) {
		if (qdos_natives_broken()) {
			qd_set_error_msg(ctx, "BREAK");
			return 1;
		}
		qd_push_i(ctx, 0);
		qd_push_i(ctx, 0);
		return qd_push_i(ctx, 0);
	}

	qd_push_i(ctx, (int64_t)event.key);
	qd_push_i(ctx, (int64_t)event.ch);
	return qd_push_i(ctx, 1);
}

/** `qdos::running` - ( -- r:i64) false once the machine is stopping */
static int native_running(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	return qd_push_i(ctx, sh->hal->running(sh->hal) ? 1 : 0);
}

/** `qdos::ticks` - ( -- ms:i64) */
static int native_ticks(qd_context* ctx, void* userdata) {
	qdos_shell* sh = userdata;
	return qd_push_i(ctx, (int64_t)sh->hal->ticks_ms(sh->hal));
}

/** `cls` - ( -- ) clear the message line */
static int native_cls(qd_context* ctx, void* userdata) {
	(void)ctx;
	qdos_shell* sh = userdata;
	sh->message[0] = '\0';
	sh->message_is_error = false;
	return 0;
}

static void register_natives(qdos_shell* sh, qd_interp* interp) {
	qd_interp_register(interp, "sto", "(value:i64 slot:i64 -- )", native_sto, sh);
	qd_interp_register(interp, "rcl", "(slot:i64 -- value:i64)", native_rcl, sh);
	qd_interp_register(interp, "clr", "(slot:i64 -- )", native_clr, sh);
	qd_interp_register(interp, "forget", "(name:str -- )", native_forget, sh);
	qd_interp_register(interp, "edit", "(name:str -- )", native_edit, sh);
	qd_interp_register(interp, "graph", "(name:str -- )", native_graph, sh);
	qd_interp_register(interp, "graph3", "(name:str -- )", native_graph3, sh);
	qd_interp_register(interp, "cls", "( -- )", native_cls, sh);

	// What CALC finds on a graph, for any word taking x and leaving y
	qd_interp_register(interp, "root", "(f:str a:f64 b:f64 -- x:f64)", native_root, sh);
	qd_interp_register(interp, "fmin", "(f:str a:f64 b:f64 -- x:f64)", native_fmin, sh);
	qd_interp_register(interp, "fmax", "(f:str a:f64 b:f64 -- x:f64)", native_fmax, sh);
	qd_interp_register(interp, "nderiv", "(f:str x:f64 -- d:f64)", native_nderiv, sh);
	qd_interp_register(interp, "fnint", "(f:str a:f64 b:f64 -- area:f64)", native_fnint, sh);
	qd_interp_register(interp, "intersect", "(f:str g:str a:f64 b:f64 -- x:f64)", native_intersect, sh);

	for (int i = 0; i < STAT_LISTS; i++) {
		char name[4];
		snprintf(name, sizeof(name), "L%d", i + 1);
		sh->list_word[i].sh = sh;
		sh->list_word[i].index = i;
		qd_interp_register(interp, name, "( -- xs:[]f64)", native_list, &sh->list_word[i]);
	}
	qd_interp_register(interp, "lsto", "(xs:[]f64 n:i64 -- )", native_lsto, sh);
	qdos_register_stats(interp);

	// The machine itself, for a program that has taken the screen
	qd_interp_register(interp, "qdos::key", "( -- key:i64 ch:i64 got:i64)", native_key, sh);
	qd_interp_register(interp, "qdos::running", "( -- r:i64)", native_running, sh);
	qd_interp_register(interp, "qdos::ticks", "( -- ms:i64)", native_ticks, sh);
	qdos_register_math(interp);
}

/** @brief A module is C and can fault; the shell is respawned, not resumed */
typedef struct {
	const qdos_shell* sh;
	char* out;
	size_t cap;
	bool found;
} module_walk;

static bool spot_new_module(const char* entry, void* user) {
	module_walk* walk = (module_walk*)user;

	// An app that has just arrived brings its own modules with it
	char folder[QDOS_PROGRAM_NAME_MAX];
	if (qdos_app_name(entry, folder, sizeof(folder))) {
		walk->sh->hal->store_list(walk->sh->hal, QDOS_SCOPE_INBOX, folder, spot_new_module, walk);
		return !walk->found;
	}

	char name[QDOS_PROGRAM_NAME_MAX];
	if (!qdos_module_name(entry, name, sizeof(name))) {
		return true;
	}
	if (qdos_natives_find(&walk->sh->natives, name) != NULL) {
		return true;
	}

	snprintf(walk->out, walk->cap, "%s", name);
	walk->found = true;
	return false;
}

static bool card_has_new_module(const qdos_shell* sh, char* out, size_t cap) {
	if (sh->hal->store_list == NULL) {
		return false;
	}

	module_walk walk = {.sh = sh, .out = out, .cap = cap, .found = false};
	sh->hal->store_list(sh->hal, QDOS_SCOPE_INBOX, NULL, spot_new_module, &walk);
	return walk.found;
}

/**
 * @brief Read the card again, something having landed on it
 *
 * A program can be declared over the top of itself. A module cannot: the
 * interpreter holds registrations inside one already open, so it is named and
 * left until a restart.
 */
static void reload_card(qdos_shell* sh) {
	const int found = qdos_programs_restore(sh->hal, QDOS_SCOPE_INBOX, sh->interp);

	// An app that has just arrived is a word the interpreter has not got yet
	register_apps(sh, sh->interp);

	// A list on screen is a snapshot, so it has to be taken again
	if (sh->mode == QDOS_MODE_LIST) {
		const size_t was = sh->list_sel;
		list_load(sh);

		const size_t count = list_count(sh);
		sh->list_sel = (was < count) ? was : (count > 0 ? count - 1 : 0);
		list_scroll_into_view(sh);
	}

	char waiting[QDOS_PROGRAM_NAME_MAX];
	char message[QDOS_COLS + 1];

	if (card_has_new_module(sh, waiting, sizeof(waiting))) {
		snprintf(message, sizeof(message), "RESTART FOR '%.10s'", waiting);
	} else {
		snprintf(message, sizeof(message), "%d FROM THE CARD", found > 0 ? found : 0);
	}

	set_message(sh, message, false);
}

static void save_session_now(void* user) {
	qdos_shell* sh = (qdos_shell*)user;
	qdos_storage_save_session(sh->hal, sh->interp);
}

qdos_shell* qdos_shell_create(qdos_hal* hal) {
	if (!hal) {
		return NULL;
	}

	qdos_shell* sh = calloc(1, sizeof(*sh));
	if (!sh) {
		return NULL;
	}

	sh->interp = qd_interp_create(STACK_SIZE);
	if (!sh->interp) {
		free(sh);
		return NULL;
	}

	sh->hal = hal;
	sh->decimals = DECIMALS_AUTO; // calloc would otherwise mean nought decimals
	sh->cursor_on = true;		  // nor a cursor that starts out invisible
	sh->auto_off = AUTO_OFF_DEFAULT;
	restore_settings(sh); // over the defaults just set, where anything was saved
	register_natives(sh, sh->interp);
	qdos_console_init(&sh->con);

	// Every scope is opened before any is registered: a module replaced by a
	// nearer one is closed, and its words would dangle
	qdos_natives_bind(hal, sh->con.fb);
	const bool faulted = qdos_natives_recover(&sh->natives, hal);
	for (int scope = 0; scope < QDOS_SCOPE__COUNT; scope++) {
		qdos_natives_load(&sh->natives, hal, (qdos_store_scope)scope);
	}
	qdos_natives_register(&sh->natives, sh->interp);
	qdos_natives_on_call(save_session_now, sh);

	// In scope order, so each one shadows the one before it
	qdos_programs_restore(hal, QDOS_SCOPE_SYSTEM, sh->interp);
	qdos_programs_restore(hal, QDOS_SCOPE_INBOX, sh->interp);
	qdos_programs_restore(hal, QDOS_SCOPE_USER, sh->interp);
	register_apps(sh, sh->interp);
	slots_restore(sh);
	window_restore(sh);
	lists_restore(sh);
	qdos_rand_seed(((uint64_t)hal->ticks_ms(hal) << 32) ^ (uint64_t)(uintptr_t)sh);

	// A calculator that is ready says so by being on screen, so a clean boot
	// leaves the message line empty. Only a restored stack is worth a word,
	// because the numbers above the prompt would otherwise be unexplained.
	const qdos_store_result restored = qdos_storage_restore_session(hal, sh->interp);
	const size_t lost = qdos_storage_session_lost(hal);
	if (lost > 0) {
		// Saying how many is the whole of it: the values are gone either way,
		// and a stack shorter than it was left is otherwise unexplained.
		char message[QDOS_COLS + 1];
		snprintf(message, sizeof(message), "%u LOST FROM STACK", (unsigned)((lost > 99) ? 99 : lost));
		set_message(sh, message, true);
	} else if (restored == QDOS_STORE_OK && qd_interp_depth(sh->interp) > 0) {
		set_message(sh, "SESSION RESTORED", false);
	}

	if (faulted) {
		char message[MESSAGE_COLS + 1];
		snprintf(message, sizeof(message), "'%.40s' FAULTED", sh->natives.faulted);
		set_message(sh, message, true);
	}

	// Only once the session is back, or the first save would be of nothing
	hal->save = save_session_now;
	hal->save_user = sh;

	return sh;
}

void qdos_shell_destroy(qdos_shell* sh) {
	if (!sh) {
		return;
	}

	sh->hal->save = NULL;

	// Interpreter first: its registrations point into the modules
	qd_interp_destroy(sh->interp);
	qdos_natives_unload(&sh->natives);
	free(sh);
}

void qdos_shell_run(qdos_shell* sh) {
	if (!sh) {
		return;
	}

	render(sh);

	uint32_t last_key = sh->hal->ticks_ms(sh->hal);
	uint32_t last_blink = last_key;

	while (sh->hal->running(sh->hal)) {
		qdos_key_event ev;
		bool dirty = false;

		while (sh->hal->poll_key(sh->hal, &ev)) {
			handle_key(sh, &ev);
			dirty = true;

			// Checked after handling rather than on the key, so however the
			// press arrives it is the handler that decides this is an off
			if (sh->powering_off) {
				qdos_storage_save_session(sh->hal, sh->interp);
				return;
			}
		}

		// Never while a host has the card: those blocks are not ours to read
		if (!sh->usb_exported && sh->hal->store_changed != NULL && sh->hal->store_changed(sh->hal)) {
			reload_card(sh);
			dirty = true;
		}

		const uint32_t now = sh->hal->ticks_ms(sh->hal);

		if (dirty) {
			// Typing is never the moment to be showing a dark cursor, so a key
			// puts it back on and starts the period again.
			last_key = now;
			last_blink = now;
			sh->cursor_on = true;
			sh->off_warned = false;
			render(sh);
		}

		const uint32_t idle_ms = now - last_key;

		// Nobody has touched it in a while: settle to a steady cursor, which
		// still says where you are but needs no further repaints.
		const bool settled = idle_ms >= CURSOR_SETTLE_MS;

		if (settled && !sh->cursor_on) {
			sh->cursor_on = true;
			render(sh);
		} else if (status_stale(sh)) {
			render(sh);
		} else if (!settled && has_cursor(sh) && (now - last_blink) >= CURSOR_BLINK_MS) {
			last_blink = now;
			sh->cursor_on = !sh->cursor_on;
			render(sh);
		}

		// Put down rather than paused: say so, then turn the machine off. The
		// session is saved on the way out, so it comes back as it was left.
		const uint32_t off_after = auto_off_ms(sh);
		if (off_after > 0) {
			if (idle_ms >= off_after) {
				qdos_storage_save_session(sh->hal, sh->interp);
				return;
			}

			if (!sh->off_warned && idle_ms >= off_after - AUTO_OFF_WARN_MS) {
				sh->off_warned = true;
				set_message(sh, "TURNING OFF", false);
				render(sh);
			}
		}

		// Draining the keys is how a backend learns it is being shut down, and
		// the wait below has no timer to come back on. Check before sleeping on
		// a machine that has already stopped.
		if (!sh->hal->running(sh->hal)) {
			break;
		}

		// How long until something is due: the next blink, the warning, or the
		// power-off. Nothing due means waiting on the keypad and nothing else,
		// which is what a calculator sitting on a desk should be doing.
		int timeout = -1;
		if (!settled && has_cursor(sh)) {
			const uint32_t since = now - last_blink;
			timeout = (since >= CURSOR_BLINK_MS) ? 0 : (int)(CURSOR_BLINK_MS - since);
		}
		// The one wakeup a calculator left alone does take: once a minute, and
		// only while there is a clock or a battery to show
		int seconds;
		if (sh->hal->time_of_day != NULL && sh->hal->time_of_day(sh->hal, &seconds)) {
			const int until = (60 - seconds % 60) * 1000;
			timeout = (timeout < 0 || until < timeout) ? until : timeout;
		} else if (sh->hal->battery != NULL && sh->hal->battery(sh->hal) >= 0) {
			timeout = (timeout < 0 || STATUS_POLL_MS < timeout) ? STATUS_POLL_MS : timeout;
		}
		if (off_after > 0) {
			const uint32_t due = sh->off_warned ? off_after : off_after - AUTO_OFF_WARN_MS;
			const int until = (idle_ms >= due) ? 0 : (int)(due - idle_ms);
			if (timeout < 0 || until < timeout) {
				timeout = until;
			}
		}
		sh->hal->wait(sh->hal, timeout);
	}

	qdos_storage_save_session(sh->hal, sh->interp);
}
