#ifndef _LINUX_KERNEL_TRACE_H
#define _LINUX_KERNEL_TRACE_H

#include <linux/fs.h>
#include <asm/atomic.h>
#include <linux/sched.h>
#include <linux/clocksource.h>
#include <linux/ring_buffer.h>
#include <linux/mmiotrace.h>
#include <linux/ftrace.h>
#include <trace/boot.h>

enum trace_type {
	__TRACE_FIRST_TYPE = 0,

	TRACE_FN,
	TRACE_CTX,
	TRACE_WAKE,
	TRACE_CONT,
	TRACE_STACK,
	TRACE_PRINT,
	TRACE_SPECIAL,
	TRACE_MMIO_RW,
	TRACE_MMIO_MAP,
	TRACE_BRANCH,
	TRACE_BOOT_CALL,
	TRACE_BOOT_RET,
	TRACE_GRAPH_RET,
	TRACE_GRAPH_ENT,
	TRACE_USER_STACK,
	TRACE_HW_BRANCHES,
	TRACE_POWER,

	__TRACE_LAST_TYPE
};

/*
 * The trace entry - the most basic unit of tracing. This is what
 * is printed in the end as a single line in the trace output, such as:
 *
 *     bash-15816 [01]   235.197585: idle_cpu <- irq_enter
 */
struct trace_entry {
	unsigned char		type;
	unsigned char		cpu;
	unsigned char		flags;
	unsigned char		preempt_count;
	int			pid;
	int			tgid;
};

/*
 * Function trace entry - function address and parent function addres:
 */
struct ftrace_entry {
	struct trace_entry	ent;
	unsigned long		ip;
	unsigned long		parent_ip;
};

/* Function call entry */
struct ftrace_graph_ent_entry {
	struct trace_entry			ent;
	struct ftrace_graph_ent		graph_ent;
};

/* Function return entry */
struct ftrace_graph_ret_entry {
	struct trace_entry			ent;
	struct ftrace_graph_ret		ret;
};
extern struct tracer boot_tracer;

/*
 * Context switch trace entry - which task (and prio) we switched from/to:
 */
struct ctx_switch_entry {
	struct trace_entry	ent;
	unsigned int		prev_pid;
	unsigned char		prev_prio;
	unsigned char		prev_state;
	unsigned int		next_pid;
	unsigned char		next_prio;
	unsigned char		next_state;
	unsigned int		next_cpu;
};

/*
 * Special (free-form) trace entry:
 */
struct special_entry {
	struct trace_entry	ent;
	unsigned long		arg1;
	unsigned long		arg2;
	unsigned long		arg3;
};

/*
 * Stack-trace entry:
 */

#define FTRACE_STACK_ENTRIES	8

struct stack_entry {
	struct trace_entry	ent;
	unsigned long		caller[FTRACE_STACK_ENTRIES];
};

struct userstack_entry {
	struct trace_entry	ent;
	unsigned long		caller[FTRACE_STACK_ENTRIES];
};

/*
 * ftrace_printk entry:
 */
struct print_entry {
	struct trace_entry	ent;
	unsigned long		ip;
	int			depth;
	char			buf[];
};

#define TRACE_OLD_SIZE		88

struct trace_field_cont {
	unsigned char		type;
	/* Temporary till we get rid of this completely */
	char			buf[TRACE_OLD_SIZE - 1];
};

struct trace_mmiotrace_rw {
	struct trace_entry	ent;
	struct mmiotrace_rw	rw;
};

struct trace_mmiotrace_map {
	struct trace_entry	ent;
	struct mmiotrace_map	map;
};

struct trace_boot_call {
	struct trace_entry	ent;
	struct boot_trace_call boot_call;
};

struct trace_boot_ret {
	struct trace_entry	ent;
	struct boot_trace_ret boot_ret;
};

#define TRACE_FUNC_SIZE 30
#define TRACE_FILE_SIZE 20
struct trace_branch {
	struct trace_entry	ent;
	unsigned	        line;
	char			func[TRACE_FUNC_SIZE+1];
	char			file[TRACE_FILE_SIZE+1];
	char			correct;
};

struct hw_branch_entry {
	struct trace_entry	ent;
	u64			from;
	u64			to;
};

struct trace_power {
	struct trace_entry	ent;
	struct power_trace	state_data;
};

