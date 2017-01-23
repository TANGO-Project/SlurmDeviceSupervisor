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

/********************
 * Global Variables *
 ********************/
 typedef struct {
	bool pack;
	bool packleader;
	int pack_count;
	uint32_t argc;
	char **argv;
} srun_info_new_t;
//static pthread_t asym_resource_thread = (pthread_t) 0;

/********************
 * Global Variables *
 ********************/

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

/*---- global variables, defined in opt.h ----*/
int _start_srun(int ac, char **av);
int srun(int ac, char **av);

static struct termios termdefaults;
static uint32_t global_rc = 0;
static srun_job_t *job = NULL;
char *pack_job_id = NULL;
uint32_t pack_job_count = 0;
uint32_t pack_desc_count = 0;
bool packjob = false;
bool packleader = false;
uint16_t packl_dependency_position = 0;
pack_job_env_t *pack_job_env = NULL;
uint32_t group_number;
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

static resource_allocation_response_msg_t  *_get_resp(int job_idx);
static opt_t *_get_opt(int job_idx);
static srun_job_t *_get_srun_job(int job_idx);
static env_t *_get_env(int job_idx);

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
#

int _count_jobs(int ac, char **av)
{
	int index;
	for (index = 0; index < ac; index++) {
		if ((strcmp(av[index], ":") == 0)) {
			pack_desc_count ++;
		}
	}
	if(pack_desc_count) pack_desc_count++;
	return pack_desc_count;
}

int _build_env_structs(int ac, char **av)
{
	int rc = 0;
	int i;
/*
	int index;
	info("in _build_env_structs ac contains %u", ac);
	for (index = 0; index < ac; index++) {
		info ("av[%u] is %s", index, av[index]);
	}
*/

	pack_job_env = xmalloc(sizeof(pack_job_env_t) * pack_desc_count);
	for (i = 0; i < pack_desc_count; i++) {
		pack_job_env[i].opt = xmalloc(sizeof(opt_t));
		memset(pack_job_env[i].opt, 0, sizeof(opt_t));
		pack_job_env[i].env = xmalloc(sizeof(env_t));
		memset(pack_job_env[i].env, 0, sizeof(env_t));
		pack_job_env[i].job = xmalloc(sizeof(srun_job_t));
		memset(pack_job_env[i].job, 0, sizeof(srun_job_t));
		pack_job_env[i].resp = xmalloc(sizeof(resource_allocation_response_msg_t));
		memset(pack_job_env[i].resp, 0, sizeof(resource_allocation_response_msg_t));
		pack_job_env[i].packleader = false;
		pack_job_env[i].pack_job = false;
		pack_job_env[i].job_id = 0;

		/* initialize default values for env structure */

		pack_job_env[i].env->stepid = -1;
		pack_job_env[i].env->procid = -1;
		pack_job_env[i].env->localid = -1;
		pack_job_env[i].env->nodeid = -1;
		pack_job_env[i].env->cli = NULL;
		pack_job_env[i].env->env = NULL;
		pack_job_env[i].env->ckpt_dir = NULL;
	}
	return rc;
}

int _establish_env(int ac, char **av)
{
	int rc = 0;
	int index, index2;
	int i = 0;
	int j = 0;
	int current = 1;
	int job_index = 0;
	char *srun_str = xstrdup("srun");
	char *pack_str = xstrdup("-dpack");
	char *packleader_str = xstrdup("-dpackleader");
	char *command = NULL;
	char **newcmd;
	char **newcmd_cpy;
	bool _pack_l;
	uint16_t dependency_position = 0;

	pack_job_id = xstrdup("");

/*
	int index3;
	info("in _establish_env ac contains %u", ac);
	for (index3 = 0; index3 < ac; index3++) {
		info ("av[%u] is %s", index3, av[index3]);
	}
*/

	while (current < ac){
		newcmd = xmalloc(sizeof(char *) * (ac + 1));
		newcmd[0] = srun_str;
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

		newcmd_cpy = xmalloc(sizeof(char *) * (j+1));
		newcmd_cpy[0] =  xstrdup(newcmd[0]);
		i=1;
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
				newcmd_cpy[1] = xstrdup(packleader_str);
				packl_dependency_position = 1;
				j++;
				i++;
			} else if ((_pack_l == false) && (job_index >= 1)) {
				newcmd_cpy[1] = xstrdup(pack_str);
				j++;
				i++;
			}
		}
		int k = 1;
		for (index2 = i; index2 < j + 1; index2++) {
			newcmd_cpy[index2] =  xstrdup(newcmd[k]);
			k++;
		}

	pack_job_env[job_index].ac = j;
	pack_job_env[job_index].av = newcmd_cpy;
