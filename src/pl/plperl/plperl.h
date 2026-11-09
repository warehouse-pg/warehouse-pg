/*-------------------------------------------------------------------------
 *
 * plperl.h
 *	  Common include file for PL/Perl files
 *
 * This should be included _AFTER_ postgres.h and system include files
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/pl/plperl/plperl.h
 */

#ifndef PL_PERL_H
#define PL_PERL_H

/* stop perl headers from hijacking stdio and other stuff on Windows */
#ifdef WIN32
#define WIN32IO_IS_STDIO
/*
 * isnan is defined in both the perl and mingw headers. We don't use it,
 * so this just clears up the compile warning.
 */
#ifdef isnan
#undef isnan
#endif
#endif							/* WIN32 */

/*
 * Supply a value of PERL_UNUSED_DECL that will satisfy gcc - the one
 * perl itself supplies doesn't seem to.
 */
#if defined(__GNUC__)
#define PERL_UNUSED_DECL __attribute__ ((unused))
#endif

/*
 * Sometimes perl carefully scribbles on our *printf macros.
 * So we undefine them here and redefine them after it's done its dirty deed.
 */

#ifdef USE_REPL_SNPRINTF
#undef snprintf
#undef vsnprintf
#endif

/*
 * ActivePerl 5.18 and later are MinGW-built, and their headers use GCC's
 * __inline__.  Translate to something MSVC recognizes.
 */
#ifdef _MSC_VER
#define __inline__ inline
#endif


/*
 * Get the basic Perl API.  We use PERL_NO_GET_CONTEXT mode so that our code
 * can compile against MULTIPLICITY Perl builds without including XSUB.h.
 */
#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"

/*
 * We want to include XSUB.h only within .xs files, because on some platforms
 * it undesirably redefines a lot of libc functions.  But it must appear
 * before ppport.h, so use a #define flag to control inclusion here.
 */
#ifdef PG_NEED_PERL_XSUB_H
#include "XSUB.h"
#endif

/* put back our snprintf and vsnprintf */
#ifdef USE_REPL_SNPRINTF
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef __GNUC__
#define vsnprintf(...)	pg_vsnprintf(__VA_ARGS__)
#define snprintf(...)	pg_snprintf(__VA_ARGS__)
#else
#define vsnprintf		pg_vsnprintf
#define snprintf		pg_snprintf
#endif   /* __GNUC__ */
#endif   /* USE_REPL_SNPRINTF */

/* perl version and platform portability */
#define NEED_eval_pv
#define NEED_newRV_noinc
#define NEED_sv_2pv_flags
#include "ppport.h"

/* perl may have a different width of "bool", don't buy it */
#ifdef bool
#undef bool
#endif

/* supply HeUTF8 if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef HeUTF8
#define HeUTF8(he)			   ((HeKLEN(he) == HEf_SVKEY) ?			   \
								SvUTF8(HeKEY_sv(he)) :				   \
								(U32)HeKUTF8(he))
#endif

/* supply GvCV_set if it's missing - ppport.h doesn't supply it, unfortunately */
#ifndef GvCV_set
#define GvCV_set(gv, cv)		(GvCV(gv) = cv)
#endif

/* declare routines from plperl.c for access by .xs files */
HV		   *plperl_spi_exec(char *, int);
void		plperl_return_next(SV *);
SV		   *plperl_spi_query(char *);
SV		   *plperl_spi_fetchrow(char *);
SV		   *plperl_spi_prepare(char *, int, SV **);
HV		   *plperl_spi_exec_prepared(char *, HV *, int, SV **);
SV		   *plperl_spi_query_prepared(char *, int, SV **);
void		plperl_spi_freeplan(char *);
void		plperl_spi_cursor_close(char *);
char	   *plperl_sv_to_literal(SV *, char *);
void		plperl_util_elog(int level, SV *msg);


/* helper functions */

/*
 * If the SV has get magic, run FETCH to convert it to the intended value.
 *
 * While Perl functions such as SvPV() will handle get magic automatically,
 * we must run this before primitive checks such as SvOK() or SvROK().
 * This must be invoked within the scope of a dTHX declaration.
 */
#define plperl_materialize_sv(sv) \
	do { if (sv) SvGETMAGIC(sv); } while(0)