/*
 * trace_flag_type is an enumeration that holds different
 * states when a trace occurs. These are:
 *  IRQS_OFF		- interrupts were disabled
 *  IRQS_NOSUPPORT 	- arch does not support irqs_disabled_flags
 *  NEED_RESCED		- reschedule is requested
 *  HARDIRQ		- inside an interrupt handler
 *  SOFTIRQ		- inside a softirq handler
 *  CONT		- multiple entries hold the trace item
 */
enum trace_flag_type {
	TRACE_FLAG_IRQS_OFF		= 0x01,
	TRACE_FLAG_IRQS_NOSUPPORT	= 0x02,
	TRACE_FLAG_NEED_RESCHED		= 0x04,
	TRACE_FLAG_HARDIRQ		= 0x08,
	TRACE_FLAG_SOFTIRQ		= 0x10,
	TRACE_FLAG_CONT			= 0x20,
};

#define TRACE_BUF_SIZE		1024

/*
 * The CPU trace array - it consists of thousands of trace entries
 * plus some other descriptor data: (for example which task started
 * the trace, etc.)
 */
struct trace_array_cpu {
	atomic_t		disabled;

	/* these fields get copied into max-trace: */
	unsigned long		trace_idx;
	unsigned long		overrun;
	unsigned long		saved_latency;
	unsigned long		critical_start;
	unsigned long		critical_end;
	unsigned long		critical_sequence;
	unsigned long		nice;
	unsigned long		policy;
	unsigned long		rt_priority;
	cycle_t			preempt_timestamp;
	pid_t			pid;
	uid_t			uid;
	char			comm[TASK_COMM_LEN];
};

struct trace_iterator;

/*
 * The trace array - an array of per-CPU trace arrays. This is the
 * highest level data structure that individual tracers deal with.
 * They have on/off state as well:
 */
struct trace_array {
	struct ring_buffer	*buffer;
	unsigned long		entries;
	int			cpu;
	cycle_t			time_start;
	struct task_struct	*waiter;
	struct trace_array_cpu	*data[NR_CPUS];
};

#define FTRACE_CMP_TYPE(var, type) \
	__builtin_types_compatible_p(typeof(var), type *)

#undef IF_ASSIGN
#define IF_ASSIGN(var, entry, etype, id)		\
	if (FTRACE_CMP_TYPE(var, etype)) {		\
		var = (typeof(var))(entry);		\
		WARN_ON(id && (entry)->type != id);	\
		break;					\
	}

/* Will cause compile errors if type is not found. */
extern void __ftrace_bad_type(void);

/*
 * The trace_assign_type is a verifier that the entry type is
 * the same as the type being assigned. To add new types simply
 * add a line with the following format:
 *
 * IF_ASSIGN(var, ent, type, id);
 *
 *  Where "type" is the trace type that includes the trace_entry
 *  as the "ent" item. And "id" is the trace identifier that is
 *  used in the trace_type enum.
 *
 *  If the type can have more than one id, then use zero.
 */
#define trace_assign_type(var, ent)					\
	do {								\
		IF_ASSIGN(var, ent, struct ftrace_entry, TRACE_FN);	\
		IF_ASSIGN(var, ent, struct ctx_switch_entry, 0);	\
		IF_ASSIGN(var, ent, struct trace_field_cont, TRACE_CONT); \
		IF_ASSIGN(var, ent, struct stack_entry, TRACE_STACK);	\
		IF_ASSIGN(var, ent, struct userstack_entry, TRACE_USER_STACK);\
		IF_ASSIGN(var, ent, struct print_entry, TRACE_PRINT);	\
		IF_ASSIGN(var, ent, struct special_entry, 0);		\
		IF_ASSIGN(var, ent, struct trace_mmiotrace_rw,		\
			  TRACE_MMIO_RW);				\
		IF_ASSIGN(var, ent, struct trace_mmiotrace_map,		\
			  TRACE_MMIO_MAP);				\
		IF_ASSIGN(var, ent, struct trace_boot_call, TRACE_BOOT_CALL);\
		IF_ASSIGN(var, ent, struct trace_boot_ret, TRACE_BOOT_RET);\
		IF_ASSIGN(var, ent, struct trace_branch, TRACE_BRANCH); \
		IF_ASSIGN(var, ent, struct ftrace_graph_ent_entry,	\
			  TRACE_GRAPH_ENT);		\
		IF_ASSIGN(var, ent, struct ftrace_graph_ret_entry,	\
			  TRACE_GRAPH_RET);		\
		IF_ASSIGN(var, ent, struct hw_branch_entry, TRACE_HW_BRANCHES);\
 		IF_ASSIGN(var, ent, struct trace_power, TRACE_POWER); \
		__ftrace_bad_type();					\
	} while (0)

