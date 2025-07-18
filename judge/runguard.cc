/*
   runguard -- run command with restrictions.

   Part of the DOMjudge Programming Contest Jury System and licensed
   under the GNU GPL. See README and COPYING for details.

   Multiple minor improvements ported from the DOMjudge-ETH tree.

   Based on an idea from the timeout program, written by Wietse Venema
   as part of The Coroner's Toolkit.


   Program specifications:

   This program will run the specified command in a separate process
   group (session) and apply the restrictions as specified after
   forking, before executing the command.

   The stdin and stdout streams are passed to the command and runguard
   does not read or write to these. Error and verbose messages from
   runguard are by default written to stderr, hence mixed with stderr
   output of the command, unless that is optionally redirected to file.

   The command and its children are sent a SIGTERM after the runtime
   has passed, followed by a SIGKILL after 'killdelay'. The program is
   considered to have finished when the main program thread exits. At
   that time any children still running are killed.
 */

#include "config.h"

/* Some system/site specific config: VALID_USERS, CHROOT_PREFIX */
#include "runguard-config.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <cerrno>
#include <fcntl.h>
#include <csignal>
#include <cstdlib>
#include <mntent.h>
#include <unistd.h>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <getopt.h>
#include <fnmatch.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>
#include <ctime>
#include <cmath>
#include <climits>
#include <cinttypes>
#include <libcgroup.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <vector>
#include <string>

#define PROGRAM "runguard"
#define VERSION DOMJUDGE_VERSION "/" REVISION

#define max(x,y) ((x) > (y) ? (x) : (y))
#define min(x,y) ((x) < (y) ? (x) : (y))

/* Array indices for input/output file descriptors as used by pipe() */
#define PIPE_IN  1
#define PIPE_OUT 0

#define BUF_SIZE 4*1024

/* Types of time for writing to file. */
#define WALL_TIME_TYPE 0
#define CPU_TIME_TYPE  1

/* Strings to write to file when exceeding no/soft/hard/both limits. */
const char output_timelimit_str[4][16] = {
	"",
	"soft-timelimit",
	"hard-timelimit",
	"hard-timelimit"
};
/* Bitmask of soft/hard timelimit (used in array above and
 * {wall,cpu}timelimit_reached variabled below). */
const int soft_timelimit = 1;
const int hard_timelimit = 2;

const struct timespec killdelay = { 0, 100000000L }; /* 0.1 seconds */
const struct timespec cg_delete_delay = { 0, 10000000L }; /* 0.01 seconds */

extern int errno;

const int exit_failure = -1;

char  *progname;
char  *cmdname;
char **cmdargs;
char  *rootdir;
char  *rootchdir;
char  *stdoutfilename;
char  *stderrfilename;
char  *metafilename;
std::vector<std::string> environment_variables;
FILE  *metafile;

char  cgroupname[255];
const char *cpuset;

/* Linux Out-Of-Memory adjustment for current process. */
#define OOM_PATH_NEW "/proc/self/oom_score_adj"
#define OOM_PATH_OLD "/proc/self/oom_adj"
#define OOM_RESET_VALUE 0

char *runuser;
char *rungroup;
int runuid;
int rungid;
int use_root;
int use_walltime;
int use_cputime;
int use_user;
int use_group;
int redir_stdout;
int redir_stderr;
int limit_streamsize;
int outputmeta;
int outputtimetype;
int no_coredump;
int preserve_environment;
int be_verbose;
int be_quiet;
int show_help;
int show_version;
int in_error_handling = 0;
pid_t runpipe_pid = -1;

bool is_cgroup_v2 = false;

double walltimelimit[2], cputimelimit[2]; /* in seconds, soft and hard limits */
int walllimit_reached, cpulimit_reached; /* 1=soft, 2=hard, 3=both limits reached */
rlim_t memsize;
rlim_t filesize;
rlim_t nproc;
size_t streamsize;
int use_splice;

pid_t child_pid = -1;

static volatile sig_atomic_t received_SIGCHLD = 0;
static volatile sig_atomic_t received_signal = -1;
static volatile sig_atomic_t error_in_signalhandler = 0;

int child_pipefd[3][2];
int child_redirfd[3];

struct timeval progstarttime, starttime, endtime;
struct tms startticks, endticks;

struct option const long_opts[] = {
	{"root",       required_argument, nullptr,         'r'},
	{"user",       required_argument, nullptr,         'u'},
	{"group",      required_argument, nullptr,         'g'},
	{"chdir",      required_argument, nullptr,         'd'},
	{"walltime",   required_argument, nullptr,         't'},
	{"cputime",    required_argument, nullptr,         'C'},
	{"memsize",    required_argument, nullptr,         'm'},
	{"filesize",   required_argument, nullptr,         'f'},
	{"nproc",      required_argument, nullptr,         'p'},
	{"cpuset",     required_argument, nullptr,         'P'},
	{"no-core",    no_argument,       nullptr,         'c'},
	{"stdout",     required_argument, nullptr,         'o'},
	{"stderr",     required_argument, nullptr,         'e'},
	{"streamsize", required_argument, nullptr,         's'},
	{"environment",no_argument,       nullptr,         'E'},
	{"variable",   required_argument, nullptr,         'V'},
	{"outmeta",    required_argument, nullptr,         'M'},
	{"runpipepid", required_argument, nullptr,         'U'},
	{"verbose",    no_argument,       nullptr,         'v'},
	{"quiet",      no_argument,       nullptr,         'q'},
	{"help",       no_argument,       &show_help,       1 },
	{"version",    no_argument,       &show_version,    1 },
	{ nullptr,     0,                 nullptr,          0 }
};

void warning(   const char *, ...) __attribute__((format (printf, 1, 2)));
void verbose(   const char *, ...) __attribute__((format (printf, 1, 2)));
void error(int, const char *, ...) __attribute__((format (printf, 2, 3)));
void write_meta(const char *, const char *, ...) __attribute__((format (printf, 2, 3)));

void warning(const char *format, ...)
{
	va_list ap;
	va_start(ap,format);

	if ( ! be_quiet ) {
		fprintf(stderr,"%s: warning: ",progname);
		vfprintf(stderr,format,ap);
		fprintf(stderr,"\n");
	}

	va_end(ap);
}

void verbose(const char *format, ...)
{
	va_list ap;
	va_start(ap,format);

	if ( ! be_quiet && be_verbose ) {
        struct timeval currtime{};
		gettimeofday(&currtime,nullptr);
		double runtime = (currtime.tv_sec  - progstarttime.tv_sec ) +
		                 (currtime.tv_usec - progstarttime.tv_usec)*1E-6;
		fprintf(stderr,"%s [%d @ %10.6lf]: verbose: ",progname,getpid(),runtime);
		vfprintf(stderr,format,ap);
		fprintf(stderr,"\n");
	}

	va_end(ap);
}

// These functions are called from signal handlers, so they
// must only call async-signal-safe functions.
// write() is async-signal-safe, printf and variants are not.
void verbose_from_signalhandler(const char* msg)
{
	if (!be_quiet && be_verbose) {
		write(STDERR_FILENO, msg, strlen(msg));
	}
}

void warning_from_signalhandler(const char* msg)
{
	if (!be_quiet) {
		// Do not include timing here, as it wouldn't be safe from a signalhandler.
		// TODO: Consider rewriting using clock_gettime in the future.
		write(STDERR_FILENO, msg, strlen(msg));
	}
}

