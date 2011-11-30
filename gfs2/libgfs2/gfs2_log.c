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

static __attribute__((format (printf, 4, 0)))
void print_msg(int priority, const char *file, int line,
	       const char *format, va_list args) {

	switch (priority) {

	case MSG_DEBUG:
		printf("(%s:%d) ", file, line);
		vprintf(format, args);
		break;
	case MSG_INFO:
	case MSG_NOTICE:
	case MSG_WARN:
		vprintf(format, args);
		fflush(NULL);
		break;
	case MSG_ERROR:
	case MSG_CRITICAL:
	default:
		vfprintf(stderr, format, args);
		break;
	}
}


void print_fsck_log(int priority, const char *file, int line,
		    const char *format, ...)
{
	va_list args;
	va_start(args, format);
	print_msg(priority, file, line, format, args);
	va_end(args);
}

char gfs2_getch(void)
{
	struct termios termattr, savetermattr;
	char ch;
	ssize_t size;

	tcgetattr (STDIN_FILENO, &termattr);
	savetermattr = termattr;
	termattr.c_lflag &= ~(ICANON | IEXTEN | ISIG);
	termattr.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	termattr.c_cflag &= ~(CSIZE | PARENB);
	termattr.c_cflag |= CS8;
	termattr.c_oflag &= ~(OPOST);
   	termattr.c_cc[VMIN] = 0;
	termattr.c_cc[VTIME] = 0;

	tcsetattr (STDIN_FILENO, TCSANOW, &termattr);
	do {
		size = read(STDIN_FILENO, &ch, 1);
		if (size)
			break;
		usleep(50000);
	} while (!size);

	tcsetattr (STDIN_FILENO, TCSANOW, &savetermattr);
	return ch;
}
