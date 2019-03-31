#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <errno.h>

extern char **environ;

#define MAX_ARG_LIST	64
#define DEFAULT_PROMPT 	'$'
#define MAX_PROMPT_SIZE	200
#define MAX_HOSTNAME_SIZE	200
#define MAX_PATH_SIZE	200
#define MAX_CMD_LIST    10

const char *const condensePathStr(char *path);
const char *const expandPathStr(char *path);

#define MAX_CMD_SIZE	50
#define SEARCH_FOR_CMD	-1
typedef void (*builtInFunc)(char **);
typedef struct {
	char cmd[MAX_CMD_SIZE];
	builtInFunc func;
} builtInCmd;

/* built-in commands */
#define MAX_PATH_SIZE	200
void execCD(char *args[]);
void execExit(char *args[]);
void execHelp(char *args[]);
builtInCmd builtInCmds[] = {
	{"help", execHelp},
	{"exit", execExit},
	{"cd", execCD},
};
int builtInCnt = sizeof(builtInCmds)/sizeof(builtInCmd);

int isBuiltIn(char *cmd);
void execBuiltIn(int i, char *args[]);

/* control buffer and handler for SIGINT signal capture */
sigjmp_buf ctrlc_buf;
void ctrl_hndlr(int signo) {
   	siglongjmp(ctrlc_buf, 1);
}

/* error functions */
char *shname;
void error(int code, char *msg);
void warn(char *msg);

/* pipe and redirection functions */
void reader(int fd[], char *cmd);
void writer(int fd[], char *cmd);
void input_redirection(char *cmd);
void output_redirection(char *cmd);
void append_redirection(char *cmd);

int main(int argc, char *argv[]) {

	char *line;
	pid_t childPID;
	int argn;
	char *args[MAX_ARG_LIST];
	char *tok;
	int cmdn;
	char hostname[MAX_HOSTNAME_SIZE];
	char prompt[MAX_PROMPT_SIZE];
	char prompt_sep = DEFAULT_PROMPT;
    char *sub_cmds[MAX_CMD_LIST];       // Capable of executing MAX_CMD_LIST commands delimited by ';'
    char *sub_cmd;
    int sub_cmds_count;
    char *tok1;
    char *tok2;
    int i;
    int pipe_fd[2];
    char *p_char;
    int status;

	// get shell name
	shname = strrchr(argv[0], '/');
	if (shname == NULL)
		shname = argv[0];
	else
		++shname;

	// command-line completion
	rl_bind_key('\t', rl_complete);

	// get hostname
	gethostname(hostname, MAX_HOSTNAME_SIZE);

	/* set up control of SIGINT signal */
	if (signal(SIGINT, ctrl_hndlr) == SIG_ERR) 
		error(100, "Failed to register interrupts in kernel\n");
	while (sigsetjmp(ctrlc_buf, 1) != 0) 
		/* empty */;

	for(;;) {
		// build prompt
		snprintf(prompt, MAX_PROMPT_SIZE, "%s@%s:%s%c ", 
			getenv("USER"), hostname, condensePathStr(getcwd(NULL,0)), prompt_sep);

		// get command-line
		line = readline(prompt);

		if (!line) // feof(stdin)
			break;
		
		// process command-line
		if (line[strlen(line)-1] == '\n')
			line[strlen(line)-1] = '\0';
		add_history(line);

        for (i=0; i < MAX_CMD_LIST; i++)
        {
            sub_cmds[i] = NULL;
        }

        // parse command line to fetch sub commands delimited by ';'
        if((sub_cmd = strtok(line, ";")) != NULL)
        {
            i = 0;
            do
            {
                // validate the limit on sub commands that is capable of executing
                if(i > MAX_CMD_LIST)
                {
                    fputs("ERROR: too many sub commands!\n", stderr);
                    exit(2);
                }
                sub_cmds[i] = sub_cmd;
                i++;
            } while ((sub_cmd = strtok(NULL, ";")) != NULL);
            sub_cmds_count = i;
        }
	
        // execute all sub commands
        for (i = 0; i < sub_cmds_count; i++)
        {
            // Check for pipe or redirection in a subcommand
            if ((p_char = strchr(sub_cmds[i], '|')) != NULL)
            {
                tok1 = strtok(sub_cmds[i], "|");
			    tok2 = strtok(NULL, "|");
                pipe(pipe_fd);

                writer(pipe_fd, tok1);
                reader(pipe_fd, tok2);
    
                close(pipe_fd[0]);
                close(pipe_fd[1]);

                while ((childPID = wait(&status)) != -1);
                continue;
            }
            else if ((p_char = strchr(sub_cmds[i], '<')) != NULL)
            {
                input_redirection(sub_cmds[i]);
                while ((childPID = wait(&status)) != -1);
                continue;
            }
            else if ((p_char = strstr(sub_cmds[i], ">>")) != NULL)
            {
                append_redirection(sub_cmds[i]);
                while ((childPID = wait(&status)) != -1);
                continue;
            }
            else if ((p_char = strchr(sub_cmds[i], '>')) != NULL)
            {
                output_redirection(sub_cmds[i]);
                while ((childPID = wait(&status)) != -1);
                continue;
            }
		    // build argument list
            else
            {
        		tok = strtok(sub_cmds[i], " \t");
        		for (argn=0; tok!=NULL && argn<MAX_ARG_LIST; ++argn) {
        			args[argn] = tok;
    	    		tok = strtok(NULL, " \t");
        		}
        		args[argn] = NULL;

        		if ((cmdn=isBuiltIn(args[0])) >= 0) { // process built-in command
        			execBuiltIn(cmdn, args);
        		} else { // execute command
        			childPID = fork();
        			if (childPID == 0) {
        				execvpe(args[0], args, environ);
        				warn("command failed to execute");
        				_exit(2);
        			} else {
        				waitpid(childPID, NULL, 0);
        			}
        		}
            }
        }
		fflush(stderr);
		fflush(stdout);
		free(line);
	}
	fputs("exit\n", stdout);

	return 0;
}

