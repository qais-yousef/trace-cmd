/*
 * Copyright (C) 2014 Red Hat Inc, Steven Rostedt <srostedt@redhat.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License (not later!)
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not,  see <http://www.gnu.org/licenses>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

/** FIXME: Convert numbers based on machine and file */
#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace-local.h"
#include "trace-hash.h"

#define TASK_STATE_TO_CHAR_STR "RSDTtXZxKWP"
#define TASK_STATE_MAX		1024

#define task_from_item(item)	container_of(item, struct task_data, hash)
#define start_from_item(item)	container_of(item, struct start_data, hash)
#define event_from_item(item)	container_of(item, struct event_hash, hash)
#define stack_from_item(item)	container_of(item, struct stack_data, hash)

struct handle_data;
struct event_hash;
struct event_data;

typedef void (*event_data_print)(struct trace_seq *s, struct event_hash *hash);
typedef int (*handle_event_func)(struct handle_data *h, unsigned long long pid,
				 struct event_data *data,
				 struct pevent_record *record, int cpu);

enum event_data_type {
	EVENT_TYPE_UNDEFINED,
	EVENT_TYPE_STACK,
	EVENT_TYPE_SCHED_SWITCH,
	EVENT_TYPE_WAKEUP,
	EVENT_TYPE_FUNC,
	EVENT_TYPE_SYSCALL,
	EVENT_TYPE_IRQ,
	EVENT_TYPE_SOFTIRQ,
	EVENT_TYPE_SOFTIRQ_RAISE,
	EVENT_TYPE_PROCESS_EXEC,
};

struct event_data {
	struct event_data	*next;
	int			id;
	int			trace;
	struct event_format	*event;

	struct event_data	*end;
	struct event_data	*start;

	struct format_field	*pid_field;
	struct format_field	*start_match_field;	/* match with start */
	struct format_field	*end_match_field;	/* match with end */
	struct format_field	*data_field;	/* optional */

	event_data_print	print_func;
	handle_event_func	handle_event;
	void			*private;
	int			migrate;	/* start/end pairs can migrate cpus */
	enum event_data_type	type;
};

struct stack_data {
	struct trace_hash_item  hash;
	unsigned long long	count;
	unsigned long long	time;
	unsigned long long	time_min;
	unsigned long long	time_max;
	unsigned long long	time_avg;
	unsigned long		size;
	char			caller[];
};

struct stack_holder {
	unsigned long		size;
	void			*caller;
	struct pevent_record	*record;
};

struct start_data {
	struct trace_hash_item	hash;
	struct event_data	*event_data;
	struct list_head	list;
	struct task_data	*task;
	unsigned long long 	timestamp;
	unsigned long long 	search_val;
	unsigned long long	val;
	int			cpu;

	struct stack_holder	stack;
};

struct event_hash {
	struct trace_hash_item	hash;
	struct event_data	*event_data;
	unsigned long long	search_val;
	unsigned long long	val;
	unsigned long long	count;
	unsigned long long	time_total;
	unsigned long long	time_avg;
	unsigned long long	time_max;
	unsigned long long	time_min;
	unsigned long long	time_std;
	unsigned long long	last_time;

	struct trace_hash	stacks;
};

struct task_data {
	struct trace_hash_item	hash;
	int			pid;
	int			sleeping;

	char			*comm;

	struct trace_hash	start_hash;
	struct trace_hash	event_hash;

	struct task_data	*proxy;
	struct start_data	*last_start;
	struct event_hash	*last_event;
	struct handle_data	*handle;
};

struct cpu_info {
	int			current;
};

struct sched_switch_data {
	struct format_field	*prev_state;
	int			match_state;
};

struct handle_data {
	struct handle_data	*next;
	struct tracecmd_input	*handle;
	struct pevent		*pevent;
	struct event_data	*events;

	struct cpu_info		**cpu_data;

	struct format_field	*common_pid;
	struct format_field	*wakeup_comm;
	struct format_field	*switch_prev_comm;
	struct format_field	*switch_next_comm;

	struct sched_switch_data sched_switch_blocked;
	struct sched_switch_data sched_switch_preempt;

	struct trace_hash	task_hash;
	struct list_head	all_starts;

	int			cpus;
};

static struct handle_data *handles;

static struct start_data *
add_start(struct task_data *task,
	  struct event_data *event_data, struct pevent_record *record,
	  unsigned long long search_val, unsigned long long val)
{
	struct start_data *start;

	start = malloc_or_die(sizeof(*start));
	memset(start, 0, sizeof(*start));
	start->hash.key = trace_hash(search_val);
	start->search_val = search_val;
	start->val = val;
	start->timestamp = record->ts;
	start->event_data = event_data;
	start->cpu = record->cpu;
	start->task = task;
	trace_hash_add(&task->start_hash, &start->hash);
	list_add(&start->list, &task->handle->all_starts);
	return start;
}

struct event_data_match {
	struct event_data	*event_data;
	unsigned long long	search_val;
	unsigned long long	val;
};

static int match_start(struct trace_hash_item *item, void *data)
{
	struct start_data *start = start_from_item(item);
	struct event_data_match *edata = data;

	return start->event_data == edata->event_data &&
		start->search_val == edata->search_val;
}