/*
 * Test whether ERRSV indicates that Perl threw an error.
 *
 * The traditional way to write this is SvTRUE(ERRSV), but that is unsafe.
 * If the error is an object whose class overloads "bool", SvTRUE invokes
 * that user-defined Perl code, which is free to throw an error of its own.
 * Since we typically ask this question while unwinding from an error, such
 * a nested throw tends to land right back where we started, giving an
 * infinite loop.  Avoid the problem by not asking Perl at all in the case
 * that matters: any reference is true, whatever its class may claim.  Only
 * non-references need the full test, and overloading cannot apply to those.
 */
static inline bool
errsv_is_true(SV *errsv)
{
	dTHX;

	if (!errsv)
		return false;
	if (SvROK(errsv))
		return true;
	return SvTRUE(errsv);
}

/*
 * These macros provide a version of the PG_TRY facility that can catch
 * Perl's longjmps as well as our own.  Use these, not plain PG_TRY, in any
 * code that can invoke Perl functions, to ensure that the two jmp_buf stacks
 * remain consistent.  Without this, any Perl function that is called directly
 * (rather than via a runops loop such as call_sv's) will wreak havoc if it
 * throws an error.
 *
 * These must be invoked within the scope of a dTHX declaration.  That
 * declaration controls which interpreter we manipulate the jmp_buf stack of.
 *
 * Like PG_TRY, we accept a __VA_ARGS__ string to append to local variable
 * names, but that's of limited use since we can't modify the "cur_env" name
 * declared by dJMPENV; so you can't nest these without drawing compiler
 * warnings about shadowed variables.  However, it is helpful in some places
 * to nest PG_PERL_TRY within PG_TRY, so we make sure not to conflict with
 * the names that PG_TRY declares.  (Beware of nesting them too closely,
 * for fear of breaking clang.  It does seem to work so long as there's a
 * nontrivial amount of code in between.)
 *
 * Since nothing in life is easy, we need two implementations of this idea.
 * They are equivalent from the outside.
 */

#if !(defined(WIN32) && defined(__MINGW64__))

/*
 * This is our preferred implementation, which we use everywhere but MinGW-64.
 * It depends on the assumption that Perl is built with the same sigsetjmp()
 * infrastructure that we are.
 *
 * A key design choice here is to use just one sigsetjmp() call and link
 * that jmp_buf structure into both jmp_buf stacks.  That requires being more
 * intimate with both error mechanisms than one could wish, but using two
 * sigsetjmp() calls is expensive, and more importantly it completely breaks
 * clang's optimizer, so much so that clang generates buggy code even at -O0.
 * To provide at least a fig-leaf of separation from Perl innards, we use
 * JMPENV_PUSH/JMPENV_POP to manipulate their jmp_buf stack and modify our
 * own stack by hand.  (Thus, these macros look a good deal like elog.h's.)
 * The main trick to using a single jmp_buf is how to tell whether a caught
 * longjmp was raised by our code or Perl's.  We handle that by detecting
 * whether elog.c has a complete error report that's not been popped.
 * If so, we assume the already-stacked Postgres error is what to report.
 * If not, the error is Perl's, so we convert their error to a Postgres one.
 */
