/*****************************************************************************\
 **  setup.c - PMI2 server setup
 *****************************************************************************
 *  Copyright (C) 2011-2012 National University of Defense Technology.
 *  Written by Hongjia Cao <hjcao@nudt.edu.cn>.
 *  All rights reserved.
 *  Portions copyright (C) 2015 Mellanox Technologies Inc.
 *  Written by Artem Y. Polyakov <artemp@mellanox.com>.
 *  All rights reserved.
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

#if defined(__FreeBSD__)
#include <sys/socket.h>	/* AF_INET */
#endif

#include <dlfcn.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/common/slurm_xlator.h"
#include "src/common/net.h"
#include "src/common/proc_args.h"
#include "src/common/slurm_mpi.h"
#include "src/common/xstring.h"
#include "src/slurmd/slurmstepd/slurmstepd_job.h"
#include "src/slurmd/common/reverse_tree_math.h"

#include "setup.h"
#include "tree.h"
#include "pmi.h"
#include "spawn.h"
#include "kvs.h"
#include "ring.h"

#define PMI2_SOCK_ADDR_FMT "%s/sock.pmi2.%u.%u"


extern char **environ;

static bool run_in_stepd = 0;

int  tree_sock;
int *task_socks;
char tree_sock_addr[128];
pmi2_job_info_t job_info;
pmi2_tree_info_t tree_info;

// MNP PMI start
void _replace(char *s, char *old, char *new)
{
	char *p, *p1;
	size_t newlen = strlen(new);
	size_t oldlen = strlen(old);
	char buf[50];

	p = strstr(s, old); 	// p  = ptr to first char in old substring
//	printf("p = %s\n", p);
	p1 = p + oldlen;	// p1 = ptr to first char after old substring
//	printf("p1 = %s\n", p1);
	strcpy(buf, p1);	// copy rest of orig string to buffer
	strncpy(s, new, newlen);// insert new substring into orig string
//	printf("s = %s\n", s);
	strcpy(buf, s + newlen);// copy rest of orig string back
//	printf("buf = %s\n", buf);
}

void _append(char* s, char c)
{
        int len = strlen(s);
        s[len] = c;
 }

void _adjust_vect_nodeidx(char *vectstr, char *newvecstr, int curnodeidx)
{
	char *sub, *sub1, *last;
	char *numstr = xmalloc(50 * sizeof(char));
	int num;
	char newnumstr[50];

//	debug("******** MNP pid=%d, entering _adjust_vect_nodeidx, vectstr='%s', curnodeidx=%d", getpid(), vectstr, curnodeidx);
	strcpy(newvecstr, vectstr);
	last=newvecstr;
//	printf("newvecstr = '%s'\n", newvecstr);
	sub = newvecstr;
	numstr[0]='\0';
	while ((sub = strstr(sub, ",(")) != NULL) {
		sub+=2;
		sub1=sub;
//		printf("sub = '%s'\n", sub);
//		printf("last = '%s'\n", last);
//		printf("numstr = '%s'\n", numstr);
		while(1) {
			if (isdigit(*sub)) {
//				printf("Found digit = '%c' \n", *sub);
//				_append(numstr, *sub);
				xstrcatchar(numstr, *sub);
				sub++;
			} else
				break;
		}
		sub=sub1;
//		_append(numstr, '\0');
		xstrcatchar(numstr, '\0');
//		printf("numstr = '%s'\n", numstr);
		num = atoi(numstr);
//		printf("num = %d\n", num);
		num+=curnodeidx;
//		printf("num = %d\n", num);
		sprintf(newnumstr, "%d", num);
//		printf("newnumstr = '%s'\n", newnumstr);
//		xstrsubstitute(&sub, numstr, newnumstr);
		_replace(sub, numstr, newnumstr); // should use xstrsubstitute in Slurm code
//		printf("after _replace, newvecstr = '%s'\n", newvecstr);
		numstr[0]= '\0';
	}
//	debug("******** MNP pid=%d, exiting _adjust_vect_nodeidx, newvecstr='%s'", getpid(), newvecstr);
}

// combine vector lists vect_list1 and vect_list2. Return combined lists in vect_list1
void _combine_vect_lists(char *vect_list1, char *vect_list2)
{
	char *p, *p1;
	// point after "(vector" in vect_list2
	p = strstr(vect_list2, "(vector");
	p+=7;
	// point before final ")" in vect_list1
	p1 = strstr(vect_list1, "))");
	p1[1]='\0';;
	// insert rest of vect_list2 into vect_list1
	strcat(p1, p);
	// remove extraneous ")" from combined vectors
}

static void _write_pmi_port(void)
{
	FILE *fd;

	fd = fopen("/var/tmp/mnp-slurm/pmi_port", "w");
	if (!fd) {
//		debug("******** MNP pid=%d, in _write_pmi_port, error opening file", getpid());
	}
	fprintf(fd, "%d", tree_info.pmi_port);
	fclose(fd);
}

/*
static int _read_pmi_port(void)
{
	FILE *fd;
	int i;

	fd = fopen("/var/tmp/mnp-slurm/pmi_port","r");
	if (!fd) {
		debug("******** MNP pid=%d, in _read_pmi_port, error opening file", getpid());
		return SLURM_ERROR;
	}
	fscanf(fd, "%d", &i);
	fclose(fd);
	return i;

}
// MNP PMI end
*/

extern bool
in_stepd(void)
{
	return run_in_stepd;
}

static void
_remove_tree_sock(void)
{
	unlink(tree_sock_addr);
}

