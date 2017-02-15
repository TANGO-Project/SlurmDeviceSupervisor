/*****************************************************************************\
 * src/common/srun_globals.h - Global variables for srun
 *****************************************************************************
 *  Copyright (C) 2016 Atos Inc.
 *  Written by Martin Perry <martin.perry@atos.net>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifndef _SRUN_GLOBALS_H
#define _SRUN_GLOBALS_H

#if HAVE_CONFIG_H
#  include "config.h"
#  if HAVE_INTTYPES_H
#    include <inttypes.h>
#  else
#    if HAVE_STDINT_H
#      include <stdint.h>
#    endif
#  endif			/* HAVE_INTTYPES_H */
#else				/* !HAVE_CONFIG_H */
#  include <inttypes.h>
#endif				/*  HAVE_CONFIG_H */
#include <stdbool.h>

/* pipes for inter-srun communication */
extern int *vector_pipe_out;
extern int *vector_pipe_in;
extern int *stepindex_pipe_in;
extern int *nnodes_pipe;
extern int *pmi2port_pipe;
extern int *pmi1port_pipe;

/* srun globals */
extern int srun_num_steps;	/* number of steps in this srun command */
extern int srun_step_idx;	/* step index of this step */
extern uint32_t packjobid;	/* jobid of srun first step */
extern uint32_t packstepid;	/* stepid of srun first step */
extern bool srun_mpi_combine;	/* --mpi-combine option */

#endif /*__SRUN_GLOBALS_H__*/