static int match_event(struct trace_hash_item *item, void *data)
{
	struct event_data_match *edata = data;
	struct event_hash *event = event_from_item(item);

	return event->event_data == edata->event_data &&
		event->search_val == edata->search_val &&
		event->val == edata->val;
}

static struct event_hash *
find_event_hash(struct task_data *task, struct event_data *event_data,
		struct start_data *start)
{
	struct trace_hash_item *item;
	struct event_hash *event_hash;
	struct event_data_match edata;
	unsigned long long key;

	edata.event_data = event_data;
	edata.search_val = start->search_val;
	edata.val = start->val;

	key = trace_hash((unsigned long)event_data);
	item = trace_hash_find(&task->event_hash, key, match_event, &edata);
	if (item)
		return event_from_item(item);

	event_hash = malloc_or_die(sizeof(*event_hash));
	memset(event_hash, 0, sizeof(*event_hash));

	event_hash->event_data = event_data;
	event_hash->search_val = start->search_val;
	event_hash->val = start->val;
	event_hash->hash.key = key;
	trace_hash_init(&event_hash->stacks, 32);

	trace_hash_add(&task->event_hash, &event_hash->hash);

	return event_hash;
}

static struct start_data *
find_start(struct task_data *task, struct event_data *event_data,
	   unsigned long long search_val)
{
	unsigned long long key = trace_hash(search_val);
	struct event_data_match edata;
	void *data = &edata;
	struct trace_hash_item *item;
	struct start_data *start;

	edata.event_data = event_data;
	edata.search_val = search_val;

	item = trace_hash_find(&task->start_hash, key, match_start, data);
	if (!item)
		return NULL;

	start = start_from_item(item);
	return start;
}

struct stack_match {
	void		*caller;
	unsigned long	size;
};

static int match_stack(struct trace_hash_item *item, void *data)
{
	struct stack_data *stack = stack_from_item(item);
	struct stack_match *match = data;

	if (match->size != stack->size)
		return 0;

	return memcmp(stack->caller, match->caller, stack->size) == 0;
}


static void add_event_stack(struct event_hash *event_hash,
			    void *caller, unsigned long size,
			    unsigned long long time)
{
	unsigned long long key;
	struct stack_data *stack;
	struct stack_match match;
	struct trace_hash_item *item;
	int i;

	match.caller = caller;
	match.size = size;

	if (size < sizeof(int))
		die("Stack size of less than sizeof(int)??");

	for (key = 0, i = 0; i <= size - sizeof(int); i += sizeof(int))
		key += trace_hash(*(int *)(caller + i));

	item = trace_hash_find(&event_hash->stacks, key, match_stack, &match);
	if (!item) {
		stack = malloc_or_die(sizeof(*stack) + size);
		memset(stack, 0, sizeof(*stack));
		memcpy(&stack->caller, caller, size);
		stack->size = size;
		stack->hash.key = key;
		trace_hash_add(&event_hash->stacks, &stack->hash);
	} else
		stack = stack_from_item(item);

	stack->count++;
	stack->time += time;
	if (stack->count == 1 || time < stack->time_min)
		stack->time_min = time;
	if (time > stack->time_max)
		stack->time_max = time;
}

static void free_start(struct start_data *start)
{
	if (start->task->last_start == start)
		start->task->last_start = NULL;
	if (start->stack.record)
		free_record(start->stack.record);
	trace_hash_del(&start->hash);
	list_del(&start->list);
	free(start);
}

static struct event_hash *
add_and_free_start(struct task_data *task, struct start_data *start,
		   struct event_data *event_data, unsigned long long ts)
{
	struct event_hash *event_hash;
	unsigned long long delta;

	delta = ts - start->timestamp;

	event_hash = find_event_hash(task, event_data, start);
	event_hash->count++;
	event_hash->time_total += delta;
	event_hash->last_time = delta;

	if (delta > event_hash->time_max)
		event_hash->time_max = delta;

	if (event_hash->count == 1 || delta < event_hash->time_min)
		event_hash->time_min = delta;

	if (start->stack.record) {
		unsigned long size;
		void *caller;

		size = start->stack.size;
		caller = start->stack.caller;

		add_event_stack(event_hash, caller, size, delta);
		free_record(start->stack.record);
		start->stack.record = NULL;
	}

	free_start(start);

	return event_hash;
}

static struct event_hash *
find_and_update_start(struct task_data *task, struct event_data *event_data,
		      unsigned long long ts, unsigned long long search_val)
{
	struct start_data *start;

	start = find_start(task, event_data, search_val);
	if (!start)
		return NULL;
	return add_and_free_start(task, start, event_data, ts);
}

static int match_task(struct trace_hash_item *item, void *data)
{
	struct task_data *task = task_from_item(item);
	int pid = *(unsigned long *)data;

	return task->pid == pid;
}

static struct task_data *
add_task(struct handle_data *h, int pid)
{
	unsigned long long key = trace_hash(pid);
	struct task_data *task;

	task = malloc_or_die(sizeof(*task));
	memset(task, 0, sizeof(*task));
	task->pid = pid;
	task->hash.key = key;
	trace_hash_add(&h->task_hash, &task->hash);
	task->handle = h;

	trace_hash_init(&task->start_hash, 16);
	trace_hash_init(&task->event_hash, 32);

	return task;
}