static int
_setup_stepd_job_info(const stepd_step_rec_t *job, char ***env)
{
	char *p;
	int i;

	debug("******** MNP pid=%d, entering _setup_stepd_job_info", getpid());
//	debug("******** MNP pid=%d, setting up global job_info from stepd_step_rec_t", getpid());
	memset(&job_info, 0, sizeof(job_info));
	job_info.jobid  = job->mpi_jobid; // MNP PMI
	job_info.stepid = job->stepid;
//	job_info.nnodes = job->nnodes;
	job_info.nodeid = job->nodeid;
	job_info.ntasks = job->mpi_ntasks; // MNP PMI
	job_info.nnodes = job->mpi_nnodes; // MNP PMI
	job_info.ltasks = job->node_tasks;
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job->nodeid=%d", getpid(), job->nodeid);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job->node_tasks=%d", getpid(), job->node_tasks);
	job_info.gtids = xmalloc(job->node_tasks * sizeof(uint32_t));
	for (i = 0; i < job->node_tasks; i++) { // MNP PMI
		job_info.gtids[i] = job->task[i]->mpi_taskid; // MNP PMI
	}
	job_info.switch_job = (void*)job->switch_job;
	p = getenvp(*env, PMI2_PMI_DEBUGGED_ENV);
	if (p) {
		job_info.pmi_debugged = atoi(p);
	} else {
		job_info.pmi_debugged = 0;
	}
	p = getenvp(*env, PMI2_SPAWN_SEQ_ENV);
	if (p) { 		/* spawned */
		job_info.spawn_seq = atoi(p);
		unsetenvp(*env, PMI2_SPAWN_SEQ_ENV);
		p = getenvp(*env, PMI2_SPAWNER_JOBID_ENV);
		job_info.spawner_jobid = xstrdup(p);
		unsetenvp(*env, PMI2_SPAWNER_JOBID_ENV);
	} else {
		job_info.spawn_seq = 0;
		job_info.spawner_jobid = NULL;
	}
//	debug("******** MNP pid=%d, in _setup_stepd_job_info 6", getpid()); // MNP PMI
	p = getenvp(*env, PMI2_PMI_JOBID_ENV);
//	debug("******** MNP pid=%d, in _setup_stepd_job_info 7", getpid()); // MNP PMI
	if (p) {
//		debug("******** MNP pid=%d, in _setup_stepd_job_info, setting up job_info.pmi_jobid from PMI2_PMI_JOBID_ENV", getpid());
		job_info.pmi_jobid = xstrdup(p);
		unsetenvp(*env, PMI2_PMI_JOBID_ENV);
	} else {
//		debug("******** MNP pid=%d, in _setup_stepd_job_info, setting up job_info.pmi_jobid from stepd_step_rec_t", getpid());
		xstrfmtcat(job_info.pmi_jobid, "%u.%u", job->mpi_jobid, // MNP PMI
			   job->stepid);
	}
	p = getenvp(*env, PMI2_STEP_NODES_ENV);
	if (!p) {
		error("mpi/pmi2: unable to find nodes in job environment");
		return SLURM_ERROR;
	} else {
		job_info.step_nodelist = xstrdup(p);
		unsetenvp(*env, PMI2_STEP_NODES_ENV);
	}
	job_info.nodeid = nodelist_find(job_info.step_nodelist, job->node_name); // MNP PMI

	/*
	 * how to get the mapping info from stepd directly?
	 * there is the task distribution info in the launch_tasks_request_msg_t,
	 * but it is not stored in the stepd_step_rec_t.
	 */
	p = getenvp(*env, PMI2_PROC_MAPPING_ENV);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, PMI2_PROC_MAPPING_ENV=%s", getpid(), p);
	if (!p) {
		error("PMI2_PROC_MAPPING_ENV not found");
		return SLURM_ERROR;
	} else {
		job_info.proc_mapping = xstrdup(p);
		unsetenvp(*env, PMI2_PROC_MAPPING_ENV);
	}

	job_info.job_env = env_array_copy((const char **)*env);

	job_info.MPIR_proctable = NULL;
	job_info.srun_opt = NULL;

	/* get the SLURM_STEP_RESV_PORTS
	 */
	p = getenvp(*env, SLURM_STEP_RESV_PORTS);
	if (!p) {
		debug("%s: %s not found in env", __func__, SLURM_STEP_RESV_PORTS);
	} else {
		job_info.resv_ports = xstrdup(p);
		info("%s: SLURM_STEP_RESV_PORTS found %s", __func__, p);
	}

	debug("******** MNP pid=%d, in _setup_stepd_job_info, job->node_tasks=%d", getpid(), job->node_tasks);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.jobid=%d", getpid(), job_info.jobid);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.stepid=%d", getpid(), job_info.stepid);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.nnodes=%d", getpid(), job_info.nnodes);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.nodeid=%d", getpid(), job_info.nodeid);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.ntasks=%d", getpid(), job_info.ntasks);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.ltasks=%d", getpid(), job_info.ltasks);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.step_nodelist=%s", getpid(), job_info.step_nodelist);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.proc_mapping=%s", getpid(), job_info.proc_mapping);
	debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.pmi_jobid=%s", getpid(), job_info.pmi_jobid);
	for (i = 0; i < job->node_tasks; i++) {
		debug("******** MNP pid=%d, in _setup_stepd_job_info, job_info.gtids[%d]=%d", getpid(), i, job_info.gtids[i]);
	}
	debug("******** MNP pid=%d, exiting _setup_stepd_job_info", getpid());
	return SLURM_SUCCESS;
}

