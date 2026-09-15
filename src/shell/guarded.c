/**
 * @file guarded.c
 * @brief Evaluation that survives a fatal runtime error
 */

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

/* stdout on a pipe for the duration of one evaluation, then read back */
static int g_pipe[2] = {-1, -1};
static int g_stdout = -1;

/*
 * Evaluation nests: `forget` restores a shipped program by evaluating it from
 * inside the native word, which is already inside an evaluation. The outer
 * frame owns the recovery buffer and the capture both -- see qdos_guarded_eval.
 */
static bool g_evaluating;

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
	if (g_pipe[0] < 0)
		return;

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

bool qdos_guarded_eval(qd_interp* interp, const char* source) {
	qd_context* ctx = qd_interp_context(interp);

	// Already inside an evaluation: arming again would overwrite the jump
	// buffer the outer setjmp is waiting on, and capturing again would hand
	// the outer's pipe back as the real stdout. Let the outer frame keep both.
	if (g_evaluating)
		return qd_interp_eval(interp, source);

	if (setjmp(*qd_recovery_buf(ctx)) != 0) {
		qd_recovery_disarm(ctx);
		// Unwinding jumped straight over the way out, so finish up here
		g_evaluating = false;
		capture_end();
		g_recovered = true;
		return false;
	}

	qd_recovery_arm(ctx);
	g_evaluating = true;
	capture_begin();

	const bool ok = qd_interp_eval(interp, source);

	capture_end();
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