static struct task_data *
find_task(struct handle_data *h, int pid)
{
	unsigned long long key = trace_hash(pid);
	struct trace_hash_item *item;
	static struct task_data *last_task;
	void *data = (unsigned long *)&pid;

	if (last_task && last_task->pid == pid)
		return last_task;

	item = trace_hash_find(&h->task_hash, key, match_task, data);

	if (item)
		last_task = task_from_item(item);
	else
		last_task = add_task(h, pid);

	return last_task;
}

static void
add_task_comm(struct task_data *task, struct format_field *field,
	      struct pevent_record *record)
{
	const char *comm;

	task->comm = malloc_or_die(field->size + 1);
	comm = record->data + field->offset;
	memcpy(task->comm, comm, field->size);
	task->comm[field->size] = 0;
}

static void account_task(struct task_data *task, struct event_data *event_data)
{
}

static int handle_event_data(struct handle_data *h,
			     unsigned long long pid,
			     struct event_data *event_data,
			     struct pevent_record *record, int cpu)
{
	unsigned long long epid;
	unsigned long long val;
	struct task_data *task = NULL;
	struct event_hash *event_hash;
	struct start_data *start;

	/* If this is the end of a event pair (start is set) */
	if (event_data->start) {

		/* If pid_field is defined, use that to find the task */
		if (event_data->pid_field)
			pevent_read_number_field(event_data->pid_field,
						 record->data, &epid);
		else
			epid = pid;

		task = find_task(h, epid);

		pevent_read_number_field(event_data->start_match_field, record->data,
					 &val);
		event_hash = find_and_update_start(task, event_data->start, record->ts, val);
		task->last_start = NULL;
		task->last_event = event_hash;
	}

	/* If this is the start of a event pair (end is set) */
	if (event_data->end) {

		/* If end_pid is defined, use that to find the task */
		if (event_data->pid_field)
			pevent_read_number_field(event_data->pid_field,
						 record->data, &epid);
		else
			epid = pid;

		task = find_task(h, epid);

		pevent_read_number_field(event_data->end_match_field, record->data,
					 &val);
		start = add_start(task, event_data, record, val, val);
		task->last_start = start;
		task->last_event = NULL;
	}

	if (!task) {
		task = find_task(h, pid);
		task->last_start = NULL;
		task->last_event = NULL;
		account_task(task, event_data);
	}

	return 0;
}

static void handle_missed_events(struct handle_data *h, int cpu)
{
	struct start_data *start;
	struct start_data *n;

	list_for_each_entry_safe(start, n, &h->all_starts, list) {
		if (start->cpu == cpu || start->event_data->migrate)
			free_start(start);
	}
}

int trace_profile_record(struct tracecmd_input *handle,
			 struct pevent_record *record, int cpu)
{
	static struct handle_data *last_handle;
	struct event_data *event_data;
	struct handle_data *h;
	struct pevent *pevent;
	unsigned long long pid;
	int id;

	if (last_handle && last_handle->handle == handle)
		h = last_handle;
	else {
		for (h = handles; h; h = h->next) {
			if (h->handle == handle)
				break;
		}
		if (!h)
			die("Handle not found?");
		last_handle = h;
	}

	if (record->missed_events)
		handle_missed_events(h, cpu);

	pevent = h->pevent;

	id = pevent_data_type(pevent, record);

	for (event_data = h->events; event_data; event_data = event_data->next) {
		if (event_data->id == id)
			break;
	}

	if (!event_data)
		return -1;


	/* Get this current PID */
	pevent_read_number_field(h->common_pid, record->data, &pid);

	if (event_data->handle_event)
		event_data->handle_event(h, pid, event_data, record, cpu);
	else
		handle_event_data(h, pid, event_data, record, cpu);

	return 0;
}

static struct event_data *
add_event(struct handle_data *h, const char *system, const char *event_name,
	  enum event_data_type type)
{
	struct event_format *event;
	struct event_data *event_data;

	event = pevent_find_event_by_name(h->pevent, system, event_name);
	if (!event)
		return NULL;

	if (!h->common_pid) {
		h->common_pid = pevent_find_common_field(event, "common_pid");
		if (!h->common_pid)
			die("No 'common_pid' found in event");
	}

	event_data = malloc_or_die(sizeof(*event_data));
	memset(event_data, 0, sizeof(*event_data));
	event_data->id = event->id;
	event_data->event = event;
	event_data->type = type;

	event_data->next = h->events;
	h->events = event_data;

	return event_data;
}

void mate_events(struct handle_data *h, struct event_data *start,
		 const char *pid_field, const char *end_match_field,
		 struct event_data *end, const char *start_match_field,
		 int migrate)
{
	start->end = end;
	end->start = start;

	if (pid_field) {
		start->pid_field = pevent_find_field(start->event, pid_field);
		if (!start->pid_field)
			die("Event: %s does not have field %s",
			    start->event->name, pid_field);
	}

	/* Field to match with end */
	start->end_match_field = pevent_find_field(start->event, end_match_field);
	if (!start->end_match_field)
		die("Event: %s does not have field %s",
		    start->event->name, end_match_field);

	/* Field to match with start */
	end->start_match_field = pevent_find_field(end->event, start_match_field);
	if (!end->start_match_field)
		die("Event: %s does not have field %s",
		    end->event->name, start_match_field);

	start->migrate = migrate;
}

