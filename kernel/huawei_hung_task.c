/*
 * Detect Hung Task
 *
 * kernel/hisi_hung_task.c - kernel thread for detecting tasks stuck in D state
 *
 */

#include <linux/mm.h>
#include <linux/cpu.h>
#include <linux/nmi.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/lockdep.h>
#include <linux/export.h>
#include <linux/sysctl.h>
#include <linux/utsname.h>
#include <trace/events/sched.h>

#include <huawei_platform/log/log_jank.h>
#include <linux/ptrace.h>
#include <linux/module.h>
#include <linux/sched.h>

#include <linux/proc_fs.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <asm/traps.h>
#include <linux/jiffies.h>
#include <linux/suspend.h>
#ifdef CONFIG_HISI_BB
#include <linux/hisi/rdr_hisi_ap_hook.h>
#endif

#ifdef CONFIG_TZDRIVER
#include <linux/hisi/hisi_teeos.h>
#endif
#include <huawei_platform/log/imonitor.h>
#include <huawei_platform/log/imonitor_keys.h>
/****************************MICRO DEFINITION*********************************/
#define ENABLE_SHOW_LEN         8
#define WHITELIST_STORE_LEN     200
#define IGNORELIST_STORE_LEN    500
#define TASKLIST_STORE_LEN      500

#define WHITE_LIST              1
#define BLACK_LIST              2

#define HT_ENABLE               1
#define HT_DISABLE              0

#define JANK_TASK_MAXNUM        8
#define HEARTBEAT_TIME          3
#define MAX_LOOP_NUM            (CONFIG_DEFAULT_HUNG_TASK_TIMEOUT/\
						HEARTBEAT_TIME)
#define ONE_MINUTE              (60/HEARTBEAT_TIME)
#define ONE_AND_HALF_MINUTE     (90/HEARTBEAT_TIME)
#define THREE_MINUTE		(180/HEARTBEAT_TIME)
#define THIRTY_SECONDS		(30/HEARTBEAT_TIME)
#define HUNG_ONE_HOUR		(3600/HEARTBEAT_TIME)
#define HUNG_TEN_MINUTES	(600/HEARTBEAT_TIME)

#define HUNGTASK_DUMP_IN_PANIC_LOOSE	3
#define HUNGTASK_DUMP_IN_PANIC_STRICT	2
#define MAX_DUMP_TIMES		10

#define FLAG_NONE               0
#define FLAG_DUMP_WHITE         1
#define FLAG_DUMP_APP           2
#define FLAG_DUMP_NOSCHEDULE    4
#define FLAG_DUMP_JANK          8
#define FLAG_PANIC              16

#define TASK_TYPE_IGNORE        0
#define TASK_TYPE_WHITE         1
#define TASK_TYPE_APP           2	/*android java process, and should
				not be TASK_TYPE_WHITE at the same time*/
#define TASK_TYPE_JANK          4	/*can combined with TASK_TYPE_APP or
					TASK_TYPE_WHITE at the same time*/
#define TASK_TYPE_KERNEL        8	/*it's kernel task*/
#define TASK_TYPE_NATIVE        16	/*it's native task*/
#define TASK_TYPE_NOSCHEDULE    32	/*it's android watchdog task*/

#define PID_INIT                1	/*PID for init process,always 1*/
#define PID_KTHREAD             2	/*PID for kernel kthreadd, always 2*/

#define DEFAULT_WHITE_DUMP_CNT          MAX_LOOP_NUM
#define DEFAULT_WHITE_PANIC_CNT         MAX_LOOP_NUM
#define DEFAULT_APP_DUMP_CNT            MAX_LOOP_NUM
#define DEFAULT_APP_PANIC_CNT           0
#define DEFAULT_NOSCHEDULE_PANIC_CNT    ONE_MINUTE
#define DEFAULT_JANK_DUMP_CNT           10
#define DEFAULT_OTHER_LOG_CNT		MAX_LOOP_NUM
#define HUNG_TASK_RECORDTIME_CNT	4
#define HUNG_TASK_UPLOAD_ONCE		1

#define IGN_STATE_INIT          1
#define IGN_STATE_FIRST         2
#define IGN_STATE_DONE          3

#define DSTATE_DUMP_EVENT	901004002

#define WATCHDOG_THREAD_NAME    "watchdog"
/*
 * Limit number of tasks checked in a batch.
 * This value controls the preemptibility of khungtaskd since preemption
 * is disabled during the critical section. It also controls the size of
 * the RCU grace period. So it needs to be upper-bound.
 */
#define HUNG_TASK_BATCHING      1024
#define TIME_REFRESH_PIDS	20
/****************************STRUCTURE DEFINITION*********************/
struct task_item {
	struct rb_node node;
	pid_t pid;
	pid_t tgid;
	char name[TASK_COMM_LEN + 1];
	unsigned long switchCount;
	unsigned int task_type;
	int dump_wa;
	int panic_wa;
	int dump_jank;
	int time_in_D_state;
	bool isDonewa;
	bool isDonejank;
};
struct list_item {
	struct rb_node node;
	pid_t pid;
	char name[TASK_COMM_LEN + 1];
};
struct task_hung_upload {
	char  name[TASK_COMM_LEN + 1];
	pid_t pid;
	pid_t tgid;
	int   flag;
};
/************************GLOBAL VARIABLE DEFINITION**************************/
static struct rb_root whitelist = RB_ROOT;	/*tgid = pid as key*/
static struct rb_root janklist = RB_ROOT;	/*tgid = pid as key*/
static struct rb_root ignorelist = RB_ROOT;	/*pid*/
static struct rb_root ignoretemplist = RB_ROOT;	/*pid*/
static struct rb_root list_tasks = RB_ROOT;	/*pid*/

