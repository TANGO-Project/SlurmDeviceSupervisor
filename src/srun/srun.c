/*****************************************************************************\
 *  srun.c - user interface to allocate resources, submit jobs, and execute
 *	parallel jobs.
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Mark Grondona <grondona@llnl.gov>, et. al.
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
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

#include "config.h"

#include <ctype.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "src/common/fd.h"

#include "src/common/hostlist.h"
#include "src/common/log.h"
#include "src/common/net.h"
#include "src/common/plugstack.h"
#include "src/common/read_config.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_jobacct_gather.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurm_rlimits_info.h"
#include "src/common/switch.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xsignal.h"
#include "src/common/xstring.h"
#include "src/common/env.h"

#include "src/bcast/file_bcast.h"

#include "launch.h"
#include "allocate.h"
#include "srun_job.h"
#include "opt.h"
#include "debugger.h"
#include "src/srun/srun_pty.h"
#include "multi_prog.h"
#include "src/api/pmi_server.h"
#include "src/api/step_ctx.h"
#include "src/api/step_launch.h"

// MNP PMI pipe test start
#include "src/common/mpi.h"
// MNP PMI pipe test end

/********************
 * Global Variables *
 ********************/

#if defined (HAVE_DECL_STRSIGNAL) && !HAVE_DECL_STRSIGNAL
#  ifndef strsignal
 extern char *strsignal(int);
#  endif
#endif /* defined HAVE_DECL_STRSIGNAL && !HAVE_DECL_STRSIGNAL */

#ifndef OPEN_MPI_PORT_ERROR
/* This exit code indicates the launched Open MPI tasks could
 *	not open the reserved port. It was already open by some
 *	other process. */
#define OPEN_MPI_PORT_ERROR 108
#endif

#define MAX_RETRIES 20
#define MAX_ENTRIES 50

#define	TYPE_NOT_TEXT	0
#define	TYPE_TEXT	1
#define	TYPE_SCRIPT	2

static struct termios termdefaults;
static uint32_t global_rc = 0;
static srun_job_t *job = NULL;
char *pack_job_id = NULL;
uint32_t pack_desc_count = 0;
bool packjob = false;
bool packleader = false;
uint16_t packl_dependency_position = 0;
pack_group_struct_t *desc = NULL;
pack_job_env_t *pack_job_env = NULL;
uint32_t group_number = -1;
uint32_t group_index = 0;
uint32_t job_index = 0;
static uint32_t total_jobs = 0;
int *group_ids;

bool srun_max_timer = false;
bool srun_shutdown  = false;
int sig_array[] = {
	SIGINT,  SIGQUIT, SIGCONT, SIGTERM, SIGHUP,
	SIGALRM, SIGUSR1, SIGUSR2, SIGPIPE, 0 };

/*
 * forward declaration of static funcs
 */
static int   _file_bcast(void);
static void  _pty_restore(void);
static void  _set_exit_code(void);
static void  _set_node_alias(void);
static void  _setup_env_working_cluster();
static int   _slurm_debug_env_val (void);
static char *_uint16_array_to_str(int count, const uint16_t *array);

static resource_allocation_response_msg_t  *_get_resp(int desc_idx, int job_idx);
static opt_t *_get_opt(int desc_idx, int job_idx);
static srun_job_t *_get_srun_job(int desc_idx, int job_idx);
static env_t *_get_env(int desc_idx, int job_idx);

static int _srun_jobpack(int ac, char **av);
static void _create_srun_steps_jobpack(void);
static void _enhance_env_jobpack(bool got_alloc);
static void _pre_launch_srun_jobpack(void);
static int _launch_srun_steps_jobpack(bool got_alloc);

/*
 * from libvirt-0.6.2 GPL2
 *
 * console.c: A dumb serial console client
 *
 * Copyright (C) 2007, 2008 Red Hat, Inc.
 *
 */
#ifndef HAVE_CFMAKERAW
void cfmakeraw(struct termios *attr)
{
	attr->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP
				| INLCR | IGNCR | ICRNL | IXON);
	attr->c_oflag &= ~OPOST;
	attr->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	attr->c_cflag &= ~(CSIZE | PARENB);
	attr->c_cflag |= CS8;
}
#endif

int _count_jobs(int ac, char **av)
{
	int index, pgj;
	char *tmp = NULL;
	bool pack_group_job = false;

	for (index = 0; index < ac; index++) {
		if ((strcmp(av[index], ":") == 0)) {
			pack_desc_count ++;
			if (index+1 == ac)
			        fatal( "Missing pack job specification "
				       "following pack job delimiter" );
		}
		if ((strncmp(av[index], "--pack", 6) == 0)) {
			pack_group_job = true;
		}
	}
	if(pack_desc_count) pack_desc_count++;
	if ((tmp = getenv ("SLURM_NUMPACK"))) {
		pgj = atoi(tmp);
                if (pgj > 1) {
			pack_group_job = true;
		}
	}
	if ((pack_desc_count == 0) && (pack_group_job == true))
		pack_desc_count ++;
	return pack_desc_count;
}