void fgraph_print(struct trace_seq *s, struct event_hash *event_hash)
{
	const char *func;

	func = pevent_find_function(event_hash->event_data->event->pevent,
				    event_hash->val);
	if (func)
		trace_seq_printf(s, "func: %s()", func);
	else
		trace_seq_printf(s, "func: 0x%llx", event_hash->val);
}

void sched_switch_print(struct trace_seq *s, struct event_hash *event_hash)
{
	const char states[] = TASK_STATE_TO_CHAR_STR;
	int i;

	trace_seq_printf(s, "%s:", event_hash->event_data->event->name);

	if (event_hash->val) {
		int val = event_hash->val;

		for (i = 0; val && i < sizeof(states) - 1; i++, val >>= 1) {
			if (val & 1)
				trace_seq_putc(s, states[i+1]);
		}
	} else
		trace_seq_putc(s, 'R');
}

static int handle_sched_switch_event(struct handle_data *h,
				     unsigned long long pid,
				     struct event_data *event_data,
				     struct pevent_record *record, int cpu)
{
	struct task_data *task;
	unsigned long long prev_pid;
	unsigned long long prev_state;
	unsigned long long next_pid;
	struct start_data *start;

	/* pid_field holds prev_pid, data_field holds prev_state */
	pevent_read_number_field(event_data->pid_field,
				 record->data, &prev_pid);

	pevent_read_number_field(event_data->data_field,
				 record->data, &prev_state);

	/* only care about real states */
	prev_state &= TASK_STATE_MAX - 1;

	/* end_match_field holds next_pid */
	pevent_read_number_field(event_data->end_match_field,
				 record->data, &next_pid);

	task = find_task(h, prev_pid);
	if (!task->comm)
		add_task_comm(task, h->switch_prev_comm, record);

	if (prev_state)
		task->sleeping = 1;
	else
		task->sleeping = 0;

	/* task is being scheduled out. prev_state tells why */
	start = add_start(task, event_data, record, prev_pid, prev_state);
	task->last_start = start;

	task = find_task(h, next_pid);
	if (!task->comm)
		add_task_comm(task, h->switch_next_comm, record);

	/*
	 * If the next task was blocked, it required a wakeup to
	 * restart, and there should be one.
	 * But if it was preempted, we look for the previous sched switch.
	 * Unfortunately, we have to look for both types of events as
	 * we do not know why next_pid scheduled out.
	 *
	 * event_data->start holds the sched_wakeup event data.
	 */
	find_and_update_start(task, event_data->start, record->ts, next_pid);

	/* Look for this task if it was preempted (no wakeup found). */
	find_and_update_start(task, event_data, record->ts, next_pid);

	return 0;
}

static int handle_stacktrace_event(struct handle_data *h,
				   unsigned long long pid,
				   struct event_data *event_data,
				   struct pevent_record *record, int cpu)
{
	struct task_data *proxy;
	struct task_data *task;
	unsigned long long size;
	struct event_hash *event_hash;
	struct start_data *start;
	void *caller;

	task = find_task(h, pid);

	if ((proxy = task->proxy)) {
		task->proxy = NULL;
		task = proxy;
	}

	if (!task->last_start && !task->last_event)
		return 0;

	/*
	 * start_match_field holds the size.
	 * data_field holds the caller location.
	 */
	size = record->size - event_data->data_field->offset;
	caller = record->data + event_data->data_field->offset;

	/*
	 * If there's a "start" then don't add the stack until
	 * it finds a matching "end".
	 */
	if ((start = task->last_start)) {
		tracecmd_record_ref(record);
		start->stack.record = record;
		start->stack.size = size;
		start->stack.caller = caller;
		task->last_start = NULL;
		task->last_event = NULL;
		return 0;
	}

	event_hash = task->last_event;
	task->last_event = NULL;

	add_event_stack(event_hash, caller, size, event_hash->last_time);
	
	return 0;
}

static int handle_process_exec(struct handle_data *h,
			       unsigned long long pid,
			       struct event_data *event_data,
			       struct pevent_record *record, int cpu)
{
	struct task_data *task;
	unsigned long long val;

	/* Task has execed, remove the comm for it */
	if (event_data->data_field) {
		pevent_read_number_field(event_data->data_field,
					 record->data, &val);
		pid = val;
	}

	task = find_task(h, pid);
	free(task->comm);
	task->comm = NULL;

	return 0;
}

static int handle_sched_wakeup_event(struct handle_data *h,
				     unsigned long long pid,
				     struct event_data *event_data,
				     struct pevent_record *record, int cpu)
{
	struct task_data *proxy;
	struct task_data *task = NULL;
	struct start_data *start;
	unsigned long long success;

	proxy = find_task(h, pid);

	/* If present, data_field holds "success" */
	if (event_data->data_field) {
		pevent_read_number_field(event_data->data_field,
					 record->data, &success);

		/* If not a successful wakeup, ignore this */
		if (!success)
			return 0;
	}

	pevent_read_number_field(event_data->pid_field,
				 record->data, &pid);

	task = find_task(h, pid);
	if (!task->comm)
		add_task_comm(task, h->wakeup_comm, record);

	/* if the task isn't sleeping, then ignore the wake up */
	if (!task->sleeping)
		return 0;

	/* It's being woken up */
	task->sleeping = 0;

	/*
	 * We need the stack trace to be hooked to the woken up
	 * task, not the waker.
	 */
	proxy->proxy = task;