/*the number of tasks checked*/
static int __read_mostly sysctl_hung_task_check_count = PID_MAX_LIMIT;
/*zero means infinite timeout - no checking done*/
static unsigned long __read_mostly huawei_hung_task_timeout_secs = CONFIG_DEFAULT_HUNG_TASK_TIMEOUT;
extern unsigned long __read_mostly sysctl_hung_task_timeout_secs;

/*static int __read_mostly sysctl_hung_task_warnings = MAX_DUMP_TIMES;*/
static int did_panic = 0;	/*if not 0, then check no more*/
/*static struct task_struct *watchdog_task;*/
/*should we panic (and reboot, if panic_timeout= is set)
when a hung task is detected:*/
extern unsigned int __read_mostly sysctl_hung_task_panic;
/*control the switch on/off at "/sys/kernel/hubngtask/enable"*/
unsigned int hungtask_enable = HT_DISABLE;

/*used for white list D state detect*/
static unsigned int whitelist_type = WHITE_LIST;
static int whitelist_dump_cnt = DEFAULT_WHITE_DUMP_CNT;
static int whitelist_panic_cnt = DEFAULT_WHITE_PANIC_CNT;
/*used for jank D state dectect*/
static int jankproc_pids[JANK_TASK_MAXNUM];
static int jankproc_pids_size;
static int topapp_pid;
static int janklist_dump_cnt = DEFAULT_JANK_DUMP_CNT;
/*used for app D state detect*/
static int zygote64_pid;
static int zygote_pid;
static int systemserver_pid;
static int applist_dump_cnt = DEFAULT_APP_DUMP_CNT;
static int applist_panic_cnt = DEFAULT_APP_PANIC_CNT;
/*used for S state task that not be schedule for long time*/
static int noschedule_panic_cnt = DEFAULT_NOSCHEDULE_PANIC_CNT;
static int other_log_cnt = DEFAULT_OTHER_LOG_CNT;
/*indicate ignorelist is generated or not, the list is used
to avoid too much D state task printing during dump*/
static int ignore_state = IGN_STATE_INIT;
/*how many heartbeat happend after power on*/
static unsigned long cur_heartbeat;
static int hung_task_dump_and_upload = 0;
static int time_since_upload = 0;
static int hung_task_must_panic = 0;
static struct task_hung_upload upload_hungtask = {0};
static bool in_suspend = false;
/****************************FUNCTION DEFINITION*************************/
static struct task_item *find_task(pid_t pid, struct rb_root *root);
static bool rcu_lock_break(struct task_struct *g, struct task_struct *t);
static struct list_item *find_list_item(pid_t pid, struct rb_root *root);
static void empty_list(struct rb_root *root);
static bool insert_list_item(struct list_item *item, struct rb_root *root);

extern void sysrq_sched_debug_show(void);

static pid_t get_pid_by_name(const char *name)
{
	int max_count = sysctl_hung_task_check_count;
	int batch_count = HUNG_TASK_BATCHING;
	struct task_struct *g, *t;
	int pid = 0;

	rcu_read_lock();
	do_each_thread(g, t) {
		if (!max_count--)
			goto unlock_f;
		if (!--batch_count) {
			batch_count = HUNG_TASK_BATCHING;
			if (!rcu_lock_break(g, t))
				goto unlock_f;
		}
		if (!strncmp(t->comm, name, TASK_COMM_LEN)) {
/*the function is used to match whitelist, janklist, for system_server,
some thread has the same name as main thread, so we use tgid*/
			pid = t->tgid;
			goto unlock_f;
		}
	} while_each_thread(g, t);

unlock_f:
	rcu_read_unlock();
	return pid;
}
static int get_task_type(pid_t pid, pid_t tgid, struct task_struct *parent)
{
	unsigned int flag = TASK_TYPE_IGNORE;

	/*check tgid of it's parent as PPID*/
	if (parent) {
		pid_t ppid = parent->tgid;

		if (PID_KTHREAD == ppid) {
			flag |= TASK_TYPE_KERNEL;
		} else if ((ppid == zygote_pid || ppid == zygote64_pid) &&
			   tgid != systemserver_pid) {
			flag |= TASK_TYPE_APP;
		} else if (ppid == PID_INIT) {
			flag |= TASK_TYPE_NATIVE;
		}
	}
	if (find_list_item(tgid, &whitelist)) {
		flag |= TASK_TYPE_WHITE;
		/*pr_err("hungtask: Task: %d in whitelist found in D state\n", pid);*/
	}
	if (find_list_item(tgid, &janklist))
		flag |= TASK_TYPE_JANK;

	return flag;
}
static void refresh_zygote_pids(void)
{
	int max_count = sysctl_hung_task_check_count;
	int batch_count = HUNG_TASK_BATCHING;
	struct task_struct *g, *t;

	rcu_read_lock();
	do_each_thread(g, t) {
		if (!max_count--)
			goto unlock_f;
		if (!--batch_count) {
			batch_count = HUNG_TASK_BATCHING;
			if (!rcu_lock_break(g, t))
				goto unlock_f;
		}
		if (!strncmp(t->comm, "main", TASK_COMM_LEN) &&
			systemserver_pid) {
			if (zygote64_pid && t->tgid != zygote64_pid)
				zygote_pid = t->tgid;
		} else if (!strncmp(t->comm, "system_server", TASK_COMM_LEN)) {
			systemserver_pid = t->tgid;
			zygote64_pid = t->real_parent->tgid;
		}
	} while_each_thread(g, t);
unlock_f:
	rcu_read_unlock();
}
static int refresh_pids(void)
{
	int i = 0, ret = 0;
	struct rb_node *n;
	struct list_item *new_item;

	for (n = rb_first(&whitelist); n != NULL; n = rb_next(n)) {
		struct list_item *item;

		item = rb_entry(n, struct list_item, node);
		item->pid = get_pid_by_name(item->name);
	}
	/*refresh janklist*/
	empty_list(&janklist);
	if (topapp_pid) {
		new_item = kmalloc(sizeof(*new_item), GFP_ATOMIC);
		if (new_item) {
			memset(new_item, 0, sizeof(*new_item));
			new_item->pid = topapp_pid;
			insert_list_item(new_item, &janklist);
		} else {
			ret = -1;
		}
	}
	for (i = 0; i < jankproc_pids_size; i++) {
		new_item = kmalloc(sizeof(*new_item), GFP_ATOMIC);
		if (!new_item) {
			ret = -1;
			continue;
		}
		memset(new_item, 0, sizeof(*new_item));
		new_item->pid = jankproc_pids[i];
		insert_list_item(new_item, &janklist);
	}
	if (ret < 0 && !RB_EMPTY_ROOT(&janklist))
		empty_list(&janklist);
	/*refresh zygote_pid and zygote64_pid*/
	refresh_zygote_pids();
	return ret;
}