static void _build_env_structs(int count, pack_job_env_t *pack_job_env)
{
	int i;

	for (i = 0; i < count; i++) {
		pack_job_env[i].opt = xmalloc(sizeof(opt_t));
		memset(pack_job_env[i].opt, 0, sizeof(opt_t));
		pack_job_env[i].env = xmalloc(sizeof(env_t));
		memset(pack_job_env[i].env, 0, sizeof(env_t));
		pack_job_env[i].job = xmalloc(sizeof(srun_job_t));
		memset(pack_job_env[i].job, 0, sizeof(srun_job_t));
		pack_job_env[i].resp =
			xmalloc(sizeof(resource_allocation_response_msg_t));
		memset(pack_job_env[i].resp, 0,
			sizeof(resource_allocation_response_msg_t));
		pack_job_env[i].packleader = false;
		pack_job_env[i].pack_job = false;
		pack_job_env[i].job_id = 0;
		pack_job_env[i].group_number = 0;
		pack_job_env[i].av = (char **) NULL;
		pack_job_env[i].ac = 0;

//		 initialize default values for env structure

		pack_job_env[i].env->stepid = -1;
		pack_job_env[i].env->procid = -1;
		pack_job_env[i].env->localid = -1;
		pack_job_env[i].env->nodeid = -1;
		pack_job_env[i].env->cli = NULL;
		pack_job_env[i].env->env = NULL;
		pack_job_env[i].env->ckpt_dir = NULL;
	}
	return;
}

static void _free_env_structs(int count, pack_job_env_t *pack_job_env)
{
	int i;

	for (i = 0; i < count; i++) {
		xfree(pack_job_env[i].opt);
		xfree(pack_job_env[i].env);
		xfree(pack_job_env[i].job);
		xfree(pack_job_env[i].resp);
	}
	return;
}

static void _build_pack_group_struct(uint32_t index, pack_job_env_t *env_struct)
{
	int i, j, struct_index;
	char *tmp = NULL;
	uint32_t numpack;
	if ((tmp = getenv ("SLURM_NUMPACK"))) {
		numpack = atoi(tmp);
		if (numpack <= 0)
			numpack=1;
	}

	desc = xmalloc(sizeof(pack_group_struct_t) * index);
	total_jobs = index;
	for (i = 0; i < index; i++) {
		desc[i].groupjob = false;
		initialize_and_process_args(env_struct[i].ac,
					    env_struct[i].av);
		desc[i].pack_group_count = opt.ngrpidx;

		/*  there always needs to be a non-zero count so
		 * at least 1 set of structures is built */
		struct_index = opt.ngrpidx;
		if (struct_index == 0) struct_index ++;
		total_jobs += (struct_index-1);
		desc[i].pack_job_env =
			xmalloc(sizeof(pack_job_env_t) * struct_index);
		_build_env_structs(struct_index, &desc[i].pack_job_env[0]);
		if (opt.ngrpidx != 0) desc[i].groupjob = true;
		for (j = 0; j < opt.ngrpidx; j++) {
			desc[i].pack_job_env[j].group_number =
			  opt.groupidx[j];
		}
	}
	return;
}

static void _identify_job_descriptions(int ac, char **av)
{
	int index, index2;
	int i = 0;
	int j = 0;
	int current = 1;
	int job_index = 0;
	char *pack_str = xstrdup("-dpack");
	char *packleader_str = xstrdup("-dpackleader");
	char *command = NULL;
	char **newcmd;
	bool _pack_l;
	uint16_t dependency_position = 0;

/*
	int index3;
	info("in _identify_job_descriptions ac contains %u\n", ac);
	for (index3 = 0; index3 < ac; index3++) {

		info("av[%u] is %s\n", index3, av[index3]);
	}
*/
	newcmd = xmalloc(sizeof(char *) * (ac + 1));
	while (current < ac){
		newcmd[0] = xstrdup(av[0]);
		for (i = 1; i < (ac + 1); i++) {
			newcmd[i] = NULL;
		}
		i = 1;
		j = 1;
		_pack_l = false;
		dependency_position = 0;
		for (index = current; index < ac; index++) {
			command = xstrdup(av[index]);
			if ((strcmp(command, ":") != 0)) {
				newcmd[i] = command;
				if ((strncmp(command, "-d", 2) == 0) ||
				    (strncmp(command, "--d", 3) == 0)) {
					dependency_position = i;
				}
				i++;
				j++;
			} else {
				if (job_index == 0) {
					_pack_l = true;
				}
				break;
			}
		}

		if (_pack_l == false) {
			if (job_index >= 1)
				pack_job_env[job_index].pack_job = true;
		} else {
				pack_job_env[job_index].packleader = true;
		}
		current = index + 1;
		i = 1;

		if (dependency_position == 0) j++;
		pack_job_env[job_index].av = xmalloc(sizeof(char *) * (j + 1));
		pack_job_env[job_index].av[0] = (char *) xstrdup(newcmd[0]);
		i = 1;
		if (dependency_position != 0) {
			if ((_pack_l == false) && (job_index >= 1)){
				xstrcat(newcmd[dependency_position], ",pack");
			} else if (_pack_l == true) {
				xstrfmtcat(newcmd[dependency_position],
					   ",packleader");
				packl_dependency_position = dependency_position;
			}
		} else {
			if (_pack_l == true) {
				pack_job_env[job_index].av[1] =
					(char *) xstrdup(packleader_str);
				packl_dependency_position = 1;
				i++;
			} else if ((_pack_l == false) && (job_index >= 1)) {
				pack_job_env[job_index].av[1] =
					(char *) xstrdup(pack_str);
				i++;
			}
		}
		int k = 1;
		for (index2 = i; index2 < j; index2++) {
			pack_job_env[job_index].av[index2] =
				(char * ) xstrdup(newcmd[k]);
			k++;
		}
		pack_job_env[job_index].ac = j;
/*
		int index1;
		for (index1=0; index1 < j; index1++)
			info("pack_job_env[%u].av[%u] = %s\n", job_index,
			     index1,pack_job_env[job_index].av[index1]);
*/
		job_index++;
	}
	for (i = 0; i < (ac + 1); i++) {
		if(newcmd[i] != NULL)
			xfree(newcmd[i]);
	}
	return;
}