	/* There should be a blocked schedule out of this task */
	find_and_update_start(task, event_data->start, record->ts, pid);

	/* Set this up for timing how long the wakeup takes */
	start = add_start(task, event_data, record, pid, pid);
	task->last_start = start;

	return 0;
}

void trace_init_profile(struct tracecmd_input *handle)
{
	struct pevent *pevent = tracecmd_get_pevent(handle);
	struct handle_data *h;
	struct event_data *sched_switch;
	struct event_data *sched_wakeup;
	struct event_data *irq_entry;
	struct event_data *irq_exit;
	struct event_data *softirq_entry;
	struct event_data *softirq_exit;
	struct event_data *softirq_raise;
	struct event_data *fgraph_entry;
	struct event_data *fgraph_exit;
	struct event_data *syscall_enter;
	struct event_data *syscall_exit;
	struct event_data *process_exec;
	struct event_data *stacktrace;

	h = malloc_or_die(sizeof(*h));
	memset(h, 0, sizeof(*h));
	h->next = handles;
	handles = h;

	trace_hash_init(&h->task_hash, 1024);
	list_head_init(&h->all_starts);

	h->handle = handle;
	h->pevent = pevent;

	h->cpus = tracecmd_cpus(handle);

	h->cpu_data = malloc_or_die(h->cpus * sizeof(*h->cpu_data));
	memset(h->cpu_data, 0, h->cpus * sizeof(h->cpu_data));

	irq_entry = add_event(h, "irq", "irq_handler_entry", EVENT_TYPE_IRQ);
	irq_exit = add_event(h, "irq", "irq_handler_exit", EVENT_TYPE_IRQ);
	softirq_entry = add_event(h, "irq", "softirq_entry", EVENT_TYPE_SOFTIRQ);
	softirq_exit = add_event(h, "irq", "softirq_exit", EVENT_TYPE_SOFTIRQ);
	softirq_raise = add_event(h, "irq", "softirq_raise", EVENT_TYPE_SOFTIRQ_RAISE);
	sched_wakeup = add_event(h, "sched", "sched_wakeup", EVENT_TYPE_WAKEUP);
	sched_switch = add_event(h, "sched", "sched_switch", EVENT_TYPE_SCHED_SWITCH);
	fgraph_entry = add_event(h, "ftrace", "funcgraph_entry", EVENT_TYPE_FUNC);
	fgraph_exit = add_event(h, "ftrace", "funcgraph_exit", EVENT_TYPE_FUNC);
	syscall_enter = add_event(h, "raw_syscalls", "sys_enter", EVENT_TYPE_SYSCALL);
	syscall_exit = add_event(h, "raw_syscalls", "sys_exit", EVENT_TYPE_SYSCALL);

	process_exec = add_event(h, "sched", "sched_process_exec",
				 EVENT_TYPE_PROCESS_EXEC);

	stacktrace = add_event(h, "ftrace", "kernel_stack", EVENT_TYPE_STACK);
	if (stacktrace) {
		stacktrace->handle_event = handle_stacktrace_event;

		stacktrace->data_field = pevent_find_field(stacktrace->event,
							    "caller");
		if (!stacktrace->data_field)
			die("Event: %s does not have field caller",
			    stacktrace->event->name);
	}

	if (process_exec) {
		process_exec->handle_event = handle_process_exec;
		process_exec->data_field = pevent_find_field(process_exec->event,
							     "old_pid");
	}

	if (sched_switch) {
		sched_switch->handle_event = handle_sched_switch_event;
		sched_switch->data_field = pevent_find_field(sched_switch->event,
							     "prev_state");
		if (!sched_switch->data_field)
			die("Event: %s does not have field prev_state",
			    sched_switch->event->name);

		h->switch_prev_comm = pevent_find_field(sched_switch->event,
							"prev_comm");
		if (!h->switch_prev_comm)
			die("Event: %s does not have field prev_comm",
			    sched_switch->event->name);

		h->switch_next_comm = pevent_find_field(sched_switch->event,
							"next_comm");
		if (!h->switch_next_comm)
			die("Event: %s does not have field next_comm",
			    sched_switch->event->name);

		sched_switch->print_func = sched_switch_print;
	}

	if (sched_switch && sched_wakeup) {
		mate_events(h, sched_switch, "prev_pid", "next_pid", 
			    sched_wakeup, "pid", 1);
		mate_events(h, sched_wakeup, "pid", "pid",
			    sched_switch, "prev_pid", 1);
		sched_wakeup->handle_event = handle_sched_wakeup_event;

		/* The 'success' field may or may not be present */
		sched_wakeup->data_field = pevent_find_field(sched_wakeup->event,
							     "success");

		h->wakeup_comm = pevent_find_field(sched_wakeup->event, "comm");
		if (!h->wakeup_comm)
			die("Event: %s does not have field comm",
			    sched_wakeup->event->name);
	}

	if (irq_entry && irq_exit)
		mate_events(h, irq_entry, NULL, "irq", irq_exit, "irq", 0);

	if (softirq_entry && softirq_exit)
		mate_events(h, softirq_entry, NULL, "vec", softirq_exit, "vec", 0);

	if (softirq_entry && softirq_raise)
		mate_events(h, softirq_raise, NULL, "vec", softirq_entry, "vec", 0);

	if (fgraph_entry && fgraph_exit) {
		mate_events(h, fgraph_entry, NULL, "func", fgraph_exit, "func", 1);
		fgraph_entry->print_func = fgraph_print;
	}

	if (syscall_enter && syscall_exit)
		mate_events(h, syscall_enter, NULL, "id", syscall_exit, "id", 1);
}

