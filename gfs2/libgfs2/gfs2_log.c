#include "clusterautoconfig.h"

#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/select.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "libgfs2.h"

int print_level = MSG_NOTICE;

void increase_verbosity(void)
{
	print_level++;
}

void decrease_verbosity(void)
{
	print_level--;
}