//int index1;
//for (index1=0; index1 < j; index1++)
//	info("pack_job_env[%u].av[%u] = %s", job_index, index1, pack_job_env[job_index].av[index1]);	/* wjb */
//	pack_job_env[job_index]->ac = j;
	job_index++;
//info("job_index contains %u", job_index);					/* wjb */
//	for (i = 0; i < (ac + 1); i++) {
//		xfree(newcmd[i]);
//	}

	}

	return rc;
}


void *_srun_asym_resource_mgr(void *in_data)
{
int rc;
srun_info_new_t srun_info = *(srun_info_new_t *)in_data;
	rc = _start_srun(srun_info.argc, srun_info.argv);
	return NULL;
}

void srun_info_new_t_init (srun_info_new_t *ptr)
{
	ptr->packleader = false;
	ptr->pack_count = 0;
}
static resource_allocation_response_msg_t  *_get_resp(int job_idx)
{
	return pack_job_env[job_idx].resp;
}

static opt_t *_get_opt(int job_idx)
{
	return pack_job_env[job_idx].opt;
}

static srun_job_t *_get_srun_job(int job_idx)
{
	return pack_job_env[job_idx].job;
}

static env_t *_get_env(int job_idx)
{
	return pack_job_env[job_idx].env;
}

*
void
srun_info_new_destroy(srun_info_new_t *srun_info)
{
int index, ac;
	if (srun_info) {
		ac = srun_info->ac;
		for (index=1; index < (ac + 1); index++){
			xfree(srun_info->command[index]);
		}
	}
}
*/
int srun(int ac, char **av)
{
srun_info_new_t *srun_info;
srun_info_new_t *pack_l_srun_info;
	int rc = 0;
	int index, index1, index2;
	int i = 0;
	int j = 0;
	int current = 1;
	char *srun_str = xstrdup("srun");
	char *pack_str = xstrdup("-dpack");
	char *packleader_str = xstrdup("-dpackleader");
	char *command = NULL;
	char **newcmd;
	char **newcmd_cpy;
	char **rest;
	rest = xmalloc(sizeof(char *) * (ac + 1));
	rest[0] = srun_str;
	bool _pack_l;
	uint16_t _pack_c = 0;
	pthread_t asym_resource_thread;
	pthread_attr_t thread_attr;
	uint16_t dependency_position = 0;
	uint16_t packl_dependency_position = 0;

	pack_job_id = xstrdup("");

	while (current < ac){
		newcmd = xmalloc(sizeof(char *) * (ac + 1));
		newcmd[0] = srun_str;
		for (i = 1; i < (ac + 1); i++) {
			newcmd[i] = NULL;
			rest[i] = NULL;
		}
		i = 1;
		j = 1;
		srun_info = xmalloc(sizeof(srun_info_new_t));
		srun_info_new_t_init(srun_info);
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
				if (_pack_c == 0) {
					_pack_l = true;
				}
				break;
			}
		}

		_pack_c++;
		current = index + 1;
		i = 1;

		for (index1 = current; index1 < ac; index1++) {
			command = xstrdup(av[index1]);
			rest[i] = command;
			i++;
		}
		newcmd_cpy = xmalloc(sizeof(char *) * (j+1));
		newcmd_cpy[0] =  xstrdup(newcmd[0]);
		i=1;
		if (dependency_position != 0) {
			if ((_pack_l == false) && (_pack_c >= 2)){
				xstrcat(newcmd[dependency_position], ",pack");
			} else if (_pack_l == true) {
				xstrfmtcat(newcmd[dependency_position],
					   ",packleader");
				packl_dependency_position = dependency_position;
			}
		} else {
			if (_pack_l == true) {
				newcmd_cpy[1] = xstrdup(packleader_str);
				packl_dependency_position = 1;
				j++;
				i++;
			} else if ((_pack_l == false) && (_pack_c >= 2)) {
				newcmd_cpy[1] = xstrdup(pack_str);
				j++;
				i++;
			}
		}
		int k = 1;
		for (index2 = i; index2 < j + 1; index2++) {
			newcmd_cpy[index2] =  xstrdup(newcmd[k]);
			k++;
		}
			srun_info->argc = j;
			srun_info->argv = newcmd_cpy;
			if ((_pack_l == false) && (_pack_c >= 2)){
				srun_info->pack = true;
			} else if (_pack_l == true) {
				srun_info->packleader = true;
				pack_l_srun_info = srun_info;
			} else {
				pack_l_srun_info = srun_info;
			}
		if ((_pack_l == false) && (_pack_c >= 2)) {
			sleep(1);
			slurm_attr_init(&thread_attr);
			while (pthread_create(&asym_resource_thread,
			       &thread_attr, _srun_asym_resource_mgr,
			       (void *)srun_info)) {
				error("pthread_create error %m");
				sleep(1);
			}
			slurm_attr_destroy(&thread_attr);

		}
	}
	if (pack_l_srun_info->packleader == true) {
		while (pack_job_count != (_pack_c -1)){
			sleep(1);
		}
		xstrcat(pack_l_srun_info->argv[packl_dependency_position],
		        pack_job_id);
		info("established packleader dependencies as %s",
		pack_l_srun_info->argv[packl_dependency_position]);	/* wjb */
		pack_l_srun_info->pack_count = _pack_c - 1;
	}

	rc = _start_srun(pack_l_srun_info->argc,
			 pack_l_srun_info->argv);
