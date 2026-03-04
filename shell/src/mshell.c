#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "config.h"
#include "siparse.h"
#include "utils.h"
#include "builtins.h"
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#define BG_PIDS_MAX 1024
#define BG_Q_MAX    64
static pid_t bg_pids[BG_PIDS_MAX];
static int bg_pids_cnt = 0;
static void bg_add(pid_t p) {
	if (bg_pids_cnt < BG_PIDS_MAX) bg_pids[bg_pids_cnt++] = p;
}
static int bg_has(pid_t p) {
	for (int i=0;i<bg_pids_cnt;i++) {
		if (bg_pids[i]==p) {
			return 1;
		}
	}

	return 0;
}
static void bg_del(pid_t p) {
	for (int i=0;i<bg_pids_cnt;i++) {
		if (bg_pids[i]==p) {
			bg_pids[i]=bg_pids[--bg_pids_cnt];
			return;
		}
	}

}

struct bg_notice { pid_t pid; int status; };
static volatile sig_atomic_t bg_head = 0, bg_tail = 0;
static struct bg_notice bg_q[BG_Q_MAX];

static void push_bg_notice(pid_t p, int st) {
	sig_atomic_t n = (bg_head + 1) % BG_Q_MAX;
	if (n == bg_tail) bg_tail = (bg_tail + 1) % BG_Q_MAX;
	bg_q[bg_head].pid = p;
	bg_q[bg_head].status = st;
	bg_head = n;
}
static void sigchld_handler(int sig) {
	int saved = errno;
	while (1) {
		int st;
		pid_t p = waitpid(-1, &st, WNOHANG);
		if (p <= 0) break;
		if (bg_has(p)) push_bg_notice(p, st);
	}
	errno = saved;
}


static void flush_bg_notices() {
	sigset_t set, old; sigemptyset(&set); sigaddset(&set, SIGCHLD);
	sigprocmask(SIG_BLOCK, &set, &old);

	while (bg_tail != bg_head) {
		pid_t p = bg_q[bg_tail].pid;
		int   st = bg_q[bg_tail].status;
		bg_tail = (bg_tail + 1) % BG_Q_MAX;

		if (WIFEXITED(st)) {
			printf("Background process %d terminated. (exited with status %d)\n",
				p, WEXITSTATUS(st));
		} else if (WIFSIGNALED(st)) {
			printf("Background process %d terminated. (killed by signal %d)\n",
				   p, WTERMSIG(st));
		}
		fflush(stdout);
		bg_del(p);
	}

	sigprocmask(SIG_SETMASK, &old, NULL);
}
static int run_if_builtin(char **argv) {
	if (!argv || !argv[0]) return 0;
	for (int i = 0; builtins_table[i].name; i++) {
		if (strcmp(argv[0], builtins_table[i].name) == 0) {
			(void)builtins_table[i].fun(argv);
			return 1;
		}
	}
	return 0;
}
void report_exec_error(const char *name) {
	if (errno == ENOENT)      fprintf(stderr, "%s: no such file or directory\n", name);
	else if (errno == EACCES) fprintf(stderr, "%s: permission denied\n",         name);
	else                      fprintf(stderr, "%s: exec error\n",                name);
}

void handle_result(int f, redir *r) {
	if (f<0) {
		report_exec_error(r->filename);
		_exit(EXEC_FAILURE);
	}

}

