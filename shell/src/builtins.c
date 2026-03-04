#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include<errno.h>
#include "builtins.h"
#include <signal.h>
#include <stdlib.h>
#include <dirent.h>
int echo(char*[]);
int undefined(char *[]);
int er(char*[]);
int lexit(char*[]);
int lkill(char*[]);
int lls(char*[]);
int lcd(char*[]);
builtin_pair builtins_table[]={
	{"exit",	&lexit},
	{"lecho",	&echo},
	{"lcd",		&lcd},
	{"lkill",	&lkill},
	{"lls",		&lls},
	{NULL,NULL}
};
int 
echo( char * argv[])
{
	int i =1;
	if (argv[i]) printf("%s", argv[i++]);
	while  (argv[i])
		printf(" %s", argv[i++]);

	printf("\n");
	fflush(stdout);
	return 0;
}
int lexit(char * argv[]) {
	if (argv[1]!=NULL) {
		return er(argv);
	}
	exit(0);
}
int lcd(char * argv[]) {
	const char *target;
	if (argv[1]==NULL) {
		target = getenv("HOME");
		if (!target || !*target) {
			return er(argv);
		}
	}
	else if (argv[2]!=NULL) {
		return er(argv);
	}
	else {
		target = argv[1];
	}
	if (chdir(target)==-1) {
		return er(argv);
	}
	return 0;

}
int to_long(const char *s, long *out) {
	if (!s || !*s) {
		return 0;
	}
	errno=0;
	char *end = NULL;
	long result = strtol(s, &end, 10);
	if (errno || *end) return 0;
	*out=result;
	return 1;
}
int lkill(char * argv[]) {
	int argc = 0;
	while (argv[argc]) {
		argc++;
	}

	int  sig = SIGTERM;
	long pid;
	long lsig;

	if (argc == 2) {
		if (!to_long(argv[1], &pid))
			return er(argv);
	}
	else if (argc == 3 && argv[1][0] == '-') {
		if (argv[1][1] == '\0' || !to_long(argv[1] + 1, &lsig) || !to_long(argv[2], &pid)) {
			return er(argv);
		}
		sig = (int)lsig;
	}
	else {
		return er(argv);
	}

	if (kill((pid_t)pid, sig) == -1)
		return er(argv);

	return 0;
}
int lls(char * argv[]) {
	if (argv[1]) return er(argv);
	DIR *dir = opendir(".");
	if (!dir) return er(argv);
	errno=0;
	struct dirent *d;
	while ((d=readdir(dir))!=NULL) {
		const char *name = d->d_name;
		if (name[0] == '.') continue;
		printf("%s\n", name);
		errno=0;
	}


	if (errno!=0 || closedir(dir) == -1) {
		return er(argv);
	}
	return 0;

}
int er(char * argv[]) {
	fprintf(stderr, "Builtin %s error.\n", argv[0]);
	return BUILTIN_ERROR;
}
int 
undefined(char * argv[])
{
	fprintf(stderr, "Command %s undefined.\n", argv[0]);
	return BUILTIN_ERROR;
}