static struct task_item *find_task(pid_t pid, struct rb_root *root)
{
	struct rb_node **p = &root->rb_node;
	struct task_item *cur = NULL;

	while (*p) {
		struct rb_node *parent = NULL;

		parent = *p;
		cur = rb_entry(parent, struct task_item, node);
		if (!cur)
			return NULL;
		if (pid < cur->pid)
			p = &(*p)->rb_left;
		else if (pid > cur->pid)
			p = &(*p)->rb_right;
		else
			return cur;
	}
	return NULL;
}
static bool insert_task(struct task_item *item, struct rb_root *root)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct task_item *cur = NULL;

	while (*p) {
		parent = *p;

		cur = rb_entry(parent, struct task_item, node);
		if (!cur)
			return false;
		if (item->pid < cur->pid)
			p = &(*p)->rb_left;
		else if (item->pid > cur->pid)
			p = &(*p)->rb_right;
		else {
			pr_info("hungtask: insert failed due to already"
				" exist pid=%d,tgid=%d,name=%s,type=%d\n",
				item->pid, item->tgid, item->name,
				item->task_type);
			return false;
		}
	}
	rb_link_node(&item->node, parent, p);
	rb_insert_color(&item->node, root);
	if (item->task_type & (TASK_TYPE_WHITE | TASK_TYPE_JANK)) {
		pr_info("hungtask: insert success pid=%d,tgid=%d,name=%s,"
		"type=%d\n", item->pid, item->tgid, item->name, item->task_type);
	}
	return true;
}
static struct list_item *find_list_item(pid_t pid, struct rb_root *root)
{
	struct rb_node **p = &root->rb_node;

	while (*p) {
		struct rb_node *parent = NULL;
		struct list_item *cur = NULL;

		parent = *p;
		cur = rb_entry(parent, struct list_item, node);
		if (pid < cur->pid)
			p = &(*p)->rb_left;
		else if (pid > cur->pid)
			p = &(*p)->rb_right;
		else
			return cur;
	}
	return NULL;
}
static bool insert_list_item(struct list_item *item, struct rb_root *root)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*p) {
		struct list_item *cur = NULL;

		parent = *p;
		cur = rb_entry(parent, struct list_item, node);

		if (item->pid < cur->pid)
			p = &(*p)->rb_left;
		else if (item->pid > cur->pid)
			p = &(*p)->rb_right;
		else
			return false;
	}
	rb_link_node(&item->node, parent, p);
	rb_insert_color(&item->node, root);
	return true;
}
static void empty_list(struct rb_root *root)
{
	struct rb_node *n;

	if (!root)
		return;
	n = rb_first(root);
	while (n) {
		struct list_item *item = rb_entry(n, struct list_item, node);

		rb_erase(&item->node, root);
		kfree(item);
		n =  rb_first(root);
	}
	*root = RB_ROOT;
}

