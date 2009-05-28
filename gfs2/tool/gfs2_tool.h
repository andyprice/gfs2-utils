#ifndef __GFS2_TOOL_DOT_H__
#define __GFS2_TOOL_DOT_H__


#define OUTPUT_BLOCKS 0
#define OUTPUT_K      1
#define OUTPUT_HUMAN  2

extern char *action;
extern int override;
extern int expert;
extern int debug;
extern int continuous;
extern int interval;
extern int output_type;

/* From counters.c */

void print_counters(int argc, char **argv);


/* From main.c */

void print_usage(void);


/* From misc.c */

void do_freeze(int argc, char **argv);
void print_lockdump(int argc, char **argv);
void set_flag(int argc, char **argv);
void print_stat(int argc, char **argv);
void print_sb(int argc, char **argv);
void print_jindex(int argc, char **argv);
void print_journals(int argc, char **argv);
void print_rindex(int argc, char **argv);
void print_quota(int argc, char **argv);
void do_withdraw(int argc, char **argv);


/* From sb.c */

void do_sb(int argc, char **argv);


/* From tune.c */

void get_tune(int argc, char **argv);
void set_tune(int argc, char **argv);

#endif /* __GFS2_TOOL_DOT_H__ */