/* error functions */
void error(int code, char *msg) {
	fputs(shname, stderr);
	fputs(": ", stderr);
	fputs(msg, stderr);
	fputs("\n", stderr);
	if (code > 0)
		exit(code);
}
void warn(char *msg) {
	error(0, msg);
}

/* manage '~' for home path */
const char *const condensePathStr(char *path) {
	static char newpath[MAX_PATH_SIZE];

	newpath[0] = '\0';
	if (path != NULL) {
		if (strstr(path, getenv("HOME")) == path)
			snprintf(newpath, MAX_PATH_SIZE, "%c%s", '~', &path[strlen(getenv("HOME"))]);
		else
			snprintf(newpath, MAX_PATH_SIZE, "%s", path);
	}

	return newpath;
}

const char *const expandPathStr(char *path) {
	static char newpath[MAX_PATH_SIZE];

	newpath[0] = '\0';
	if (path != NULL) {
		if (path[0] == '~')
			snprintf(newpath, MAX_PATH_SIZE, "%s%s", getenv("HOME"), &path[1]);
		else
			snprintf(newpath, MAX_PATH_SIZE, "%s", path);
	}

	return newpath;
}

/* return index in the builtInCmds array or -1 for failure */
int isBuiltIn(char *cmd) {
	int i;
	for (i = 0; i < builtInCnt; ++i)
		if (strcmp(cmd,builtInCmds[i].cmd)==0)
			break;
	return i<builtInCnt?i:-1;
}

/* i is the index or SEARCH_FOR_CMD */
void execBuiltIn(int i, char *args[]) {
	if (i==SEARCH_FOR_CMD)
		i = isBuiltIn(args[0]);
	if (i>-1) 
		builtInCmds[i].func(args);
	else
		warn("unknown built-in command");
}

/* built-in functions */
void execHelp(char *args[]) {
	warn("help unavailable at the moment");
}

void execExit(char *args[]) {
	int code = 0;
    int i;
	if (args[2] != NULL)
		error(1, "exit: too many arguments");
	for(i=0;i<strlen(args[1]);++i)
		if (!isdigit(args[1][i]))
			error(2, "exit: numeric argument required");
	code = atoi(args[1]);
	exit(code);
}

void execCD(char *args[]) {
	int err = 0;
	char path[MAX_PATH_SIZE];
	if (args[1] == NULL)
		snprintf(path, MAX_PATH_SIZE, "%s", getenv("HOME"));
	else
		snprintf(path, MAX_PATH_SIZE, "%s", expandPathStr(args[1]));
	err = chdir(path);
	if (err<0)
		warn(strerror(errno));
}