static void output_event_stack(struct pevent *pevent, struct stack_data *stack)
{
	int longsize = pevent_get_long_size(pevent);
	unsigned long long val;
	const char *func;
	unsigned long long stop = -1ULL;
	void *ptr;
	int i;

	if (longsize < 8)
		stop &= (1ULL << (longsize * 8)) - 1;

	if (stack->count)
		stack->time_avg = stack->time / stack->count;

	printf("     <stack> %lld total:%lld min:%lld max:%lld avg=%lld\n",
	       stack->count, stack->time, stack->time_min, stack->time_max,
	       stack->time_avg);

	for (i = 0; i < stack->size; i += longsize) {
		ptr = stack->caller + i;
		switch (longsize) {
		case 4:
			/* todo, read value from pevent */
			val = *(unsigned int *)ptr;
			break;
		case 8:
			val = *(unsigned long long *)ptr;
			break;
		default:
			die("Strange long size %d", longsize);
		}
		if (val == stop)
			break;
		func = pevent_find_function(pevent, val);
		if (func)
			printf("       => %s (0x%llx)\n", func, val);
		else
			printf("       => 0x%llx\n", val);
	}
}

struct stack_chain {
	struct stack_chain *children;
	unsigned long long	val;
	unsigned long long	time;
	unsigned long long	time_min;
	unsigned long long	time_max;
	unsigned long long	time_avg;
	unsigned long long	count;
	int			percent;
	int			nr_children;
};

static int compare_chains(const void *a, const void *b)
{
	const struct stack_chain * A = a;
	const struct stack_chain * B = b;

	if (A->time > B->time)
		return -1;
	if (A->time < B->time)
		return 1;
	/* If stacks don't use time, then use count */
	if (A->count > B->count)
		return -1;
	if (A->count < B->count)
		return 1;
	return 0;
}

static int calc_percent(unsigned long long val, unsigned long long total)
{
	return (val * 100 + total / 2) / total;
}

static int stack_overflows(struct stack_data *stack, int longsize, int level)
{
	return longsize * level > stack->size - longsize;
}

static unsigned long long
stack_value(struct stack_data *stack, int longsize, int level)
{
	void *ptr;

	ptr = &stack->caller[longsize * level];
	return longsize == 8 ? *(u64 *)ptr : *(unsigned *)ptr;
}

static struct stack_chain *
make_stack_chain(struct stack_data **stacks, int cnt, int longsize, int level,
		 int *nr_children)
{
	struct stack_chain *chain;
	unsigned long long	total_time = 0;
	unsigned long long	total_count = 0;
	unsigned long long	time;
	unsigned long long	time_min;
	unsigned long long	time_max;
	unsigned long long	count;
	unsigned long long	stop = -1ULL;
	int nr_chains = 0;
	u64 last = 0;
	u64 val;
	int start;
	int i;
	int x;

	if (longsize < 8)
		stop &= (1ULL << (longsize * 8)) - 1;

	/* First find out how many diffs there are */
	for (i = 0; i < cnt; i++) {
		if (stack_overflows(stacks[i], longsize, level))
			continue;

		val = stack_value(stacks[i], longsize, level);

		if (val == stop)
			continue;

		if (!nr_chains || val != last)
			nr_chains++;
		last = val;
	}

	if (!nr_chains) {
		*nr_children = 0;
		return NULL;
	}

	chain = malloc_or_die(sizeof(*chain) * nr_chains);
	memset(chain, 0, sizeof(*chain) * nr_chains);

	x = 0;
	count = 0;
	start = 0;
	time = 0;
	time_min = 0;
	time_max = 0;

	for (i = 0; i < cnt; i++) {
		if (stack_overflows(stacks[i], longsize, level)) {
			start = i+1;
			continue;
		}

		val = stack_value(stacks[i], longsize, level);

		if (val == stop) {
			start = i+1;
			continue;
		}

		count += stacks[i]->count;
		time += stacks[i]->time;
		if (stacks[i]->time_max > time_max)
			time_max = stacks[i]->time_max;
		if (i == start || stacks[i]->time_min < time_min)
			time_min = stacks[i]->time_min;

		if (i == cnt - 1 ||
		    stack_overflows(stacks[i+1], longsize, level) ||
		    val != stack_value(stacks[i+1], longsize, level)) {

			total_time += time;
			total_count += count;
			chain[x].val = val;
			chain[x].time_avg = time / count;
			chain[x].count = count;
			chain[x].time = time;
			chain[x].time_min = time_min;
			chain[x].time_max = time_max;
			chain[x].children =
				make_stack_chain(&stacks[start], (i - start) + 1,
						 longsize, level+1,
						 &chain[x].nr_children);
			x++;
			start = i + 1;
			count = 0;
			time = 0;
			time_min = 0;
			time_max = 0;
		}
	}

	qsort(chain, nr_chains, sizeof(*chain), compare_chains);

	*nr_children = nr_chains;

	/* Should never happen */
	if (!total_time && !total_count)
		return chain;


	/* Now calculate percentage */
	time = 0;
	for (i = 0; i < nr_chains; i++) {
		if (total_time)
			chain[i].percent = calc_percent(chain[i].time, total_time);
		/* In case stacks don't have time */
		else if (total_count)
			chain[i].percent = calc_percent(chain[i].count, total_count);
	}

	return chain;
}