static int
_setup_stepd_tree_info(const stepd_step_rec_t *job, char ***env)
{
	hostlist_t hl;
	char *srun_host;
	uint16_t port;
	char *p;
	int tree_width;

	/* job info available */

	memset(&tree_info, 0, sizeof(tree_info));

	hl = hostlist_create(job_info.step_nodelist);
	p = hostlist_nth(hl, job_info.nodeid); /* strdup-ed */
	tree_info.this_node = xstrdup(p);
	free(p);

	/* this only controls the upward communication tree width */
	p = getenvp(*env, PMI2_TREE_WIDTH_ENV);
	if (p) {
		tree_width = atoi(p);
		if (tree_width < 2) {
			info("invalid PMI2 tree width value (%d) detected. "
			     "fallback to default value.", tree_width);
			tree_width = slurm_get_tree_width();
		}
	} else {
		tree_width = slurm_get_tree_width();
	}

	/* TODO: cannot launch 0 tasks on node */

	/*
	 * In tree position calculation, root of the tree is srun with id 0.
	 * Stepd's id will be its nodeid plus 1.
	 */
	reverse_tree_info(job_info.nodeid + 1, job_info.nnodes + 1,
			  tree_width, &tree_info.parent_id,
			  &tree_info.num_children, &tree_info.depth,
			  &tree_info.max_depth);
	tree_info.parent_id --;	       /* restore real nodeid */
	if (tree_info.parent_id < 0) {	/* parent is srun */
		tree_info.parent_node = NULL;
	} else {
		p = hostlist_nth(hl, tree_info.parent_id);
		tree_info.parent_node = xstrdup(p);
		free(p);
	}
	hostlist_destroy(hl);

	tree_info.pmi_port = 0;	/* not used */

	srun_host = getenvp(*env, "SLURM_SRUN_COMM_HOST");
	if (!srun_host) {
		error("mpi/pmi2: unable to find srun comm ifhn in env");
		return SLURM_ERROR;
	}
	p = getenvp(*env, PMI2_SRUN_PORT_ENV);
	if (!p) {
		error("mpi/pmi2: unable to find srun pmi2 port in env");
		return SLURM_ERROR;
	}
	port = atoi(p);

	tree_info.srun_addr = xmalloc(sizeof(slurm_addr_t));
	slurm_set_addr(tree_info.srun_addr, port, srun_host);

	unsetenvp(*env, PMI2_SRUN_PORT_ENV);

	/* init kvs seq to 0. TODO: reduce array size */
	tree_info.children_kvs_seq = xmalloc(sizeof(uint32_t) *
					     job_info.nnodes);

	return SLURM_SUCCESS;
}

/*
 * setup sockets for slurmstepd
 */
static int
_setup_stepd_sockets(const stepd_step_rec_t *job, char ***env)
{
	struct sockaddr_un sa;
	int i;
	char *spool;

	info("******** MNP pid=%d, entering pmi2/setup.c:_setup_stepd_sockets", getpid());
	debug("mpi/pmi2: setup sockets");

	tree_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (tree_sock < 0) {
		error("mpi/pmi2: failed to create tree socket: %m");
		return SLURM_ERROR;
	}
	sa.sun_family = PF_UNIX;

	/* tree_sock_addr has to remain unformatted since the formatting
	 * happens on the slurmd side */
	spool = slurm_get_slurmd_spooldir(NULL);
	snprintf(tree_sock_addr, sizeof(tree_sock_addr), PMI2_SOCK_ADDR_FMT,
		 spool, job->jobid, job->stepid);
	/* Make sure we adjust for the spool dir coming in on the address to
	 * point to the right spot.
	 */
	xstrsubstitute(spool, "%n", job->node_name);
	xstrsubstitute(spool, "%h", job->node_name);
	snprintf(sa.sun_path, sizeof(sa.sun_path), PMI2_SOCK_ADDR_FMT,
		 job->mpi_jobid, job->stepid); // MNP PMI attempted fix
	unlink(sa.sun_path);    /* remove possible old socket */
	debug("******** MNP pid=%d, in pmi2/setup.c:_setup_stepd_sockets, sa.sun_path=%s", getpid(),sa.sun_path);
	if (bind(tree_sock, (struct sockaddr *)&sa, SUN_LEN(&sa)) < 0) {
		error("mpi/pmi2: failed to bind tree socket: %m");
		unlink(sa.sun_path);
		return SLURM_ERROR;
	}
	if (listen(tree_sock, 64) < 0) {
		error("mpi/pmi2: failed to listen tree socket: %m");
		unlink(sa.sun_path);
		return SLURM_ERROR;
	}

	task_socks = xmalloc(2 * job->node_tasks * sizeof(int));
	for (i = 0; i < job->node_tasks; i ++) {
		socketpair(AF_UNIX, SOCK_STREAM, 0, &task_socks[i * 2]);
		/* this must be delayed after the tasks have been forked */
/* 		close(TASK_PMI_SOCK(i)); */
	}
	for (i = 0; i < job->node_tasks; i ++) {
		debug("******** MNP, pid=%d, in _setup_stepd_sockets, task_socks[%d]=%d", getpid(), i*2, task_socks[i*2]);
		debug("******** MNP, pid=%d, in _setup_stepd_sockets, task_socks[%d]=%d", getpid(), (i*2)+1, task_socks[(i*2)+1]);
	}
	return SLURM_SUCCESS;
}