void redirect(command *com) {
	redirseq *start=com->redirs;
	int fd_in = -1, fd_out = -1;
	if (start) {
		redirseq *rs = start;
		do {
			redir *r = rs->r;
			if (!r) { rs = rs->next; continue; }
			if (IS_RIN(r->flags)) {
				int fd = open(r->filename, O_RDONLY);
				handle_result(fd, r);
				if (fd_in>=0) close(fd_in);
				fd_in=fd;
			}
			else if (IS_ROUT(r->flags) || IS_RAPPEND(r->flags)) {
				int flags=O_WRONLY|O_CREAT|(IS_RAPPEND(r->flags) ? O_APPEND : O_TRUNC);
				int fd = open(r->filename, flags, 0666);
				handle_result(fd, r);
				if (fd_out>=0) close(fd_out);
				fd_out=fd;
			}
			rs = rs->next;
		}
		while (rs && rs!=start);
	}
	if (fd_in  >= 0) { dup2(fd_in,  STDIN_FILENO);  close(fd_in);  }
	if (fd_out >= 0) { dup2(fd_out, STDOUT_FILENO); close(fd_out); }
}
int count_commands(commandseq *head) {
	if (!head) return 0;
	int n = 1;
	for (commandseq *c = head->next; c && c != head; c = c->next) n++;
	return n;
}
bool is_in_redir(int flags) {
	return IS_RIN(flags);
}
bool is_out_redir(int flags) {
	return IS_ROUT(flags) || IS_RAPPEND(flags);
}
bool has_redir(command *com, bool (*pred)(int)) {
	if (!com) return false;
	redirseq *s = com->redirs;
	if (!s) return false;

	for (redirseq *rs = s; ; rs = rs->next) {
		if (rs->r && pred(rs->r->flags)) return true;
		if (!rs->next || rs->next == s) break;
	}
	return false;
}
int build_argv(command *com, char ***argv, int *argc) {
	if (!argv) return -1;
	*argv= NULL;
	if (!com || !com->args)
		return 0;
	argseq *start = com->args;
	int argcnt=1;
	for (argseq *a = start ? start->next : NULL; a && a != start; a = a->next) argcnt++;
	char **argvv = malloc(((size_t)argcnt + 1) * sizeof(char*));
	if (!argvv) return -1;
	int i = 0;
	argvv[i++] = start->arg;
	for (argseq *a = start->next; a != start; a = a->next) argvv[i++] = a->arg;
	argvv[i] = NULL;
	if (argc) *argc = argcnt;
	*argv=argvv;
	return argcnt;
}
void run_pipeline(pipeline *pl, int run_bg) {
	if (!pl || !pl->commands) return;

	int N = 0;
	commandseq *head = pl->commands, *node = head;
	do {
		N++;
		node = node->next;
	} while (node && node != head);
	if (N >= 2) {
		node = head;
		do {
			if (!node->com || !node->com->args) { fprintf(stderr, "%s\n", SYNTAX_ERROR_STR); return; }
			node = node->next;
		}
		while (node && node != head);
	}
	if (N == 0) return;
	if (N == 1) {
		command *com = head->com;
		if (!com || !com->args) return;

		char **argvv = NULL;
		int argcnt = 0;
		if (build_argv(com, &argvv, &argcnt) <= 0) return;

		if (!run_bg && run_if_builtin(argvv)) {
			fflush(stdout);
			free(argvv);
			return;
		}

		pid_t ch = fork();
		if (ch == 0) {
			struct sigaction dfl = {0};
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			sigaction(SIGINT, &dfl, NULL);
			if (run_bg) setpgid(0, 0);
			redirect(com);
			execvp(argvv[0], argvv);
			report_exec_error(argvv[0]);
			free(argvv);
			_exit(EXEC_FAILURE);
		} else if (ch > 0) {
			if (run_bg) {
				setpgid(ch, ch);
				bg_add(ch);
			} else {
				while (waitpid(ch, NULL, 0) < 0 && errno == EINTR) {}
			}
			free(argvv);
		}
		return;
	}


	node = head;
	pid_t pids[N];
	int prev_in = -1;

	int idx= 0;
	do
	{
		command *com = node->com;
		if (!com || !com->args) { pids[idx++] = -1; node = node->next; continue; }
		int curr[2]={-1,-1};
		bool last = idx == N-1;

		if (!last) {
			if (pipe(curr) < 0) {
				perror("pipe");
				if (prev_in != -1) close(prev_in);
				int left = 0; for (int i = 0; i < idx; i++) if (pids[i] > 0) left++;
				while (left > 0) {
					pid_t p = waitpid(-1, NULL, 0);
					if (p < 0) { if (errno == EINTR) continue; break; }
					for (int j = 0; j < idx; j++) if (pids[j] == p) { pids[j] = -1; left--; break; }
				}
				return;
			}
		}
		pid_t ch = fork();
		if (ch == 0) {
			struct sigaction dfl = {0};
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			sigaction(SIGINT, &dfl, NULL);

			if (run_bg) setpgid(0, 0);
			bool inR = false, outR = false;
			redirseq *rs0 = com->redirs;
			if (rs0) {
				redirseq *rs = rs0;
				do {
					if (rs->r) {
						int f = rs->r->flags;
						inR|=IS_RIN(f);
						outR|=(IS_ROUT(f) || IS_RAPPEND(f));
					}
					rs = rs->next;
				} while (rs && rs != rs0);
			}
			redirect(com);
			if (!inR && idx>0) {
				if (dup2(prev_in, STDIN_FILENO) <0) { perror("dup2"); _exit(EXEC_FAILURE); }
			}
			if (!outR && !last) {
				if (dup2(curr[1], STDOUT_FILENO) < 0) { perror("dup2"); _exit(EXEC_FAILURE); }
			}
			if (prev_in != -1) close(prev_in);
			if (!last) {
				close(curr[0]);
				close(curr[1]);
			}
			char **argvv = NULL; int argcnt = 0;
			if (build_argv(com, &argvv, &argcnt) <= 0) _exit(EXEC_FAILURE);

			execvp(argvv[0], argvv);
			report_exec_error(argvv[0]);
			free(argvv);
			_exit(EXEC_FAILURE);
		}
		else if (ch > 0) {
			pids[idx++] = ch;

			if (run_bg) {
				setpgid(ch, ch);
				bg_add(ch);
			}
			if (prev_in != -1) { close(prev_in); prev_in = -1; }
			if (!last) {
				close(curr[1]);
				prev_in = curr[0];
			}

			node = node->next;
		} else {
			perror("fork");
			if (prev_in != -1) close(prev_in);
			if (!last) {
				close(curr[0]);
				close(curr[1]);
			}

			int left = 0;
			for (int i = 0; i < idx; i++) if (pids[i] > 0) left++;
			while (left > 0) {
				pid_t p = waitpid(-1, NULL, 0);
				if (p < 0) {
					if (errno == EINTR) continue;
					break;
				}
				for (int i = 0; i < idx; i++) {
					if (pids[i] == p) {
						pids[i] = -1;
						left--;
						break;
					}
				}
			}
			return;
		}
	}
	while (node && node != head);
	if (!run_bg) {
		int left = 0;
		for (int i = 0; i < N; i++)
			if (pids[i] > 0) left++;


		while (left > 0) {
			int st;
			pid_t p = waitpid(-1, &st, 0);
			if (p < 0) {
				if (errno == EINTR) continue;
				break;
			}
			for (int i = 0; i < N; i++) {
				if (pids[i] == p) { pids[i] = -1; left--; break; }
			}
		}
	}

}
void run_line(char* buf){
	pipelineseq *ln = parseline(buf);

	if (!ln) { fprintf(stderr, "%s\n", SYNTAX_ERROR_STR); return; }
	pipelineseq *pp=ln;

	do {
		pipeline *pl=pp->pipeline;
		int run_bg=(pl && (pl->flags & INBACKGROUND));
		if (pl && pl->commands) {
			commandseq* cs = pl->commands;
			int cnt=0;
			do { cnt++; cs = cs->next; } while (cs && cs != pl->commands);
			cs = pl->commands;
			if (cnt>=2)
			do {

				if (!cs->com || !cs->com->args) { fprintf(stderr, "%s\n", SYNTAX_ERROR_STR); return; }
				cs=cs->next;
			}while (cs && cs != pl->commands);
		}
		pp=pp->next;
	}while (pp && pp!=ln);
	pp=ln;
	do {
		pipeline *pipe=pp->pipeline;
		int run_bg=(pipe && (pipe->flags & INBACKGROUND));
		run_pipeline(pipe, run_bg);
		pp=pp->next;
	}while (pp && pp!=ln);

}