/* Return values for print_line callback */
enum print_line_t {
	TRACE_TYPE_PARTIAL_LINE	= 0,	/* Retry after flushing the seq */
	TRACE_TYPE_HANDLED	= 1,
	TRACE_TYPE_UNHANDLED	= 2	/* Relay to other output functions */
};


/*
 * An option specific to a tracer. This is a boolean value.
 * The bit is the bit index that sets its value on the
 * flags value in struct tracer_flags.
 */
struct tracer_opt {
	const char 	*name; /* Will appear on the trace_options file */
	u32 		bit; /* Mask assigned in val field in tracer_flags */
};

/*
 * The set of specific options for a tracer. Your tracer
 * have to set the initial value of the flags val.
 */
struct tracer_flags {
	u32			val;
	struct tracer_opt 	*opts;
};

/* Makes more easy to define a tracer opt */
#define TRACER_OPT(s, b)	.name = #s, .bit = b

/*
 * A specific tracer, represented by methods that operate on a trace array:
 */
struct tracer {
	const char		*name;
	/* Your tracer should raise a warning if init fails */
	int			(*init)(struct trace_array *tr);
	void			(*reset)(struct trace_array *tr);
	void			(*start)(struct trace_array *tr);
	void			(*stop)(struct trace_array *tr);
	void			(*open)(struct trace_iterator *iter);
	void			(*pipe_open)(struct trace_iterator *iter);
	void			(*close)(struct trace_iterator *iter);
	ssize_t			(*read)(struct trace_iterator *iter,
					struct file *filp, char __user *ubuf,
					size_t cnt, loff_t *ppos);
#ifdef CONFIG_FTRACE_STARTUP_TEST
	int			(*selftest)(struct tracer *trace,
					    struct trace_array *tr);
#endif
	void			(*print_header)(struct seq_file *m);
	enum print_line_t	(*print_line)(struct trace_iterator *iter);
	/* If you handled the flag setting, return 0 */
	int			(*set_flag)(u32 old_flags, u32 bit, int set);
	struct tracer		*next;
	int			print_max;
	struct tracer_flags 	*flags;
};

struct trace_seq {
	unsigned char		buffer[PAGE_SIZE];
	unsigned int		len;
	unsigned int		readpos;
};

/*
 * Trace iterator - used by printout routines who present trace
 * results to users and which routines might sleep, etc:
 */
struct trace_iterator {
	struct trace_array	*tr;
	struct tracer		*trace;
	void			*private;
	struct ring_buffer_iter	*buffer_iter[NR_CPUS];

	/* The below is zeroed out in pipe_read */
	struct trace_seq	seq;
	struct trace_entry	*ent;
	int			cpu;
	u64			ts;

	unsigned long		iter_flags;
	loff_t			pos;
	long			idx;

	cpumask_t		started;
};

int tracing_is_enabled(void);
void trace_wake_up(void);
void tracing_reset(struct trace_array *tr, int cpu);
void tracing_reset_online_cpus(struct trace_array *tr);
int tracing_open_generic(struct inode *inode, struct file *filp);
struct dentry *tracing_init_dentry(void);
void init_tracer_sysprof_debugfs(struct dentry *d_tracer);

struct trace_entry *tracing_get_trace_entry(struct trace_array *tr,
						struct trace_array_cpu *data);
void tracing_generic_entry_update(struct trace_entry *entry,
				  unsigned long flags,
				  int pc);

void ftrace(struct trace_array *tr,
			    struct trace_array_cpu *data,
			    unsigned long ip,
			    unsigned long parent_ip,
			    unsigned long flags, int pc);
void tracing_sched_switch_trace(struct trace_array *tr,
				struct trace_array_cpu *data,
				struct task_struct *prev,
				struct task_struct *next,
				unsigned long flags, int pc);
void tracing_record_cmdline(struct task_struct *tsk);

void tracing_sched_wakeup_trace(struct trace_array *tr,
				struct trace_array_cpu *data,
				struct task_struct *wakee,
				struct task_struct *cur,
				unsigned long flags, int pc);
void trace_special(struct trace_array *tr,
		   struct trace_array_cpu *data,
		   unsigned long arg1,
		   unsigned long arg2,
		   unsigned long arg3, int pc);
void trace_function(struct trace_array *tr,
		    struct trace_array_cpu *data,
		    unsigned long ip,
		    unsigned long parent_ip,
		    unsigned long flags, int pc);