static int
_setup_stepd_kvs(const stepd_step_rec_t *job, char ***env)
{
	int rc = SLURM_SUCCESS, i = 0, pp_cnt = 0;
	char *p, env_key[32], *ppkey, *ppval;

//	debug("******** MNP pid=%d in pmi2 plugin _setup_stepd_kvs, setting kvs_seq=1", getpid());
//	kvs_seq = 1;
	rc = temp_kvs_init();
	if (rc != SLURM_SUCCESS)
		return rc;

	rc = kvs_init();
	if (rc != SLURM_SUCCESS)
		return rc;

	/* preput */
	p = getenvp(*env, PMI2_PREPUT_CNT_ENV);
	if (p) {
		pp_cnt = atoi(p);
	}

	for (i = 0; i < pp_cnt; i ++) {
		snprintf(env_key, 32, PMI2_PPKEY_ENV"%d", i);
		p = getenvp(*env, env_key);
		ppkey = p; /* getenvp will not modify p */
		snprintf(env_key, 32, PMI2_PPVAL_ENV"%d", i);
		p = getenvp(*env, env_key);
		ppval = p;
		kvs_put(ppkey, ppval);
	}

	/*
	 * For PMI11.
	 * A better logic would be to put PMI_process_mapping in KVS only if
	 * the task distribution method is not "arbitrary", because in
	 * "arbitrary" distribution the process mapping varible is not correct.
	 * MPICH2 may deduce the clique info from the hostnames. But that
	 * is rather costly.
	 */
//	debug("******** MNP in pmi2 plugin _setup_stepd_kvs, job_info.proc_mapping=%s", job_info.proc_mapping);
	kvs_put("PMI_process_mapping", job_info.proc_mapping);

	return SLURM_SUCCESS;
}