static void free_chain(struct stack_chain *chain, int nr_chains)
{
	int i;

	if (!chain)
		return;

	for (i = 0; i < nr_chains; i++)
		free_chain(chain[i].children, chain[i].nr_children);

	free(chain);
}

#define INDENT	5

static void print_indent(int level, unsigned long long mask)
{
	char line;
	int p;

	for (p = 0; p < level + 1; p++) {
		if (mask & (1ULL << p))
			line = '|';
		else
			line = ' ';
		printf("%*c ", INDENT, line);
	}
}

static void print_chain_func(struct pevent *pevent, struct stack_chain *chain)
{
	unsigned long long val = chain->val;
	const char *func;

	func = pevent_find_function(pevent, val);
	if (func)
		printf("%s (0x%llx)\n", func, val);
	else
		printf("0x%llx\n", val);
}

static void output_chain(struct pevent *pevent, struct stack_chain *chain, int level,
			 int nr_chains, unsigned long long *mask)
{
	struct stack_chain *child;
	int nr_children;
	int i;
	char line = '|';

	if (!nr_chains)
		return;

	*mask |= (1ULL << (level + 1));
	print_indent(level + 1, *mask);
	printf("\n");

	for (i = 0; i < nr_chains; i++) {

		print_indent(level, *mask);

		printf("%*c ", INDENT, '+');

		if (i == nr_chains - 1) {
			*mask &= ~(1ULL << (level + 1));
			line = ' ';
		}

		print_chain_func(pevent, &chain[i]);

		print_indent(level, *mask);

		printf("%*c ", INDENT, line);
		printf("  %d%% (%lld)", chain[i].percent, chain[i].count);
		if (chain[i].time)
			printf(" time:%lld max:%lld min:%lld avg:%lld",
			       chain[i].time, chain[i].time_max,
			       chain[i].time_min, chain[i].time_avg);
		printf("\n");

		for (child = chain[i].children, nr_children = chain[i].nr_children;
		     child && nr_children == 1;
		     nr_children = child->nr_children, child = child->children) {
			print_indent(level, *mask);
			printf("%*c ", INDENT, line);
			printf("   ");
			print_chain_func(pevent, child);
		}

		if (child)
			output_chain(pevent, child, level+1, nr_children, mask);

		print_indent(level + 1, *mask);
		printf("\n");
	}
	*mask &= ~(1ULL << (level + 1));
	print_indent(level, *mask);
	printf("\n");
}

static int compare_stacks(const void *a, const void *b)
{
	struct stack_data * const *A = a;
	struct stack_data * const *B = b;
	unsigned int sa, sb;
	int size;
	int i;

	/* only compare up to the smaller size of the two */
	if ((*A)->size > (*B)->size)
		size = (*B)->size;
	else
		size = (*A)->size;

	for (i = 0; i < size; i += sizeof(sa)) {
		sa = *(unsigned *)&(*A)->caller[i];
		sb = *(unsigned *)&(*B)->caller[i];
		if (sa > sb)
			return 1;
		if (sa < sb)
			return -1;
	}

	/* They are the same up to size. Then bigger size wins */
	if ((*A)->size > (*B)->size)
		return 1;
	if ((*A)->size < (*B)->size)
		return -1;
	return 0;
}

static void output_stacks(struct pevent *pevent, struct trace_hash *stack_hash)
{
	struct trace_hash_item **bucket;
	struct trace_hash_item *item;
	struct stack_data **stacks;
	struct stack_chain *chain;
	unsigned long long mask = 0;
	int nr_chains;
	int longsize = pevent_get_long_size(pevent);
	int nr_stacks;
	int i;

	nr_stacks = 0;
	trace_hash_for_each_bucket(bucket, stack_hash) {
		trace_hash_for_each_item(item, bucket) {
			nr_stacks++;
		}
	}

	stacks = malloc_or_die(sizeof(*stacks) * nr_stacks);

	nr_stacks = 0;
	trace_hash_for_each_bucket(bucket, stack_hash) {
		trace_hash_for_each_item(item, bucket) {
			stacks[nr_stacks++] = stack_from_item(item);
		}
	}

	qsort(stacks, nr_stacks, sizeof(*stacks), compare_stacks);

	chain = make_stack_chain(stacks, nr_stacks, longsize, 0, &nr_chains);

	output_chain(pevent, chain, 0, nr_chains, &mask);

	if (0)
		for (i = 0; i < nr_stacks; i++)
			output_event_stack(pevent, stacks[i]);

	free(stacks);
	free_chain(chain, nr_chains);
}

static void output_event(struct event_hash *event_hash)
{
	struct event_data *event_data = event_hash->event_data;
	struct pevent *pevent = event_data->event->pevent;
	struct trace_seq s;

	trace_seq_init(&s);

	if (event_data->print_func)
		event_data->print_func(&s, event_hash);
	else
		trace_seq_printf(&s, "%s:%lld",
				 event_data->event->name,
				 event_hash->val);
	trace_seq_terminate(&s);

	event_hash->time_avg = event_hash->time_total / event_hash->count;

	printf("  Event: %s (%lld) Total: %lld Avg: %lld Max: %lld Min:%lld\n",
	       s.buffer,
	       event_hash->count, event_hash->time_total, event_hash->time_avg,
	       event_hash->time_max, event_hash->time_min);

	trace_seq_destroy(&s);

	output_stacks(pevent, &event_hash->stacks);
}