void show_state_filter_ext(long state_filter)
{
	struct task_struct *g, *p;
	struct task_item *taskitem;

#if BITS_PER_LONG == 32
	printk(KERN_INFO "  task                PC stack   pid father\n");
#else
	printk(KERN_INFO
	       "  task                        PC stack   pid father\n");
#endif
	rcu_read_lock();
	for_each_process_thread(g, p) {
		/*
		 * reset the NMI-timeout, listing all files on a slow
		 * console might take a lot of time:
		 */
		touch_nmi_watchdog();
		if (((p->state == TASK_RUNNING) || (p->state & state_filter))
		    && !find_list_item(p->pid, &ignorelist)) {
			taskitem = find_task(p->pid, &list_tasks);
			if (taskitem) {
				pr_err("hungtask: PID=%d type=%d"
					" blocked for %ds SP=0x%08lx,tgid=%d,la:%llu/lq:%llu\n", taskitem->pid,
					taskitem->task_type,
					taskitem->time_in_D_state * HEARTBEAT_TIME,
					p->thread.cpu_context.sp, p->tgid,
					p->sched_info.last_arrival,
					p->sched_info.last_queued);
			} else {
				pr_err("hungtask: PID=%d,SP=0x%08lx,tgid=%d,la:%llu/lq:%llu\n", p->pid,
					p->thread.cpu_context.sp, p->tgid,
					p->sched_info.last_arrival,
					p->sched_info.last_queued);
			}
			sched_show_task(p);
		}
	}
	touch_all_softlockup_watchdogs();
#ifdef CONFIG_SCHED_DEBUG
	sysrq_sched_debug_show();
#endif
	rcu_read_unlock();
	/*
	 * Only show locks if all tasks are dumped:
	 */
	if (!state_filter)
		debug_show_all_locks();
}
static void jank_print_task_wchan(struct task_struct *task)
{
	unsigned long wchan = 0;
	char symname[KSYM_NAME_LEN] = {0};

	wchan = get_wchan(task);
	if (lookup_symbol_name(wchan, symname) < 0) {
		if (!ptrace_may_access(task, PTRACE_MODE_READ_FSCREDS))
			return;
		printk(KERN_CONT "hungtask:[<%08lx>]\n", wchan);
	} else {
		printk(KERN_CONT "hungtask:[<%08lx>] %s\n", wchan, symname);
	}
}
static int do_upload_log(struct task_hung_upload task)
{
	struct imonitor_eventobj *obj;
	int ret = 0;

	obj = imonitor_create_eventobj(DSTATE_DUMP_EVENT);
	if (obj) {
		ret = imonitor_set_param(obj, E901004002_PID_INT,
		task.pid);/*[false alarm]:ret does not need be used*/
		ret = imonitor_set_param(obj, E901004002_TGID_INT,
		task.tgid);/*[false alarm]:ret does not need be used*/
		ret = imonitor_set_param(obj, E901004002_NAME_VARCHAR,
		(long)task.name);/*[false alarm]:ret does not need be used*/
		ret = imonitor_set_param(obj, E901004002_TYPE_INT,
		task.flag);/*[false alarm]:ret does not need be used*/
		ret = imonitor_send_event(obj); /*[false alarm]*/
		imonitor_destroy_eventobj(obj);
	} else {
		ret = -1;
	}
	return ret;
}
static void do_dump_task(struct task_struct *task)
{
	sched_show_task(task);
	debug_show_held_locks(task);
}
static void do_dump(struct task_struct *task, int flag, int time_in_D_state)
{
	pr_err("hungtask: do_dump, flag=%d\n", flag);

	rcu_read_lock();
	if (!pid_alive(task)) {
		rcu_read_unlock();
		return;
	}
#ifdef CONFIG_HUAWEI_PRINTK_CTRL
	printk_level_setup(LOGLEVEL_DEBUG);
#endif

	if (flag & (FLAG_DUMP_WHITE | FLAG_DUMP_APP)) {
		int cnt = 0;

		trace_sched_process_hang(task);
		/*if (!sysctl_hung_task_warnings)
			return;
		if (sysctl_hung_task_warnings > 0)
			sysctl_hung_task_warnings--;*/
		cnt = time_in_D_state;
		pr_err("INFO: task %s:%d blocked for more than %d seconds"
			" in %s.\n", task->comm, task->pid,
			(HEARTBEAT_TIME * cnt),
			(flag & FLAG_DUMP_WHITE) ? "whitelist" : "applist");
		/*should this RDR hook bind with dump or panic---TBD ??*/
#ifdef CONFIG_HISI_BB
		hung_task_hook((void *)task,
			       (u32) sysctl_hung_task_timeout_secs);
#endif
		pr_err("      %s %s %.*s\n",
		       print_tainted(), init_utsname()->release,
		       (int)strcspn(init_utsname()->version, " "),
		       init_utsname()->version);
		pr_err("\"echo 0 > /proc/sys/kernel/hung_task_timeout_secs\""
		       " disables this message.\n");
		do_dump_task(task);
		touch_nmi_watchdog();
		if (flag & FLAG_DUMP_WHITE && (!hung_task_dump_and_upload)) {
			hung_task_dump_and_upload++;
			upload_hungtask.pid = task->pid;
			upload_hungtask.tgid = task->tgid;
			memset(upload_hungtask.name, 0,sizeof(upload_hungtask.name));
			strncpy(upload_hungtask.name, task->comm, sizeof(task->comm));
			upload_hungtask.flag = flag;
		}
	} else if (flag & FLAG_DUMP_JANK) {
		do_dump_task(task);
	} else if (flag & FLAG_DUMP_NOSCHEDULE) {
		/*watchdog is not schedule, check the cpu
		is busy for what task*/
		pr_err("hungtask: print all running cpu stack and"
			" D state stack due to systemserver watchdog"
			" thread not schedule\n");
		trigger_all_cpu_backtrace();
	}

#ifdef CONFIG_HUAWEI_PRINTK_CTRL
	printk_level_setup(sysctl_printk_level);
#endif
	rcu_read_unlock();
}
static void do_panic(void)
{
	if (sysctl_hung_task_panic) {
		trigger_all_cpu_backtrace();
		panic("hungtask: blocked tasks");
	}
}