extern int
pmi2_setup_stepd(const stepd_step_rec_t *job, char ***env)
{
	int rc;

	run_in_stepd = true;

	/* job info */
	rc = _setup_stepd_job_info(job, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	/* tree info */
	rc = _setup_stepd_tree_info(job, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	/* sockets */
	rc = _setup_stepd_sockets(job, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	/* kvs */
	rc = _setup_stepd_kvs(job, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	/* TODO: finalize pmix_ring state somewhere */
	/* initialize pmix_ring state */
	rc = pmix_ring_init(&job_info, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	return SLURM_SUCCESS;
}

extern void
pmi2_cleanup_stepd()
{
	close(tree_sock);
	_remove_tree_sock();
}
/**************************************************************/

/* returned string should be xfree-ed by caller */
static char *
_get_proc_mapping(const mpi_plugin_client_info_t *job)
{
	uint32_t node_cnt, task_cnt, task_mapped, node_task_cnt, **tids,
		block;
	uint16_t task_dist, *tasks, *rounds;
	int i, start_id, end_id;
	char *mapping = NULL;

	debug("******** MNP pid=%d, entering _get_proc_mapping", getpid());
	node_cnt = job->step_layout->node_cnt;
	task_cnt = job->step_layout->task_cnt;
	task_dist = job->step_layout->task_dist & SLURM_DIST_STATE_BASE;
	tasks = job->step_layout->tasks;
	tids = job->step_layout->tids; // MNP PMI old code

	// MNP PMI start debug info only, delete when done
	int j,k;
	for (j=0; j<node_cnt; j++) {
		for(k=0; k<tasks[j]; k++) {
			debug("******** MNP pid=%d, tids[%d][%d]=%d, mpi_tids[%d][%d]=%d", getpid(),j,k,job->step_layout->tids[j][k],j,k,job->step_layout->mpi_tids[j][k]);
		}
	} // MNP PMI end

	/* for now, PMI2 only supports vector processor mapping */

	if (task_dist == SLURM_DIST_CYCLIC ||
	    task_dist == SLURM_DIST_CYCLIC_CFULL ||
	    task_dist == SLURM_DIST_CYCLIC_CYCLIC ||
	    task_dist == SLURM_DIST_CYCLIC_BLOCK) {
		debug("******** MNP pid=%d, in _get_proc_mapping, dist is CYCLIC", getpid());
		mapping = xstrdup("(vector");

		rounds = xmalloc (node_cnt * sizeof(uint16_t));
		task_mapped = 0;
		while (task_mapped < task_cnt) {
			start_id = 0;
			/* find start_id */
			while (start_id < node_cnt) {
				while (start_id < node_cnt &&
				       ( rounds[start_id] >= tasks[start_id] ||
					 (task_mapped !=
					  tids[start_id][rounds[start_id]]) )) {
					start_id ++;
				}
				if (start_id >= node_cnt)
					break;
				/* block is always 1 */
				/* find end_id */
				end_id = start_id;
				while (end_id < node_cnt &&
				       ( rounds[end_id] < tasks[end_id] &&
					 (task_mapped ==
					  tids[end_id][rounds[end_id]]) )) {
					rounds[end_id] ++;
					task_mapped ++;
					end_id ++;
				}
				xstrfmtcat(mapping, ",(%u,%u,1)", start_id,
					   end_id - start_id);
				start_id = end_id;
			}
		}
		xfree(rounds);
		xstrcat(mapping, ")");
	} else if (task_dist == SLURM_DIST_ARBITRARY) {
		/*
		 * MPICH2 will think that each task runs on a seperate node.
		 * The program will run, but no SHM will be used for
		 * communication.
		 */
		mapping = xstrdup("(vector");
		xstrfmtcat(mapping, ",(0,%u,1)", job->step_layout->task_cnt);
		xstrcat(mapping, ")");

	} else if (task_dist == SLURM_DIST_PLANE) {
		mapping = xstrdup("(vector");

		rounds = xmalloc (node_cnt * sizeof(uint16_t));
		task_mapped = 0;
		while (task_mapped < task_cnt) {
			start_id = 0;
			/* find start_id */
			while (start_id < node_cnt) {
				while (start_id < node_cnt &&
				       ( rounds[start_id] >= tasks[start_id] ||
					 (task_mapped !=
					  tids[start_id][rounds[start_id]]) )) {
					start_id ++;
				}
				if (start_id >= node_cnt)
					break;
				/* find start block. block may be less
				 * than plane size */
				block = 0;
				while (rounds[start_id] < tasks[start_id] &&
				       (task_mapped ==
					tids[start_id][rounds[start_id]])) {
					block ++;
					rounds[start_id] ++;
					task_mapped ++;
				}
				/* find end_id */
				end_id = start_id + 1;
				while (end_id < node_cnt &&
				       (rounds[end_id] + block - 1 <
					tasks[end_id])) {
					for (i = 0;
					     i < tasks[end_id] - rounds[end_id];
					     i ++) {
						if (task_mapped + i !=
						    tids[end_id][rounds[end_id]
								 + i]) {
							break;
						}
					}
					if (i != block)
						break;
					rounds[end_id] += block;
					task_mapped += block;
					end_id ++;
				}
				xstrfmtcat(mapping, ",(%u,%u,%u)", start_id,
					   end_id - start_id, block);
				start_id = end_id;
			}
		}
		xfree(rounds);
		xstrcat(mapping, ")");

	} else {		/* BLOCK mode */
		debug("******** MNP pid=%d, in _get_proc_mapping, dist is BLOCK", getpid());
		mapping = xstrdup("(vector"); // MNP PMI old code start
		start_id = 0;
		node_task_cnt = tasks[start_id];
		for (i = start_id + 1; i < node_cnt; i ++) {
			if (node_task_cnt == tasks[i])
				continue;
			xstrfmtcat(mapping, ",(%u,%u,%u)", start_id,
				   i - start_id, node_task_cnt);
			start_id = i;
			node_task_cnt = tasks[i];
		}
		xstrfmtcat(mapping, ",(%u,%u,%u))", start_id, i - start_id,
			   node_task_cnt); // MNP PMI old code end
//		mapping = xstrdup("(vector"); // MNP PMI new code start
//		start_id = 0;
//		node_task_cnt = tasks[start_id];
//		for (i = start_id + 1; i < node_cnt; i ++) {
//			if (node_task_cnt == tasks[i])
//				continue;
//			xstrfmtcat(mapping, ",(%u,%u,%u)", tids[i][start_id],
//				   tids[i][i - start_id], node_task_cnt);
//			start_id = i;
//			node_task_cnt = tasks[i];
//		}
//		xstrfmtcat(mapping, ",(%u,%u,%u))", tids[i][start_id], tids[i][i - start_id],
//			   node_task_cnt); // MNP PMI new code end
	}

	debug("mpi/pmi2: processor mapping: %s", mapping);
	debug("******** MNP pid=%d, exiting _get_proc_mapping", getpid());
	return mapping;
}

static int
_setup_srun_job_info(const mpi_plugin_client_info_t *job)
{
	char *p;
	void *handle = NULL, *sym = NULL;

//	debug("******** MNP pid=%d, entering _setup_srun_job_info", getpid());
	memset(&job_info, 0, sizeof(job_info));

	job_info.jobid  = job->jobid;
	job_info.orig_jobid = job->orig_jobid; // MNP PMI
	job_info.stepid = job->stepid;
	job_info.nnodes = job->step_layout->node_cnt;
	job_info.nodeid = -1;	/* id in tree. not used. */
	job_info.ntasks = job->step_layout->task_cnt;
	job_info.ltasks = 0;	/* not used */
	job_info.gtids = NULL;	/* not used */
	job_info.switch_job = NULL; /* not used */


	p = getenv(PMI2_PMI_DEBUGGED_ENV);
	if (p) {
		job_info.pmi_debugged = atoi(p);
	} else {
		job_info.pmi_debugged = 0;
	}
	p = getenv(PMI2_SPAWN_SEQ_ENV);
	if (p) { 		/* spawned */
		job_info.spawn_seq = atoi(p);
		p = getenv(PMI2_SPAWNER_JOBID_ENV);
		job_info.spawner_jobid = xstrdup(p);
		/* env unset in stepd */
	} else {
		job_info.spawn_seq = 0;
		job_info.spawner_jobid = NULL;
	}
//	debug("******** MNP pid=%d, in _setup_srun_job_info 1", getpid());
	job_info.step_nodelist = xstrdup(job->step_layout->node_list);
	job_info.proc_mapping = _get_proc_mapping(job);
	if (job_info.proc_mapping == NULL) {
		return SLURM_ERROR;
	}
//	debug("******** MNP pid=%d, in _setup_srun_job_info 2", getpid());
	p = getenv(PMI2_PMI_JOBID_ENV);
	if (p) {		/* spawned */
		job_info.pmi_jobid = xstrdup(p);
	} else {
		xstrfmtcat(job_info.pmi_jobid, "%u.%u", job->jobid,
			   job->stepid);
	}
	job_info.job_env = env_array_copy((const char **)environ);

//	debug("******** MNP pid=%d, in _setup_srun_job_info 3", getpid());

	/* hjcao: this is really dirty.
	   But writing a new launcher is not desirable. */
	handle = dlopen(NULL, RTLD_LAZY);
	if (handle == NULL) {
		error("mpi/pmi2: failed to dlopen()");
		return SLURM_ERROR;
	}
	sym = dlsym(handle, "MPIR_proctable");
	if (sym == NULL) {
		/* if called directly in API, there may be no symbol available */
		verbose ("mpi/pmi2: failed to find symbol 'MPIR_proctable'");
		job_info.MPIR_proctable = NULL;
	} else {
		job_info.MPIR_proctable = *(MPIR_PROCDESC **)sym;
	}
//	debug("******** MNP pid=%d, in _setup_srun_job_info 5", getpid());
	sym = dlsym(handle, "opt");
	if (sym == NULL) {
		verbose("mpi/pmi2: failed to find symbol 'opt'");
		job_info.srun_opt = NULL;
	} else {
		job_info.srun_opt = (opt_t *)sym;
	}
	dlclose(handle);

	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.jobid=%d", getpid(), job_info.jobid);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.orig_jobid=%d", getpid(), job_info.orig_jobid);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.stepid=%d", getpid(), job_info.stepid);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.nnodes=%d", getpid(), job_info.nnodes);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.ntasks=%d", getpid(), job_info.ntasks);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.ltasks=%d (not used in srun)", getpid(), job_info.ltasks);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.step_nodelist=%s", getpid(), job_info.step_nodelist);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.proc_mapping=%s", getpid(), job_info.proc_mapping);
	debug("******** MNP pid=%d, in _setup_srun_job_info, job_info.pmi_jobid=%s", getpid(), job_info.pmi_jobid);
	debug("******** MNP pid=%d, exiting _setup_srun_job_info", getpid());
	return SLURM_SUCCESS;
}

static int
_setup_srun_tree_info(const mpi_plugin_client_info_t *job)
{
	char *p;
	uint16_t p_port;
	char *spool;

	memset(&tree_info, 0, sizeof(tree_info));

	tree_info.this_node = "launcher"; /* not used */
	tree_info.parent_id = -2;   /* not used */
	tree_info.parent_node = NULL; /* not used */
	tree_info.num_children = job_info.nnodes;
	tree_info.depth = 0;	 /* not used */
	tree_info.max_depth = 0; /* not used */
	/* pmi_port set in _setup_srun_sockets */
	p = getenv(PMI2_SPAWNER_PORT_ENV);
	if (p) {		/* spawned */
		p_port = atoi(p);
		tree_info.srun_addr = xmalloc(sizeof(slurm_addr_t));
		/* assume there is always a lo interface */
		slurm_set_addr(tree_info.srun_addr, p_port, "127.0.0.1");
	} else
		tree_info.srun_addr = NULL;

	/* FIXME: We need to handle %n and %h in the spool dir, but don't have
	 * the node name here */
	spool = slurm_get_slurmd_spooldir(NULL);
	snprintf(tree_sock_addr, 128, PMI2_SOCK_ADDR_FMT,
		 spool, job->jobid, job->stepid);
	xfree(spool);

	/* init kvs seq to 0. TODO: reduce array size */
	tree_info.children_kvs_seq = xmalloc(sizeof(uint32_t) *
					     job_info.nnodes);

	return SLURM_SUCCESS;
}

static int
_setup_srun_socket(const mpi_plugin_client_info_t *job)
{
	if (net_stream_listen(&tree_sock,
			      &tree_info.pmi_port) < 0) {
		error("mpi/pmi2: Failed to create tree socket");
		return SLURM_ERROR;
	}
	debug("mpi/pmi2: srun pmi port: %hu", tree_info.pmi_port);
	// MNP PMI start
	if (job_info.jobid == job_info.orig_jobid) {
		//debug("******** MNP pid=%d in pmi2 plugin _setup_srun_socket, THIS IS THE PACK LEADER", getpid());
		//debug("******** MNP pid=%d in pmi2 plugin _setup_srun_socket, tree_info.pmi_port=%d", getpid(), tree_info.pmi_port);
		_write_pmi_port();
	} else {
		//debug("******** MNP pid=%d in pmi2 plugin _setup_srun_socket, THIS IS A PACK MEMBER", getpid());
//		tree_info.pmi_port = 0; // temp disable for testing
//		while (tree_info.pmi_port == 0)
//			tree_info.pmi_port = _read_pmi_port();
		//debug("******** MNP pid=%d in pmi2 plugin _setup_srun_socket, setting tree_info.pmi_port to %d", getpid(), tree_info.pmi_port);
	}
	// MNP PMI end
	return SLURM_SUCCESS;
}

// MNP PMI start
static int
_setup_srun_job_info_combined(const mpi_plugin_client_info_t *job)
{
	char adj_vect_list[100];	// MNP PMI array for testing
	char comb_vect_list[100];	// MNP PMI array for testing
	int curnodeidx = 0;
	int ntasks = 0;
	int nnodes = 0;
	int i;

	if (mpi_step_idx == 0) {
		debug("******** MNP pid=%d in _setup_srun_job_info_combined, THIS IS STEP#0", getpid());
		char readbuffer[80]; 		// MNP PMI
		debug("******** MNP pid=%d, in _setup_srun_job_info_combined, num_steps=%d", getpid(), num_steps);
		curnodeidx=job_info.nnodes;

		/* In srun process for first step, read and combine vector lists, nodelists, task counts & node counts
		   for all steps */
		for (i=1; i < num_steps; i++) {
			// Read vector list from step#i and create combined vector list in step#0
			read(vector_pipe[(i*2)+0], readbuffer, sizeof(readbuffer)); // MNP PMI
//			debug("******** MNP pid=%d, step#%d srun received vector string: %s\n", getpid(), mpi_step_idx, readbuffer);	// MNP PMI
			strcpy(comb_vect_list, job_info.proc_mapping);
			_adjust_vect_nodeidx(readbuffer, adj_vect_list, curnodeidx);
			_combine_vect_lists(comb_vect_list, adj_vect_list);
			job_info.proc_mapping = xstrdup(comb_vect_list);
//			debug("******** MNP pid=%d, in _setup_srun_job_info_combined, combined vectors: job_info.proc_mapping=%s", getpid(), job_info.proc_mapping);

			// Read nodelist from step#i and create combined nodelist in step#0
			read(nodelist_pipe[(i*2)+0], readbuffer, sizeof(readbuffer)); // MNP PMI
//			debug("******** MNP pid=%d, step#0 srun received nodelist string: %s\n", getpid(), readbuffer);	// MNP PMI
			xstrfmtcat(job_info.step_nodelist, ",%s", readbuffer);
			debug("******** MNP pid=%d, in _setup_srun_job_info_combined, combined step_nodelist: %s", getpid(), job_info.step_nodelist);	// MNP PMI

			// Read ntasks from step#i and create combined ntasks in step#0
			read(ntasks_pipe[(i*2)+0], &ntasks, sizeof(ntasks)); // MNP PMI
//			debug("******** MNP pid=%d, step#%d srun received ntasks int: %d\n", getpid(), mpi_step_idx, ntasks);	// MNP PMI
			job_info.ntasks+=ntasks;
//			debug("******** MNP pid=%d, in _setup_srun_job_info_combined, combined job_info.ntasks: %d\n", getpid(), job_info.ntasks);	// MNP PMI

			// Read nnodes from step#i and create combined nnodes in step#0
			read(nnodes_pipe[(i*2)+0], &nnodes, sizeof(nnodes)); // MNP PMI
//			debug("******** MNP pid=%d, step#%d srun received nnodes int: %d\n", getpid(), mpi_step_idx, nnodes);	// MNP PMI
			job_info.nnodes+=nnodes;
			curnodeidx+=nnodes;
//			debug("******** MNP pid=%d, in _setup_srun_job_info_combined, combined job_info.nnodes: %d\n", getpid(), job_info.nnodes);	// MNP PMI

			// Close read end of pipes
			close(vector_pipe[(i*2)+0]);	// MNP PMI
			close(nodelist_pipe[(i*2)+0]);	// MNP PMI
			close(ntasks_pipe[(i*2)+0]);	// MNP PMI
			close(nnodes_pipe[(i*2)+0]);	// MNP PMI
		}
//		debug("******** MNP pid=%d, step#%d srun, finished reading and combining data", getpid(), mpi_step_idx);	// MNP PMI

		/* Write combined data to srun processes for all other steps */
		for (i=1; i < num_steps; i++) {
			// Write combined values into pipe for reading by step#0
			write(vector_pipe[(i*2)+1], job_info.proc_mapping, strlen(job_info.proc_mapping)+1);		// MNP PMI
			write(nodelist_pipe[(i*2)+1], job_info.step_nodelist, strlen(job_info.step_nodelist)+1);	// MNP PMI
			write(ntasks_pipe[(i*2)+1], &job_info.ntasks, sizeof(job_info.ntasks));			// MNP PMI
			write(nnodes_pipe[(i*2)+1], &job_info.nnodes, sizeof(job_info.nnodes));			// MNP PMI

			// Close write end of pipes
			close(vector_pipe[(i*2)+1]);		// MNP PMI
			close(nodelist_pipe[(i*2)+1]);		// MNP PMI
			close(ntasks_pipe[(i*2)+1]);		// MNP PMI
			close(nnodes_pipe[(i*2)+1]);		// MNP PMI
		}
//		debug("******** MNP pid=%d, step#%d srun, finished writing combined data to all step sruns", getpid(), mpi_step_idx);	// MNP PMI

		debug("******** MNP pid=%d, step#%d srun, combined job_info.jobid=%d", getpid(), mpi_step_idx, job_info.jobid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.orig_jobid=%d", getpid(), mpi_step_idx, job_info.orig_jobid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.stepid=%d", getpid(), mpi_step_idx, job_info.stepid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.nnodes=%d", getpid(), mpi_step_idx, job_info.nnodes);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.ntasks=%d", getpid(), mpi_step_idx, job_info.ntasks);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.ltasks=%d (not used in srun)", getpid(), mpi_step_idx, job_info.ltasks);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.step_nodelist=%s", getpid(), mpi_step_idx, job_info.step_nodelist);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.proc_mapping=%s", getpid(), mpi_step_idx, job_info.proc_mapping);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.pmi_jobid=%s", getpid(), mpi_step_idx, job_info.pmi_jobid);
	}
	else {
		char readbuffer[80]; 		// MNP PMI

		debug("******** MNP pid=%d in _setup_srun_job_info_combined, THIS IS STEP#%d", getpid(), mpi_step_idx);
		i = mpi_step_idx;

		// Write this step's values into pipes for reading/combining by step#0
		write(vector_pipe[(i*2)+1], job_info.proc_mapping, strlen(job_info.proc_mapping)+1);		// MNP PMI
		write(nodelist_pipe[(i*2)+1], job_info.step_nodelist, strlen(job_info.step_nodelist)+1);	// MNP PMI
		write(ntasks_pipe[(i*2)+1], &job_info.ntasks, sizeof(job_info.ntasks));				// MNP PMI
		write(nnodes_pipe[(i*2)+1], &job_info.nnodes, sizeof(job_info.nnodes));				// MNP PMI

		// Close write end of pipes
		close(vector_pipe[(i*2)+1]);	// MNP PMI
		close(nodelist_pipe[(i*2)+1]);	// MNP PMI
		close(ntasks_pipe[(i*2)+1]);	// MNP PMI
		close(nnodes_pipe[(i*2)+1]);	// MNP PMI

		// Read combined values from pipes and store into srun job_info struct for this step
		read(vector_pipe[(i*2)+0], readbuffer, sizeof(readbuffer)); // MNP PMI
//		debug("******** MNP pid=%d, step#%d srun received combined vector string: %s", getpid(), mpi_step_idx, readbuffer);	// MNP PMI
		job_info.proc_mapping = xstrdup(readbuffer);
		read(nodelist_pipe[(i*2)+0], readbuffer, sizeof(readbuffer)); // MNP PMI
//		debug("******** MNP pid=%d, step#%d srun received combined nodelist string: %s", getpid(), mpi_step_idx, readbuffer);	// MNP PMI
		job_info.step_nodelist = xstrdup(readbuffer);
		read(ntasks_pipe[(i*2)+0], &job_info.ntasks, sizeof(job_info.ntasks)); // MNP PMI
//		debug("******** MNP pid=%d, step#%d srun received combined ntasks int: %d", getpid(), mpi_step_idx, job_info.ntasks);	// MNP PMI
		read(nnodes_pipe[(i*2)+0], &job_info.nnodes, sizeof(job_info.nnodes)); // MNP PMI
//		debug("******** MNP pid=%d, step#%d srun received combined nnodes int: %d", getpid(), mpi_step_idx, job_info.nnodes);	// MNP PMI

		// Close read end of pipes
		close(vector_pipe[(i*2)+0]);	// MNP PMI
		close(nodelist_pipe[(i*2)+0]);	// MNP PMI
		close(ntasks_pipe[(i*2)+0]);	// MNP PMI
		close(nnodes_pipe[(i*2)+0]);	// MNP PMI


		debug("******** MNP pid=%d, step#%d srun, combined job_info.jobid=%d", getpid(), mpi_step_idx, job_info.jobid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.orig_jobid=%d", getpid(), mpi_step_idx, job_info.orig_jobid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.stepid=%d", getpid(), mpi_step_idx, job_info.stepid);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.nnodes=%d", getpid(), mpi_step_idx, job_info.nnodes);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.ntasks=%d", getpid(), mpi_step_idx, job_info.ntasks);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.ltasks=%d (not used in srun)", getpid(), mpi_step_idx, job_info.ltasks);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.step_nodelist=%s", getpid(), mpi_step_idx, job_info.step_nodelist);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.proc_mapping=%s", getpid(), mpi_step_idx, job_info.proc_mapping);
		debug("******** MNP pid=%d, step#%d srun, combined job_info.pmi_jobid=%s", getpid(), mpi_step_idx, job_info.pmi_jobid);
	}
	return SLURM_SUCCESS;
}
// MNP PMI end

