/* 
 * tsh - A tiny shell program with job control
 * 
 * Sanghyeon Park - tkdgus2916
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>




/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

//Use macro for formmatted output and verbose option
#define DPRINT(fmt, ...) \
    do { \
        if (verbose) { \
                       printf("%s: ", __func__); \
            printf(fmt, ##__VA_ARGS__); \
        } \
    } while(0)

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
		printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    char buf[MAXLINE];
    int bin_funcnum;
    int bg;
    pid_t pid;

    //For signal mask
    sigset_t mask, prev_mask;
    
    //For check bg/fg argument
    struct stat s_buffer;   
    int exist;

    strcpy(buf, cmdline);

    //No input in shell
    if(strlen(buf) == 1)
	    return;

    bg = parseline(buf, argv);

    bin_funcnum = builtin_cmd(argv);

    //When the cmd line command is built in command
    switch(bin_funcnum){
	case 1 :
		exit(0);
		return;
	case 2 :
	case 3 :
		do_bgfg(argv);
		return;
	case 4 :
		listjobs(jobs);
		return;
		break;
    }

    //Check the input cmdline file is exist
    exist = stat(argv[0], &s_buffer);
    if(exist != 0){
	printf("%s: Command not found\n", argv[0]);
	return;
    }


    //Set the signal mask
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    //Block the SIGCHLD signal
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);


    //Not the built in command 
    //Then fork the program
    pid = fork();
    if (pid == -1) {
	// Fork is failed
	perror("fork");
	exit(EXIT_FAILURE);
    }
    // In child process
    if (pid == 0) {
	// When create new child, set group id to new one 
	setpgid(0, 0);

	// Create new process using execvp
	execvp(argv[0], argv);

	// When failed
	perror("execvp");
	exit(EXIT_FAILURE);
    } 
    else {
    // In parent process
	//Foreground execute
	if(!bg){
	    addjob(jobs, pid, FG, cmdline); 

	    //Bloking End
	    sigprocmask(SIG_SETMASK, &prev_mask, NULL);

	    //sigchld_handler can invoke from this position
	    waitfg(pid);
	}
	//Background execute
	else{
	    int jid;
	    
	    jid = addjob(jobs, pid, BG, cmdline); 

	    //Bloking End
	    sigprocmask(SIG_SETMASK, &prev_mask, NULL);

	    //sigchld_handler can invoke from this position
	    printf("[%d] (%d) %s", jid, pid, cmdline);
	}
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
    char *builtinp[4] = {"quit", "fg", "bg", "jobs"};

    //Compare the four builtin command
    for(int i = 0; i < 4; i++){
	if(strcmp(builtinp[i], argv[0]) == 0)
	    return i+1;
    }

    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    struct job_t *job;

    //When the input has no argumant 
    if(argv[1]==NULL){
	printf("%s command requires PID or %%jobid argument\n",argv[0]);
	return;
    }
    //Input argument is JID
    else if(argv[1][0] == '%'){
	char *endptr;
	//Convert to integer and check the argument is only numbers
	//[1][1] means to except % character
	int jid = strtol(&argv[1][1], &endptr, 10);

	if(endptr == &argv[1][1]){
	    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
	    return;
	}

	job = getjobjid(jobs, jid);
	if(job == NULL){
	    printf("%s: No such job\n", argv[1]);
	    return;
	}
    }
    //Input argument is PID
    else{
	char *endptr;

	//Convert to integer and check the argument is only numbers
	int pid = strtol(argv[1], &endptr, 10);

	if(endptr == argv[1]){
	    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
	    return;
	}

	job = getjobpid(jobs, pid);
	if(job == NULL){
	    printf("(%s): No such process\n", argv[1]);
	    return;
	}
    }
    

    //Check the command is bg or fg
    if(strcmp(argv[0], "bg") == 0){
	printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
	
	//Send SIGCONT signal
	kill(-(job->pid), SIGCONT);
	job->state = BG;
    }
    else{
	job->state = FG;

	//Send SIGCONT signal
	kill(-(job->pid), SIGCONT);

	//This because the current process wait
	//And background process will be forground process
	waitfg(job->pid);
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    struct job_t *job;

    do{
	//Wait some time 
	sleep(1);

	job = getjobpid(jobs, pid);

	//When the job is deleted
	if(job == NULL)
		break;
    }
    //Check the job is no longer the fg process
    while((job->state) == FG);

    DPRINT("Process (%d) no longer the fg process\n",  pid);

    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    DPRINT("entering\n");

    int status;

    //Find terminated or stopped child process
    //Option means no hang on for not existing process
    //and check the stopped process too
    pid_t child_pid = waitpid(-1, &status, WNOHANG|WUNTRACED);


    //When exist process
    if (child_pid > 0) {
	int jid = pid2jid(child_pid);

	//Terminated
	if(WIFEXITED(status) || WIFSIGNALED(status)){
	    if(deletejob(jobs, child_pid))
		    DPRINT("Job [%d] (%d) deleted\n", jid, child_pid);

	    if (WIFEXITED(status))
		    DPRINT("Job [%d] (%d) terminates OK (status %d)\n", jid, 
				    child_pid, WEXITSTATUS(status));
	    
	    if (WIFSIGNALED(status)) 
		    printf("Job [%d] (%d) terminated by signal %d\n",
				    jid, child_pid, WTERMSIG(status));
	}

	//Stopped 
	if (WIFSTOPPED(status)) {
	    struct job_t *job;
	    job = getjobpid(jobs, child_pid);

	    if(job){
		//Change the state to ST
		job->state = ST;
		printf("Job [%d] (%d) stopped by signal %d\n",
				    jid, child_pid, WSTOPSIG(status));
	    }
	    else
		printf("Job is not exist\n");
	}
    }

    DPRINT("exiting\n");

    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    DPRINT("entering\n");
    

    //Find the FG process and terminate all process in proces group
    for(int i = 0; i < MAXJOBS; i++){
	if(jobs[i].state == FG){
	    DPRINT("Job [%d] (%d) killed\n"
			    ,jobs[i].jid, jobs[i].pid);

	    kill(-jobs[i].pid, SIGINT);
	}
    }

    DPRINT("exiting\n");

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    DPRINT("entring\n");

    //Find the FG process and stop all process in proces group
    for(int i = 0; i < MAXJOBS; i++){
	if(jobs[i].state == FG){
	    DPRINT("Job [%d] (%d) stopped\n"
			    ,jobs[i].jid, jobs[i].pid);

	    kill(-jobs[i].pid, SIGTSTP);
	}
    }

    DPRINT("exiting\n");
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
		return jobs[i].jid;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if ((jobs[i].pid != 0)) {
	    switch (jobs[i].state) {
		case BG: 
		    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
		    printf("Running ");
		    printf("%s", jobs[i].cmdline);
		    break;
    //		case FG: 
    //		    printf("Foreground ");
    //		    break;
		case ST: 
		    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
		    printf("Stopped ");
		    printf("%s", jobs[i].cmdline);
		    break;
//		default:
//		    printf("listjobs: Internal error: job[%d].state=%d\n", 
//					   i, jobs[i].state);
	    }
    //      printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