void trace_graph_return(struct ftrace_graph_ret *trace);
int trace_graph_entry(struct ftrace_graph_ent *trace);
void trace_hw_branch(struct trace_array *tr, u64 from, u64 to);

void tracing_start_cmdline_record(void);
void tracing_stop_cmdline_record(void);
void tracing_sched_switch_assign_trace(struct trace_array *tr);
void tracing_stop_sched_switch_record(void);
void tracing_start_sched_switch_record(void);
int register_tracer(struct tracer *type);
void unregister_tracer(struct tracer *type);

extern unsigned long nsecs_to_usecs(unsigned long nsecs);

extern unsigned long tracing_max_latency;
extern unsigned long tracing_thresh;

void update_max_tr(struct trace_array *tr, struct task_struct *tsk, int cpu);
void update_max_tr_single(struct trace_array *tr,
			  struct task_struct *tsk, int cpu);

extern cycle_t ftrace_now(int cpu);

#ifdef CONFIG_FUNCTION_TRACER
void tracing_start_function_trace(void);
void tracing_stop_function_trace(void);
#else
# define tracing_start_function_trace()		do { } while (0)
# define tracing_stop_function_trace()		do { } while (0)
#endif

#ifdef CONFIG_CONTEXT_SWITCH_TRACER
typedef void
(*tracer_switch_func_t)(void *private,
			void *__rq,
			struct task_struct *prev,
			struct task_struct *next);

struct tracer_switch_ops {
	tracer_switch_func_t		func;
	void				*private;
	struct tracer_switch_ops	*next;
};

char *trace_find_cmdline(int pid);
#endif /* CONFIG_CONTEXT_SWITCH_TRACER */

#ifdef CONFIG_DYNAMIC_FTRACE
extern unsigned long ftrace_update_tot_cnt;
#define DYN_FTRACE_TEST_NAME trace_selftest_dynamic_test_func
extern int DYN_FTRACE_TEST_NAME(void);
#endif

#ifdef CONFIG_FTRACE_STARTUP_TEST
extern int trace_selftest_startup_function(struct tracer *trace,
					   struct trace_array *tr);
extern int trace_selftest_startup_irqsoff(struct tracer *trace,
					  struct trace_array *tr);
extern int trace_selftest_startup_preemptoff(struct tracer *trace,
					     struct trace_array *tr);
extern int trace_selftest_startup_preemptirqsoff(struct tracer *trace,
						 struct trace_array *tr);
extern int trace_selftest_startup_wakeup(struct tracer *trace,
					 struct trace_array *tr);
extern int trace_selftest_startup_nop(struct tracer *trace,
					 struct trace_array *tr);
extern int trace_selftest_startup_sched_switch(struct tracer *trace,
					       struct trace_array *tr);
extern int trace_selftest_startup_sysprof(struct tracer *trace,
					       struct trace_array *tr);
extern int trace_selftest_startup_branch(struct tracer *trace,
					 struct trace_array *tr);
#endif /* CONFIG_FTRACE_STARTUP_TEST */

extern void *head_page(struct trace_array_cpu *data);
extern int trace_seq_printf(struct trace_seq *s, const char *fmt, ...);
extern void trace_seq_print_cont(struct trace_seq *s,
				 struct trace_iterator *iter);

extern int
seq_print_ip_sym(struct trace_seq *s, unsigned long ip,
		unsigned long sym_flags);
extern ssize_t trace_seq_to_user(struct trace_seq *s, char __user *ubuf,
				 size_t cnt);
extern long ns2usecs(cycle_t nsec);
extern int
trace_vprintk(unsigned long ip, int depth, const char *fmt, va_list args);

extern unsigned long trace_flags;

/* Standard output formatting function used for function return traces */
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
extern enum print_line_t print_graph_function(struct trace_iterator *iter);

#ifdef CONFIG_DYNAMIC_FTRACE
/* TODO: make this variable */
#define FTRACE_GRAPH_MAX_FUNCS		32
extern int ftrace_graph_count;
extern unsigned long ftrace_graph_funcs[FTRACE_GRAPH_MAX_FUNCS];

static inline int ftrace_graph_addr(unsigned long addr)
{
	int i;

	if (!ftrace_graph_count || test_tsk_trace_graph(current))
		return 1;

	for (i = 0; i < ftrace_graph_count; i++) {
		if (addr == ftrace_graph_funcs[i])
			return 1;
	}

	return 0;
}
#else
static inline int ftrace_trace_addr(unsigned long addr)
{
	return 1;
}
static inline int ftrace_graph_addr(unsigned long addr)
{
	return 1;
}
#endif /* CONFIG_DYNAMIC_FTRACE */