void error(int errnum, const char *format, ...)
{
	// Silently ignore errors that happen while handling other errors.
	if (in_error_handling) return;
	in_error_handling = 1;

	va_list ap;
	va_start(ap,format);

	/*
	 * Make sure the signal handler for these (terminate()) does not
	 * interfere, we are exiting now anyway.
	 */
	sigset_t sigs;
	sigaddset(&sigs, SIGALRM);
	sigaddset(&sigs, SIGTERM);
	sigprocmask(SIG_BLOCK, &sigs, nullptr);

	/* First print to string to be able to reuse the message. */
	size_t errlen = strlen(progname)+255;
	if ( format!=nullptr ) errlen += strlen(format);

	char *errstr = (char *)malloc(errlen);
	if ( errstr==nullptr ) abort();

	sprintf(errstr,"%s",progname);
	size_t errpos = strlen(errstr);

	if ( format!=nullptr ) {
		snprintf(errstr+errpos,errlen-errpos,": ");
		errpos += 2;
		vsnprintf(errstr+errpos,errlen-errpos,format,ap);
		errpos += strlen(errstr+errpos);
	}
	if ( errnum!=0 ) {
		/* Special case libcgroup error codes. */
		if ( errnum==ECGOTHER ) {
			snprintf(errstr+errpos,errlen-errpos,": libcgroup");
			errpos += strlen(errstr+errpos);
			errnum = errno;
		}
		if ( errnum>=ECGROUPNOTCOMPILED && errnum<=ECGROUPNOTCOMPILED ) {
			snprintf(errstr+errpos,errlen-errpos,": %s",cgroup_strerror(errnum));
		} else {
			snprintf(errstr+errpos,errlen-errpos,": %s",strerror(errnum));
		}
		errpos += strlen(errstr+errpos);
	}
	if ( format==nullptr && errnum==0 ) {
		snprintf(errstr+errpos,errlen-errpos,": unknown error");
	}

	fprintf(stderr,"%s\nTry `%s --help' for more information.\n",errstr,progname);
	va_end(ap);

	write_meta("internal-error","%s",errstr);
	if ( outputmeta && metafile != nullptr && fclose(metafile)!=0 ) {
		fprintf(stderr,"\nError writing to metafile '%s'.\n",metafilename);
	}

	/* Make sure that all children are killed before terminating */
	if ( child_pid > 0) {
		verbose("sending SIGKILL");
		if ( kill(-child_pid,SIGKILL)!=0 && errno!=ESRCH ) {
			fprintf(stderr,"unable to send SIGKILL to children while terminating "
					"due to previous error: %s\n", strerror(errno));
			/*
			 * continue, there is not much we can do here.
			 * In the worst case, this will trigger an error
			 * in testcase_run.sh, as the runuser may still be
			 * running processes
			 */
		}

		/* Wait a while to make sure the process is killed by now. */
		nanosleep(&killdelay,nullptr);
	}

	exit(exit_failure);
}

void write_meta(const char *key, const char *format, ...)
{
	if ( !outputmeta ) return;

	va_list ap;
	va_start(ap,format);

	if ( fprintf(metafile,"%s: ",key)<=0 ) {
		outputmeta = 0;
		error(0,"cannot write to file `%s'",metafilename);
	}
	if ( vfprintf(metafile,format,ap)<0 ) {
		outputmeta = 0;
		error(0,"cannot write to file `%s'(vfprintf)",metafilename);
	}
	if ( fprintf(metafile,"\n")<=0 ) {
		outputmeta = 0;
		error(0,"cannot write to file `%s'",metafilename);
	}

	va_end(ap);
}

void version(const char *prog, const char *vers)
{
	printf("\
%s -- part of DOMjudge version %s\n\
Written by the DOMjudge developers\n\n\
DOMjudge comes with ABSOLUTELY NO WARRANTY.  This is free software, and you\n\
are welcome to redistribute it under certain conditions.  See the GNU\n\
General Public Licence for details.\n", prog, vers);
	exit(0);
}

void usage()
{
	printf("\
Usage: %s [OPTION]... COMMAND...\n\
Run COMMAND with restrictions.\n\
\n", progname);
	printf("\
  -r, --root=ROOT        run COMMAND with root directory set to ROOT\n\
  -u, --user=USER        run COMMAND as user with username or ID USER\n\
  -g, --group=GROUP      run COMMAND under group with name or ID GROUP\n\
  -d, --chdir=DIR        change to directory DIR after setting root directory\n\
  -t, --walltime=TIME    kill COMMAND after TIME wallclock seconds\n\
  -C, --cputime=TIME     set maximum CPU time to TIME seconds\n\
  -m, --memsize=SIZE     set total memory limit to SIZE kB\n\
  -f, --filesize=SIZE    set maximum created filesize to SIZE kB;\n");
	printf("\
  -p, --nproc=N          set maximum no. processes to N\n\
  -P, --cpuset=ID        use only processor number ID (or set, e.g. \"0,2-3\")\n\
  -c, --no-core          disable core dumps\n\
  -o, --stdout=FILE      redirect COMMAND stdout output to FILE\n\
  -e, --stderr=FILE      redirect COMMAND stderr output to FILE\n\
  -s, --streamsize=SIZE  truncate COMMAND stdout/stderr streams at SIZE kB\n\
  -E, --environment      preserve environment variables (default only PATH)\n\
  -V, --variable         add additional environment variables\n\
                           (in form KEY=VALUE;KEY2=VALUE2); may be passed\n\
                           multiple times\n\
  -M, --outmeta=FILE     write metadata (runtime, exitcode, etc.) to FILE\n\
  -U, --runpipepid=PID   process ID of runpipe to send SIGUSR1 signal when\n\
                           timelimit is reached\n");
	printf("\
  -v, --verbose          display some extra warnings and information\n\
  -q, --quiet            suppress all warnings and verbose output\n\
      --help             display this help and exit\n\
      --version          output version information and exit\n");
	printf("\n\
Note that root privileges are needed for the `root' and `user' options.\n\
If `user' is set, then `group' defaults to the same to prevent security\n\
issues, since otherwise the process would retain group root permissions.\n\
The COMMAND path is relative to the changed ROOT directory if specified.\n\
TIME may be specified as a float; two floats separated by `:' are treated\n\
as soft and hard limits. The runtime written to file is that of the last\n\
of wall/cpu time options set, and defaults to CPU time when neither is set.\n\
When run setuid without the `user' option, the user ID is set to the\n\
real user ID.\n");
	exit(0);
}