static void create_taskitem(struct task_item *taskitem, struct task_struct *task)
{
	taskitem->pid = task->pid;
	taskitem->tgid = task->tgid;
	memset(taskitem->name, 0, sizeof(taskitem->name));
	strncpy(taskitem->name, task->comm, sizeof(task->comm));
	taskitem->switchCount = task->nvcsw + task->nivcsw;
	taskitem->dump_wa = 0;	/*whitelist or applist task dump times*/
	taskitem->panic_wa = 0;	/*whitelist or applist task panic times*/
	taskitem->dump_jank = 0;	/*janklist task dump times*/
	taskitem->time_in_D_state = -1;/*D time_cnt is 1 less than dump_wa or dump_jank*/
	taskitem->isDonewa = true;	/*if task in  white or app dealed */
	taskitem->isDonejank = true;	/*if task in  jank dealed */
}
static bool refresh_task(struct task_item *taskitem, struct task_struct *task)
{
	bool is_called = false;

	if (taskitem->switchCount != (task->nvcsw + task->nivcsw)) {
		taskitem->switchCount = task->nvcsw + task->nivcsw;
		is_called = true;
		return is_called;
	}

	if (taskitem->task_type & TASK_TYPE_WHITE) {
		taskitem->isDonewa = false;
		taskitem->dump_wa++;
		taskitem->panic_wa++;
	} else if (taskitem->task_type & TASK_TYPE_APP) {
		taskitem->isDonewa = false;
		taskitem->dump_wa++;
		taskitem->panic_wa++;
	}
	if (taskitem->task_type & TASK_TYPE_JANK) {
		taskitem->isDonejank = false;
		taskitem->dump_jank++;
	} else if (!(taskitem->task_type & (TASK_TYPE_WHITE | TASK_TYPE_APP))) {
		taskitem->dump_wa++;
		taskitem->isDonewa = false;
	}
	taskitem->time_in_D_state++;

	return is_called;
}
static void generate_ignorelist(struct task_struct *t)
{
	struct list_item *listitem;

	if (IGN_STATE_INIT == ignore_state && cur_heartbeat > ONE_MINUTE) {
		listitem = kmalloc(sizeof(*listitem), GFP_ATOMIC);
		if (listitem) {
			memset(listitem, 0, sizeof(*listitem));
			listitem->pid = t->pid;
			/*we use pid, not tgid here*/
			insert_list_item(listitem, &ignoretemplist);
		}
	} else if (IGN_STATE_FIRST == ignore_state &&
			cur_heartbeat > ONE_AND_HALF_MINUTE) {
		listitem = find_list_item(t->pid, &ignoretemplist);
		if (listitem) {
			rb_erase(&listitem->node, &ignoretemplist);
			insert_list_item(listitem, &ignorelist);
		}
	}
}
static void remove_list_tasks(struct task_item *item)
{
	if(item->task_type & (TASK_TYPE_WHITE | TASK_TYPE_JANK)) {
		pr_info("hungtask: remove from list_tasks pid=%d,tgid=%d,"
			"name=%s\n", item->pid, item->tgid, item->name);
	}
	rb_erase(&item->node, &list_tasks);
	kfree(item);
}
static void shrink_list_tasks(void)
{
	bool found = false;

	do {
		struct rb_node *n;
		struct task_item *item = NULL;

		found = false;
		for (n = rb_first(&list_tasks); n != NULL; n = rb_next(n)) {
			item = rb_entry(n, struct task_item, node);
			if (!item)
				continue;
			if (item->isDonewa && item->isDonejank) {
				found = true;
				break;
			}
		}
		if (found)
			remove_list_tasks(item);
	} while (found);
}
static void check_parameters(void)
{
	if (whitelist_dump_cnt < 0 || whitelist_dump_cnt > DEFAULT_WHITE_DUMP_CNT)
		whitelist_dump_cnt = DEFAULT_WHITE_DUMP_CNT;
	if (whitelist_panic_cnt <= 0 || whitelist_panic_cnt > DEFAULT_WHITE_PANIC_CNT)
		whitelist_panic_cnt = DEFAULT_WHITE_PANIC_CNT;
	if (applist_dump_cnt < 0 || applist_dump_cnt > MAX_LOOP_NUM)
		applist_dump_cnt = DEFAULT_APP_DUMP_CNT;
	if (applist_panic_cnt != DEFAULT_APP_PANIC_CNT)
		applist_panic_cnt = DEFAULT_APP_PANIC_CNT;
	if (noschedule_panic_cnt < 0 /*DEFAULT_NOSCHEDULE_PANIC_CNT*/ ||
		noschedule_panic_cnt > MAX_LOOP_NUM) {
		noschedule_panic_cnt = DEFAULT_NOSCHEDULE_PANIC_CNT;
	}
	if (janklist_dump_cnt < 0 || janklist_dump_cnt > MAX_LOOP_NUM)
		janklist_dump_cnt = DEFAULT_JANK_DUMP_CNT;
}
static void post_process(void)
{
	struct rb_node *n;

	if (hung_task_dump_and_upload == HUNG_TASK_UPLOAD_ONCE) {
		show_state_filter_ext(TASK_UNINTERRUPTIBLE);
		do_upload_log(upload_hungtask);
		hung_task_dump_and_upload++;
	}
	if (hung_task_dump_and_upload > 0) {
		time_since_upload++;
		if (time_since_upload > (whitelist_panic_cnt - whitelist_dump_cnt)) {
			hung_task_dump_and_upload = 0;
			time_since_upload = 0;
		}
	}
	if (hung_task_must_panic) {
		pr_err("hungtask: The whitelist task %s:%d blocked more than %ds is causing panic.\n",
			upload_hungtask.name, upload_hungtask.pid,
			whitelist_panic_cnt * HEARTBEAT_TIME);
		show_state_filter_ext(TASK_UNINTERRUPTIBLE);
		hung_task_must_panic = 0;
		do_panic();
	}

	if (IGN_STATE_INIT == ignore_state && cur_heartbeat > ONE_MINUTE) {
		ignore_state = IGN_STATE_FIRST;
	} else if (IGN_STATE_FIRST == ignore_state && cur_heartbeat >
		ONE_AND_HALF_MINUTE) {
		ignore_state = IGN_STATE_DONE;
		if (!RB_EMPTY_ROOT(&ignoretemplist))
			empty_list(&ignoretemplist);
	}
	shrink_list_tasks();
	for (n = rb_first(&list_tasks); n != NULL; n = rb_next(n)) {
		struct task_item *item = rb_entry(n, struct task_item,
								node);
		item->isDonewa = true;
		item->isDonejank = true;
	}
}

/*
 * To avoid extending the RCU grace period for an unbounded amount of time,
 * periodically exit the critical section and enter a new one.
 *
 * For preemptible RCU it is sufficient to call rcu_read_unlock in order
 * to exit the grace period. For classic RCU, a reschedule is required.
 */
static bool rcu_lock_break(struct task_struct *g, struct task_struct *t)
{
	bool can_cont = false;

	get_task_struct(g);
	get_task_struct(t);
	rcu_read_unlock();
	cond_resched();
	rcu_read_lock();
	can_cont = pid_alive(g) && pid_alive(t);
	put_task_struct(t);
	put_task_struct(g);

	return can_cont;
}
static int dump_task_wa(struct task_item *item, int dump_cnt, bool is_called,
				struct task_struct *task,  int flag)
{
	int ret = 0;