//	srun_info_new_destroy(srun_info);
	for (i = 0; i < index; i++) {
		xfree(newcmd[i]);
	}

	return (int)global_rc;
}

//int _start_srun(int ac, char **av, srun_job_t *job)
int _start_srun(int ac, char **av)
{
	int debug_level, i, pid;
	env_t *env = xmalloc(sizeof(env_t));
	opt_t * opt_ptr;
	resource_allocation_response_msg_t *resp;
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	bool got_alloc = false;
	slurm_step_io_fds_t cio_fds = SLURM_STEP_IO_FDS_INITIALIZER;
	slurm_step_launch_callbacks_t step_callbacks;
/*
	int index1;
	info(" _start_srun ac contains %u", ac);
	for (index1 = 0; index1 < ac; index1++) {
		info ("av[%u] is %s", index1, av[index1]);
	}
*/							/* wjb */

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

	rc= _count_jobs(ac, av);
	if(pack_desc_count) {
		rc = _build_env_structs(ac, av);
		rc = _establish_env(ac, av);
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
	setup_env(env, opt.preserve_env);
	xfree(env->task_count);
	xfree(env);
	_set_node_alias();

	memset(&step_callbacks, 0, sizeof(step_callbacks));
	step_callbacks.step_signal   = launch_g_fwd_signal;

	/* re_launch: */
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
	int rc = 0;
	log_options_t logopt = LOG_OPTS_STDERR_ONLY;
	bool got_alloc = false;
	int job_index;

/*
	int index1;
	info(" srun ac contains %u", ac);
	for (index1 = 0; index1 < ac; index1++) {
		info ("av[%u] is %s", index1, av[index1]);
	}
*/
	slurm_conf_init(NULL);
	debug_level = _slurm_debug_env_val();
	logopt.stderr_level += debug_level;
	log_init(xbasename(av[0]), logopt, 0, NULL);
	_set_exit_code();

	rc = _build_env_structs(ac, av);
	rc = _establish_env(ac, av);

	if (slurm_select_init(1) != SLURM_SUCCESS )
		fatal( "failed to initialize node selection plugin" );

	if (switch_init() != SLURM_SUCCESS )
		fatal("failed to initialize switch plugin");
	for (job_index = pack_desc_count; job_index > 0; job_index--) {
		group_number = job_index - 1;
		packleader = pack_job_env[group_number].packleader;
		packjob = pack_job_env[group_number].pack_job;
		if (packleader == true)
			xstrcat(pack_job_env[group_number].av[packl_dependency_position],
			        pack_job_id);

		_copy_opt_struct( &opt, pack_job_env[group_number].opt);
		log_init(xbasename(pack_job_env[group_number].av[0]), logopt, 0, NULL);
		init_srun(pack_job_env[group_number].ac, pack_job_env[group_number].av, &logopt, debug_level, 1);
		_copy_opt_struct(pack_job_env[group_number].opt,  &opt);
		create_srun_jobpack(&pack_job_env[group_number].job, &got_alloc, 0, 1);
}

	_create_srun_steps_jobpack();
	info("******** MNP all job steps now created");
	_enhance_env_jobpack(got_alloc);
	info("******** MNP all jobs now environment enhanced");
	_pre_launch_srun_jobpack();
	info("******** MNP pre_launch_srun_job finished");
	info("******** MNP parent srun pid = %d", getpid());
	_launch_srun_steps_jobpack(got_alloc);
	info("******** MNP all job steps now launched");
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
	if(listjobids != NULL) {                                    //dhp
	        lastcomma = strrchr(listjobids, ',');
		*lastcomma = '\0';
	}
	setenv("SLURM_LISTJOBIDS", listjobids, 0);                  //dhp
	snprintf(numpack, sizeof(numpack), "%d", pack_desc_count);  //dhp
	setenv("SLURM_NUMPACK", numpack, 0);                        //dhp
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
static resource_allocation_response_msg_t  *_get_resp(int job_idx)
{
	return pack_job_env[job_idx].resp;
}

static opt_t *_get_opt(int job_idx)
{
	return pack_job_env[job_idx].opt;
}

static srun_job_t *_get_srun_job(int job_idx)
{
	return pack_job_env[job_idx].job;
}

static env_t *_get_env(int job_idx)
{
	return pack_job_env[job_idx].env;
}

static void _create_srun_steps_jobpack(void)
{
	int i;
	opt_t *opt_ptr;
	resource_allocation_response_msg_t *resp;

	/* For each job description, create a job step */
	for (i = 0; i < pack_desc_count; i++) {
		info("******** MNP creating step for pack desc# %d", i);
		opt_ptr = _get_opt(i);
		memcpy(&opt, opt_ptr, sizeof(opt_t));
		job = _get_srun_job(i);
		resp = _get_resp(i);

//		info("******** MNP calling create_job_step for pack desc# %d", i);
		if (!job || create_job_step(job, true) < 0) {
			info("******** MNP error from create_job_step for pack desc# %d", i);
			slurm_complete_job(resp->job_id, 1);
			exit(error_exit);
		}
//		info("******** MNP returned from create_job_step for pack desc# %d", i);
		slurm_free_resource_allocation_response_msg(resp);
		/* What about code at lines 625-638 in srun_job.c? */
		memcpy(opt_ptr, &opt, sizeof(opt_t));
	}
}

static void _enhance_env_jobpack(bool got_alloc)
{
	int i;
	opt_t *opt_ptr;
	env_t *env;
	srun_job_t *job;
	slurm_step_launch_callbacks_t step_callbacks;

	/* For each job description, enhance environment for job */
	for (i = 0; i < pack_desc_count; i++) {
		info("******** MNP enhancing environment for pack desc# %d", i);
		opt_ptr = _get_opt(i);
		memcpy(&opt, opt_ptr, sizeof(opt_t));
		job = _get_srun_job(i);
		env = _get_env(i);
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
			tcsetattr(fd, TCSANOW, &term);
			atexit(&_pty_restore);

			block_sigwinch();
			pty_thread_create(job);
			env->pty_port = job->pty_port;
			env->ws_col   = job->ws_col;
			env->ws_row   = job->ws_row;
		}
		setup_env(env, opt.preserve_env);
		xfree(env->task_count);
		xfree(env);
		_set_node_alias();

		memset(&step_callbacks, 0, sizeof(step_callbacks));
		step_callbacks.step_signal   = launch_g_fwd_signal;
		memcpy(opt_ptr, &opt, sizeof(opt_t));
	}
}

static void _pre_launch_srun_jobpack(void)
{
	srun_job_t *job;

	/* For now, do pre_launch_srun_job for pack leader only */
	job = _get_srun_job(0);
	pre_launch_srun_job_pack(job, 0, 1);
}

static int _launch_srun_steps_jobpack(bool got_alloc)
{
	int i, pid, *forkpids;
	opt_t *opt_ptr;
	env_t *env;
	slurm_step_io_fds_t cio_fds = SLURM_STEP_IO_FDS_INITIALIZER;
	slurm_step_launch_callbacks_t step_callbacks;

	/* MNP start experimental code to pipe stdin */
//	int stdinpipe[2];
//	if (pipe(stdinpipe) < 0) {
//		info("******** MNP error creating stdin pipe in parent srun");
//		exit(0);
//	}
	/* MNP end experimental code to pipe stdin */

	/* For each job description, set stdio fds and fork child srun
	 * to handle I/O redirection and step launch
	 */
	forkpids = xmalloc(pack_desc_count * sizeof(int));
	for (i = 0; i < pack_desc_count; i++) {
		opt_ptr = _get_opt(i);
		memcpy(&opt, opt_ptr, sizeof(opt_t));
		job = _get_srun_job(i);
		env = _get_env(i);
		launch_common_set_stdio_fds(job, &cio_fds);
		info("******** MNP forking child srun for pack desc# %d", i);
		pid = fork();
		if (pid < 0) {
			/* Error creating child srun process */
			info("******** MNP fork failed for pack desc# %d", i);
			exit(0);
		} else if (pid == 0) {
			/* Child srun process */
			info("******** MNP child srun (PID %d) running", getpid());
			info("******** MNP %d: launching step for pack desc# %d", getpid(), i);
			/* MNP start experimental code to pipe stdin */
//			dup2(stdinpipe[0], STDIN_FILENO);
//			close(stdinpipe[0]);
			/* MNP end experimental code to pipe stdin */
			if (!launch_g_step_launch(job, &cio_fds, &global_rc, &step_callbacks)) {
				info("******** MNP %d: error from launch_g_step_launch, global_rc=%d", getpid(), global_rc);
				if (launch_g_step_wait(job, got_alloc) == -1) {
					info("******** MNP child srun PID %d: error from launch_g_step_wait", getpid());
					exit(0);
				}
			}
			fini_srun(job, got_alloc, &global_rc, 0);
			info("******** MNP pid=%d, child has finished fini_srun, global_rc=%d", getpid(),global_rc);
			exit(0);
//			return (int)global_rc;
		} else {
			forkpids[i] = pid;
			info("******** MNP in parent srun, adding child pid=%d to forkpids[%d]", pid, i);
		}
	}
	info("******** MNP parent srun has forked all child sruns");
	/* Wait for all child sruns to exit */
	/* MNP start experimental code to pipe stdin */
//	dup2(stdinpipe[1],STDOUT_FILENO);
//	close(stdinpipe[1]);
//	printf("This is the parent");
	/* MNP end experimental code to pipe stdin */
	for (i = 0; i < pack_desc_count; i++) {
		int status;
		while (waitpid(forkpids[i], &status, 0) == -1);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			info("******** MNP pid=%d, child srun pid=%d has failed, WEXITSTATUS(status)=%d", getpid(),forkpids[i], WEXITSTATUS(status));
			info("******** MNP pid=%d, child srun pid=%d has failed, WIFEXITED(status)=%d", getpid(),forkpids[i], WIFEXITED(status));
			exit(1);
		}
	}
	info("******** MNP all child sruns have exited successfully");
	return (int)global_rc;
}