#define PG_PERL_TRY(...)  \
	do { \
		dJMPENV; \
		OP	   *_pp_save_op = PL_op; \
		volatile bool _pp_leavetry_needed = true; \
		volatile bool _pp_reporting = false; \
		int		_pp_ret_; \
		sigjmp_buf *_save_exception_stack##__VA_ARGS__ = PG_exception_stack; \
		ErrorContextCallback *_save_context_stack##__VA_ARGS__ = error_context_stack; \
		bool	_do_rethrow##__VA_ARGS__ = false; \
		bool	_cleanup_needed##__VA_ARGS__ = true; \
		JMPENV_PUSH(_pp_ret_); \
		if (_pp_ret_ == 0) \
		{ \
			PG_exception_stack = &cur_env.je_buf; \
			plperl_do_entertry(aTHX)

#define PG_PERL_CATCH(...)	\
		} \
		else \
		{ \
			/* Is the thrown error ours, or Perl's? */ \
			if (!HaveCompleteError()) \
			{ \
				/* Perl's, so assume we don't need LEAVETRY any more */ \
				_pp_leavetry_needed = false; \
				/* Reset our error stacks to ensure they're not dangling */ \
				PG_exception_stack = &cur_env.je_buf; \
				error_context_stack = _save_context_stack##__VA_ARGS__; \
				/* This'll longjmp back here, but now with an error stacked */ \
				PG_PERL_REPORT_LONGJMP(_pp_ret_); \
			} \
			PG_PERL_POP_STACKS(__VA_ARGS__)

#define PG_PERL_FINALLY(...) \
		} \
		else \
		{ \
			/* Is the thrown error ours, or Perl's? */ \
			if (!HaveCompleteError()) \
			{ \
				/* Perl's, so assume we don't need LEAVETRY any more */ \
				_pp_leavetry_needed = false; \
				/* Reset our error stacks to ensure they're not dangling */ \
				PG_exception_stack = &cur_env.je_buf; \
				error_context_stack = _save_context_stack##__VA_ARGS__; \
				/* This'll longjmp back here, but now with an error stacked */ \
				PG_PERL_REPORT_LONGJMP(_pp_ret_); \
			} \
			_do_rethrow##__VA_ARGS__ = true; \
		} \
		{ \
			PG_PERL_POP_STACKS(__VA_ARGS__)

#define PG_PERL_END_TRY(...)  \
		} \
		if (_do_rethrow##__VA_ARGS__) \
				PG_RE_THROW(); \
		if (_cleanup_needed##__VA_ARGS__) \
		{ \
			PG_PERL_POP_STACKS(__VA_ARGS__); \
		} \
	} while (0)

/*
 * This helper macro is split out just to reduce code duplication.
 * It pops both jmp_buf stacks and does some other end-of-block cleanup.
 *
 * Unlike the case in PG_TRY and friends, we must do this only once,
 * so we need an explicit _cleanup_needed flag to avoid doing it twice
 * during successful exit from a FINALLY block.
 */
#define PG_PERL_POP_STACKS(...) \
		_cleanup_needed##__VA_ARGS__ = false; \
		JMPENV_POP; \
		PG_exception_stack = _save_exception_stack##__VA_ARGS__; \
		error_context_stack = _save_context_stack##__VA_ARGS__; \
		if (_pp_leavetry_needed) \
			plperl_do_leavetry(aTHX); \
		PL_op = _pp_save_op

#else							/* defined(WIN32) && defined(__MINGW64__) */

/*
 * We use this implementation on MinGW-64, where we use __builtin_setjmp()
 * (see c.h), but Perl is content with MinGW's setjmp().  Of necessity, we
 * maintain the two jmp_buf stacks separately and pay the price of two setjmp
 * calls per level.
 *
 * Here, the forced updates of PL_top_env are needed to prevent an assertion
 * failure in JMPENV_POP when what we are catching is a Postgres longjmp that
 * has exited past the stack level of some Perl JMPENV_PUSH.
 */
#define PG_PERL_TRY(...)  \
	do { \
		dJMPENV; \
		OP	   *_pp_save_op = PL_op; \
		volatile bool _pp_leavetry_needed = true; \
		volatile bool _pp_reporting = false; \
		PG_TRY(__VA_ARGS__); \
		{ \
			int		_pp_ret_; \
			JMPENV_PUSH(_pp_ret_); \
			if (_pp_ret_ == 0) \
			{ \
				plperl_do_entertry(aTHX);

#define PG_PERL_CATCH(...)	\
			} \
			else \
			{ \
				_pp_leavetry_needed = false; \
				PG_PERL_REPORT_LONGJMP(_pp_ret_); \
			} \
			PL_top_env = &cur_env; \
			JMPENV_POP; \
			if (_pp_leavetry_needed) \
				plperl_do_leavetry(aTHX); \
			PL_op = _pp_save_op; \
		} \
		PG_CATCH(__VA_ARGS__); \
		{ \
			PL_top_env = &cur_env; \
			JMPENV_POP; \
			if (_pp_leavetry_needed) \
				plperl_do_leavetry(aTHX); \
			PL_op = _pp_save_op; \
			{

#define PG_PERL_FINALLY(...) \
			} \
			else \
			{ \
				_pp_leavetry_needed = false; \
				PG_PERL_REPORT_LONGJMP(_pp_ret_); \
			} \
		} \
		PG_FINALLY(__VA_ARGS__); \
		{ \
			PL_top_env = &cur_env; \
			JMPENV_POP; \
			if (_pp_leavetry_needed) \
				plperl_do_leavetry(aTHX); \
			PL_op = _pp_save_op; \
			{

#define PG_PERL_END_TRY(...)  \
			} \
		} \
		PG_END_TRY(__VA_ARGS__); \
	} while (0)

#endif							/* !(defined(WIN32) && defined(__MINGW64__)) */

/*
 * This helper macro is split out just to reduce code duplication.
 * It converts an error thrown by Perl into a Postgres ereport.
 */
#define PG_PERL_REPORT_LONGJMP(_pp_ret_)	\
	do { \
		SV		   *errsv; \
		const char *errmessage; \
		/* \
		 * Since we're still within the scope of PG_PERL_TRY's JMPENV_PUSH, \
		 * any error thrown by the Perl functions used herein will jump right \
		 * back here, risking an infinite loop.  While we try to prevent \
		 * that, provide a way to break out of such a loop, just in case. \
		 */ \
		CHECK_FOR_INTERRUPTS(); \
		/* \
		 * If we already failed while converting Perl's error to a string, \
		 * don't try again.  The conversion runs user-supplied Perl code \
		 * if the error is an object with an overloaded "" operator, and if \
		 * that throws we'll come right back here. \
		 */ \
		if (_pp_reporting) \
			ereport(ERROR, \
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), \
					 errmsg("Perl error could not be converted to a string"))); \
		_pp_reporting = true; \
		/* \
		 * Convert the error to a string before starting the ereport, not \
		 * within it.  A throw from an overloaded "" operator would otherwise \
		 * leave an incomplete error stacked, leaking an errordata slot. \
		 */ \
		errsv = ERRSV; \
		if (!errsv_is_true(errsv)) \
			elog(ERROR, "unexpected Perl longjmp, code %d", _pp_ret_); \
		errmessage = strip_trailing_ws(sv2cstr(errsv)); \
		_pp_reporting = false; \
		/* This'll longjmp back here, but now with a Postgres error ready */ \
		ereport(ERROR, \
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION), \
				 errmsg_internal("%s", errmessage))); \
	} while (0)