	if (is_called) {
		item->dump_wa = 1;
		item->panic_wa = 1;
		item->time_in_D_state = 0;
		return ret;
	}
	if (item->time_in_D_state > HUNG_TEN_MINUTES &&
		(item->time_in_D_state % HUNG_TEN_MINUTES != 0))
		return ret;
	if (item->time_in_D_state > HUNG_ONE_HOUR &&
                (item->time_in_D_state % HUNG_ONE_HOUR != 0))
                return ret;
	if (dump_cnt && item->dump_wa > dump_cnt) {
		if (!hung_task_dump_and_upload) {
			pr_err("hungtask: Ready to dump a task %s.\n", item->name);
			do_dump(task, flag, item->time_in_D_state);
		}
		item->dump_wa = 1;
		ret++;
	}
	return ret;
}
static void deal_task(struct task_item *item, struct task_struct *task, bool is_called)
{
	int any_dumped_num = 0;

	if (item->task_type & TASK_TYPE_WHITE) {
		any_dumped_num =
			dump_task_wa(item, whitelist_dump_cnt, is_called, task, FLAG_DUMP_WHITE);
	} else if (item->task_type & TASK_TYPE_APP) {
		any_dumped_num =
			dump_task_wa(item, applist_dump_cnt, is_called, task, FLAG_DUMP_APP);
	}
	if (item->task_type & TASK_TYPE_JANK) {
		if (is_called) {
			item->dump_jank = 1;
			item->time_in_D_state = 0;
		} else {
			if (item->dump_jank > 1) {
				LOG_JANK_D(JLID_KERNEL_HUNG_TASK,
				"#ARG1:<%s>#ARG2:<%d>", item->name,
				(item->dump_jank - 1) * HEARTBEAT_TIME);
			}
			if (janklist_dump_cnt &&
				item->dump_jank > janklist_dump_cnt
				&& (!hung_task_dump_and_upload)) {
					do_dump(task, FLAG_DUMP_JANK, item->time_in_D_state);
					item->dump_jank = 1;
					item->isDonejank = true;
					any_dumped_num++;
			} else if (item->dump_jank == 2) {
				jank_print_task_wchan(task);
			}
		}
	} else if (!(item->task_type & (TASK_TYPE_WHITE | TASK_TYPE_APP))) {
		if (is_called) {
			item->dump_wa = 1;
			item->time_in_D_state = 0;
		} else if (item->dump_wa > other_log_cnt && item->time_in_D_state < HUNG_ONE_HOUR) {
			pr_err("hungtask: Unconcerned task %s:%d blocked more than %d seconds\n",
				item->name, item->pid, item->time_in_D_state * HEARTBEAT_TIME);
			item->dump_wa = 1;
			any_dumped_num++;
		}
	}
#ifdef CONFIG_TZDRIVER
        if (any_dumped_num && is_tee_hungtask(task)) {
                pr_info("hungtask: related to teeos was detected, dump status of teeos\n");
                wakeup_tc_siq();
        }
#endif
	if (item->time_in_D_state < HUNG_ONE_HOUR && (item->dump_wa % HUNG_TASK_RECORDTIME_CNT == 0)
		&& (item->task_type & (TASK_TYPE_WHITE | TASK_TYPE_APP))) {
		pr_err("hungtask: Task %s:%d type is %d blocked %d seconds\n",
			item->name, item->pid, item->task_type, item->time_in_D_state * HEARTBEAT_TIME);
	}

	if (!is_called && (item->task_type & TASK_TYPE_WHITE)) {
		if(whitelist_panic_cnt && item->panic_wa > whitelist_panic_cnt) {
			pr_err("hungtask: A whitelist task %s is about to cause panic.\n", item->name);
			item->panic_wa = 0;
			hung_task_must_panic++;
		} else
			item->isDonewa = false;
	}
	if (item->isDonewa && item->isDonejank)
		remove_list_tasks(item);
}