void output_exit_time(int exitcode, double cpudiff)
{
	verbose("command exited with exitcode %d",exitcode);
	write_meta("exitcode","%d",exitcode);

	if (received_signal != -1) {
		write_meta("signal", "%d", received_signal);
	}

	double walldiff = (endtime.tv_sec  - starttime.tv_sec ) +
	                  (endtime.tv_usec - starttime.tv_usec)*1E-6;

	unsigned long ticks_per_second = sysconf(_SC_CLK_TCK);
	double userdiff = (double)(endticks.tms_cutime - startticks.tms_cutime) / ticks_per_second;
	double sysdiff  = (double)(endticks.tms_cstime - startticks.tms_cstime) / ticks_per_second;

	write_meta("wall-time","%.3f", walldiff);
	write_meta("user-time","%.3f", userdiff);
	write_meta("sys-time", "%.3f", sysdiff);
	write_meta("cpu-time", "%.3f", cpudiff);

	verbose("runtime is %.3f seconds real, %.3f user, %.3f sys",
	        walldiff, userdiff, sysdiff);

	if ( use_walltime && walldiff > walltimelimit[0] ) {
		walllimit_reached |= soft_timelimit;
		warning("timelimit exceeded (soft wall time)");
	}

	if ( use_cputime && cpudiff > cputimelimit[0] ) {
		cpulimit_reached |= soft_timelimit;
		warning("timelimit exceeded (soft cpu time)");
	}

	int timelimit_reached = 0;
	switch ( outputtimetype ) {
	case WALL_TIME_TYPE:
		write_meta("time-used","wall-time");
		timelimit_reached = walllimit_reached;
		break;
	case CPU_TIME_TYPE:
		write_meta("time-used","cpu-time");
		timelimit_reached = cpulimit_reached;
		break;
	default:
		error(0,"cannot write unknown time type `%d' to file",outputtimetype);
	}

	/* Hard limitlimit reached always has precedence. */
	if ( (walllimit_reached | cpulimit_reached) & hard_timelimit ) {
		timelimit_reached |= hard_timelimit;
	}

	write_meta("time-result","%s",output_timelimit_str[timelimit_reached]);
}

void check_remaining_procs()
{
	char path[1024];
	if (is_cgroup_v2) {
		snprintf(path, 1023, "/sys/fs/cgroup/%s/cgroup.procs", cgroupname);
	} else {
		snprintf(path, 1023, "/sys/fs/cgroup/cpuacct/%s/cgroup.procs", cgroupname);
	}

	FILE *file = fopen(path, "r");
	if (file == nullptr) {
		error(errno, "opening cgroups file `%s'", path);
	}

	fseek(file, 0L, SEEK_END);
	if (ftell(file) > 0) {
		error(0, "found left-over processes in cgroup controller, please check!");
	}
	if (fclose(file) != 0) error(errno, "closing file `%s'", path);
}

void output_cgroup_stats_v1(double *cputime)
{
	struct cgroup *cg;
	if ( (cg = cgroup_new_cgroup(cgroupname))==nullptr ) error(0,"cgroup_new_cgroup");

	int ret;
	if ((ret = cgroup_get_cgroup(cg)) != 0) error(ret,"get cgroup information");

	int64_t max_usage;
	struct cgroup_controller *cg_controller;
	cg_controller = cgroup_get_controller(cg, "memory");
	ret = cgroup_get_value_int64(cg_controller, "memory.memsw.max_usage_in_bytes", &max_usage);
	if ( ret!=0 ) error(ret,"get cgroup value memory.memsw.max_usage_in_bytes");

	verbose("total memory used: %" PRId64 " kB", max_usage/1024);
	write_meta("memory-bytes","%" PRId64, max_usage);

	int64_t cpu_time_int;
	cg_controller = cgroup_get_controller(cg, "cpuacct");
	ret = cgroup_get_value_int64(cg_controller, "cpuacct.usage", &cpu_time_int);
	if ( ret!=0 ) error(ret,"get cgroup value cpuacct.usage");

	*cputime = (double) cpu_time_int / 1.e9;

	cgroup_free(&cg);
}

void output_cgroup_stats_v2(double *cputime)
{
	struct cgroup *cg;
	if ( (cg = cgroup_new_cgroup(cgroupname))==NULL ) error(0,"cgroup_new_cgroup");

	int ret;
	if ((ret = cgroup_get_cgroup(cg)) != 0) error(ret,"get cgroup information");

	struct cgroup_controller *cg_controller = cgroup_get_controller(cg, "memory");
	int64_t max_usage = 0;
	ret = cgroup_get_value_int64(cg_controller, "memory.peak", &max_usage);
	if ( ret == ECGROUPVALUENOTEXIST ) {
		error(ret, "kernel too old and does not support memory.peak");
	} else if ( ret!=0 ) {
		error(ret,"get cgroup value memory.peak");
	}

	// There is no need to check swap usage, as we limit it to 0.
	verbose("total memory used: %" PRId64 " kB", max_usage/1024);
	write_meta("memory-bytes","%" PRId64, max_usage);

	struct cgroup_stat stat;
	void *handle;
	ret = cgroup_read_stats_begin("cpu", cgroupname, &handle, &stat);
	while (ret == 0) {
		verbose("cpu.stat: %s = %s", stat.name, stat.value);
		if (strcmp(stat.name, "usage_usec") == 0) {
			long long usec = strtoll(stat.value, NULL, 10);
			*cputime = usec / 1e6;
		}
		ret = cgroup_read_stats_next(&handle, &stat);
	}
	if ( ret!=ECGEOF ) error(ret,"get cgroup value cpu.stat");
	cgroup_read_stats_end(&handle);

	cgroup_free(&cg);

}