static void _identify_group_job_descriptions(int ac, char **av)
{
	int index, index1, index2;
	int i = 0;
	int j = 0;
	int current = 1;
	int job_index = 0;
	char *pack_str = xstrdup("-dpack");
	char *packleader_str = xstrdup("-dpackleader");
	char *command = NULL;
	char **newcmd;
	bool _pack_l;
	uint16_t dependency_position = 0;

/*
	int index3;
	info("in _identify_group_job_descriptions ac contains %u", ac);
	for (index3 = 0; index3 < ac; index3++) {
		info ("av[%u] is %s", index3, av[index3]);
	}
*/

	newcmd = xmalloc(sizeof(char *) * (ac + 1));
	while (current < ac){
		newcmd[0] = xstrdup(av[0]);
		for (i = 1; i < (ac + 1); i++) {
			newcmd[i] = NULL;
		}
		i = 1;
		j = 1;
		_pack_l = false;
		dependency_position = 0;
		for (index = current; index < ac; index++) {
			command = xstrdup(av[index]);
			if ((strcmp(command, ":") != 0)) {
				newcmd[i] = command;
				if ((strncmp(command, "-d", 2) == 0) ||
				    (strncmp(command, "--d", 3) == 0)) {
					dependency_position = i;
				}
				i++;
				j++;
			} else {
				if (job_index == 0) {
					_pack_l = true;
				}
				break;
			}
		}
		if(desc[job_index].groupjob == false) {
			if (_pack_l == false) {
				if (job_index >= 1)
					desc[job_index].pack_job_env[
						0].pack_job = true;
			} else {
				desc[job_index].pack_job_env[0].packleader =
					true;
			}
		}
		/* establish the start of the next command */
		current = index + 1;

		if ((dependency_position == 0) &&
		    (desc[job_index].groupjob == false)) j++;
		desc[job_index].pack_job_env[0].av =
			xmalloc(sizeof(char *) * (j+1));
		desc[job_index].pack_job_env[0].av [0] =  xstrdup(newcmd[0]);
		i = 1;
		if(desc[job_index].groupjob == false) {
			if (dependency_position != 0) {
				if ((_pack_l == false) && (job_index >= 1)){
					xstrcat(newcmd[dependency_position],
						",pack");
				} else if (_pack_l == true) {
					xstrfmtcat(newcmd[dependency_position],
						   ",packleader");
					packl_dependency_position =
						dependency_position;
				}
			} else {
				if (_pack_l == true) {
					desc[job_index].pack_job_env[0].av [1] =
						xstrdup(packleader_str);
					packl_dependency_position = 1;
					i++;
				} else if ((_pack_l == false) &&
					   (job_index >= 1)) {
					desc[job_index].pack_job_env[0].av [1] =
						xstrdup(pack_str);
					i++;
				}
			}
		}

		int k = 1;
		for (index2 = i; index2 < j; index2++) {
			desc[job_index].pack_job_env[0].av [index2] =
				xstrdup(newcmd[k]);
			k++;
		}

		desc[job_index].pack_job_env[0].ac = j;

		if (desc[job_index].groupjob == true) {
			for (index = 1; index <
			     desc[job_index].pack_group_count; index++) {
				desc[job_index].pack_job_env[index].av =
					xmalloc(sizeof(char *) * (j+2));
				for (index1 = 0; index1 < j + 1; index1++) {
					desc[job_index].pack_job_env[index].av[
						index1] =
						xstrdup(newcmd[index1]);
				}
				desc[job_index].pack_job_env[index].ac = j;
			}
		}
		job_index++;
	}
	for (i = 0; i < (ac + 1); i++) {
		if(newcmd[i] != NULL)
			xfree(newcmd[i]);
	}
	return;
}

