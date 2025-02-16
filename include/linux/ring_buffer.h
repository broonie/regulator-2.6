#ifndef _LINUX_RING_BUFFER_H
#define _LINUX_RING_BUFFER_H

#include <linux/mm.h>
#include <linux/seq_file.h>

struct ring_buffer;
struct ring_buffer_iter;

/*
 * Don't reference this struct directly, use functions below.
 */
struct ring_buffer_event {
	u32		type:2, len:3, time_delta:27;
	u32		array[];
};

/**
 * enum ring_buffer_type - internal ring buffer types
 *
 * @RINGBUF_TYPE_PADDING:	Left over page padding
 *				 array is ignored
 *				 size is variable depending on how much
 *				  padding is needed
 *
 * @RINGBUF_TYPE_TIME_EXTEND:	Extend the time delta
 *				 array[0] = time delta (28 .. 59)
 *				 size = 8 bytes
 *
 * @RINGBUF_TYPE_TIME_STAMP:	Sync time stamp with external clock
 *				 array[0]    = tv_nsec
 *				 array[1..2] = tv_sec
 *				 size = 16 bytes
 *
 * @RINGBUF_TYPE_DATA:		Data record
 *				 If len is zero:
 *				  array[0] holds the actual length
 *				  array[1..(length+3)/4] holds data
 *				  size = 4 + 4 + length (bytes)
 *				 else
 *				  length = len << 2
 *				  array[0..(length+3)/4-1] holds data
 *				  size = 4 + length (bytes)
 */
enum ring_buffer_type {
	RINGBUF_TYPE_PADDING,
	RINGBUF_TYPE_TIME_EXTEND,
	/* FIXME: RINGBUF_TYPE_TIME_STAMP not implemented */
	RINGBUF_TYPE_TIME_STAMP,
	RINGBUF_TYPE_DATA,
};

unsigned ring_buffer_event_length(struct ring_buffer_event *event);
void *ring_buffer_event_data(struct ring_buffer_event *event);

/**
 * ring_buffer_event_time_delta - return the delta timestamp of the event
 * @event: the event to get the delta timestamp of
 *
 * The delta timestamp is the 27 bit timestamp since the last event.
 */
static inline unsigned
ring_buffer_event_time_delta(struct ring_buffer_event *event)
{
	return event->time_delta;
}

/*
 * size is in bytes for each per CPU buffer.
 */
struct ring_buffer *
ring_buffer_alloc(unsigned long size, unsigned flags);
void ring_buffer_free(struct ring_buffer *buffer);

int ring_buffer_resize(struct ring_buffer *buffer, unsigned long size);

struct ring_buffer_event *
ring_buffer_lock_reserve(struct ring_buffer *buffer,
			 unsigned long length,
			 unsigned long *flags);
int ring_buffer_unlock_commit(struct ring_buffer *buffer,
			      struct ring_buffer_event *event,
			      unsigned long flags);
int ring_buffer_write(struct ring_buffer *buffer,
		      unsigned long length, void *data);

struct ring_buffer_event *
ring_buffer_peek(struct ring_buffer *buffer, int cpu, u64 *ts);
struct ring_buffer_event *
ring_buffer_consume(struct ring_buffer *buffer, int cpu, u64 *ts);

struct ring_buffer_iter *
ring_buffer_read_start(struct ring_buffer *buffer, int cpu);
void ring_buffer_read_finish(struct ring_buffer_iter *iter);

struct ring_buffer_event *
ring_buffer_iter_peek(struct ring_buffer_iter *iter, u64 *ts);
struct ring_buffer_event *
ring_buffer_read(struct ring_buffer_iter *iter, u64 *ts);
void ring_buffer_iter_reset(struct ring_buffer_iter *iter);
int ring_buffer_iter_empty(struct ring_buffer_iter *iter);

unsigned long ring_buffer_size(struct ring_buffer *buffer);

void ring_buffer_reset_cpu(struct ring_buffer *buffer, int cpu);
void ring_buffer_reset(struct ring_buffer *buffer);

int ring_buffer_swap_cpu(struct ring_buffer *buffer_a,
			 struct ring_buffer *buffer_b, int cpu);

int ring_buffer_empty(struct ring_buffer *buffer);
int ring_buffer_empty_cpu(struct ring_buffer *buffer, int cpu);

void ring_buffer_record_disable(struct ring_buffer *buffer);
void ring_buffer_record_enable(struct ring_buffer *buffer);
void ring_buffer_record_disable_cpu(struct ring_buffer *buffer, int cpu);
void ring_buffer_record_enable_cpu(struct ring_buffer *buffer, int cpu);

unsigned long ring_buffer_entries(struct ring_buffer *buffer);
unsigned long ring_buffer_overruns(struct ring_buffer *buffer);

u64 ring_buffer_time_stamp(int cpu);
void ring_buffer_normalize_time_stamp(int cpu, u64 *ts);

void tracing_on(void);
void tracing_off(void);
void tracing_off_permanent(void);

void *ring_buffer_alloc_read_page(struct ring_buffer *buffer);
void ring_buffer_free_read_page(struct ring_buffer *buffer, void *data);
int ring_buffer_read_page(struct ring_buffer *buffer,
			  void **data_page, int cpu, int full);

enum ring_buffer_flags {
	RB_FL_OVERWRITE		= 1 << 0,
};

#endif /* _LINUX_RING_BUFFER_H */