static int
_setup_srun_kvs(const mpi_plugin_client_info_t *job)
{
	int rc;

//	debug("******** MNP pid=%d in pmi2 plugin _setup_srun_kvs, setting kvs_seq=1", getpid());
	kvs_seq = 1;
	rc = temp_kvs_init();
	return rc;
}

static int
_setup_srun_environ(const mpi_plugin_client_info_t *job, char ***env)
{
	/* ifhn will be set in SLURM_SRUN_COMM_HOST by slurmd */
	env_array_overwrite_fmt(env, PMI2_SRUN_PORT_ENV, "%hu",
				tree_info.pmi_port);
	env_array_overwrite_fmt(env, PMI2_STEP_NODES_ENV, "%s",
				job_info.step_nodelist);
	env_array_overwrite_fmt(env, PMI2_PROC_MAPPING_ENV, "%s",
				job_info.proc_mapping);
	return SLURM_SUCCESS;
}

inline static int
_tasks_launched (void)
{
	int i, all_launched = 1;
	if (job_info.MPIR_proctable == NULL)
		return 1;

	for (i = 0; i < job_info.ntasks; i ++) {
		if (job_info.MPIR_proctable[i].pid == 0) {
			all_launched = 0;
			break;
		}
	}
	return all_launched;
}

static void *
_task_launch_detection(void *unused)
{
	spawn_resp_t *resp;
	time_t start;
	int rc = 0;

	/*
	 * mpir_init() is called in plugins/launch/slurm/launch_slurm.c before
	 * mpi_hook_client_prelaunch() is called in api/step_launch.c
	 */
	start = time(NULL);
	while (_tasks_launched() == 0) {
		usleep(1000*50);
		if (time(NULL) - start > 600) {
			rc = 1;
			break;
		}
	}

	/* send a resp to spawner srun */
	resp = spawn_resp_new();
	resp->seq = job_info.spawn_seq;
	resp->jobid = xstrdup(job_info.pmi_jobid);
	resp->error_cnt = 0;	/* TODO */
	resp->rc = rc;
	resp->pmi_port = tree_info.pmi_port;

	spawn_resp_send_to_srun(resp);
	spawn_resp_free(resp);
	return NULL;
}