/* Process write with pipes */
void writer(int pipefd[], char *command)
{
    int child_pid;
    char *tok;
	char *args[MAX_ARG_LIST];
    int argn;

    switch(child_pid = fork())
    {
        case -1:
            fputs("Fork error while processing pipe!\n", stdout);
            break;

        case 0:
            dup2(pipefd[1], STDOUT_FILENO);

            // Close the read end of the pipe
            close(pipefd[0]);
            
            // build argument list
            tok = strtok(command, " \t");
            for (argn=0; tok!=NULL && argn<MAX_ARG_LIST; ++argn) {
                args[argn] = tok;
                tok = strtok(NULL, " \t");
            }
            args[argn] = NULL;
            execvpe(args[0], args, environ);
            fputs("Error in executing the pipe command\n", stdout);
            break;
    }
}

/* Process read with pipes */
void reader(int pipefd[], char *command)
{
    int child_pid;
    char *tok;
	char *args[MAX_ARG_LIST];
    int argn;

    switch(child_pid = fork())
    {
        case -1:
            fputs("Fork error while processing pipe!\n", stdout);
            break;

        case 0:
            dup2(pipefd[0], STDIN_FILENO);

            // Close the read end of the pipe
            close(pipefd[1]);
            
            // build argument list
            tok = strtok(command, " \t");
            for (argn=0; tok!=NULL && argn<MAX_ARG_LIST; ++argn) {
                args[argn] = tok;
                tok = strtok(NULL, " \t");
            }
            args[argn] = NULL;
            
            // Execute the command
            execvpe(args[0], args, environ);
            fputs("Error in executing the pipe command\n", stdout);
            break;
    }
}

// Process input redirection
void input_redirection(char *command)
{
    int child_pid;
    int fd;
    char *tok1;
    char *tok2;
	char *args[2];

    tok1 = strtok(command, "<");
    tok2 = strtok(NULL, " ");     // Strip off the white spaces in the path
    tok1 = strtok(tok1, " \t");   // Strip off the white spaces in the command
    args[0] = tok1;
    args[1] = NULL;
    if((fd = open(tok2, O_RDONLY)) == -1)
    {
        fputs("Input redirection error. Unable to open the file\n", stdout);
        return;
    }
    
    switch(child_pid = fork())
    {
        case -1:
            fputs("Fork error!\n", stdout);
            break;

        case 0:
            dup2(fd, STDIN_FILENO);

            // Execute the command
            execvpe(args[0], args, environ);
            fputs("Error in executing the pipe command\n", stdout);
            break;
    }
}

// Process input redirection
void output_redirection(char *command)
{
    int child_pid;
    int fd;
    int flags;
    mode_t perms;
    char *tok1;
    char *tok2;
    char *tok3;
	char *args[3];

    // Extract the actual command and file and strip the whitespaces on both the ends
    tok1 = strtok(command, " ");
    tok2 = strtok(NULL, ">"); 
    tok3 = strtok(NULL, " ");
    tok2 = strtok(tok2, " \t"); 

    args[0] = tok1;
    args[1] = tok2;
    args[2] = NULL;
    flags = O_CREAT | O_WRONLY | O_TRUNC;
    perms = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH;
    if((fd = open(tok3, flags, perms)) == -1)
    {
        fputs("Output redirection error. Unable to open the file\n", stdout);
        return;
    }
    
    switch(child_pid = fork())
    {
        case -1:
            fputs("Fork error!\n", stdout);
            break;

        case 0:
            dup2(fd, STDOUT_FILENO);

            // Execute the command
            execvpe(args[0], args, environ);
            fputs("Error in executing the pipe command\n", stdout);
            break;
    }
}

// Process input redirection
void append_redirection(char *command)
{
    int child_pid;
    int fd;
    int flags;
    mode_t perms;
    char *pchar;
    char *tok1;
    char *tok2;
    char *tok3;
	char *args[3];

    // Extract the actual command and file and strip the whitespaces on both ends
    tok1 = strtok(command, " ");
    tok2 = strtok(NULL, ">");     
    tok3 = strtok(NULL, " ");    
    tok3 = strtok(NULL, " ");   
    tok2 = strtok(tok2, " \t");

    args[0] = tok1;
    args[1] = tok2;
    args[2] = NULL;
    flags =  O_WRONLY | O_APPEND;
    perms = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH;
    if((fd = open(tok3, flags, perms)) == -1)
    {
        fputs("Append redirection error. Unable to open the file\n", stdout);
        return;
    }
    
    switch(child_pid = fork())
    {
        case -1:
            fputs("Fork error!\n", stdout);
            break;

        case 0:
            dup2(fd, STDOUT_FILENO);

            // Execute the command
            execvpe(args[0], args, environ);
            fputs("Error in executing the pipe command\n", stdout);
            break;
    }
}