static int compare_events(const void *a, const void *b)
{
	struct event_hash * const *A = a;
	struct event_hash * const *B = b;
	const struct event_data *event_data_a = (*A)->event_data;
	const struct event_data *event_data_b = (*B)->event_data;

	/* Schedule switch goes first */
	if (event_data_a->type == EVENT_TYPE_SCHED_SWITCH) {
		if (event_data_b->type != EVENT_TYPE_SCHED_SWITCH)
			return -1;
		/* lower the state the better */
		if ((*A)->val > (*B)->val)
			return 1;
		if ((*A)->val < (*B)->val)
			return -1;
		return 0;
	} else if (event_data_b->type == EVENT_TYPE_SCHED_SWITCH)
			return 1;

	/* Wakeups are next */
	if (event_data_a->type == EVENT_TYPE_WAKEUP) {
		if (event_data_b->type != EVENT_TYPE_WAKEUP)
			return -1;
		return 0;
	} else if (event_data_b->type == EVENT_TYPE_WAKEUP)
		return 1;

	if (event_data_a->id > event_data_b->id)
		return 1;
	if (event_data_a->id < event_data_b->id)
		return -1;
	return 0;
}

static void output_task(struct handle_data *h, struct task_data *task)
{
	struct trace_hash_item **bucket;
	struct trace_hash_item *item;
	struct event_hash **events;
	const char *comm;
	int nr_events = 0;
	int i;

	if (task->comm)
		comm = task->comm;
	else
		comm = pevent_data_comm_from_pid(h->pevent, task->pid);

	printf("\ntask: %s-%d\n", comm, task->pid);

	trace_hash_for_each_bucket(bucket, &task->event_hash) {
		trace_hash_for_each_item(item, bucket) {
			nr_events++;
		}
	}

	events = malloc_or_die(sizeof(*events) * nr_events);

	i = 0;
	trace_hash_for_each_bucket(bucket, &task->event_hash) {
		trace_hash_for_each_item(item, bucket) {
			events[i++] = event_from_item(item);
		}
	}

	qsort(events, nr_events, sizeof(*events), compare_events);

	for (i = 0; i < nr_events; i++)
		output_event(events[i]);

	free(events);
}

static int compare_tasks(const void *a, const void *b)
{
	struct task_data * const *A = a;
	struct task_data * const *B = b;

	if ((*A)->pid > (*B)->pid)
		return 1;
	else if ((*A)->pid < (*B)->pid)
		return -1;
	return 0;
}

static void free_event_hash(struct event_hash *event_hash)
{
	struct trace_hash_item **bucket;
	struct trace_hash_item *item;
	struct stack_data *stack;

	trace_hash_for_each_bucket(bucket, &event_hash->stacks) {
		trace_hash_while_item(item, bucket) {
			stack = stack_from_item(item);
			trace_hash_del(&stack->hash);
			free(stack);
		}
	}
	trace_hash_free(&event_hash->stacks);
	free(event_hash);
}

static void free_task(struct task_data *task)
{
	struct trace_hash_item **bucket;
	struct trace_hash_item *item;
	struct start_data *start;
	struct event_hash *event_hash;

	free(task->comm);

	trace_hash_for_each_bucket(bucket, &task->start_hash) {
		trace_hash_while_item(item, bucket) {
			start = start_from_item(item);
			if (start->stack.record)
				free_record(start->stack.record);
			list_del(&start->list);
			trace_hash_del(item);
			free(start);
		}
	}
	trace_hash_free(&task->start_hash);

	trace_hash_for_each_bucket(bucket, &task->event_hash) {
		trace_hash_while_item(item, bucket) {
			event_hash = event_from_item(item);
			trace_hash_del(item);
			free_event_hash(event_hash);
		}
	}
	trace_hash_free(&task->event_hash);

	free(task);
}

static void output_handle(struct handle_data *h)
{
	struct trace_hash_item **bucket;
	struct trace_hash_item *item;
	struct task_data **tasks;
	int nr_tasks = 0;
	int i;

	trace_hash_for_each_bucket(bucket, &h->task_hash) {
		trace_hash_for_each_item(item, bucket) {
			nr_tasks++;
		}
	}

	tasks = malloc_or_die(sizeof(*tasks) * nr_tasks);

	nr_tasks = 0;

	trace_hash_for_each_bucket(bucket, &h->task_hash) {
		trace_hash_while_item(item, bucket) {
			tasks[nr_tasks++] = task_from_item(item);
			trace_hash_del(item);
		}
	}

	qsort(tasks, nr_tasks, sizeof(*tasks), compare_tasks);

	for (i = 0; i < nr_tasks; i++) {
		output_task(h, tasks[i]);
		free_task(tasks[i]);
	}

	free(tasks);
}

int trace_profile(void)
{
	struct handle_data *h;

	for (h = handles; h; h = h->next) {
		output_handle(h);
		trace_hash_free(&h->task_hash);
	}

	return 0;
}
