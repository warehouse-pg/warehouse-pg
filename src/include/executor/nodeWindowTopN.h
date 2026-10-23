/*-------------------------------------------------------------------------
 *
 * nodeWindowTopN.h
 *	  prototypes for nodeWindowTopN.c
 *
 * src/include/executor/nodeWindowTopN.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEWINDOWTOPN_H
#define NODEWINDOWTOPN_H

#include "nodes/execnodes.h"

extern WindowTopNState *ExecInitWindowTopN(WindowTopN *node, EState *estate, int eflags);
extern void ExecEndWindowTopN(WindowTopNState *node);
extern void ExecReScanWindowTopN(WindowTopNState *node);
extern void ExecSquelchWindowTopN(WindowTopNState *node);

#endif							/* NODEWINDOWTOPN_H */