/* Temporary shorthand define for error handling. */
#define cgroup_add_value(type,name,value) \
	ret = cgroup_add_value_ ## type(cg_controller, name, value); \
	if ( ret!=0 ) error(ret,"set cgroup value " #name);

void cgroup_create()
{
	struct cgroup *cg;
	cg = cgroup_new_cgroup(cgroupname);
	if (!cg) error(0,"cgroup_new_cgroup");

	/* Set up the memory restrictions; these two options limit ram use
	   and ram+swap use. They are the same so no swapping can occur */
	struct cgroup_controller *cg_controller;
	if ( (cg_controller = cgroup_add_controller(cg, "memory"))==nullptr ) {
		error(0,"cgroup_add_controller memory");
	}

	int ret;
	if (is_cgroup_v2) {
		// TODO: do we want to set cpu.weight here as well?
		if (memsize != RLIM_INFINITY) {
			cgroup_add_value(uint64, "memory.max", memsize);
			cgroup_add_value(uint64, "memory.swap.max", 0);
		} else {
			cgroup_add_value(string, "memory.max", "max");
			cgroup_add_value(string, "memory.swap.max", "max");
		}
	} else {
		cgroup_add_value(uint64, "memory.limit_in_bytes", memsize);
		cgroup_add_value(uint64, "memory.memsw.limit_in_bytes", memsize);
	}

	/* Set up cpu restrictions; we pin the task to a specific set of
	   cpus. We also give it exclusive access to those cores, and set
	   no limits on memory nodes */
	if ( cpuset!=nullptr && strlen(cpuset)>0 ) {
		if ( (cg_controller = cgroup_add_controller(cg, "cpuset"))==nullptr ) {
			error(0,"cgroup_add_controller cpuset");
		}
		/* To make a cpuset exclusive, some additional setup outside of domjudge is
		   required, so for now, we will leave this commented out. */
		/* cgroup_add_value_int64(cg_controller, "cpuset.cpu_exclusive", 1); */
		cgroup_add_value(string, "cpuset.mems", "0");
		cgroup_add_value(string, "cpuset.cpus", cpuset);
	} else {
		verbose("cpuset undefined");
	}

	if (!is_cgroup_v2) {
		if ( (cg_controller = cgroup_add_controller(cg, "cpu"))==nullptr ) {
			error(0,"cgroup_add_controller cpu");
		}
		if ((cg_controller = cgroup_add_controller(cg, "cpuacct")) == nullptr) {
			error(0, "cgroup_add_controller cpuacct");
		}
	}

	/* Perform the actual creation of the cgroup */
	if ( (ret = cgroup_create_cgroup(cg, 1))!=0 ) error(ret,"creating cgroup");

	cgroup_free(&cg);
	verbose("created cgroup '%s'",cgroupname);
}

#undef cgroup_setval

void cgroup_attach()
{
	struct cgroup *cg;
	cg = cgroup_new_cgroup(cgroupname);
	if (!cg) error(0,"cgroup_new_cgroup");

	int ret;
	if ( (ret = cgroup_get_cgroup(cg))!=0 ) error(ret,"get cgroup information");

	/* Attach task to the cgroup */
	if ( (ret = cgroup_attach_task(cg))!=0 ) error(ret,"attach task to cgroup");

	cgroup_free(&cg);
}

void cgroup_kill()
{
	/* kill any remaining tasks, and wait for them to be gone */
	char mem_controller[10] = "memory";
	if (is_cgroup_v2) {
		int size;
		do {
			pid_t* pids;
			int ret = cgroup_get_procs(cgroupname, mem_controller, &pids, &size);
			if (ret != 0) error(ret, "cgroup_get_procs");
			for(int i = 0; i < size; i++) {
				kill(pids[i], SIGKILL);
			}
			free(pids);
		} while (size > 0);
	} else {
		while(1) {
			void *handle = nullptr;
			pid_t pid;
			int ret = cgroup_get_task_begin(cgroupname, mem_controller, &handle, &pid);
			cgroup_get_task_end(&handle);
			if (ret == ECGEOF) break;
			kill(pid, SIGKILL);
		}
	}
}

void cgroup_delete()
{
	struct cgroup *cg;
	cg = cgroup_new_cgroup(cgroupname);
	if (!cg) error(0,"cgroup_new_cgroup");

	if (cgroup_add_controller(cg, "cpu") == nullptr) error(0, "cgroup_add_controller cpu");
	if (!is_cgroup_v2) {
		if (cgroup_add_controller(cg, "cpuacct") == nullptr) error(0, "cgroup_add_controller cpuacct");
	}
	if ( cgroup_add_controller(cg, "memory")==nullptr ) error(0,"cgroup_add_controller memory");

	if ( cpuset!=nullptr && strlen(cpuset)>0 ) {
		if ( cgroup_add_controller(cg, "cpuset")==nullptr ) error(0,"cgroup_add_controller cpuset");
	}
	/* Clean up our cgroup */
	nanosleep(&cg_delete_delay,nullptr);
	int ret = cgroup_delete_cgroup_ext(cg, CGFLAG_DELETE_IGNORE_MIGRATION | CGFLAG_DELETE_RECURSIVE);
	// TODO: is this actually benign to ignore ECGOTHER here?
	if ( ret!=0 && ret!=ECGOTHER ) error(ret,"deleting cgroup");

	cgroup_free(&cg);

	verbose("deleted cgroup '%s'",cgroupname);
}

void terminate(int sig)
{
	struct sigaction sigact;

	/* Reset signal handlers to default */
	sigact.sa_handler = SIG_DFL;
	sigact.sa_flags = 0;
	if ( sigemptyset(&sigact.sa_mask)!=0 ) {
		warning_from_signalhandler("could not initialize signal mask");
	}
	if ( sigaction(SIGTERM,&sigact,nullptr)!=0 ) {
		warning_from_signalhandler("could not restore signal handler");
	}
	if ( sigaction(SIGALRM,&sigact,nullptr)!=0 ) {
		warning_from_signalhandler("could not restore signal handler");
	}

	if ( sig==SIGALRM ) {
		if (runpipe_pid > 0) {
			warning_from_signalhandler("sending SIGUSR1 to runpipe");
			kill(runpipe_pid, SIGUSR1);
		}

		walllimit_reached |= hard_timelimit;
		warning_from_signalhandler("timelimit exceeded (hard wall time): aborting command");
	} else {
		warning_from_signalhandler("received signal: aborting command");
	}

	received_signal = sig;

	/* First try to kill graciously, then hard.
	   Don't report an already exited process as error. */
	verbose_from_signalhandler("sending SIGTERM");
	if ( kill(-child_pid,SIGTERM)!=0 && errno!=ESRCH ) {
		warning_from_signalhandler("error sending SIGTERM to command");
		error_in_signalhandler = 1;
		return;
	}

	/* Prefer nanosleep over sleep because of higher resolution and
	   it does not interfere with signals. */
	nanosleep(&killdelay,nullptr);

	verbose_from_signalhandler("sending SIGKILL");
	if ( kill(-child_pid,SIGKILL)!=0 && errno!=ESRCH ) {
		warning_from_signalhandler("error sending SIGKILL to command");
		error_in_signalhandler = 1;
		return;
	}

	/* Wait another while to make sure the process is killed by now. */
	nanosleep(&killdelay,nullptr);
}

static void child_handler(int sig)
{
	received_SIGCHLD = 1;
}

int userid(char *name)
{
	errno = 0; /* per the linux GETPWNAM(3) man-page */

	struct passwd *pwd;
	pwd = getpwnam(name);

	if ( pwd==nullptr || errno ) return -1;

	return (int) pwd->pw_uid;
}

int groupid(char *name)
{
	struct group *grp;

	errno = 0; /* per the linux GETGRNAM(3) man-page */
	grp = getgrnam(name);

	if ( grp==nullptr || errno ) return -1;

	return (int) grp->gr_gid;
}

long read_optarg_int(const char *desc, long minval, long maxval)
{
	char *ptr;

	errno = 0;
	long arg = strtol(optarg,&ptr,10);
	if ( errno || *ptr!='\0' || arg<minval || arg>maxval ) {
		error(errno,"invalid %s specified: `%s'",desc,optarg);
	}

	return arg;
}

void read_optarg_time(const char *desc, double *times)
{
	char *optcopy;
	if ( (optcopy=strdup(optarg))==nullptr ) error(0,"strdup() failed");

	/* Check for soft:hard limit separator and cut string. */
	char *sep;
	if ( (sep=strchr(optcopy,':'))!=nullptr ) *sep = 0;

	char *ptr;
	errno = 0;
	times[0] = strtod(optcopy,&ptr);
	if ( errno || *ptr!='\0' || !finite(times[0]) || times[0]<=0 ) {
		error(errno,"invalid %s specified: `%s'",desc,optarg);
	}

	/* And repeat for hard limit if we found the ':' separator. */
	if ( sep!=nullptr ) {
		errno = 0;
		times[1] = strtod(sep+1,&ptr);
		if ( errno || *(sep+1)=='\0' || *ptr!='\0' || !finite(times[1]) || times[1]<=0 ) {
			error(errno,"invalid %s specified: `%s'",desc,optarg);
		}
		if ( times[1]<times[0] ) {
			error(0,"invalid %s specified: hard limit is lower than soft limit",desc);
		}
	} else {
		/* Set soft and hard limits equal. */
		times[1] = times[0];
	}

	free(optcopy);
}

void setrestrictions()
{
	/* Clear environment to prevent all kinds of security holes, save PATH */
	if ( !preserve_environment ) {
		char *path;
		path = getenv("PATH");
		environ[0] = nullptr;
		/* FIXME: Clean path before setting it again? */
		if ( path!=nullptr ) setenv("PATH",path,1);
	}

	/* Set additional environment variables. */
	for (const auto &tokens : environment_variables) {
		// Note that we explicitly *do not* free the string created by
		// strdup as putenv does not copy that string, but uses it as is.
		char *token = strtok(strdup(tokens.c_str()), ";");
		while (token != nullptr) {
			verbose("setting environment variable: %s", token);
			putenv(token);
			token = strtok(nullptr, ";");
		}
	}

	/* Set resource limits: must be root to raise hard limits.
	   Note that limits can thus be raised from the systems defaults! */

	/* First define shorthand macro function */
	struct rlimit lim;
#define setlim(type) \
	if ( setrlimit(RLIMIT_ ## type, &lim)!=0 ) { \
		if ( errno==EPERM ) { \
			warning("no permission to set resource RLIMIT_" #type); \
		} else { \
			error(errno,"setting resource RLIMIT_" #type); \
		} \
	}

	if ( use_cputime ) {
		/* The CPU-time resource limit can only be specified in
		   seconds, so round up: we can measure actual CPU time used
		   more accurately. Also set the real hard limit one second
		   higher: at the soft limit the kernel will send SIGXCPU at
		   the hard limit a SIGKILL. The SIGXCPU can be caught, but is
		   not by default and gives us a reliable way to detect if the
		   CPU-time limit was reached. */
		rlim_t cputime_limit = (rlim_t)ceil(cputimelimit[1]);
		verbose("setting hard CPU-time limit to %d(+1) seconds",(int)cputime_limit);
		lim.rlim_cur = cputime_limit;
		lim.rlim_max = cputime_limit+1;
		setlim(CPU);
	}

	/* Memory limits should be unlimited, since we use cgroups. */
	lim.rlim_cur = lim.rlim_max = RLIM_INFINITY;
	setlim(AS);
	setlim(DATA);

	/* Always set the stack size to be unlimited. */
	lim.rlim_cur = lim.rlim_max = RLIM_INFINITY;
	setlim(STACK);

	if ( filesize!=RLIM_INFINITY ) {
		verbose("setting filesize limit to %lu bytes",filesize);
		lim.rlim_cur = lim.rlim_max = filesize;
		setlim(FSIZE);
	}

	if ( nproc!=RLIM_INFINITY ) {
		verbose("setting process limit to %d",(int)nproc);
		lim.rlim_cur = lim.rlim_max = nproc;
		setlim(NPROC);
	}

#undef setlim

	if ( no_coredump ) {
		verbose("disabling core dumps");
		lim.rlim_cur = lim.rlim_max = 0;
		if ( setrlimit(RLIMIT_CORE,&lim)!=0 ) error(errno,"disabling core dumps");
	}

	/* Put the child process in the cgroup */
	if (is_cgroup_v2) {
		const char *controllers[] = { "memory", NULL };
		if (cgroup_change_cgroup_path(cgroupname, getpid(), controllers) != 0) {
			error(0, "Failed to move the process to the cgroup");
		}
	} else {
		cgroup_attach();
	}

	/* Run the command in a separate process group so that the command
	   and all its children can be killed off with one signal. */
	if ( setsid()==-1 ) error(errno,"setsid failed");

	/* Set root-directory and change directory to there. */
	if ( use_root ) {
		/* Small security issue: when running setuid-root, people can find
		   out which directories exist from error message. */
		if ( chdir(rootdir)!=0 ) error(errno,"cannot chdir to `%s'",rootdir);

		/* Get absolute pathname of rootdir, by reading it. */
		char  cwd[PATH_MAX+1];
		if ( getcwd(cwd,PATH_MAX)==nullptr ) error(errno,"cannot get directory");
		if ( cwd[strlen(cwd)-1]!='/' ) strcat(cwd,"/");

		/* Canonicalize CHROOT_PREFIX. */
		char *path;
		if ( (path = (char *) malloc(PATH_MAX+1))==nullptr ) {
			error(errno,"allocating memory");
		}
		if ( realpath(CHROOT_PREFIX,path)==nullptr ) {
			error(errno,"cannot canonicalize path '%s'",CHROOT_PREFIX);
		}

		/* Check that we are within prescribed path. */
		if ( strncmp(cwd,path,strlen(path))!=0 ) {
			error(0,"invalid root: must be within `%s'",path);
		}
		free(path);

		if ( chroot(".")!=0 ) error(errno,"cannot change root to `%s'",cwd);
		if ( chdir("/")!=0 ) error(errno,"cannot chdir to `/' in chroot");
		if ( rootchdir!=nullptr ) {
			if ( chdir(rootchdir)!=0 ) error(errno,"cannot chdir to `%s' in chroot", rootchdir);
		}
		verbose("using root-directory `%s'",cwd);
	}

	/* Set group-id (must be root for this, so before setting user). */
	if ( use_group ) {
		if ( setgid(rungid) ) error(errno,"cannot set group ID to `%d'",rungid);
		if ( setgroups(0, NULL) ) error(errno,"cannot clear auxiliary groups");

		verbose("using group ID `%d'",rungid);
	}
	/* Set user-id (must be root for this). */
	if ( use_user ) {
		if ( setuid(runuid) ) error(errno,"cannot set user ID to `%d'",runuid);
		verbose("using user ID `%d' for command",runuid);
	} else {
		/* Permanently reset effective uid to real uid, to prevent
		   child command from having root privileges.
		   Note that this means that the child runs as the same user
		   as the watchdog process and can thus manipulate it, e.g. by
		   sending SIGSTOP/SIGCONT! */
		if ( setuid(getuid()) ) error(errno,"cannot reset real user ID");
		verbose("reset user ID to `%d' for command",getuid());
	}
	if ( geteuid()==0 || getuid()==0 ) {
		error(0,"root privileges not dropped. Do not run judgedaemon as root.");
	}
}

void pump_pipes(fd_set* readfds, size_t data_read[], size_t data_passed[])
{
	char buf[BUF_SIZE];
	ssize_t nread, nwritten;
	size_t to_read, to_write;

	/* Check to see if data is available and pass it on */
	for(int i=1; i<=2; i++) {
		if ( child_pipefd[i][PIPE_OUT] != -1 &&
		     FD_ISSET(child_pipefd[i][PIPE_OUT], readfds) ) {

			if (limit_streamsize && data_passed[i] == streamsize) {
				/* Throw away data if we're at the output limit, but
				   still count how much data we consumed  */
				nread = read(child_pipefd[i][PIPE_OUT], buf, BUF_SIZE);
			} else {
				/* Otherwise copy the output to a file */
				to_read = BUF_SIZE;
				if (limit_streamsize) {
					to_read = min(BUF_SIZE, streamsize-data_passed[i]);
				}

				if ( use_splice ) {
					nread = splice(child_pipefd[i][PIPE_OUT], nullptr,
					               child_redirfd[i], nullptr,
					               to_read, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

					if ( nread==-1 && errno==EINVAL ) {
						use_splice = 0;
						verbose("splice failed, switching to read/write");
						/* Setting errno here to repeat the copy. */
						errno = EAGAIN;
					}
					if ( nread==-1 && errno==EPIPE ) {
						/* This happens when the child process has
						   exited and the pipe is closed. */
						nread = 0;
						errno = 0;
					}
				} else {
					nread = read(child_pipefd[i][PIPE_OUT], buf, to_read);
					if ( nread>0 ) {
						to_write = nread;
						while ( to_write>0 ) {
							nwritten = write(child_redirfd[i], buf, to_write);
							if ( nwritten==-1 ) {
								nread = -1;
								break;
							}
							to_write -= nwritten;
						}
					}
				}

				if ( nread>0 ) data_passed[i] += nread;

				/* print message if we're at the streamsize limit */
				if (limit_streamsize && data_passed[i] == streamsize) {
					verbose("child fd %i limit reached",i);
				}
			}
			if ( nread==-1 ) {
				if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
				error(errno,"copying data fd %d",i);
			}
			if ( nread==0 ) {
				/* EOF detected: close fd and indicate this with -1 */
				if ( close(child_pipefd[i][PIPE_OUT])!=0 ) {
					error(errno,"closing pipe for fd %d",i);
				}
				child_pipefd[i][PIPE_OUT] = -1;
				continue;
			}
			data_read[i] += nread;
		}
	}

}

bool cgroup_is_v2() {
	bool ret = false;
	FILE *fp = setmntent("/proc/mounts", "r");
	if (!fp) {
		perror("Error opening /proc/mounts");
		return false;
	}

	struct mntent *entry;
	while ((entry = getmntent(fp)) != nullptr) {
		if (strcmp(entry->mnt_dir, "/sys/fs/cgroup") == 0) {
			if (strcmp(entry->mnt_type, "cgroup2") == 0) {
				ret = true;
			}
			break;
		}
	}

	endmntent(fp);

	return ret;
}

int main(int argc, char **argv)
{
	int   ret;
	regex_t userregex;
	int   opt;
	double tmpd;
	size_t data_read[3];
	size_t data_passed[3];
	size_t total_data;
	char str[256];

	struct itimerval itimer;
	struct sigaction sigact;

	progname = argv[0];

	if ( gettimeofday(&progstarttime,nullptr) ) error(errno,"getting time");

	/* Parse command-line options */
	use_root = use_walltime = use_cputime = use_user = no_coredump = 0;
	outputmeta = walllimit_reached = cpulimit_reached = 0;
	outputtimetype = CPU_TIME_TYPE;
	preserve_environment = 0;
	memsize = filesize = nproc = RLIM_INFINITY;
	redir_stdout = redir_stderr = limit_streamsize = 0;
	be_verbose = be_quiet = 0;
	show_help = show_version = 0;
	opterr = 0;
	char *ptr;
	while ( (opt = getopt_long(argc,argv,"+r:u:g:d:t:C:m:f:p:P:co:e:s:EV:M:vqU:",long_opts,(int *) 0))!=-1 ) {
		switch ( opt ) {
		case 0:   /* long-only option */
			break;
		case 'r': /* rootdir option */
			use_root = 1;
			rootdir = (char *) malloc(strlen(optarg)+2);
			if ( rootdir==nullptr ) error(errno,"allocating memory");
			strcpy(rootdir,optarg);
			break;
		case 'u': /* user option: uid or string */
			use_user = 1;
			runuser = strdup(optarg);
			if ( runuser==NULL ) error(errno,"strdup() failed");
			errno = 0;
			runuid = strtol(optarg,&ptr,10);
			if ( errno || *ptr!='\0' ) {
				runuid = userid(optarg);
				if ( regcomp(&userregex,"^[A-Za-z][A-Za-z0-9\\._-]*$", REG_NOSUB)!=0 ) {
					error(0,"could not create username regex");
				}
				if ( regexec(&userregex, runuser, 0, nullptr, 0)!=0 ) {
					error(0,"username `%s' does not match POSIX pattern", runuser);
				}
			}
			if ( runuid<0 ) error(0,"invalid username or ID specified: `%s'",optarg);
			break;
		case 'g': /* group option: gid or string */
			use_group = 1;
			rungroup = strdup(optarg);
			if ( rungroup==NULL ) error(errno,"strdup() failed");
			errno = 0;
			rungid = strtol(optarg,&ptr,10);
			if ( errno || *ptr!='\0' ) rungid = groupid(optarg);
			if ( rungid<0 ) error(0,"invalid groupname or ID specified: `%s'",optarg);
			break;
		case 'd': /* chdir option */
			rootchdir = (char *) malloc(strlen(optarg)+2);
			if ( rootchdir==nullptr ) error(errno,"allocating memory");
			strcpy(rootchdir,optarg);
			break;
		case 't': /* wallclock time option */
			use_walltime = 1;
			outputtimetype = WALL_TIME_TYPE;
			read_optarg_time("walltime",walltimelimit);
			break;
		case 'C': /* CPU time option */
			use_cputime = 1;
			outputtimetype = CPU_TIME_TYPE;
			read_optarg_time("cputime",cputimelimit);
			break;
		case 'm': /* memsize option */
			memsize = (rlim_t) read_optarg_int("memory limit",1,LONG_MAX);
			/* Convert limit from kB to bytes and check for overflow */
			if ( memsize!=(memsize*1024)/1024 ) {
				memsize = RLIM_INFINITY;
			} else {
				memsize *= 1024;
			}
			break;
		case 'f': /* filesize option */
			filesize = (rlim_t) read_optarg_int("filesize limit",1,LONG_MAX);
			/* Convert limit from kB to bytes and check for overflow */
			if ( filesize!=(filesize*1024)/1024 ) {
				filesize = RLIM_INFINITY;
			} else {
				filesize *= 1024;
			}
			break;
		case 'p': /* nproc option */
			nproc = (rlim_t) read_optarg_int("process limit",1,LONG_MAX);
			break;
		case 'P': /* cpuset option */
			cpuset = optarg;
			break;
		case 'c': /* no-core option */
			no_coredump = 1;
			break;
		case 'o': /* stdout option */
			redir_stdout = 1;
			stdoutfilename = strdup(optarg);
			break;
		case 'e': /* stderr option */
			redir_stderr = 1;
			stderrfilename = strdup(optarg);
			break;
		case 's': /* streamsize option */
			limit_streamsize = 1;
			streamsize = (size_t) read_optarg_int("streamsize limit",0,LONG_MAX);
			/* Convert limit from kB to bytes and check for overflow */
			if ( streamsize!=(streamsize*1024)/1024 ) {
				streamsize = (size_t) LONG_MAX;
			} else {
				streamsize *= 1024;
			}
			break;
		case 'E': /* environment option */
			preserve_environment = 1;
			break;
		case 'V': /* set environment variable */
			environment_variables.push_back(std::string(optarg));
			break;
		case 'M': /* outputmeta option */
			outputmeta = 1;
			metafilename = strdup(optarg);
			break;
		case 'v': /* verbose option */
			be_verbose = 1;
			break;
		case 'q': /* quiet option */
			be_quiet = 1;
			break;
		case 'U':
			runpipe_pid = strtol(optarg, &ptr, 10);
			break;
		case ':': /* getopt error */
		case '?':
			error(0,"unknown option or missing argument `%c'",optopt);
			break;
		default:
			error(0,"getopt returned character code `%c' ??",(char)opt);
		}
	}

	verbose("starting in verbose mode, PID = %d", getpid());

	/* Make sure that we change from group root if we change to an
	   unprivileged user to prevent unintended permissions. */
	if ( use_user && !use_group ) {
		verbose("using unprivileged user `%s' also as group",runuser);
		use_group = 1;
		rungroup = strdup(runuser);
		rungid = groupid(rungroup);
		if ( rungid<0 ) error(0,"invalid groupname or ID specified: `%s'",rungroup);
	}

	if ( show_help ) usage();
	if ( show_version ) version(PROGRAM,VERSION);

	if ( argc<=optind ) error(0,"no command specified");

	/* Command to be executed */
	cmdname = argv[optind];
	cmdargs = argv+optind;

	is_cgroup_v2 = cgroup_is_v2();

	if ( outputmeta && (metafile = fopen(metafilename,"w"))==nullptr ) {
		error(errno,"cannot open `%s'",metafilename);
	}

	/* Check that new uid is in list of valid uid's. When the new user
	   was given as a username string, then '*' matches an arbitrary
	   length string of valid POSIX username characters [A-Za-z0-9._-].
	   This check must be done before chroot for /etc/passwd lookup. */
	if ( use_user ) {
		char *valid_users = strdup(VALID_USERS);
		for(ptr=strtok(valid_users,","); ptr!=nullptr; ptr=strtok(nullptr,",")) {
			if ( runuid==userid(ptr) ) break;
			if ( runuser!=nullptr ) {
				ret = fnmatch(ptr,runuser,0);
				if ( ret==0 ) break;
				if ( ret!=FNM_NOMATCH ) {
					error(0,"matching username `%s' against `%s'",runuser,ptr);
				}
			}
		}
		if ( ptr==nullptr || runuid<=0 ) error(0,"illegal user specified: %d",runuid);
	}

	/* Setup pipes connecting to child stdout/err streams (ignore stdin). */
	for(int i=1; i<=2; i++) {
		if ( pipe(child_pipefd[i])!=0 ) error(errno,"creating pipe for fd %d",i);
	}

	sigset_t emptymask;
	if ( sigemptyset(&emptymask)!=0 ) error(errno,"creating empty signal mask");

	/* unmask all signals, except SIGCHLD: detected in pselect() below */
	sigset_t sigmask = emptymask;
	if ( sigaddset(&sigmask, SIGCHLD)!=0 ) error(errno,"setting signal mask");
	if ( sigprocmask(SIG_SETMASK, &sigmask, nullptr)!=0 ) {
		error(errno,"unmasking signals");
	}

	/* Construct signal handler for SIGCHLD detection in pselect(). */
	received_SIGCHLD = 0;
	sigact.sa_handler = child_handler;
	sigact.sa_flags   = 0;
	sigact.sa_mask    = emptymask;
	if ( sigaction(SIGCHLD,&sigact,nullptr)!=0 ) {
		error(errno,"installing signal handler");
	}

	if ( cpuset!=nullptr && strlen(cpuset)>0 ) {
		int ret = strtol(cpuset, &ptr, 10);
		/* check if input is only a single integer */
		if ( *ptr == '\0' ) {
			/* check if we have enough cores available */
			int nprocs = get_nprocs_conf();
			if ( ret < 0 || ret >= nprocs ) {
				error(0, "processor ID %d given as cpuset, but only %d cores configured",
				      ret, nprocs);
			}
		}
	}

	/* Make libcgroup ready for use */
	ret = cgroup_init();
	if ( ret!=0 ) {
		error(0,"libcgroup initialization failed: %s(%d)\n", cgroup_strerror(ret), ret);
	}
	/* Define the cgroup name that we will use and make sure it will
	 * be unique. Note: group names must have slashes!
	 */
	if ( cpuset!=nullptr && strlen(cpuset)>0 ) {
		strncpy(str, cpuset, 16);
	} else {
		str[0] = 0;
	}
	snprintf(cgroupname, 255, "domjudge/dj_cgroup_%d_%.16s_%d.%06d",
	         getpid(), str, (int)progstarttime.tv_sec, (int)progstarttime.tv_usec);

	cgroup_create();

	if ( unshare(CLONE_FILES|CLONE_FS|CLONE_NEWIPC|CLONE_NEWNET|CLONE_NEWNS|CLONE_NEWUTS|CLONE_SYSVSEM)!=0 ) {
		error(errno, "calling unshare");
	}

	/* Check if any Linux Out-Of-Memory killer adjustments have to
	 * be made. The oom_adj or oom_score_adj is inherited by child
	 * processes, and at least older versions of sshd seemed to set
	 * it, leading to processes getting a timelimit instead of memory
	 * exceeded, when running via SSH. */
	FILE *fp = nullptr;
	char *oom_path;
	if ( !fp && (fp = fopen(OOM_PATH_NEW,"r+")) ) oom_path = strdup(OOM_PATH_NEW);
	if ( !fp && (fp = fopen(OOM_PATH_OLD,"r+")) ) oom_path = strdup(OOM_PATH_OLD);
	if ( fp!=nullptr ) {
		if ( fscanf(fp,"%d",&ret)!=1 ) error(errno,"cannot read from `%s'",oom_path);
		if ( ret<0 ) {
			verbose("resetting `%s' from %d to %d",oom_path,ret,OOM_RESET_VALUE);
			rewind(fp);
			if ( fprintf(fp,"%d\n",OOM_RESET_VALUE)<=0 ) {
				error(errno,"cannot write to `%s'",oom_path);
			}
		}
		if ( fclose(fp)!=0 ) error(errno,"closing file `%s'",oom_path);
	}

	switch ( child_pid = fork() ) {
	case -1: /* error */
		error(errno,"cannot fork");
	case  0: /* run controlled command */
		/* Apply all restrictions for child process. */
		setrestrictions();
		verbose("setrestrictions() done");

		/* Connect pipes to command (stdin/)stdout/stderr and close
		 * unneeded fd's. Do this after setting restrictions to let
		 * any messages not go to command stderr pipe. */
		for(int i=1; i<=2; i++) {
			if ( dup2(child_pipefd[i][PIPE_IN],i)<0 ) {
				error(errno,"redirecting child fd %d",i);
			}
			if ( close(child_pipefd[i][PIPE_IN] )!=0 ||
			     close(child_pipefd[i][PIPE_OUT])!=0 ) {
				error(errno,"closing pipe for fd %d",i);
			}
		}
		verbose("pipes closed in child");

		if ( outputmeta ) {
			if ( fclose(metafile)!=0 ) {
				error(errno,"closing file `%s'",metafilename);
			}
			verbose("metafile closed in child");
		}

		/* And execute child command. */
		execvp(cmdname,cmdargs);
		struct rlimit limit;
		getrlimit(RLIMIT_NPROC, &limit);
		error(errno,"cannot start `%s', limit: %ld/%ld | ",cmdname, limit.rlim_cur, limit.rlim_max);

	default: /* become watchdog */
		verbose("child pid = %d", child_pid);
		/* Shed privileges, only if not using a separate child uid,
		   because in that case we may need root privileges to kill
		   the child process. Do not use Linux specific setresuid()
		   call with saved set-user-ID. */
		if ( !use_user ) {
			if ( setuid(getuid())!=0 ) error(errno, "setting watchdog uid");
			verbose("watchdog using user ID `%d'",getuid());
		}

		if ( gettimeofday(&starttime,nullptr) ) error(errno,"getting time");

		/* Close unused file descriptors */
		for(int i=1; i<=2; i++) {
			if ( close(child_pipefd[i][PIPE_IN])!=0 ) {
				error(errno,"closing pipe for fd %i",i);
			}
		}

		/* Redirect child stdout/stderr to file */
		for(int i=1; i<=2; i++) {
			child_redirfd[i] = i; /* Default: no redirects */
			data_read[i] = data_passed[i] = 0; /* Reset data counters */
		}
		data_read[0] = 0;
		if ( redir_stdout ) {
			child_redirfd[STDOUT_FILENO] = creat(stdoutfilename, S_IRUSR | S_IWUSR);
			if ( child_redirfd[STDOUT_FILENO]<0 ) {
				error(errno,"opening file '%s'",stdoutfilename);
			}
		}
		if ( redir_stderr ) {
			child_redirfd[STDERR_FILENO] = creat(stderrfilename, S_IRUSR | S_IWUSR);
			if ( child_redirfd[STDERR_FILENO]<0 ) {
				error(errno,"opening file '%s'",stderrfilename);
			}
		}
		verbose("redirection done in parent");

		if ( sigemptyset(&emptymask)!=0 ) error(errno,"creating empty signal mask");

		/* Construct one-time signal handler to terminate() for TERM
		   and ALRM signals. */
		sigset_t sigmask = emptymask;
		if ( sigaddset(&sigmask,SIGALRM)!=0 ||
		     sigaddset(&sigmask,SIGTERM)!=0 ) error(errno,"setting signal mask");

		sigact.sa_handler = terminate;
		sigact.sa_flags   = SA_RESETHAND | SA_RESTART;
		sigact.sa_mask    = sigmask;

		/* Kill child command when we receive SIGTERM */
		if ( sigaction(SIGTERM,&sigact,nullptr)!=0 ) {
			error(errno,"installing signal handler");
		}

		if ( use_walltime ) {
			/* Kill child when we receive SIGALRM */
			if ( sigaction(SIGALRM,&sigact,nullptr)!=0 ) {
				error(errno,"installing signal handler");
			}

			/* Trigger SIGALRM via setitimer:  */
			itimer.it_interval.tv_sec  = 0;
			itimer.it_interval.tv_usec = 0;
			itimer.it_value.tv_sec  = (int) walltimelimit[1];
			itimer.it_value.tv_usec = (int)(modf(walltimelimit[1],&tmpd) * 1E6);

			if ( setitimer(ITIMER_REAL,&itimer,nullptr)!=0 ) {
				error(errno,"setting timer");
			}
			verbose("setting hard wall-time limit to %.3f seconds",walltimelimit[1]);
		}

		if ( times(&startticks)==(clock_t) -1 ) {
			error(errno,"getting start clock ticks");
		}

		/* Wait for child data or exit.
		   Initialize status here to quelch clang++ warning about
		   uninitialized value; it is set by the wait() call. */
		int status = 0;
		/* We start using splice() to copy data from child to parent
		   I/O file descriptors. If that fails (not all I/O
		   source - dest combinations support it), then we revert to
		   using read()/write(). */
		use_splice = 1;
		fd_set readfds;
		while ( 1 ) {

			FD_ZERO(&readfds);
			int nfds = -1;
			for(int i=1; i<=2; i++) {
				if ( child_pipefd[i][PIPE_OUT]>=0 ) {
					FD_SET(child_pipefd[i][PIPE_OUT],&readfds);
					nfds = max(nfds,child_pipefd[i][PIPE_OUT]);
				}
			}

			int r = pselect(nfds+1, &readfds, nullptr, NULL, NULL, &emptymask);
			if ( r==-1 && errno!=EINTR ) error(errno,"waiting for child data");
			if (error_in_signalhandler) {
				error(errno, "error in signal handler, exiting");
			}

			if ( received_SIGCHLD || received_signal == SIGALRM ) {
				pid_t pid;
				if ( (pid = wait(&status))<0 ) error(errno,"waiting on child");
				if ( pid==child_pid ) break;
			}

			pump_pipes(&readfds, data_read, data_passed);
		}

		/* Reset pipe filedescriptors to use blocking I/O. */
		FD_ZERO(&readfds);
		for(int i=1; i<=2; i++) {
			if ( child_pipefd[i][PIPE_OUT]>=0 ) {
				FD_SET(child_pipefd[i][PIPE_OUT],&readfds);
				int r = fcntl(child_pipefd[i][PIPE_OUT], F_GETFL);
				if (r == -1) {
					error(errno, "fcntl, getting flags");
				}
				r = fcntl(child_pipefd[i][PIPE_OUT], F_SETFL, r ^ O_NONBLOCK);
				if (r == -1) {
					error(errno, "fcntl, setting flags");
				}
			}
		}

		do {
			total_data = data_passed[1] + data_passed[2];
			pump_pipes(&readfds, data_read, data_passed);
		} while ( data_passed[1] + data_passed[2] > total_data );

		/* Close the output files */
		for(int i=1; i<=2; i++) {
			ret = close(child_redirfd[i]);
			if( ret!=0 ) error(errno,"closing output fd %d", i);
		}

		if ( times(&endticks)==(clock_t) -1 ) {
			error(errno,"getting end clock ticks");
		}

		if ( gettimeofday(&endtime,nullptr) ) error(errno,"getting time");

		/* Test whether command has finished abnormally */
		int exitcode = 0;
		if ( ! WIFEXITED(status) ) {
			if ( WIFSIGNALED(status) ) {
				if ( WTERMSIG(status)==SIGXCPU ) {
					cpulimit_reached |= hard_timelimit;
					warning("timelimit exceeded (hard cpu time)");
				} else {
					warning("command terminated with signal %d",WTERMSIG(status));
				}
				exitcode = 128+WTERMSIG(status);
			} else
			if ( WIFSTOPPED(status) ) {
				warning("command stopped with signal %d",WSTOPSIG(status));
				exitcode = 128+WSTOPSIG(status);
			} else {
				error(0,"command exit status unknown: %d",status);
			}
		} else {
			exitcode = WEXITSTATUS(status);
		}
		verbose("child exited with exit code %d", exitcode);

		if ( use_walltime ) {
			/* Disarm timer we set previously so if any of the
			 * clean-up steps below are slow we are not mistaking
			 * this for a wall-time timeout. */
			itimer.it_interval.tv_sec  = 0;
			itimer.it_interval.tv_usec = 0;
			itimer.it_value.tv_sec  = 0;
			itimer.it_value.tv_usec = 0;

			if ( setitimer(ITIMER_REAL,&itimer,nullptr)!=0 ) {
				error(errno,"disarming timer");
			}
		}

		check_remaining_procs();

		double cputime = -1;
		if (is_cgroup_v2) {
			output_cgroup_stats_v2(&cputime);
		} else {
			output_cgroup_stats_v1(&cputime);
		}
		cgroup_kill();
		cgroup_delete();

		/* Drop root before writing to output file(s). */
		if ( setuid(getuid())!=0 ) error(errno,"dropping root privileges");

		output_exit_time(exitcode, cputime);

		/* Check if the output stream was truncated. */
		if ( limit_streamsize ) {
			str[0] = 0;
			ptr = str;
			if ( data_passed[1]<data_read[1] ) {
				ptr = stpcpy(ptr,"stdout");
			}
			if ( data_passed[2]<data_read[2] ) {
				if ( ptr!=str ) ptr = stpcpy(ptr,",");
				ptr = stpcpy(ptr,"stderr");
			}
			write_meta("output-truncated","%s",str);
		}

		write_meta("stdin-bytes", "%zu",data_read[0]);
		write_meta("stdout-bytes","%zu",data_read[1]);
		write_meta("stderr-bytes","%zu",data_read[2]);

		if ( outputmeta && fclose(metafile)!=0 ) {
			error(errno,"closing file `%s'",metafilename);
		}

		/* Return the exitstatus of the command */
		return exitcode;
	}

	/* This should never be reached */
	error(0,"unexpected end of program");
}