static int
_setup_srun_task_launch_detection(void)
{
	int retries = 0;
	pthread_t tid;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	while ((errno = pthread_create(&tid, &attr,
				       &_task_launch_detection, NULL))) {
		if (++retries > 5) {
			error ("mpi/pmi2: pthread_create error %m");
			slurm_attr_destroy(&attr);
			return SLURM_ERROR;
		}
		sleep(1);
	}
	slurm_attr_destroy(&attr);
	debug("mpi/pmi2: task launch detection thread (%lu) started",
	      (unsigned long) tid);

	return SLURM_SUCCESS;
}

extern int
pmi2_setup_srun(const mpi_plugin_client_info_t *job, char ***env)
{
	int rc;

	debug("******** MNP pid=%d, entering pmi2_setup_srun", getpid());
	run_in_stepd = false;

	rc = _setup_srun_job_info(job);
	if (rc != SLURM_SUCCESS)
		return rc;

	// MNP PMI start
	rc = _setup_srun_job_info_combined(job);
	if (rc != SLURM_SUCCESS)
		return rc;
	// MNP PMI end

	rc = _setup_srun_tree_info(job);
	if (rc != SLURM_SUCCESS)
		return rc;

	rc = _setup_srun_socket(job);
	if (rc != SLURM_SUCCESS)
		return rc;

	rc = _setup_srun_kvs(job);
	if (rc != SLURM_SUCCESS)
		return rc;

	rc = _setup_srun_environ(job, env);
	if (rc != SLURM_SUCCESS)
		return rc;

	if (job_info.spawn_seq) {
		rc = _setup_srun_task_launch_detection();
		if (rc != SLURM_SUCCESS)
			return rc;
	}

	debug("******** MNP pid=%d, exiting pmi2_setup_srun", getpid());
	return SLURM_SUCCESS;
}
