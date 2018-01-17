# Slurm Workload Manager: Device Supervisor

Slurm Device Supervisor is a component of the European Project TANGO (http://tango-project.eu ).

Slurm is distributed under a [Apache License, version 2.0](http://www.apache.org/licenses/LICENSE-2.0).
Slurm is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version.


## Description

Slurm is an open-source cluster resource management and job scheduling system
that strives to be simple, scalable, portable, fault-tolerant, and
interconnect agnostic. Slurm currently has been tested only under Linux.

As a cluster resource manager, Slurm provides three key functions. First,
it allocates exclusive and/or non-exclusive access to resources
(compute nodes) to users for some duration of time so they can perform
work. Second, it provides a framework for starting, executing, and
monitoring work (normally a parallel job) on the set of allocated
nodes. Finally, it arbitrates conflicting requests for resources by
managing a queue of pending work.

This version of Slurm also include the JobPack feature.
The JobPack feature is the main development for TANGO project inside Slurm.
This feature aims to allocate a job on the heterogeneous devices of a single cluster.


SOURCE DISTRIBUTION HIERARCHY
-----------------------------

The top-level distribution directory contains this README as well as
other high-level documentation files, and the scripts used to configure
and build Slurm (see INSTALL). Subdirectories contain the source-code
for Slurm as well as a DejaGNU test suite and further documentation. A
quick description of the subdirectories of the Slurm distribution follows:

  src/        [ Slurm source ]
     Slurm source code is further organized into self explanatory
     subdirectories such as src/api, src/slurmctld, etc.

  doc/        [ Slurm documentation ]
     The documentation directory contains some latex, html, and ascii
     text papers, READMEs, and guides. Manual pages for the Slurm
     commands and configuration files are also under the doc/ directory.

  etc/        [ Slurm configuration ]
     The etc/ directory contains a sample config file, as well as
     some scripts useful for running Slurm.

  slurm/      [ Slurm include files ]
     This directory contains installed include files, such as slurm.h
     and slurm_errno.h, needed for compiling against the Slurm API.

  testsuite/  [ Slurm test suite ]
     The testsuite directory contains the framework for a set of
     DejaGNU and "make check" type tests for Slurm components.
     There is also an extensive collection of Expect scripts.

  auxdir/     [ autotools directory ]
     Directory for autotools scripts and files used to configure and
     build Slurm

  contribs/   [ helpful tools outside of Slurm proper ]
     Directory for anything that is outside of slurm proper such as a
     different api or such.  To have this build you need to do a
     make contrib/install-contrib.

COMPILING AND INSTALLING THE DISTRIBUTION
-----------------------------------------

1. Make sure the clocks, users and groups (UIDs and GIDs) are synchronized across the cluster.
2. Install MUNGE for authentication. Make sure that all nodes in your cluster have the same munge.key. Make sure the MUNGE daemon, munged is started before you start the Slurm daemons.
3. cd to the directory containing the Slurm source and type ./configure with appropriate options, typically --prefix= and --sysconfdir=
4. Type make to compile Slurm.
5. Type make install to install the programs, documentation, libraries, header files, etc.
6.Build a configuration file using your favorite web browser and doc/html/configurator.html.
NOTE: The SlurmUser must exist prior to starting Slurm and must exist on all nodes of the cluster.
NOTE: The parent directories for Slurm's log files, process ID files, state save directories, etc. are not created by Slurm. They must be created and made writable by SlurmUser as needed prior to starting Slurm daemons.
NOTE: If any parent directories are created during the installation process (for the executable files, libraries, etc.), those directories will have access rights equal to read/write/execute for everyone minus the umask value (e.g. umask=0022 generates directories with permissions of "drwxr-r-x" and mask=0000 generates directories with permissions of "drwxrwrwx" which is a security problem).
7. Type ldconfig -n <library_location> so that the Slurm libraries can be found by applications that intend to use Slurm APIs directly.
8. Install the configuration file in <sysconfdir>/slurm.conf.
NOTE: You will need to install this configuration file on all nodes of the cluster.
9. systemd (optional): enable the appropriate services on each system:
* Controller: systemctl enable slurmctld
* Database: systemctl enable slurmdbd
* Compute Nodes: systemctl enable slurmd
10. Start the slurmctld and slurmd daemons.

USING SLURM WITH THE JOBPACK FEATURE
------------------------------------

The Jobpack feature aims to allocate the job on different set of resources available through Slurm.
No specific configuration is needed.
To run a job on heterogeneous resources, you must first know how to run a simple job. See man pages of srun, salloc and sbatch.
Then, you can run your job by asking several allocations split by ':'.
* example: $ salloc -p ctlnodes –N1 : -p computenodes –N10 : -p ionodes –N4
Inside your allocation, each set of resources is visible as pack on wich you can run any application.
* srun --pack-group=0 ./controller : --pack-group=1 –n 20 ./worker : --pack-group=2 –n 6 ./storer
Slurm with Jobpack support a tigh integration of srun with MPI MPMD
* single MPI_COMM_WORLD communicator with a single set of MPI ranks on the whole allocation
* support of PMI-1 and PMI-2
* new srun option --mpi-combine=no to run each step as a a separate MPI allocation

LEGAL
-----

Slurm is provided "as is" and with no warranty. This software is
distributed under the GNU General Public License, please see the files
COPYING, DISCLAIMER, and LICENSE.OpenSSL for details.