int main(int argc, char *argv[])
{
	struct sigaction sa_int = {0};
	sa_int.sa_handler = SIG_IGN;
	sigemptyset(&sa_int.sa_mask);
	sigaction(SIGINT, &sa_int, NULL);

	struct sigaction sa = {0};
	sa.sa_handler = sigchld_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	pipelineseq * ln;
	command *com;

	char buf[MAX_LINE_LENGTH+1];
	char line[MAX_LINE_LENGTH+1];
	struct stat st;
	bool print_prompt_str=false;
	if(fstat(STDIN_FILENO, &st) == 0){
		print_prompt_str = S_ISCHR(st.st_mode);
  	}
	int current_len=0;
	bool too_long=false;
	while (true){


		//printparsedline(ln);
		if(print_prompt_str && current_len == 0){
			flush_bg_notices();
		    printf("%s", PROMPT_STR);
		    fflush(stdout);
		}

		int r = read(STDIN_FILENO, buf, MAX_LINE_LENGTH);
		if(r<0){
			if (errno == EINTR) continue;
    		break;
		}
        if (r == 0) {
            if (current_len > 0) {
                if (!too_long) {
                    line[current_len] = '\0';
                    run_line(line);
                } else {
                    fprintf(stderr, "%s\n", SYNTAX_ERROR_STR);
                }
            }
            break;
       }
		int pos=0;
		while (pos<r){
			char *newline=memchr(buf+ pos, '\n', r-pos);
			int part=newline ? newline-(buf + pos) : r-pos;
			int dist=part;
			if (!too_long) {
				int cap=MAX_LINE_LENGTH - current_len;
				if (part>cap) {
					part=cap;
					too_long=true;
				}
				memcpy(line+current_len, buf + pos, part);
				current_len+=part;

			}
			if (newline) {
				if (too_long) {
					fprintf(stderr, "%s\n", SYNTAX_ERROR_STR);
				}
				else if (current_len>0){
					line[current_len]='\0';
					run_line(line);
				}
				current_len = 0;
				too_long = false;
				pos += dist+1;
			}
			else {
				pos+=r-pos;
			}
		}

        /*if (too_long) { fprintf(stderr, "%s\n", SYNTAX_ERROR_STR); continue; }
		too_long = false;*/

	}

	return 0;


	ln = parseline("ls -las | grep k | wc ; echo abc > f1 ;  cat < f2 ; echo abc >> f3\n");
	printparsedline(ln);
	printf("\n");
	com = pickfirstcommand(ln);
	printcommand(com,1);

	ln = parseline("sleep 3 &");
	printparsedline(ln);
	printf("\n");
	
	ln = parseline("echo  & abc >> f3\n");
	printparsedline(ln);
	printf("\n");
	com = pickfirstcommand(ln);
	printcommand(com,1);
}