static bool check_conditions(struct task_struct *task, unsigned int task_type, int messageTime)
{
	bool needNoCheck = true;

	if (unlikely(task->flags & PF_FROZEN)) {
		if(messageTime && !in_suspend) {
			pr_err("hungtask: task pid=%d, name=%s is FROZEN, should ignore it\n",
				task->pid, task->comm);
		}
		return true;
	}

	if (task_type & TASK_TYPE_WHITE && (whitelist_dump_cnt ||
						whitelist_panic_cnt))
		needNoCheck = false;
	else if (task_type & TASK_TYPE_APP && (applist_dump_cnt ||
						applist_panic_cnt))
		needNoCheck = false;
	if (task_type & TASK_TYPE_JANK && janklist_dump_cnt)
		needNoCheck = false;
	else if (!(task_type & (TASK_TYPE_WHITE | TASK_TYPE_APP)) && ignore_state == IGN_STATE_DONE)
		needNoCheck = false;

	return needNoCheck;
}
void check_hung_tasks_proposal(unsigned long timeout)
{
	int max_count = sysctl_hung_task_check_count;
	int batch_count = HUNG_TASK_BATCHING;
	struct task_struct *g, *t;
	static int messageTime = 0;

	if(!hungtask_enable)
		return;
	if (test_taint(TAINT_DIE) || did_panic) {
		pr_err("hungtask: heartbeart=%ld, it's going to panic, "
			"ignore this heartbeart\n", cur_heartbeat);
		return;
	}
	cur_heartbeat++;
	if((cur_heartbeat % THREE_MINUTE) == 0) {
		pr_err("hungtask: The huawei hungtask detect is running.\n");
		messageTime = 1;
	} else
		messageTime = 0;

	if(messageTime || cur_heartbeat < TIME_REFRESH_PIDS) {
		refresh_pids();
		check_parameters();
	}

	rcu_read_lock();
	for_each_process_thread(g, t) {
		bool is_called = false;

		if (!max_count--)
			goto unlock;
		if (!--batch_count) {
			batch_count = HUNG_TASK_BATCHING;
			if (!rcu_lock_break(g, t))
				goto unlock;
		}
		if (t->state == TASK_UNINTERRUPTIBLE) {
			unsigned int task_type = TASK_TYPE_IGNORE;
			unsigned long switch_count = t->nvcsw + t->nivcsw;
			struct task_item *taskitem;

			/*
			* When a freshly created task is scheduled once,
			*changes its state to TASK_UNINTERRUPTIBLE without
			*having ever been switched out once, it musn't
			*be checked.
			*/
			if (unlikely(!switch_count)) {
			pr_info("hungtask: heartbeart=%ld, switch_count"
				" is zero, ignore this heartbeart\n", cur_heartbeat);
				continue;
			}
			if (RB_EMPTY_ROOT(&whitelist))
				continue;
			/*if whitelist is not ready, it should not happend,
			*becuase whitelist generate in init.rc*/

			/*if (RB_EMPTY_ROOT(&janklist))
				continue;*/

			/*if janklist is not ready, hungtask don't need
			*work, janklist set is about 50sec after powerup*/
			generate_ignorelist(t);
			/*special case for teeos*/
			if(ignore_state == IGN_STATE_DONE && find_list_item(t->pid, &ignorelist))
				continue;
			taskitem = find_task(t->pid, &list_tasks);
			if (taskitem) {
				if (check_conditions(t, taskitem->task_type, messageTime))
					continue;
				is_called = refresh_task(taskitem, t);
			} else {
				task_type = get_task_type(t->pid,
						t->tgid, t->real_parent);
				if (check_conditions(t, task_type, messageTime))
					continue;
				taskitem = kmalloc(sizeof(*taskitem),
								GFP_ATOMIC);
				if (!taskitem) {
					pr_err("hungtask: kmalloc failed");
					continue;
				}
				memset(taskitem, 0, sizeof(*taskitem));
				taskitem->task_type = task_type;
				create_taskitem(taskitem, t);
				is_called = refresh_task(taskitem, t);
				insert_task(taskitem, &list_tasks);
			}
			deal_task(taskitem, t, is_called);
		}
	}
unlock:
	rcu_read_unlock();
	post_process();
}

static ssize_t enable_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	if (hungtask_enable)
		return snprintf(buf, ENABLE_SHOW_LEN, "on\n");
	else
		return snprintf(buf, ENABLE_SHOW_LEN, "off\n");
}
static ssize_t enable_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	char tmp[6];
	size_t len = 0;
	char *p;

	if ((count < 2) || (count > (sizeof(tmp) - 1))) {
		pr_err("hungtask: string too long or too short.\n");
		return -EINVAL;
	}
	if (!buf)
		return -EINVAL;
	p = memchr(buf, '\n', count);
	len = p ? (size_t) (p - buf) : count;
	memset(tmp, 0, sizeof(tmp));
	strncpy(tmp, buf, len);
	if (strncmp(tmp, "on", strlen(tmp)) == 0) {
		hungtask_enable = HT_ENABLE;
		pr_info("hungtask: hungtask_enable is set to enable.\n");
	} else if (strncmp(tmp, "off", strlen(tmp)) == 0) {
		hungtask_enable = HT_DISABLE;
		pr_info("hungtask: hungtask_enable is set to disable.\n");
	} else
		pr_err("hungtask: only accept on or off !\n");

	return (ssize_t) count;
}
static ssize_t monitorlist_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	char *start = buf;
	char all_buf[WHITELIST_STORE_LEN - 20];
	struct rb_node *n;
	unsigned long len = 0;

	memset(all_buf, 0, sizeof(all_buf));
	for (n = rb_first(&whitelist); n != NULL; n = rb_next(n)) {
		struct list_item *item = rb_entry(n, struct list_item, node);

		len += snprintf(all_buf + len, sizeof(all_buf) - len, "%s,",
				item->name);
		if (!(len < sizeof(all_buf))) {
			len = sizeof(all_buf) - 1;
			break;
		}
		/*pr_info("hungtask: all_buff temp =%s", all_buf);*/
	}
	if (len > 0)
		all_buf[len] = 0;
	/*pr_info("hungtask: show all_buf=%s\n", all_buf);*/
	if (whitelist_type == WHITE_LIST)
		buf += snprintf(buf, WHITELIST_STORE_LEN, "whitelist: [%s]\n",
				all_buf);
	else if (whitelist_type == BLACK_LIST)
		buf += snprintf(buf, WHITELIST_STORE_LEN, "blacklist: [%s]\n",
				all_buf);
	else
		buf += snprintf(buf, WHITELIST_STORE_LEN, "\n");

	return buf - start;
}
/*
 *    monitorlist_store    -  Called when 'write/echo' method is
 *    used on entry '/sys/kernel/hungtask/monitorlist'.
 */