int srun(int ac, char **av)
{
	int debug_level;
	env_t *env = xmalloc(sizeof(env_t));
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	bool got_alloc = false;
	slurm_step_io_fds_t cio_fds = SLURM_STEP_IO_FDS_INITIALIZER;
	slurm_step_launch_callbacks_t step_callbacks;

	env->stepid = -1;
	env->procid = -1;
	env->localid = -1;
	env->nodeid = -1;
	env->cli = NULL;
	env->env = NULL;
	env->ckpt_dir = NULL;

	slurm_conf_init(NULL);
	debug_level = _slurm_debug_env_val();
	logopt.stderr_level += debug_level;
	log_init(xbasename(av[0]), logopt, 0, NULL);
	_set_exit_code();

	if ( _count_jobs(ac, av))  {
		_srun_jobpack(ac, av);
		xfree(env);
		return (int)global_rc;
	}

	if (slurm_select_init(1) != SLURM_SUCCESS )
		fatal( "failed to initialize node selection plugin" );

	if (switch_init() != SLURM_SUCCESS )
		fatal("failed to initialize switch plugin");

	_setup_env_working_cluster();

	init_srun(ac, av, &logopt, debug_level, 1);
	create_srun_job(&job, &got_alloc, 0, 1);

	xstrfmtcat(tmp, "%d", job->jobid);      //dhp
	setenv("SLURM_LISTJOBIDS", tmp, 0);     //dhp
	setenv("SLURM_NUMPACK", "0", 0);        //dhp


		/* For each job description, create a job step */
		for (i = 0; i < pack_desc_count; i++) {
			info("******** MNP creating step for pack desc# %d", i); // MNP debug
			opt_ptr = _get_opt(i);
			memcpy(&opt, opt_ptr, sizeof(opt_t));
			job = _get_srun_job(i);
			resp = _get_resp(i);
//			info("******** MNP calling create_job_step for pack desc# %d", i); // MNP debug
			if (!job || create_job_step(job, true) < 0) {
				info("******** MNP error from create_job_step for pack desc# %d", i); // MNP debug
				slurm_complete_job(resp->job_id, 1);
				exit(error_exit);
			}
//			info("******** MNP returned from create_job_step for pack desc# %d", i); // MNP debug
			slurm_free_resource_allocation_response_msg(resp);
			/* What about code at lines 625-638 in srun_job.c? */
			memcpy(opt_ptr, &opt, sizeof(opt_t));
		}
		//info("******** MNP all job steps now created"); // MNP debug
		/* For each job description, enhance environment for job */
		for (i = 0; i < pack_desc_count; i++) {
			//info("******** MNP enhancing environment for pack desc# %d", i); // MNP debug
			opt_ptr = _get_opt(i);
			memcpy(&opt, opt_ptr, sizeof(opt_t));
			job = _get_srun_job(i);
			env = _get_env(i);

	xstrfmtcat(tmp, "%d", job->jobid);      //dhp
	setenv("SLURM_LISTJOBIDS", tmp, 0);     //dhp
	setenv("SLURM_NUMPACK", "0", 0);        //dhp
	init_srun(ac, av, &logopt, debug_level, 1);
	create_srun_job(&job, &got_alloc, 0, 1);

	/*
	 *  Enhance environment for job
	 */
	if (opt.bcast_flag)
		_file_bcast();
	if (opt.cpus_set)
		env->cpus_per_task = opt.cpus_per_task;
	if (opt.ntasks_per_node != NO_VAL)
		env->ntasks_per_node = opt.ntasks_per_node;
	if (opt.ntasks_per_socket != NO_VAL)
		env->ntasks_per_socket = opt.ntasks_per_socket;
	if (opt.ntasks_per_core != NO_VAL)
		env->ntasks_per_core = opt.ntasks_per_core;
	env->distribution = opt.distribution;
	if (opt.plane_size != NO_VAL)
		env->plane_size = opt.plane_size;
	env->cpu_bind_type = opt.cpu_bind_type;
	env->cpu_bind = opt.cpu_bind;

	env->cpu_freq_min = opt.cpu_freq_min;
	env->cpu_freq_max = opt.cpu_freq_max;
	env->cpu_freq_gov = opt.cpu_freq_gov;
	env->mem_bind_type = opt.mem_bind_type;
	env->mem_bind = opt.mem_bind;
	env->overcommit = opt.overcommit;
	env->slurmd_debug = opt.slurmd_debug;
	env->labelio = opt.labelio;
	env->comm_port = slurmctld_comm_addr.port;
	env->batch_flag = 0;
	if (opt.job_name)
		env->job_name = opt.job_name;
	if (job) {
		uint16_t *tasks = NULL;
		slurm_step_ctx_get(job->step_ctx, SLURM_STEP_CTX_TASKS,
				   &tasks);

		env->select_jobinfo = job->select_jobinfo;
		env->nodelist = job->nodelist;
		env->partition = job->partition;
		/* If we didn't get the allocation don't overwrite the
		 * previous info.
		 */
		if (got_alloc)
			env->nhosts = job->nhosts;
		env->ntasks = job->ntasks;
		env->task_count = _uint16_array_to_str(job->nhosts, tasks);
		env->jobid = job->jobid;
		env->stepid = job->stepid;
		env->account = job->account;
		env->qos = job->qos;
		env->resv_name = job->resv_name;
	}
	if (opt.pty && (set_winsize(job) < 0)) {
		error("Not using a pseudo-terminal, disregarding --pty option");
		opt.pty = false;
	}
	if (opt.pty) {
		struct termios term;
		int fd = STDIN_FILENO;

		/* Save terminal settings for restore */
		tcgetattr(fd, &termdefaults);
		tcgetattr(fd, &term);
		/* Set raw mode on local tty */
		cfmakeraw(&term);
		/* Re-enable output processing such that debug() and
		 * and error() work properly. */
		term.c_oflag |= OPOST;
		tcsetattr(fd, TCSANOW, &term);
		atexit(&_pty_restore);

		block_sigwinch();
		pty_thread_create(job);
		env->pty_port = job->pty_port;
		env->ws_col   = job->ws_col;
		env->ws_row   = job->ws_row;
	}
	opt.mpi_ntasks = opt.ntasks; // MNP PMI
	opt.mpi_stepftaskid = 0; // MNP PMI
	setup_env(env, opt.preserve_env);
	xfree(env->task_count);
	xfree(env);
	_set_node_alias();

	memset(&step_callbacks, 0, sizeof(step_callbacks));
	step_callbacks.step_signal   = launch_g_fwd_signal;
relaunch:
	pre_launch_srun_job(job, 0, 1);

	launch_common_set_stdio_fds(job, &cio_fds);

	if (!launch_g_step_launch(job, &cio_fds, &global_rc, &step_callbacks)) {
		if (launch_g_step_wait(job, got_alloc) == -1)
			goto relaunch;
	}

	fini_srun(job, got_alloc, &global_rc, 0);

	return (int)global_rc;

}

static int _file_bcast(void)
{
	struct bcast_parameters *params;
	int rc;

	if ((opt.argc == 0) || (opt.argv[0] == NULL)) {
		error("No command name to broadcast");
		return SLURM_ERROR;
	}
	params = xmalloc(sizeof(struct bcast_parameters));
	params->block_size = 8 * 1024 * 1024;
	params->compress = opt.compress;
	if (opt.bcast_file) {
		params->dst_fname = xstrdup(opt.bcast_file);
	} else {
		xstrfmtcat(params->dst_fname, "%s/slurm_bcast_%u.%u",
			   opt.cwd, job->jobid, job->stepid);
	}
	params->fanout = 0;
	params->job_id = job->jobid;
	params->force = true;
	params->preserve = true;
	params->src_fname = opt.argv[0];
	params->step_id = job->stepid;
	params->timeout = 0;
	params->verbose = 0;

	rc = bcast_file(params);
	if (rc == SLURM_SUCCESS) {
		xfree(opt.argv[0]);
		opt.argv[0] = params->dst_fname;
	} else {
		xfree(params->dst_fname);
	}
	xfree(params);

	return rc;
}

int _srun_jobpack(int ac, char **av)
{
	int debug_level;
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	bool got_alloc = false;
	int desc_index, group_count;

/*
	info("in _srun_jobpack inputs were as follows:\n");
	int index1;
	info(" _srun_jobpack ac contains %u\n", ac);
	for (index1 = 0; index1 < ac; index1++) {
		info ("av[%u] is %s\n", index1, av[index1]);
	}
*/

	slurm_conf_init(NULL);
	debug_level = _slurm_debug_env_val();
	logopt.stderr_level += debug_level;
	_set_exit_code();

	pack_job_env = xmalloc(sizeof(pack_job_env_t) * pack_desc_count);
	_build_env_structs(pack_desc_count, pack_job_env);
	_identify_job_descriptions(ac, av);
	if (slurm_select_init(1) != SLURM_SUCCESS )
		fatal( "failed to initialize node selection plugin" );

	if (switch_init() != SLURM_SUCCESS )
		fatal("failed to initialize switch plugin");

	log_init(xbasename(av[0]), logopt, 0, NULL);
	init_srun(ac, av, &logopt, debug_level, 1);
	_build_pack_group_struct(pack_desc_count, pack_job_env);
	_free_env_structs(pack_desc_count, pack_job_env);

	_identify_group_job_descriptions(ac, av);

	for (group_index = 0; group_index < pack_desc_count; group_index++) {
		group_count = desc[group_index].pack_group_count;
		if (group_count == 0) group_count++;
		for (job_index = 0; job_index < group_count; job_index++) {
			packleader = desc[group_index].pack_job_env[
				job_index].packleader;
			packjob = desc[group_index].pack_job_env[
				job_index].pack_job;

			if (packleader != true) {

				_copy_opt_struct(&opt,
				desc[group_index].pack_job_env[job_index].opt);
				log_init(xbasename(desc[
					 group_index].pack_job_env[
					 job_index].av[0]), logopt, 0, NULL);
				init_srun_jobpack(desc[
						  group_index].pack_job_env[
						  job_index].ac, desc[
						  group_index].pack_job_env[
						  job_index].av, &logopt,
						  debug_level, 1);
				_copy_opt_struct(desc[group_index].pack_job_env[
						 job_index].opt,  &opt);
			}
		}
	}

	for (desc_index = pack_desc_count; desc_index > 0; desc_index--) {
		group_index = desc_index-1;
		group_count = desc[group_index].pack_group_count;
		if (group_count == 0) group_count++;
		for (job_index = 0; job_index < group_count; job_index++) {
			packleader = desc[group_index].pack_job_env[
				job_index].packleader;
			packjob = desc[group_index].pack_job_env[
				job_index].pack_job;
			_copy_opt_struct(&opt, desc[group_index].pack_job_env[
					 job_index].opt);
			if (packleader == true) {

				if (pack_job_id == NULL)
					fatal( "found packleader but no pack "
					       "job id" );
				xstrcat(desc[group_index].pack_job_env[
					job_index].av[
					packl_dependency_position],
					pack_job_id);
				log_init(xbasename(desc[
					 group_index].pack_job_env[
					 job_index].av[0]), logopt, 0, NULL);
				init_srun_jobpack(desc[
						  group_index].pack_job_env[
						  job_index].ac, desc[
						  group_index].pack_job_env[
						  job_index].av, &logopt,
						  debug_level, 1);
//info("packleader depencency for desc[%u].pack_job_env[%u] is %s", group_index, job_index, desc[group_index].pack_job_env[job_index].av[packl_dependency_position]);	/* wjb */
			}
			create_srun_jobpack(&desc[group_index].pack_job_env[
					    job_index].job, &got_alloc, 0, 1);
		}
	}

	_create_srun_steps_jobpack();
	debug("******** MNP all job steps now created");
	_enhance_env_jobpack(got_alloc);
	debug("******** MNP all jobs now environment enhanced");
	_pre_launch_srun_jobpack();
	debug("******** MNP pre_launch_srun_job finished");
	debug("******** MNP parent srun pid = %d", getpid());
	_launch_srun_steps_jobpack(got_alloc);
	debug("******** MNP all job steps now launched");
	return (int)global_rc;
 }

static int _slurm_debug_env_val (void)
{
	long int level = 0;
	const char *val;

	if ((val = getenv ("SLURM_DEBUG"))) {
		char *p;
		if ((level = strtol (val, &p, 10)) < -LOG_LEVEL_INFO)
			level = -LOG_LEVEL_INFO;
		if (p && *p != '\0')
			level = 0;
	}
	return ((int) level);
}

/*
 * Return a string representation of an array of uint32_t elements.
 * Each value in the array is printed in decimal notation and elements
 * are separated by a comma.  If sequential elements in the array
 * contain the same value, the value is written out just once followed
 * by "(xN)", where "N" is the number of times the value is repeated.
 *
 * Example:
 *   The array "1, 2, 1, 1, 1, 3, 2" becomes the string "1,2,1(x3),3,2"
 *
 * Returns an xmalloc'ed string.  Free with xfree().
 */
static char *_uint16_array_to_str(int array_len, const uint16_t *array)
{
	int i;
	int previous = 0;
	char *sep = ",";  /* seperator */
	char *str = xstrdup("");

	if (array == NULL)
		return str;

	for (i = 0; i < array_len; i++) {
		if ((i+1 < array_len)
		    && (array[i] == array[i+1])) {
				previous++;
				continue;
		}

		if (i == array_len-1) /* last time through loop */
			sep = "";
		if (previous > 0) {
			xstrfmtcat(str, "%u(x%u)%s",
				   array[i], previous+1, sep);
		} else {
			xstrfmtcat(str, "%u%s", array[i], sep);
		}
		previous = 0;
	}

	return str;
}

static void _set_exit_code(void)
{
	int i;
	char *val;

	if ((val = getenv("SLURM_EXIT_ERROR"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_ERROR has zero value");
		else
			error_exit = i;
	}

	if ((val = getenv("SLURM_EXIT_IMMEDIATE"))) {
		i = atoi(val);
		if (i == 0)
			error("SLURM_EXIT_IMMEDIATE has zero value");
		else
			immediate_exit = i;
	}
}

static void _set_node_alias(void)
{
	char *aliases, *save_ptr = NULL, *tmp;
	char *addr, *hostname, *slurm_name;

	tmp = getenv("SLURM_NODE_ALIASES");
	if (!tmp)
		return;
	aliases = xstrdup(tmp);
	slurm_name = strtok_r(aliases, ":", &save_ptr);
	while (slurm_name) {
		addr = strtok_r(NULL, ":", &save_ptr);
		if (!addr)
			break;
		slurm_reset_alias(slurm_name, addr, addr);
		hostname = strtok_r(NULL, ",", &save_ptr);
		if (!hostname)
			break;
		slurm_name = strtok_r(NULL, ":", &save_ptr);
	}
	xfree(aliases);
}

static void _pty_restore(void)
{
	/* STDIN is probably closed by now */
	if (tcsetattr(STDOUT_FILENO, TCSANOW, &termdefaults) < 0)
		fprintf(stderr, "tcsetattr: %s\n", strerror(errno));
}

static void _setup_env_working_cluster()
{
	char *working_env  = NULL;
	char *cluster_name = NULL;

	if ((working_env = xstrdup(getenv("SLURM_WORKING_CLUSTER")))) {
		char *port_ptr = strchr(working_env, ':');
		char *rpc_ptr  = strrchr(working_env, ':');
		if (!port_ptr || !rpc_ptr || (port_ptr == rpc_ptr)) {
			error("malformed cluster addr and port in SLURM_WORKING_CLUSTER env var: '%s'",
			      working_env);
			exit(1);
		}
		*port_ptr++ = '\0';
		*rpc_ptr++  = '\0';

		cluster_name = slurm_get_cluster_name();
		if (strcmp(cluster_name, working_env)) {
			working_cluster_rec =
				xmalloc(sizeof(slurmdb_cluster_rec_t));
			slurmdb_init_cluster_rec(working_cluster_rec, false);

			working_cluster_rec->control_host = working_env;
			working_cluster_rec->control_port = strtol(port_ptr,
								   NULL, 10);
			working_cluster_rec->rpc_version  = strtol(rpc_ptr,
								   NULL, 10);
			slurm_set_addr(&working_cluster_rec->control_addr,
				       working_cluster_rec->control_port,
				       working_cluster_rec->control_host);
		} else {
			xfree(working_env);
		}
		xfree(cluster_name);
	}
}
/*
static resource_allocation_response_msg_t  *_get_resp(int job_idx)
{
	return pack_job_env[job_idx].resp;
}
*/

static resource_allocation_response_msg_t  *_get_resp(int desc_idx, int job_idx)
{
	return desc[desc_idx].pack_job_env[job_idx].resp;
}

static opt_t *_get_opt(int desc_idx, int job_idx)
{
	return desc[desc_idx].pack_job_env[job_idx].opt;
}

static srun_job_t *_get_srun_job(int desc_idx, int job_idx)
{
	return desc[desc_idx].pack_job_env[job_idx].job;
}

static env_t *_get_env(int desc_idx, int job_idx)
{
	return desc[desc_idx].pack_job_env[job_idx].env;
}

static void _create_srun_steps_jobpack(void)
{
	int i, j, job_index;
	uint32_t mpi_jobid; // MNP PMI
	opt_t *opt_ptr = NULL;
	resource_allocation_response_msg_t *resp = NULL;

	debug("******** MNP in _create_srun_steps_jobpack, pack_desc_count=%d", pack_desc_count);
	/* For each job description, set MPI jobid to jobid of first step */ // MNP PMI
	opt_ptr = _get_opt(0, 0);  // MNP PMI
	mpi_jobid = opt_ptr->jobid; // MNP PMI
	for (i = 0; i < pack_desc_count; i++) { // MNP PMI
		opt_ptr = _get_opt(i, 0); // MNP PMI
		opt_ptr->mpi_jobid = mpi_jobid; // MNP PMI
		debug("******** MNP in _create_srun_steps_jobpack, i=%d, opt_ptr->mpi_jobid=%d", i, opt_ptr->mpi_jobid);
	} // MNP PMI

	/* For each job description, create a job step */
	for (i = 0; i < pack_desc_count; i++) {
		job_index = desc[i].pack_group_count;
		if (job_index == 0) job_index++;
		for (j = 0; j <job_index; j++) {

			debug("******** MNP creating step for pack desc[%u].pack_job_env[%u]", i, j);
			opt_ptr = _get_opt(i, j);
			memcpy(&opt, opt_ptr, sizeof(opt_t));
			job = _get_srun_job(i, j);
			resp = _get_resp(i, j);

//			debug("******** MNP calling create_job_step for pack desc[%d.pack_job_env[%d]", i, j);
			if (!job || create_job_step(job, true) < 0) {
				debug("******** MNP error from create_job_step for pack desc# %d, %d", i, j);
				slurm_complete_job(resp->job_id, 1);
				exit(error_exit);
			}
//			debug("******** MNP returned from create_job_step for pack desc[%d].pack_job_env[%d]", i, j);
//			slurm_free_resource_allocation_response_msg(resp);
			/* What about code at lines 625-638 in srun_job.c? */
			memcpy(opt_ptr, &opt, sizeof(opt_t));
		}
	}

	/* For each job description, set MPI task count and MPI node count */ // MNP PMI
	for (i = 0; i < pack_desc_count; i++) { // MNP PMI
		opt_ptr = _get_opt(i, 0); // MNP PMI
		opt_ptr->mpi_jobid = mpi_jobid; // MNP PMI
		opt_ptr->mpi_ntasks = mpi_curtaskid; // MNP PMI
		debug("******** MNP in _create_srun_steps_jobpack, i=%d, opt_ptr->mpi_ntasks=%d", i, opt_ptr->mpi_ntasks);
		opt_ptr->mpi_nnodes = mpi_curnodecnt; // MNP PMI
		debug("******** MNP in _create_srun_steps_jobpack, i=%d, opt_ptr->mpi_nnodes=%d", i, opt_ptr->mpi_nnodes);
	} // MNP PMI
}

static void _enhance_env_jobpack(bool got_alloc)
{
	int i, j, job_index;
	opt_t *opt_ptr;
	env_t *env;
	srun_job_t *job;
	slurm_step_launch_callbacks_t step_callbacks;

	/* For each job description, enhance environment for job */
	for (i = 0; i < pack_desc_count; i++) {
		job_index = desc[i].pack_group_count;
		if (job_index == 0) job_index++;
		for (j = 0; j <job_index; j++) {
			debug("******** MNP enhancing environment for pack desc[%d].pack_job_env[%d]", i, j);
			opt_ptr = _get_opt(i, j);
			memcpy(&opt, opt_ptr, sizeof(opt_t));
			job = _get_srun_job(i, j);
			env = _get_env(i, j);
			/*
			*  Enhance environment for job
			*/
			if (opt.cpus_set)
				env->cpus_per_task = opt.cpus_per_task;
			if (opt.ntasks_per_node != NO_VAL)
				env->ntasks_per_node = opt.ntasks_per_node;
			if (opt.ntasks_per_socket != NO_VAL)
				env->ntasks_per_socket = opt.ntasks_per_socket;
			if (opt.ntasks_per_core != NO_VAL)
				env->ntasks_per_core = opt.ntasks_per_core;
			if (opt.job_name)
				env->job_name = opt.job_name;
			env->distribution = opt.distribution;
			if (opt.plane_size != NO_VAL)
				env->plane_size = opt.plane_size;
			env->cpu_bind_type = opt.cpu_bind_type;
			env->cpu_bind = opt.cpu_bind;

			env->cpu_freq_min = opt.cpu_freq_min;
			env->cpu_freq_max = opt.cpu_freq_max;
			env->cpu_freq_gov = opt.cpu_freq_gov;
			env->mem_bind_type = opt.mem_bind_type;
			env->mem_bind = opt.mem_bind;
			env->overcommit = opt.overcommit;
			env->slurmd_debug = opt.slurmd_debug;
			env->labelio = opt.labelio;
			env->comm_port = slurmctld_comm_addr.port;
			env->batch_flag = 0;
			if (opt.job_name)
			  env->job_name = opt.job_name;
			if (job) {
				uint16_t *tasks = NULL;
				slurm_step_ctx_get(job->step_ctx,
						   SLURM_STEP_CTX_TASKS,
						   &tasks);

				env->select_jobinfo = job->select_jobinfo;
				env->nodelist = job->nodelist;
				env->partition = job->partition;
				/* If we didn't get the allocation don't
				* overwrite the previous info.
				*/
				if (got_alloc)
					env->nhosts = job->nhosts;
				env->ntasks = job->ntasks;
				env->task_count =
					_uint16_array_to_str(job->nhosts,
							     tasks);
				env->jobid = job->jobid;
				env->stepid = job->stepid;
				env->account = job->account;
				env->qos = job->qos;
				env->resv_name = job->resv_name;
			}
			if (opt.pty && (set_winsize(job) < 0)) {
				error("Not using a pseudo-terminal, "
				      "disregarding --pty option");
				opt.pty = false;
			}
			if (opt.pty) {
				struct termios term;
				int fd = STDIN_FILENO;

				/* Save terminal settings for restore */
				tcgetattr(fd, &termdefaults);
				tcgetattr(fd, &term);
				/* Set raw mode on local tty */
				cfmakeraw(&term);
				tcsetattr(fd, TCSANOW, &term);
				atexit(&_pty_restore);

				block_sigwinch();
				pty_thread_create(job);
				env->pty_port = job->pty_port;
				env->ws_col   = job->ws_col;
				env->ws_row   = job->ws_row;
			}
			_set_node_alias();

			memset(&step_callbacks, 0, sizeof(step_callbacks));
			step_callbacks.step_signal   = launch_g_fwd_signal;
			memcpy(opt_ptr, &opt, sizeof(opt_t));
		}
	}

	/* For each job description, set MPI task count */ // MNP PMI
	for (i = 0; i < pack_desc_count; i++) { // MNP PMI
		opt_ptr = _get_opt(i, 0); // MNP PMI
		opt_ptr->mpi_jobid = mpi_jobid; // MNP PMI
		opt_ptr->mpi_ntasks = mpi_curtaskid; // MNP PMI
		debug("******** MNP in _create_srun_steps_jobpack, i=%d, opt_ptr->mpi_ntasks=%d", i, opt_ptr->mpi_ntasks);
	} // MNP PMI
}

static void _pre_launch_srun_jobpack(void)
{
	srun_job_t *job;

	/* For now, do pre_launch_srun_job for pack leader only */
	job = _get_srun_job(0, 0);
	pre_launch_srun_job_pack(job, 0, 1);
}

/*
static void _pre_launch_srun_jobpack(void)
{
	srun_job_t *job;
	int i, j, job_index;

	for (i = 0; i < pack_desc_count; i++) {
		job_index = desc[i].pack_group_count;
		if (job_index == 0) job_index++;
		for (j = 0; j <job_index; j++) {
			job = _get_srun_job(i, j);
			pre_launch_srun_job_pack(job, 0, 1);
		}
	}
}
*/


static int _launch_srun_steps_jobpack(bool got_alloc)
{
	int i, j, job_index, pid_idx, pid, *forkpids;
	env_t *env;
	opt_t *opt_ptr;
	slurm_step_io_fds_t cio_fds = SLURM_STEP_IO_FDS_INITIALIZER;
	slurm_step_launch_callbacks_t step_callbacks;

	/* MNP start experimental code to pipe stdin */
//	int stdinpipe[2];
//	if (pipe(stdinpipe) < 0) {
//		debug("******** MNP error creating stdin pipe in parent srun");
//		exit(0);
//	}
	/* MNP end experimental code to pipe stdin */

	/* For each job description, set stdio fds and fork child srun
	 * to handle I/O redirection and step launch
	 */
	memset(&step_callbacks, 0, sizeof(step_callbacks));
	step_callbacks.step_signal   = launch_g_fwd_signal;
	forkpids = xmalloc(total_jobs * sizeof(int));
	pid_idx = 0;
	pipe(vector_pipe);	// MNP PMI pipe test
	pipe(nodelist_pipe);	// MNP PMI pipe test
	pipe(ntasks_pipe);	// MNP PMI pipe test
	pipe(nnodes_pipe);	// MNP PMI pipe test
	for (i = 0; i < pack_desc_count; i++) {
		job_index = desc[i].pack_group_count;
		if (job_index == 0) job_index++;
		for (j = 0; j <job_index; j++) {
			opt_ptr = _get_opt(i, j);
			memcpy(&opt, opt_ptr, sizeof(opt_t));
			job = _get_srun_job(i, j);
			env = _get_env(i, j);
			setup_env(env, opt.preserve_env);
			xfree(env->task_count);
			xfree(env);
			launch_common_set_stdio_fds(job, &cio_fds);
			debug("******** MNP forking child srun for pack desc[%d].pack_job_env[%d]", i, j);
			pid = fork();
			if (pid < 0) {
				/* Error creating child srun process */
				debug("******** MNP fork failed for pack desc[%d].pack_job_env[%d]", i, j);
				exit(0);
			} else if (pid == 0) {
				/* Child srun process */
				debug("******** MNP child srun (PID %d) running", getpid());
				debug("******** MNP %d: launching step for pack desc[%d].pack_job_env[%d]", getpid(), i, j);
				/* MNP start experimental code to pipe stdin */
//				dup2(stdinpipe[0], STDIN_FILENO);
//				close(stdinpipe[0]);
				/* MNP end experimental code to pipe stdin */
				if (!launch_g_step_launch(job, &cio_fds, &global_rc, &step_callbacks)) {
					debug("******** MNP %d: error from launch_g_step_launch, global_rc=%d", getpid(), global_rc);
					if (launch_g_step_wait(job, got_alloc) == -1) {
						debug("******** MNP child srun PID %d: error from launch_g_step_wait", getpid());
						exit(0);
					}
				}
				fini_srun(job, got_alloc, &global_rc, 0);
				debug("******** MNP pid=%d, child has finished fini_srun, global_rc=%d", getpid(),global_rc);
				exit(0);
//			return (int)global_rc;
			} else {
				forkpids[pid_idx] = pid;
				pid_idx++;
				debug("******** MNP in parent srun, adding child pid=%d to forkpids[%d]", pid, i);
			}
		}
	}
	debug("******** MNP parent srun has forked all child sruns");
	/* Wait for all child sruns to exit */
	/* MNP start experimental code to pipe stdin */
//	dup2(stdinpipe[1],STDOUT_FILENO);
//	close(stdinpipe[1]);
//	printf("This is the parent");
	/* MNP end experimental code to pipe stdin */
	for (i = 0; i < total_jobs; i++) {
		int status;
		while (waitpid(forkpids[i], &status, 0) == -1);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			debug("******** MNP pid=%d, child srun pid=%d has failed, WEXITSTATUS(status)=%d", getpid(),forkpids[i], WEXITSTATUS(status));
			debug("******** MNP pid=%d, child srun pid=%d has failed, WIFEXITED(status)=%d", getpid(),forkpids[i], WIFEXITED(status));
			exit(1);
		}
	}
	debug("******** MNP all child sruns have exited successfully");
	return (int)global_rc;
}