/*
 * Helper function: invoke OP_ENTERTRY to push a CXt_EVAL context
 *
 * This hackery with ENTERTRY/LEAVETRY ensures that Perl will longjmp rather
 * than exiting, even if we're not presently inside an eval{}.  We explicitly
 * save/restore PL_op rather than trying to get ENTERTRY/LEAVETRY to do that.
 *
 * A side-effect of OP_ENTERTRY that we do *not* want is CLEAR_ERRSV.
 * Hack around that by hiding the entry-time ERRSV in a local.
 */
static inline void
plperl_do_entertry(pTHX)
{
	SV		   *old_errsv = GvSV(PL_errgv);
	LOGOP		et_op;
	OP			child_op;

	/* Replace ERRSV with a dummy SV, which ENTERTRY will clear */
	GvSV(PL_errgv) = newSV(0);

	/* We had better have disabled CATCH_GET, else this does the wrong thing */
	Assert(!CATCH_GET);
	memset(&et_op, 0, sizeof(et_op));
	et_op.op_flags = OPf_WANT_VOID;
	/* We need a child OP, just to avoid a null-pointer dereference */
	memset(&child_op, 0, sizeof(child_op));
	et_op.op_other = &child_op;
	PL_op = (OP *) &et_op;
	(void) (PL_ppaddr[OP_ENTERTRY]) (aTHX);

	/* Release the dummy SV's one and only refcnt */
	SvREFCNT_dec(GvSV(PL_errgv));
	/* ... and put back the old ERRSV value */
	GvSV(PL_errgv) = old_errsv;
}

/*
 * Helper function: invoke OP_LEAVETRY to pop that context again
 *
 * Just to make things difficult, we must skip this if Perl threw a
 * longjmp, because die_unwind() would have popped the context.
 *
 * As above, don't let it change ERRSV.
 */
static inline void
plperl_do_leavetry(pTHX)
{
	SV		   *old_errsv = GvSV(PL_errgv);
	OP			lt_op;

	/* Replace ERRSV with a dummy SV, which LEAVETRY will clear */
	GvSV(PL_errgv) = newSV(0);

	memset(&lt_op, 0, sizeof(lt_op));
	PL_op = &lt_op;
	(void) (PL_ppaddr[OP_LEAVETRY]) (aTHX);

	/* Release the dummy SV's one and only refcnt */
	SvREFCNT_dec(GvSV(PL_errgv));
	/* ... and put back the old ERRSV value */
	GvSV(PL_errgv) = old_errsv;
}

#endif   /* PL_PERL_H */