static ssize_t monitorlist_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t n)
{
	size_t len = 0;
	char *p;
	int ret = 0;
	char all_buf[WHITELIST_STORE_LEN];
	char *cur = all_buf;
	char name[TASK_COMM_LEN + 1] = { 0 };
	int fake_pid = 10;

	/*input format:
	*write /sys/kernel/hungtask/monitorlist "whitelist,
	*system_server,surfaceflinger"*/
	p = strnchr(buf, '\n', n);
	len = p ? (size_t) ((p - buf) - 1) : n;	/*exclude the '\n'*/
	if ((len < 2) || (len > (sizeof(all_buf) - 1))) {
		pr_err("hungtask: input string is too long or too short\n");
		return -EINVAL;
	}
	memset(all_buf, 0, sizeof(all_buf));
	memset(name, 0, sizeof(name));
	strncpy(all_buf, buf,
		len > WHITELIST_STORE_LEN ? WHITELIST_STORE_LEN : len);
	p = strsep(&cur, ",");
	if (!p) {
		pr_err("hungtask: input string is not correct!\n");
		return -EINVAL;
	}
	if (!strcmp(p, "whitelist")) {
		whitelist_type = WHITE_LIST;
	} else{
		if (!strcmp(p, "blacklist"))
			/*whitelist_type = BLACK_LIST;*/
			pr_err("hungtask: blacklist is not support!\n");
		else
			pr_err("hungtask: wrong list type is set!\n");
		return -EINVAL;
	}
	if (!strlen(cur)) {
		pr_err("hungtask: at least one process need to be set!\n");
		return -EINVAL;
	}
	empty_list(&whitelist);

	/*generate the new whitelist*/
	for (;;) {
		char *token;

		token = strsep(&cur, ",");
		if (token && strlen(token)) {
			struct list_item *new_item;

			strncpy(name, token, TASK_COMM_LEN);
			new_item = kmalloc(sizeof(*new_item), GFP_ATOMIC);
			if (new_item) {
				memset(new_item, 0, sizeof(*new_item));

			/*pid is neccessary as the key of rb tree, it's too
			*early for system_server or surfaceflinger to start*/
				new_item->pid = fake_pid--;
				strncpy(new_item->name, token, TASK_COMM_LEN);
				insert_list_item(new_item, &whitelist);
			/*pr_info("hungtask: add new =%s", name);*/
			} else {
				ret = -1;
			}
		}
		if (!cur)
			break;
	}
	/*chreeck again in case user input "whitelist,,,,,,"*/
	if (RB_EMPTY_ROOT(&whitelist)) {
		pr_err("hungtask: at least one process need to be set!\n");
		return -EINVAL;
	}
	if (ret < 0) {
		pr_err("hungtask: failed to allocat, empty the list again\n");
		empty_list(&whitelist);
		return -EFAULT;
	}

	return (ssize_t) n;
}

static struct notifier_block pm_event = {0};
static int pm_sr_event(struct notifier_block *this,
                unsigned long event, void *ptr)
{
	switch (event) {
		case PM_SUSPEND_PREPARE:
			in_suspend = true;
			break;
		case PM_POST_SUSPEND:
			in_suspend = false;
			break;
		default:
			return NOTIFY_DONE;
	}
	return NOTIFY_OK;
}
/*used for sysctl at "/proc/sys/kernel/hung_task_timeout_secs"*/
void fetch_task_timeout_secs(unsigned long new_sysctl_hung_task_timeout_secs)
{
	if (new_sysctl_hung_task_timeout_secs > CONFIG_DEFAULT_HUNG_TASK_TIMEOUT
		|| (new_sysctl_hung_task_timeout_secs % HEARTBEAT_TIME))
		return;

 	huawei_hung_task_timeout_secs = new_sysctl_hung_task_timeout_secs;

	/*if user change panic timeout value, we sync it to dump value
	defaultly, user can set it diffrently*/
	whitelist_panic_cnt = (int)(huawei_hung_task_timeout_secs / HEARTBEAT_TIME);
	if (whitelist_panic_cnt > THIRTY_SECONDS) {
		whitelist_dump_cnt = whitelist_panic_cnt / HUNGTASK_DUMP_IN_PANIC_LOOSE;
	} else {
		whitelist_dump_cnt = whitelist_panic_cnt / HUNGTASK_DUMP_IN_PANIC_STRICT;
	}
	applist_dump_cnt = whitelist_dump_cnt;
}

void fetch_hung_task_panic(int new_did_panic)
{
	did_panic = new_did_panic;
}
/*used as main thread of khungtaskd*/
static struct kobj_attribute timeout_attribute = {
	.attr = {
		 .name = "enable",
		 .mode = 0640,
		 },
	.show = enable_show,
	.store = enable_store,
};
static struct kobj_attribute monitorlist_attr = {
	.attr = {
		 .name = "monitorlist",
		 .mode = 0640,
		 },
	.show = monitorlist_show,
	.store = monitorlist_store,
};
static struct attribute *attrs[] = {
	&timeout_attribute.attr,
	&monitorlist_attr.attr,
	NULL
};
static struct attribute_group hungtask_attr_group = {
	.attrs = attrs,
};
struct kobject *hungtask_kobj;
int create_sysfs_hungtask(void)
{
	int retval = 0;

	while (kernel_kobj == NULL)
		msleep(1000);
	/*Create kobject named "hungtask" located at /sys/kernel/huangtask */
	hungtask_kobj = kobject_create_and_add("hungtask", kernel_kobj);
	if (!hungtask_kobj)
		return -ENOMEM;

	retval = sysfs_create_group(hungtask_kobj, &hungtask_attr_group);
	if (retval)
		kobject_put(hungtask_kobj);

	pm_event.notifier_call = pm_sr_event;
	pm_event.priority = -1;
	if (register_pm_notifier(&pm_event))
	{
		pr_err("hungtask: register pm notifier failed\n");
		return -EFAULT;
	}
	return retval;
}
/*all parameters located under "/sys/module/huawei_hung_task/parameters/"*/
module_param_array_named(jankproc_pids, jankproc_pids, int,
				&jankproc_pids_size, (S_IRUGO | S_IWUSR));
MODULE_PARM_DESC(jankproc_pids, "jankproc_pids state");
module_param_named(topapp_pid, topapp_pid, int, (S_IRUGO | S_IWUSR));