#else /* CONFIG_FUNCTION_GRAPH_TRACER */
static inline enum print_line_t
print_graph_function(struct trace_iterator *iter)
{
	return TRACE_TYPE_UNHANDLED;
}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

extern struct pid *ftrace_pid_trace;

static inline int ftrace_trace_task(struct task_struct *task)
{
	if (!ftrace_pid_trace)
		return 1;

	return test_tsk_trace_trace(task);
}

/*
 * trace_iterator_flags is an enumeration that defines bit
 * positions into trace_flags that controls the output.
 *
 * NOTE: These bits must match the trace_options array in
 *       trace.c.
 */
enum trace_iterator_flags {
	TRACE_ITER_PRINT_PARENT		= 0x01,
	TRACE_ITER_SYM_OFFSET		= 0x02,
	TRACE_ITER_SYM_ADDR		= 0x04,
	TRACE_ITER_VERBOSE		= 0x08,
	TRACE_ITER_RAW			= 0x10,
	TRACE_ITER_HEX			= 0x20,
	TRACE_ITER_BIN			= 0x40,
	TRACE_ITER_BLOCK		= 0x80,
	TRACE_ITER_STACKTRACE		= 0x100,
	TRACE_ITER_SCHED_TREE		= 0x200,
	TRACE_ITER_PRINTK		= 0x400,
	TRACE_ITER_PREEMPTONLY		= 0x800,
	TRACE_ITER_BRANCH		= 0x1000,
	TRACE_ITER_ANNOTATE		= 0x2000,
	TRACE_ITER_USERSTACKTRACE       = 0x4000,
	TRACE_ITER_SYM_USEROBJ          = 0x8000,
	TRACE_ITER_PRINTK_MSGONLY	= 0x10000
};

/*
 * TRACE_ITER_SYM_MASK masks the options in trace_flags that
 * control the output of kernel symbols.
 */
#define TRACE_ITER_SYM_MASK \
	(TRACE_ITER_PRINT_PARENT|TRACE_ITER_SYM_OFFSET|TRACE_ITER_SYM_ADDR)

extern struct tracer nop_trace;

/**
 * ftrace_preempt_disable - disable preemption scheduler safe
 *
 * When tracing can happen inside the scheduler, there exists
 * cases that the tracing might happen before the need_resched
 * flag is checked. If this happens and the tracer calls
 * preempt_enable (after a disable), a schedule might take place
 * causing an infinite recursion.
 *
 * To prevent this, we read the need_recshed flag before
 * disabling preemption. When we want to enable preemption we
 * check the flag, if it is set, then we call preempt_enable_no_resched.
 * Otherwise, we call preempt_enable.
 *
 * The rational for doing the above is that if need resched is set
 * and we have yet to reschedule, we are either in an atomic location
 * (where we do not need to check for scheduling) or we are inside
 * the scheduler and do not want to resched.
 */
static inline int ftrace_preempt_disable(void)
{
	int resched;

	resched = need_resched();
	preempt_disable_notrace();

	return resched;
}

/**
 * ftrace_preempt_enable - enable preemption scheduler safe
 * @resched: the return value from ftrace_preempt_disable
 *
 * This is a scheduler safe way to enable preemption and not miss
 * any preemption checks. The disabled saved the state of preemption.
 * If resched is set, then we were either inside an atomic or
 * are inside the scheduler (we would have already scheduled
 * otherwise). In this case, we do not want to call normal
 * preempt_enable, but preempt_enable_no_resched instead.
 */
static inline void ftrace_preempt_enable(int resched)
{
	if (resched)
		preempt_enable_no_resched_notrace();
	else
		preempt_enable_notrace();
}

#ifdef CONFIG_BRANCH_TRACER
extern int enable_branch_tracing(struct trace_array *tr);
extern void disable_branch_tracing(void);
static inline int trace_branch_enable(struct trace_array *tr)
{
	if (trace_flags & TRACE_ITER_BRANCH)
		return enable_branch_tracing(tr);
	return 0;
}
static inline void trace_branch_disable(void)
{
	/* due to races, always disable */
	disable_branch_tracing();
}
#else
static inline int trace_branch_enable(struct trace_array *tr)
{
	return 0;
}
static inline void trace_branch_disable(void)
{
}
#endif /* CONFIG_BRANCH_TRACER */

#endif /* _LINUX_KERNEL_TRACE_H */
