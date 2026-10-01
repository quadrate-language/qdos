/**
 * @file guarded.c
 * @brief Evaluation that survives a fatal runtime error
 */

// fopencookie, where stdout is captured that way
#ifdef QDOS_CAPTURE_STDIO
#define _GNU_SOURCE
#endif

#include "guarded.h"

#include <quadrate/rt/runtime.h>

#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define OUTPUT_MAX 512

static char g_output[OUTPUT_MAX];

static bool g_recovered;

/*
 * Evaluation nests: `forget` restores a shipped program by evaluating it from
 * inside the native word, which is already inside an evaluation. The outer
 * frame owns the recovery buffer and the capture both -- see qdos_guarded_eval.
 */
static bool g_evaluating;

#ifdef QDOS_CAPTURE_STDIO
/*
 * No pipe and no dup2, as on ESP-IDF. newlib keeps a stdout per task, so this
 * one task's stdout is pointed at a buffer instead, which behaves as the pipe
 * does: it holds so much, the rest is dropped, and reading takes what it reads.
 */
#define CAPTURE_MAX 4096

static char g_captured[CAPTURE_MAX];
static size_t g_captured_len;
static FILE* g_capture;
static FILE* g_stdout;

static ssize_t capture_write(void* cookie, const char* buf, size_t len) {
	(void)cookie;
	const size_t room = CAPTURE_MAX - g_captured_len;
	const size_t kept = len < room ? len : room;
	memcpy(g_captured + g_captured_len, buf, kept);
	g_captured_len += kept;
	return (ssize_t)len;
}

/* Up to @p cap - 1 bytes of what is held, taken out of it */
static size_t capture_read(char* out, size_t cap) {
	const size_t got = g_captured_len < cap - 1 ? g_captured_len : cap - 1;
	memcpy(out, g_captured, got);
	out[got] = '\0';
	memmove(g_captured, g_captured + got, g_captured_len - got);
	g_captured_len -= got;
	return got;
}

static void capture_begin(void) {
	g_output[0] = '\0';
	g_captured_len = 0;
	fflush(stdout);

	const cookie_io_functions_t io = {.write = capture_write};
	g_capture = fopencookie(NULL, "w", io);
	if (g_capture == NULL) {
		return;
	}
	setvbuf(g_capture, NULL, _IONBF, 0);
	g_stdout = stdout;
	stdout = g_capture;
}

static void capture_end(void) {
	if (g_capture == NULL) {
		return;
	}

	stdout = g_stdout;
	fclose(g_capture);
	g_capture = NULL;
	capture_read(g_output, sizeof(g_output));
}

const char* qdos_guarded_output(void) {
	return g_output;
}

size_t qdos_guarded_take(char* out, size_t cap) {
	if (cap == 0) {
		return 0;
	}
	out[0] = '\0';
	if (g_capture == NULL) {
		return 0;
	}
	return capture_read(out, cap);
}
#else
/* stdout on a pipe for the duration of one evaluation, then read back */
static int g_pipe[2] = {-1, -1};
static int g_stdout = -1;

static void capture_begin(void) {
	g_output[0] = '\0';
	if (pipe(g_pipe) != 0) {
		g_pipe[0] = g_pipe[1] = -1;
		return;
	}

	// Non-blocking, so a program that prints more than the pipe holds loses
	// the overflow rather than hanging the calculator
	fcntl(g_pipe[1], F_SETFL, O_NONBLOCK);
	fflush(stdout);
	g_stdout = dup(STDOUT_FILENO);
	dup2(g_pipe[1], STDOUT_FILENO);
}

static void capture_end(void) {
	if (g_pipe[0] < 0) {
		return;
	}

	fflush(stdout);
	if (g_stdout >= 0) {
		dup2(g_stdout, STDOUT_FILENO);
		close(g_stdout);
		g_stdout = -1;
	}
	close(g_pipe[1]);

	fcntl(g_pipe[0], F_SETFL, O_NONBLOCK);
	const ssize_t got = read(g_pipe[0], g_output, sizeof(g_output) - 1);
	g_output[(got > 0) ? (size_t)got : 0] = '\0';
	close(g_pipe[0]);
	g_pipe[0] = g_pipe[1] = -1;
}

const char* qdos_guarded_output(void) {
	return g_output;
}

size_t qdos_guarded_take(char* out, size_t cap) {
	if (cap == 0) {
		return 0;
	}
	out[0] = '\0';
	if (g_pipe[0] < 0) {
		return 0;
	}

	fflush(stdout);
	fcntl(g_pipe[0], F_SETFL, fcntl(g_pipe[0], F_GETFL) | O_NONBLOCK);
	const ssize_t got = read(g_pipe[0], out, cap - 1);
	out[(got > 0) ? (size_t)got : 0] = '\0';
	return (got > 0) ? (size_t)got : 0;
}

#endif

bool qdos_guarded_eval(qd_interp* interp, const char* source) {
	qd_context* ctx = qd_interp_context(interp);

	// Already inside an evaluation: arming again would overwrite the jump
	// buffer the outer setjmp is waiting on, and capturing again would hand
	// the outer's pipe back as the real stdout. Let the outer frame keep both.
	if (g_evaluating) {
		return qd_interp_eval(interp, source);
	}

	// A program that watches the keypad earns more steps as it goes; that lasts one evaluation
	const uint64_t limit = qd_interp_step_limit(interp);

	if (setjmp(*qd_recovery_buf(ctx)) != 0) {
		qd_recovery_disarm(ctx);
		// Unwinding jumped straight over the way out, so finish up here
		g_evaluating = false;
		capture_end();
		qd_interp_set_step_limit(interp, limit);
		g_recovered = true;
		return false;
	}

	qd_recovery_arm(ctx);
	g_evaluating = true;
	capture_begin();

	const bool ok = qd_interp_eval(interp, source);

	capture_end();
	qd_interp_set_step_limit(interp, limit);
	g_evaluating = false;
	qd_recovery_disarm(ctx);
	return ok;
}

const char* qdos_guarded_error(qd_interp* interp) {
	if (g_recovered) {
		g_recovered = false;
		return "WRONG TYPE FOR THAT WORD";
	}
	return qd_interp_error(interp);
}
