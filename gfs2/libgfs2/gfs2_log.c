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

char generic_interrupt(const char *caller, const char *where,
		       const char *progress, const char *question,
		       const char *answers)
{
	fd_set rfds;
	struct timeval tv;
	char response;
	int err, i;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	/* Make sure there isn't extraneous input before asking the
	 * user the question */
	while((err = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv))) {
		if(err < 0) {
			log_debug("Error in select() on stdin\n");
			break;
		}
		if(read(STDIN_FILENO, &response, sizeof(char)) < 0) {
			log_debug("Error in read() on stdin\n");
			break;
		}
	}
	while (TRUE) {
		printf("\n%s interrupted during %s:  ", caller, where);
		if (progress)
			printf("%s.\n", progress);
		printf("%s", question);

		/* Make sure query is printed out */
		fflush(NULL);
		response = gfs2_getch();
		printf("\n");
		fflush(NULL);
		if (strchr(answers, response))
			break;
		printf("Bad response, please type ");
		for (i = 0; i < strlen(answers) - 1; i++)
			printf("'%c', ", answers[i]);
		printf(" or '%c'.\n", answers[i]);
	}
	return response;
}

