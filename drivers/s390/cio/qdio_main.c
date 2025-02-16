/*
 * linux/drivers/s390/cio/qdio_main.c
 *
 * Linux for s390 qdio support, buffer handling, qdio API and module support.
 *
 * Copyright 2000,2008 IBM Corp.
 * Author(s): Utz Bacher <utz.bacher@de.ibm.com>
 *	      Jan Glauber <jang@linux.vnet.ibm.com>
 * 2.6 cio integration by Cornelia Huck <cornelia.huck@de.ibm.com>
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <asm/atomic.h>
#include <asm/debug.h>
#include <asm/qdio.h>

#include "cio.h"
#include "css.h"
#include "device.h"
#include "qdio.h"
#include "qdio_debug.h"
#include "qdio_perf.h"

MODULE_AUTHOR("Utz Bacher <utz.bacher@de.ibm.com>,"\
	"Jan Glauber <jang@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("QDIO base support");
MODULE_LICENSE("GPL");

static inline int do_siga_sync(struct subchannel_id schid,
			       unsigned int out_mask, unsigned int in_mask)
{
	register unsigned long __fc asm ("0") = 2;
	register struct subchannel_id __schid asm ("1") = schid;
	register unsigned long out asm ("2") = out_mask;
	register unsigned long in asm ("3") = in_mask;
	int cc;

	asm volatile(
		"	siga	0\n"
		"	ipm	%0\n"
		"	srl	%0,28\n"
		: "=d" (cc)
		: "d" (__fc), "d" (__schid), "d" (out), "d" (in) : "cc");
	return cc;
}

static inline int do_siga_input(struct subchannel_id schid, unsigned int mask)
{
	register unsigned long __fc asm ("0") = 1;
	register struct subchannel_id __schid asm ("1") = schid;
	register unsigned long __mask asm ("2") = mask;
	int cc;

	asm volatile(
		"	siga	0\n"
		"	ipm	%0\n"
		"	srl	%0,28\n"
		: "=d" (cc)
		: "d" (__fc), "d" (__schid), "d" (__mask) : "cc", "memory");
	return cc;
}

/**
 * do_siga_output - perform SIGA-w/wt function
 * @schid: subchannel id or in case of QEBSM the subchannel token
 * @mask: which output queues to process
 * @bb: busy bit indicator, set only if SIGA-w/wt could not access a buffer
 * @fc: function code to perform
 *
 * Returns cc or QDIO_ERROR_SIGA_ACCESS_EXCEPTION.
 * Note: For IQDC unicast queues only the highest priority queue is processed.
 */
static inline int do_siga_output(unsigned long schid, unsigned long mask,
				 unsigned int *bb, unsigned int fc)
{
	register unsigned long __fc asm("0") = fc;
	register unsigned long __schid asm("1") = schid;
	register unsigned long __mask asm("2") = mask;
	int cc = QDIO_ERROR_SIGA_ACCESS_EXCEPTION;

	asm volatile(
		"	siga	0\n"
		"0:	ipm	%0\n"
		"	srl	%0,28\n"
		"1:\n"
		EX_TABLE(0b, 1b)
		: "+d" (cc), "+d" (__fc), "+d" (__schid), "+d" (__mask)
		: : "cc", "memory");
	*bb = ((unsigned int) __fc) >> 31;
	return cc;
}

static inline int qdio_check_ccq(struct qdio_q *q, unsigned int ccq)
{
	/* all done or next buffer state different */
	if (ccq == 0 || ccq == 32)
		return 0;
	/* not all buffers processed */
	if (ccq == 96 || ccq == 97)
		return 1;
	/* notify devices immediately */
	DBF_ERROR("%4x ccq:%3d", SCH_NO(q), ccq);
	return -EIO;
}

/**
 * qdio_do_eqbs - extract buffer states for QEBSM
 * @q: queue to manipulate
 * @state: state of the extracted buffers
 * @start: buffer number to start at
 * @count: count of buffers to examine
 * @auto_ack: automatically acknowledge buffers
 *
 * Returns the number of successfull extracted equal buffer states.
 * Stops processing if a state is different from the last buffers state.
 */
static int qdio_do_eqbs(struct qdio_q *q, unsigned char *state,
			int start, int count, int auto_ack)
{
	unsigned int ccq = 0;
	int tmp_count = count, tmp_start = start;
	int nr = q->nr;
	int rc;

	BUG_ON(!q->irq_ptr->sch_token);
	qdio_perf_stat_inc(&perf_stats.debug_eqbs_all);

	if (!q->is_input_q)
		nr += q->irq_ptr->nr_input_qs;
again:
	ccq = do_eqbs(q->irq_ptr->sch_token, state, nr, &tmp_start, &tmp_count,
		      auto_ack);
	rc = qdio_check_ccq(q, ccq);

	/* At least one buffer was processed, return and extract the remaining
	 * buffers later.
	 */
	if ((ccq == 96) && (count != tmp_count)) {
		qdio_perf_stat_inc(&perf_stats.debug_eqbs_incomplete);
		return (count - tmp_count);
	}

	if (rc == 1) {
		DBF_DEV_EVENT(DBF_WARN, q->irq_ptr, "EQBS again:%2d", ccq);
		goto again;
	}

	if (rc < 0) {
		DBF_ERROR("%4x EQBS ERROR", SCH_NO(q));
		DBF_ERROR("%3d%3d%2d", count, tmp_count, nr);
		q->handler(q->irq_ptr->cdev,
			   QDIO_ERROR_ACTIVATE_CHECK_CONDITION,
			   0, -1, -1, q->irq_ptr->int_parm);
		return 0;
	}
	return count - tmp_count;
}

/**
 * qdio_do_sqbs - set buffer states for QEBSM
 * @q: queue to manipulate
 * @state: new state of the buffers
 * @start: first buffer number to change
 * @count: how many buffers to change
 *
 * Returns the number of successfully changed buffers.
 * Does retrying until the specified count of buffer states is set or an
 * error occurs.
 */
static int qdio_do_sqbs(struct qdio_q *q, unsigned char state, int start,
			int count)
{
	unsigned int ccq = 0;
	int tmp_count = count, tmp_start = start;
	int nr = q->nr;
	int rc;

	if (!count)
		return 0;

	BUG_ON(!q->irq_ptr->sch_token);
	qdio_perf_stat_inc(&perf_stats.debug_sqbs_all);

	if (!q->is_input_q)
		nr += q->irq_ptr->nr_input_qs;
again:
	ccq = do_sqbs(q->irq_ptr->sch_token, state, nr, &tmp_start, &tmp_count);
	rc = qdio_check_ccq(q, ccq);
	if (rc == 1) {
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "SQBS again:%2d", ccq);
		qdio_perf_stat_inc(&perf_stats.debug_sqbs_incomplete);
		goto again;
	}
	if (rc < 0) {
		DBF_ERROR("%4x SQBS ERROR", SCH_NO(q));
		DBF_ERROR("%3d%3d%2d", count, tmp_count, nr);
		q->handler(q->irq_ptr->cdev,
			   QDIO_ERROR_ACTIVATE_CHECK_CONDITION,
			   0, -1, -1, q->irq_ptr->int_parm);
		return 0;
	}
	WARN_ON(tmp_count);
	return count - tmp_count;
}

/* returns number of examined buffers and their common state in *state */
static inline int get_buf_states(struct qdio_q *q, unsigned int bufnr,
				 unsigned char *state, unsigned int count,
				 int auto_ack)
{
	unsigned char __state = 0;
	int i;

	BUG_ON(bufnr > QDIO_MAX_BUFFERS_MASK);
	BUG_ON(count > QDIO_MAX_BUFFERS_PER_Q);

	if (is_qebsm(q))
		return qdio_do_eqbs(q, state, bufnr, count, auto_ack);

	for (i = 0; i < count; i++) {
		if (!__state)
			__state = q->slsb.val[bufnr];
		else if (q->slsb.val[bufnr] != __state)
			break;
		bufnr = next_buf(bufnr);
	}
	*state = __state;
	return i;
}

inline int get_buf_state(struct qdio_q *q, unsigned int bufnr,
		  unsigned char *state, int auto_ack)
{
	return get_buf_states(q, bufnr, state, 1, auto_ack);
}

/* wrap-around safe setting of slsb states, returns number of changed buffers */
static inline int set_buf_states(struct qdio_q *q, int bufnr,
				 unsigned char state, int count)
{
	int i;

	BUG_ON(bufnr > QDIO_MAX_BUFFERS_MASK);
	BUG_ON(count > QDIO_MAX_BUFFERS_PER_Q);

	if (is_qebsm(q))
		return qdio_do_sqbs(q, state, bufnr, count);

	for (i = 0; i < count; i++) {
		xchg(&q->slsb.val[bufnr], state);
		bufnr = next_buf(bufnr);
	}
	return count;
}

static inline int set_buf_state(struct qdio_q *q, int bufnr,
				unsigned char state)
{
	return set_buf_states(q, bufnr, state, 1);
}

/* set slsb states to initial state */
void qdio_init_buf_states(struct qdio_irq *irq_ptr)
{
	struct qdio_q *q;
	int i;

	for_each_input_queue(irq_ptr, q, i)
		set_buf_states(q, 0, SLSB_P_INPUT_NOT_INIT,
			       QDIO_MAX_BUFFERS_PER_Q);
	for_each_output_queue(irq_ptr, q, i)
		set_buf_states(q, 0, SLSB_P_OUTPUT_NOT_INIT,
			       QDIO_MAX_BUFFERS_PER_Q);
}

static int qdio_siga_sync(struct qdio_q *q, unsigned int output,
			  unsigned int input)
{
	int cc;

	if (!need_siga_sync(q))
		return 0;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "siga-s:%1d", q->nr);
	qdio_perf_stat_inc(&perf_stats.siga_sync);

	cc = do_siga_sync(q->irq_ptr->schid, output, input);
	if (cc)
		DBF_ERROR("%4x SIGA-S:%2d", SCH_NO(q), cc);
	return cc;
}

inline int qdio_siga_sync_q(struct qdio_q *q)
{
	if (q->is_input_q)
		return qdio_siga_sync(q, 0, q->mask);
	else
		return qdio_siga_sync(q, q->mask, 0);
}

static inline int qdio_siga_sync_out(struct qdio_q *q)
{
	return qdio_siga_sync(q, ~0U, 0);
}

static inline int qdio_siga_sync_all(struct qdio_q *q)
{
	return qdio_siga_sync(q, ~0U, ~0U);
}

static int qdio_siga_output(struct qdio_q *q, unsigned int *busy_bit)
{
	unsigned long schid;
	unsigned int fc = 0;
	u64 start_time = 0;
	int cc;

	if (q->u.out.use_enh_siga)
		fc = 3;

	if (is_qebsm(q)) {
		schid = q->irq_ptr->sch_token;
		fc |= 0x80;
	}
	else
		schid = *((u32 *)&q->irq_ptr->schid);

again:
	cc = do_siga_output(schid, q->mask, busy_bit, fc);

	/* hipersocket busy condition */
	if (*busy_bit) {
		WARN_ON(queue_type(q) != QDIO_IQDIO_QFMT || cc != 2);

		if (!start_time) {
			start_time = get_usecs();
			goto again;
		}
		if ((get_usecs() - start_time) < QDIO_BUSY_BIT_PATIENCE)
			goto again;
	}
	return cc;
}

static inline int qdio_siga_input(struct qdio_q *q)
{
	int cc;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "siga-r:%1d", q->nr);
	qdio_perf_stat_inc(&perf_stats.siga_in);

	cc = do_siga_input(q->irq_ptr->schid, q->mask);
	if (cc)
		DBF_ERROR("%4x SIGA-R:%2d", SCH_NO(q), cc);
	return cc;
}

/* called from thinint inbound handler */
void qdio_sync_after_thinint(struct qdio_q *q)
{
	if (pci_out_supported(q)) {
		if (need_siga_sync_thinint(q))
			qdio_siga_sync_all(q);
		else if (need_siga_sync_out_thinint(q))
			qdio_siga_sync_out(q);
	} else
		qdio_siga_sync_q(q);
}

inline void qdio_stop_polling(struct qdio_q *q)
{
	if (!q->u.in.polling)
		return;

	q->u.in.polling = 0;
	qdio_perf_stat_inc(&perf_stats.debug_stop_polling);

	/* show the card that we are not polling anymore */
	if (is_qebsm(q)) {
		set_buf_states(q, q->last_move_ftc, SLSB_P_INPUT_NOT_INIT,
			       q->u.in.ack_count);
		q->u.in.ack_count = 0;
	} else
		set_buf_state(q, q->last_move_ftc, SLSB_P_INPUT_NOT_INIT);
}

static void announce_buffer_error(struct qdio_q *q, int count)
{
	q->qdio_error |= QDIO_ERROR_SLSB_STATE;

	/* special handling for no target buffer empty */
	if ((!q->is_input_q &&
	    (q->sbal[q->first_to_check]->element[15].flags & 0xff) == 0x10)) {
		qdio_perf_stat_inc(&perf_stats.outbound_target_full);
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "OUTFULL FTC:%3d",
			      q->first_to_check);
		return;
	}

	DBF_ERROR("%4x BUF ERROR", SCH_NO(q));
	DBF_ERROR((q->is_input_q) ? "IN:%2d" : "OUT:%2d", q->nr);
	DBF_ERROR("FTC:%3d C:%3d", q->first_to_check, count);
	DBF_ERROR("F14:%2x F15:%2x",
		  q->sbal[q->first_to_check]->element[14].flags & 0xff,
		  q->sbal[q->first_to_check]->element[15].flags & 0xff);
}

static inline void inbound_primed(struct qdio_q *q, int count)
{
	int new;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "in prim: %3d", count);

	/* for QEBSM the ACK was already set by EQBS */
	if (is_qebsm(q)) {
		if (!q->u.in.polling) {
			q->u.in.polling = 1;
			q->u.in.ack_count = count;
			q->last_move_ftc = q->first_to_check;
			return;
		}

		/* delete the previous ACK's */
		set_buf_states(q, q->last_move_ftc, SLSB_P_INPUT_NOT_INIT,
			       q->u.in.ack_count);
		q->u.in.ack_count = count;
		q->last_move_ftc = q->first_to_check;
		return;
	}

	/*
	 * ACK the newest buffer. The ACK will be removed in qdio_stop_polling
	 * or by the next inbound run.
	 */
	new = add_buf(q->first_to_check, count - 1);
	if (q->u.in.polling) {
		/* reset the previous ACK but first set the new one */
		set_buf_state(q, new, SLSB_P_INPUT_ACK);
		set_buf_state(q, q->last_move_ftc, SLSB_P_INPUT_NOT_INIT);
	}
	else {
		q->u.in.polling = 1;
		set_buf_state(q, q->first_to_check, SLSB_P_INPUT_ACK);
	}

	q->last_move_ftc = new;
	count--;
	if (!count)
		return;

	/*
	 * Need to change all PRIMED buffers to NOT_INIT, otherwise
	 * we're loosing initiative in the thinint code.
	 */
	set_buf_states(q, next_buf(q->first_to_check), SLSB_P_INPUT_NOT_INIT,
		       count);
}

static int get_inbound_buffer_frontier(struct qdio_q *q)
{
	int count, stop;
	unsigned char state;

	/*
	 * Don't check 128 buffers, as otherwise qdio_inbound_q_moved
	 * would return 0.
	 */
	count = min(atomic_read(&q->nr_buf_used), QDIO_MAX_BUFFERS_MASK);
	stop = add_buf(q->first_to_check, count);

	/*
	 * No siga sync here, as a PCI or we after a thin interrupt
	 * will sync the queues.
	 */

	/* need to set count to 1 for non-qebsm */
	if (!is_qebsm(q))
		count = 1;

check_next:
	if (q->first_to_check == stop)
		goto out;

	count = get_buf_states(q, q->first_to_check, &state, count, 1);
	if (!count)
		goto out;

	switch (state) {
	case SLSB_P_INPUT_PRIMED:
		inbound_primed(q, count);
		/*
		 * No siga-sync needed for non-qebsm here, as the inbound queue
		 * will be synced on the next siga-r, resp.
		 * tiqdio_is_inbound_q_done will do the siga-sync.
		 */
		q->first_to_check = add_buf(q->first_to_check, count);
		atomic_sub(count, &q->nr_buf_used);
		goto check_next;
	case SLSB_P_INPUT_ERROR:
		announce_buffer_error(q, count);
		/* process the buffer, the upper layer will take care of it */
		q->first_to_check = add_buf(q->first_to_check, count);
		atomic_sub(count, &q->nr_buf_used);
		break;
	case SLSB_CU_INPUT_EMPTY:
	case SLSB_P_INPUT_NOT_INIT:
	case SLSB_P_INPUT_ACK:
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "in nop");
		break;
	default:
		BUG();
	}
out:
	return q->first_to_check;
}

int qdio_inbound_q_moved(struct qdio_q *q)
{
	int bufnr;

	bufnr = get_inbound_buffer_frontier(q);

	if ((bufnr != q->last_move_ftc) || q->qdio_error) {
		if (!need_siga_sync(q) && !pci_out_supported(q))
			q->u.in.timestamp = get_usecs();

		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "in moved");
		return 1;
	} else
		return 0;
}

static int qdio_inbound_q_done(struct qdio_q *q)
{
	unsigned char state = 0;

	if (!atomic_read(&q->nr_buf_used))
		return 1;

	/*
	 * We need that one for synchronization with the adapter, as it
	 * does a kind of PCI avoidance.
	 */
	qdio_siga_sync_q(q);

	get_buf_state(q, q->first_to_check, &state, 0);
	if (state == SLSB_P_INPUT_PRIMED)
		/* we got something to do */
		return 0;

	/* on VM, we don't poll, so the q is always done here */
	if (need_siga_sync(q) || pci_out_supported(q))
		return 1;

	/*
	 * At this point we know, that inbound first_to_check
	 * has (probably) not moved (see qdio_inbound_processing).
	 */
	if (get_usecs() > q->u.in.timestamp + QDIO_INPUT_THRESHOLD) {
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "in done:%3d",
			      q->first_to_check);
		return 1;
	} else {
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "in notd:%3d",
			      q->first_to_check);
		return 0;
	}
}

void qdio_kick_inbound_handler(struct qdio_q *q)
{
	int count, start, end;

	qdio_perf_stat_inc(&perf_stats.inbound_handler);

	start = q->first_to_kick;
	end = q->first_to_check;
	if (end >= start)
		count = end - start;
	else
		count = end + QDIO_MAX_BUFFERS_PER_Q - start;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "kih s:%3d c:%3d", start, count);

	if (unlikely(q->irq_ptr->state != QDIO_IRQ_STATE_ACTIVE))
		return;

	q->handler(q->irq_ptr->cdev, q->qdio_error, q->nr,
		   start, count, q->irq_ptr->int_parm);

	/* for the next time */
	q->first_to_kick = q->first_to_check;
	q->qdio_error = 0;
}

static void __qdio_inbound_processing(struct qdio_q *q)
{
	qdio_perf_stat_inc(&perf_stats.tasklet_inbound);
again:
	if (!qdio_inbound_q_moved(q))
		return;

	qdio_kick_inbound_handler(q);

	if (!qdio_inbound_q_done(q))
		/* means poll time is not yet over */
		goto again;

	qdio_stop_polling(q);
	/*
	 * We need to check again to not lose initiative after
	 * resetting the ACK state.
	 */
	if (!qdio_inbound_q_done(q))
		goto again;
}

/* inbound tasklet */
void qdio_inbound_processing(unsigned long data)
{
	struct qdio_q *q = (struct qdio_q *)data;
	__qdio_inbound_processing(q);
}

static int get_outbound_buffer_frontier(struct qdio_q *q)
{
	int count, stop;
	unsigned char state;

	if (((queue_type(q) != QDIO_IQDIO_QFMT) && !pci_out_supported(q)) ||
	    (queue_type(q) == QDIO_IQDIO_QFMT && multicast_outbound(q)))
		qdio_siga_sync_q(q);

	/*
	 * Don't check 128 buffers, as otherwise qdio_inbound_q_moved
	 * would return 0.
	 */
	count = min(atomic_read(&q->nr_buf_used), QDIO_MAX_BUFFERS_MASK);
	stop = add_buf(q->first_to_check, count);

	/* need to set count to 1 for non-qebsm */
	if (!is_qebsm(q))
		count = 1;

check_next:
	if (q->first_to_check == stop)
		return q->first_to_check;

	count = get_buf_states(q, q->first_to_check, &state, count, 0);
	if (!count)
		return q->first_to_check;

	switch (state) {
	case SLSB_P_OUTPUT_EMPTY:
		/* the adapter got it */
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "out empty:%1d %3d", q->nr, count);

		atomic_sub(count, &q->nr_buf_used);
		q->first_to_check = add_buf(q->first_to_check, count);
		/*
		 * We fetch all buffer states at once. get_buf_states may
		 * return count < stop. For QEBSM we do not loop.
		 */
		if (is_qebsm(q))
			break;
		goto check_next;
	case SLSB_P_OUTPUT_ERROR:
		announce_buffer_error(q, count);
		/* process the buffer, the upper layer will take care of it */
		q->first_to_check = add_buf(q->first_to_check, count);
		atomic_sub(count, &q->nr_buf_used);
		break;
	case SLSB_CU_OUTPUT_PRIMED:
		/* the adapter has not fetched the output yet */
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "out primed:%1d", q->nr);
		break;
	case SLSB_P_OUTPUT_NOT_INIT:
	case SLSB_P_OUTPUT_HALTED:
		break;
	default:
		BUG();
	}
	return q->first_to_check;
}

/* all buffers processed? */
static inline int qdio_outbound_q_done(struct qdio_q *q)
{
	return atomic_read(&q->nr_buf_used) == 0;
}

static inline int qdio_outbound_q_moved(struct qdio_q *q)
{
	int bufnr;

	bufnr = get_outbound_buffer_frontier(q);

	if ((bufnr != q->last_move_ftc) || q->qdio_error) {
		q->last_move_ftc = bufnr;
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "out moved:%1d", q->nr);
		return 1;
	} else
		return 0;
}

static void qdio_kick_outbound_q(struct qdio_q *q)
{
	unsigned int busy_bit;
	int cc;

	if (!need_siga_out(q))
		return;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "siga-w:%1d", q->nr);
	qdio_perf_stat_inc(&perf_stats.siga_out);

	cc = qdio_siga_output(q, &busy_bit);
	switch (cc) {
	case 0:
		break;
	case 2:
		if (busy_bit) {
			DBF_ERROR("%4x cc2 REP:%1d", SCH_NO(q), q->nr);
			q->qdio_error = cc | QDIO_ERROR_SIGA_BUSY;
		} else {
			DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "siga-w cc2:%1d",
				      q->nr);
			q->qdio_error = cc;
		}
		break;
	case 1:
	case 3:
		DBF_ERROR("%4x SIGA-W:%1d", SCH_NO(q), cc);
		q->qdio_error = cc;
		break;
	}
}

static void qdio_kick_outbound_handler(struct qdio_q *q)
{
	int start, end, count;

	start = q->first_to_kick;
	end = q->last_move_ftc;
	if (end >= start)
		count = end - start;
	else
		count = end + QDIO_MAX_BUFFERS_PER_Q - start;

	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "kickouth: %1d", q->nr);
	DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "s:%3d c:%3d", start, count);

	if (unlikely(q->irq_ptr->state != QDIO_IRQ_STATE_ACTIVE))
		return;

	q->handler(q->irq_ptr->cdev, q->qdio_error, q->nr, start, count,
		   q->irq_ptr->int_parm);

	/* for the next time: */
	q->first_to_kick = q->last_move_ftc;
	q->qdio_error = 0;
}

static void __qdio_outbound_processing(struct qdio_q *q)
{
	unsigned long flags;

	qdio_perf_stat_inc(&perf_stats.tasklet_outbound);
	spin_lock_irqsave(&q->lock, flags);

	BUG_ON(atomic_read(&q->nr_buf_used) < 0);

	if (qdio_outbound_q_moved(q))
		qdio_kick_outbound_handler(q);

	spin_unlock_irqrestore(&q->lock, flags);

	if (queue_type(q) == QDIO_ZFCP_QFMT) {
		if (!pci_out_supported(q) && !qdio_outbound_q_done(q))
			tasklet_schedule(&q->tasklet);
		return;
	}

	/* bail out for HiperSockets unicast queues */
	if (queue_type(q) == QDIO_IQDIO_QFMT && !multicast_outbound(q))
		return;

	if ((queue_type(q) == QDIO_IQDIO_QFMT) &&
	    (atomic_read(&q->nr_buf_used)) > QDIO_IQDIO_POLL_LVL) {
		tasklet_schedule(&q->tasklet);
		return;
	}

	if (q->u.out.pci_out_enabled)
		return;

	/*
	 * Now we know that queue type is either qeth without pci enabled
	 * or HiperSockets multicast. Make sure buffer switch from PRIMED to
	 * EMPTY is noticed and outbound_handler is called after some time.
	 */
	if (qdio_outbound_q_done(q))
		del_timer(&q->u.out.timer);
	else {
		if (!timer_pending(&q->u.out.timer)) {
			mod_timer(&q->u.out.timer, jiffies + 10 * HZ);
			qdio_perf_stat_inc(&perf_stats.debug_tl_out_timer);
		}
	}
}

/* outbound tasklet */
void qdio_outbound_processing(unsigned long data)
{
	struct qdio_q *q = (struct qdio_q *)data;
	__qdio_outbound_processing(q);
}

void qdio_outbound_timer(unsigned long data)
{
	struct qdio_q *q = (struct qdio_q *)data;
	tasklet_schedule(&q->tasklet);
}

/* called from thinint inbound tasklet */
void qdio_check_outbound_after_thinint(struct qdio_q *q)
{
	struct qdio_q *out;
	int i;

	if (!pci_out_supported(q))
		return;

	for_each_output_queue(q->irq_ptr, out, i)
		if (!qdio_outbound_q_done(out))
			tasklet_schedule(&out->tasklet);
}

static inline void qdio_set_state(struct qdio_irq *irq_ptr,
				  enum qdio_irq_states state)
{
	DBF_DEV_EVENT(DBF_INFO, irq_ptr, "newstate: %1d", state);

	irq_ptr->state = state;
	mb();
}

static void qdio_irq_check_sense(struct qdio_irq *irq_ptr, struct irb *irb)
{
	if (irb->esw.esw0.erw.cons) {
		DBF_ERROR("%4x sense:", irq_ptr->schid.sch_no);
		DBF_ERROR_HEX(irb, 64);
		DBF_ERROR_HEX(irb->ecw, 64);
	}
}

/* PCI interrupt handler */
static void qdio_int_handler_pci(struct qdio_irq *irq_ptr)
{
	int i;
	struct qdio_q *q;

	qdio_perf_stat_inc(&perf_stats.pci_int);

	for_each_input_queue(irq_ptr, q, i)
		tasklet_schedule(&q->tasklet);

	if (!(irq_ptr->qib.ac & QIB_AC_OUTBOUND_PCI_SUPPORTED))
		return;

	for_each_output_queue(irq_ptr, q, i) {
		if (qdio_outbound_q_done(q))
			continue;

		if (!siga_syncs_out_pci(q))
			qdio_siga_sync_q(q);

		tasklet_schedule(&q->tasklet);
	}
}

static void qdio_handle_activate_check(struct ccw_device *cdev,
				unsigned long intparm, int cstat, int dstat)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;
	struct qdio_q *q;

	DBF_ERROR("%4x ACT CHECK", irq_ptr->schid.sch_no);
	DBF_ERROR("intp :%lx", intparm);
	DBF_ERROR("ds: %2x cs:%2x", dstat, cstat);

	if (irq_ptr->nr_input_qs) {
		q = irq_ptr->input_qs[0];
	} else if (irq_ptr->nr_output_qs) {
		q = irq_ptr->output_qs[0];
	} else {
		dump_stack();
		goto no_handler;
	}
	q->handler(q->irq_ptr->cdev, QDIO_ERROR_ACTIVATE_CHECK_CONDITION,
		   0, -1, -1, irq_ptr->int_parm);
no_handler:
	qdio_set_state(irq_ptr, QDIO_IRQ_STATE_STOPPED);
}

static void qdio_call_shutdown(struct work_struct *work)
{
	struct ccw_device_private *priv;
	struct ccw_device *cdev;

	priv = container_of(work, struct ccw_device_private, kick_work);
	cdev = priv->cdev;
	qdio_shutdown(cdev, QDIO_FLAG_CLEANUP_USING_CLEAR);
	put_device(&cdev->dev);
}

static void qdio_int_error(struct ccw_device *cdev)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;

	switch (irq_ptr->state) {
	case QDIO_IRQ_STATE_INACTIVE:
	case QDIO_IRQ_STATE_CLEANUP:
		qdio_set_state(irq_ptr, QDIO_IRQ_STATE_ERR);
		break;
	case QDIO_IRQ_STATE_ESTABLISHED:
	case QDIO_IRQ_STATE_ACTIVE:
		qdio_set_state(irq_ptr, QDIO_IRQ_STATE_STOPPED);
		if (get_device(&cdev->dev)) {
			/* Can't call shutdown from interrupt context. */
			PREPARE_WORK(&cdev->private->kick_work,
				     qdio_call_shutdown);
			queue_work(ccw_device_work, &cdev->private->kick_work);
		}
		break;
	default:
		WARN_ON(1);
	}
	wake_up(&cdev->private->wait_q);
}

static int qdio_establish_check_errors(struct ccw_device *cdev, int cstat,
				       int dstat)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;

	if (cstat || (dstat & ~(DEV_STAT_CHN_END | DEV_STAT_DEV_END))) {
		DBF_ERROR("EQ:ck con");
		goto error;
	}

	if (!(dstat & DEV_STAT_DEV_END)) {
		DBF_ERROR("EQ:no dev");
		goto error;
	}

	if (dstat & ~(DEV_STAT_CHN_END | DEV_STAT_DEV_END)) {
		DBF_ERROR("EQ: bad io");
		goto error;
	}
	return 0;
error:
	DBF_ERROR("%4x EQ:error", irq_ptr->schid.sch_no);
	DBF_ERROR("ds: %2x cs:%2x", dstat, cstat);

	qdio_set_state(irq_ptr, QDIO_IRQ_STATE_ERR);
	return 1;
}

static void qdio_establish_handle_irq(struct ccw_device *cdev, int cstat,
				      int dstat)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;

	DBF_DEV_EVENT(DBF_INFO, irq_ptr, "qest irq");
	if (!qdio_establish_check_errors(cdev, cstat, dstat))
		qdio_set_state(irq_ptr, QDIO_IRQ_STATE_ESTABLISHED);
}

/* qdio interrupt handler */
void qdio_int_handler(struct ccw_device *cdev, unsigned long intparm,
		      struct irb *irb)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;
	int cstat, dstat;

	qdio_perf_stat_inc(&perf_stats.qdio_int);

	if (!intparm || !irq_ptr) {
		DBF_ERROR("qint:%4x", cdev->private->schid.sch_no);
		return;
	}

	if (IS_ERR(irb)) {
		switch (PTR_ERR(irb)) {
		case -EIO:
			DBF_ERROR("%4x IO error", irq_ptr->schid.sch_no);
			return;
		case -ETIMEDOUT:
			DBF_ERROR("%4x IO timeout", irq_ptr->schid.sch_no);
			qdio_int_error(cdev);
			return;
		default:
			WARN_ON(1);
			return;
		}
	}
	qdio_irq_check_sense(irq_ptr, irb);

	cstat = irb->scsw.cmd.cstat;
	dstat = irb->scsw.cmd.dstat;

	switch (irq_ptr->state) {
	case QDIO_IRQ_STATE_INACTIVE:
		qdio_establish_handle_irq(cdev, cstat, dstat);
		break;

	case QDIO_IRQ_STATE_CLEANUP:
		qdio_set_state(irq_ptr, QDIO_IRQ_STATE_INACTIVE);
		break;

	case QDIO_IRQ_STATE_ESTABLISHED:
	case QDIO_IRQ_STATE_ACTIVE:
		if (cstat & SCHN_STAT_PCI) {
			qdio_int_handler_pci(irq_ptr);
			/* no state change so no need to wake up wait_q */
			return;
		}
		if ((cstat & ~SCHN_STAT_PCI) || dstat) {
			qdio_handle_activate_check(cdev, intparm, cstat,
						   dstat);
			break;
		}
	default:
		WARN_ON(1);
	}
	wake_up(&cdev->private->wait_q);
}

/**
 * qdio_get_ssqd_desc - get qdio subchannel description
 * @cdev: ccw device to get description for
 * @data: where to store the ssqd
 *
 * Returns 0 or an error code. The results of the chsc are stored in the
 * specified structure.
 */
int qdio_get_ssqd_desc(struct ccw_device *cdev,
		       struct qdio_ssqd_desc *data)
{

	if (!cdev || !cdev->private)
		return -EINVAL;

	DBF_EVENT("get ssqd:%4x", cdev->private->schid.sch_no);
	return qdio_setup_get_ssqd(NULL, &cdev->private->schid, data);
}
EXPORT_SYMBOL_GPL(qdio_get_ssqd_desc);

/**
 * qdio_cleanup - shutdown queues and free data structures
 * @cdev: associated ccw device
 * @how: use halt or clear to shutdown
 *
 * This function calls qdio_shutdown() for @cdev with method @how
 * and on success qdio_free() for @cdev.
 */
int qdio_cleanup(struct ccw_device *cdev, int how)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;
	int rc;

	if (!irq_ptr)
		return -ENODEV;

	rc = qdio_shutdown(cdev, how);
	if (rc == 0)
		rc = qdio_free(cdev);
	return rc;
}
EXPORT_SYMBOL_GPL(qdio_cleanup);

static void qdio_shutdown_queues(struct ccw_device *cdev)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;
	struct qdio_q *q;
	int i;

	for_each_input_queue(irq_ptr, q, i)
		tasklet_disable(&q->tasklet);

	for_each_output_queue(irq_ptr, q, i) {
		tasklet_disable(&q->tasklet);
		del_timer(&q->u.out.timer);
	}
}

/**
 * qdio_shutdown - shut down a qdio subchannel
 * @cdev: associated ccw device
 * @how: use halt or clear to shutdown
 */
int qdio_shutdown(struct ccw_device *cdev, int how)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;
	int rc;
	unsigned long flags;

	if (!irq_ptr)
		return -ENODEV;

	DBF_EVENT("qshutdown:%4x", cdev->private->schid.sch_no);

	mutex_lock(&irq_ptr->setup_mutex);
	/*
	 * Subchannel was already shot down. We cannot prevent being called
	 * twice since cio may trigger a shutdown asynchronously.
	 */
	if (irq_ptr->state == QDIO_IRQ_STATE_INACTIVE) {
		mutex_unlock(&irq_ptr->setup_mutex);
		return 0;
	}

	tiqdio_remove_input_queues(irq_ptr);
	qdio_shutdown_queues(cdev);
	qdio_shutdown_debug_entries(irq_ptr, cdev);

	/* cleanup subchannel */
	spin_lock_irqsave(get_ccwdev_lock(cdev), flags);

	if (how & QDIO_FLAG_CLEANUP_USING_CLEAR)
		rc = ccw_device_clear(cdev, QDIO_DOING_CLEANUP);
	else
		/* default behaviour is halt */
		rc = ccw_device_halt(cdev, QDIO_DOING_CLEANUP);
	if (rc) {
		DBF_ERROR("%4x SHUTD ERR", irq_ptr->schid.sch_no);
		DBF_ERROR("rc:%4d", rc);
		goto no_cleanup;
	}

	qdio_set_state(irq_ptr, QDIO_IRQ_STATE_CLEANUP);
	spin_unlock_irqrestore(get_ccwdev_lock(cdev), flags);
	wait_event_interruptible_timeout(cdev->private->wait_q,
		irq_ptr->state == QDIO_IRQ_STATE_INACTIVE ||
		irq_ptr->state == QDIO_IRQ_STATE_ERR,
		10 * HZ);
	spin_lock_irqsave(get_ccwdev_lock(cdev), flags);

no_cleanup:
	qdio_shutdown_thinint(irq_ptr);

	/* restore interrupt handler */
	if ((void *)cdev->handler == (void *)qdio_int_handler)
		cdev->handler = irq_ptr->orig_handler;
	spin_unlock_irqrestore(get_ccwdev_lock(cdev), flags);

	qdio_set_state(irq_ptr, QDIO_IRQ_STATE_INACTIVE);
	mutex_unlock(&irq_ptr->setup_mutex);
	if (rc)
		return rc;
	return 0;
}
EXPORT_SYMBOL_GPL(qdio_shutdown);

/**
 * qdio_free - free data structures for a qdio subchannel
 * @cdev: associated ccw device
 */
int qdio_free(struct ccw_device *cdev)
{
	struct qdio_irq *irq_ptr = cdev->private->qdio_data;

	if (!irq_ptr)
		return -ENODEV;

	DBF_EVENT("qfree:%4x", cdev->private->schid.sch_no);
	mutex_lock(&irq_ptr->setup_mutex);

	if (irq_ptr->debug_area != NULL) {
		debug_unregister(irq_ptr->debug_area);
		irq_ptr->debug_area = NULL;
	}
	cdev->private->qdio_data = NULL;
	mutex_unlock(&irq_ptr->setup_mutex);

	qdio_release_memory(irq_ptr);
	return 0;
}
EXPORT_SYMBOL_GPL(qdio_free);

/**
 * qdio_initialize - allocate and establish queues for a qdio subchannel
 * @init_data: initialization data
 *
 * This function first allocates queues via qdio_allocate() and on success
 * establishes them via qdio_establish().
 */
int qdio_initialize(struct qdio_initialize *init_data)
{
	int rc;

	rc = qdio_allocate(init_data);
	if (rc)
		return rc;

	rc = qdio_establish(init_data);
	if (rc)
		qdio_free(init_data->cdev);
	return rc;
}
EXPORT_SYMBOL_GPL(qdio_initialize);

/**
 * qdio_allocate - allocate qdio queues and associated data
 * @init_data: initialization data
 */
int qdio_allocate(struct qdio_initialize *init_data)
{
	struct qdio_irq *irq_ptr;

	DBF_EVENT("qallocate:%4x", init_data->cdev->private->schid.sch_no);

	if ((init_data->no_input_qs && !init_data->input_handler) ||
	    (init_data->no_output_qs && !init_data->output_handler))
		return -EINVAL;

	if ((init_data->no_input_qs > QDIO_MAX_QUEUES_PER_IRQ) ||
	    (init_data->no_output_qs > QDIO_MAX_QUEUES_PER_IRQ))
		return -EINVAL;

	if ((!init_data->input_sbal_addr_array) ||
	    (!init_data->output_sbal_addr_array))
		return -EINVAL;

	/* irq_ptr must be in GFP_DMA since it contains ccw1.cda */
	irq_ptr = (void *) get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!irq_ptr)
		goto out_err;

	mutex_init(&irq_ptr->setup_mutex);
	qdio_allocate_dbf(init_data, irq_ptr);

	/*
	 * Allocate a page for the chsc calls in qdio_establish.
	 * Must be pre-allocated since a zfcp recovery will call
	 * qdio_establish. In case of low memory and swap on a zfcp disk
	 * we may not be able to allocate memory otherwise.
	 */
	irq_ptr->chsc_page = get_zeroed_page(GFP_KERNEL);
	if (!irq_ptr->chsc_page)
		goto out_rel;

	/* qdr is used in ccw1.cda which is u32 */
	irq_ptr->qdr = (struct qdr *) get_zeroed_page(GFP_KERNEL | GFP_DMA);
	if (!irq_ptr->qdr)
		goto out_rel;
	WARN_ON((unsigned long)irq_ptr->qdr & 0xfff);

	if (qdio_allocate_qs(irq_ptr, init_data->no_input_qs,
			     init_data->no_output_qs))
		goto out_rel;

	init_data->cdev->private->qdio_data = irq_ptr;
	qdio_set_state(irq_ptr, QDIO_IRQ_STATE_INACTIVE);
	return 0;
out_rel:
	qdio_release_memory(irq_ptr);
out_err:
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(qdio_allocate);

/**
 * qdio_establish - establish queues on a qdio subchannel
 * @init_data: initialization data
 */
int qdio_establish(struct qdio_initialize *init_data)
{
	struct qdio_irq *irq_ptr;
	struct ccw_device *cdev = init_data->cdev;
	unsigned long saveflags;
	int rc;

	DBF_EVENT("qestablish:%4x", cdev->private->schid.sch_no);

	irq_ptr = cdev->private->qdio_data;
	if (!irq_ptr)
		return -ENODEV;

	if (cdev->private->state != DEV_STATE_ONLINE)
		return -EINVAL;

	mutex_lock(&irq_ptr->setup_mutex);
	qdio_setup_irq(init_data);

	rc = qdio_establish_thinint(irq_ptr);
	if (rc) {
		mutex_unlock(&irq_ptr->setup_mutex);
		qdio_shutdown(cdev, QDIO_FLAG_CLEANUP_USING_CLEAR);
		return rc;
	}

	/* establish q */
	irq_ptr->ccw.cmd_code = irq_ptr->equeue.cmd;
	irq_ptr->ccw.flags = CCW_FLAG_SLI;
	irq_ptr->ccw.count = irq_ptr->equeue.count;
	irq_ptr->ccw.cda = (u32)((addr_t)irq_ptr->qdr);

	spin_lock_irqsave(get_ccwdev_lock(cdev), saveflags);
	ccw_device_set_options_mask(cdev, 0);

	rc = ccw_device_start(cdev, &irq_ptr->ccw, QDIO_DOING_ESTABLISH, 0, 0);
	if (rc) {
		DBF_ERROR("%4x est IO ERR", irq_ptr->schid.sch_no);
		DBF_ERROR("rc:%4x", rc);
	}
	spin_unlock_irqrestore(get_ccwdev_lock(cdev), saveflags);

	if (rc) {
		mutex_unlock(&irq_ptr->setup_mutex);
		qdio_shutdown(cdev, QDIO_FLAG_CLEANUP_USING_CLEAR);
		return rc;
	}

	wait_event_interruptible_timeout(cdev->private->wait_q,
		irq_ptr->state == QDIO_IRQ_STATE_ESTABLISHED ||
		irq_ptr->state == QDIO_IRQ_STATE_ERR, HZ);

	if (irq_ptr->state != QDIO_IRQ_STATE_ESTABLISHED) {
		mutex_unlock(&irq_ptr->setup_mutex);
		qdio_shutdown(cdev, QDIO_FLAG_CLEANUP_USING_CLEAR);
		return -EIO;
	}

	qdio_setup_ssqd_info(irq_ptr);
	DBF_EVENT("qDmmwc:%2x", irq_ptr->ssqd_desc.mmwc);
	DBF_EVENT("qib ac:%4x", irq_ptr->qib.ac);

	/* qebsm is now setup if available, initialize buffer states */
	qdio_init_buf_states(irq_ptr);

	mutex_unlock(&irq_ptr->setup_mutex);
	qdio_print_subchannel_info(irq_ptr, cdev);
	qdio_setup_debug_entries(irq_ptr, cdev);
	return 0;
}
EXPORT_SYMBOL_GPL(qdio_establish);

/**
 * qdio_activate - activate queues on a qdio subchannel
 * @cdev: associated cdev
 */
int qdio_activate(struct ccw_device *cdev)
{
	struct qdio_irq *irq_ptr;
	int rc;
	unsigned long saveflags;

	DBF_EVENT("qactivate:%4x", cdev->private->schid.sch_no);

	irq_ptr = cdev->private->qdio_data;
	if (!irq_ptr)
		return -ENODEV;

	if (cdev->private->state != DEV_STATE_ONLINE)
		return -EINVAL;

	mutex_lock(&irq_ptr->setup_mutex);
	if (irq_ptr->state == QDIO_IRQ_STATE_INACTIVE) {
		rc = -EBUSY;
		goto out;
	}

	irq_ptr->ccw.cmd_code = irq_ptr->aqueue.cmd;
	irq_ptr->ccw.flags = CCW_FLAG_SLI;
	irq_ptr->ccw.count = irq_ptr->aqueue.count;
	irq_ptr->ccw.cda = 0;

	spin_lock_irqsave(get_ccwdev_lock(cdev), saveflags);
	ccw_device_set_options(cdev, CCWDEV_REPORT_ALL);

	rc = ccw_device_start(cdev, &irq_ptr->ccw, QDIO_DOING_ACTIVATE,
			      0, DOIO_DENY_PREFETCH);
	if (rc) {
		DBF_ERROR("%4x act IO ERR", irq_ptr->schid.sch_no);
		DBF_ERROR("rc:%4x", rc);
	}
	spin_unlock_irqrestore(get_ccwdev_lock(cdev), saveflags);

	if (rc)
		goto out;

	if (is_thinint_irq(irq_ptr))
		tiqdio_add_input_queues(irq_ptr);

	/* wait for subchannel to become active */
	msleep(5);

	switch (irq_ptr->state) {
	case QDIO_IRQ_STATE_STOPPED:
	case QDIO_IRQ_STATE_ERR:
		mutex_unlock(&irq_ptr->setup_mutex);
		qdio_shutdown(cdev, QDIO_FLAG_CLEANUP_USING_CLEAR);
		return -EIO;
	default:
		qdio_set_state(irq_ptr, QDIO_IRQ_STATE_ACTIVE);
		rc = 0;
	}
out:
	mutex_unlock(&irq_ptr->setup_mutex);
	return rc;
}
EXPORT_SYMBOL_GPL(qdio_activate);

static inline int buf_in_between(int bufnr, int start, int count)
{
	int end = add_buf(start, count);

	if (end > start) {
		if (bufnr >= start && bufnr < end)
			return 1;
		else
			return 0;
	}

	/* wrap-around case */
	if ((bufnr >= start && bufnr <= QDIO_MAX_BUFFERS_PER_Q) ||
	    (bufnr < end))
		return 1;
	else
		return 0;
}

/**
 * handle_inbound - reset processed input buffers
 * @q: queue containing the buffers
 * @callflags: flags
 * @bufnr: first buffer to process
 * @count: how many buffers are emptied
 */
static void handle_inbound(struct qdio_q *q, unsigned int callflags,
			   int bufnr, int count)
{
	int used, cc, diff;

	if (!q->u.in.polling)
		goto set;

	/* protect against stop polling setting an ACK for an emptied slsb */
	if (count == QDIO_MAX_BUFFERS_PER_Q) {
		/* overwriting everything, just delete polling status */
		q->u.in.polling = 0;
		q->u.in.ack_count = 0;
		goto set;
	} else if (buf_in_between(q->last_move_ftc, bufnr, count)) {
		if (is_qebsm(q)) {
			/* partial overwrite, just update last_move_ftc */
			diff = add_buf(bufnr, count);
			diff = sub_buf(diff, q->last_move_ftc);
			q->u.in.ack_count -= diff;
			if (q->u.in.ack_count <= 0) {
				q->u.in.polling = 0;
				q->u.in.ack_count = 0;
				/* TODO: must we set last_move_ftc to something meaningful? */
				goto set;
			}
			q->last_move_ftc = add_buf(q->last_move_ftc, diff);
		}
		else
			/* the only ACK will be deleted, so stop polling */
			q->u.in.polling = 0;
	}

set:
	count = set_buf_states(q, bufnr, SLSB_CU_INPUT_EMPTY, count);

	used = atomic_add_return(count, &q->nr_buf_used) - count;
	BUG_ON(used + count > QDIO_MAX_BUFFERS_PER_Q);

	/* no need to signal as long as the adapter had free buffers */
	if (used)
		return;

	if (need_siga_in(q)) {
		cc = qdio_siga_input(q);
		if (cc)
			q->qdio_error = cc;
	}
}

/**
 * handle_outbound - process filled outbound buffers
 * @q: queue containing the buffers
 * @callflags: flags
 * @bufnr: first buffer to process
 * @count: how many buffers are filled
 */
static void handle_outbound(struct qdio_q *q, unsigned int callflags,
			    int bufnr, int count)
{
	unsigned char state;
	int used;

	qdio_perf_stat_inc(&perf_stats.outbound_handler);

	count = set_buf_states(q, bufnr, SLSB_CU_OUTPUT_PRIMED, count);
	used = atomic_add_return(count, &q->nr_buf_used);
	BUG_ON(used > QDIO_MAX_BUFFERS_PER_Q);

	if (callflags & QDIO_FLAG_PCI_OUT)
		q->u.out.pci_out_enabled = 1;
	else
		q->u.out.pci_out_enabled = 0;

	if (queue_type(q) == QDIO_IQDIO_QFMT) {
		if (multicast_outbound(q))
			qdio_kick_outbound_q(q);
		else
			if ((q->irq_ptr->ssqd_desc.mmwc > 1) &&
			    (count > 1) &&
			    (count <= q->irq_ptr->ssqd_desc.mmwc)) {
				/* exploit enhanced SIGA */
				q->u.out.use_enh_siga = 1;
				qdio_kick_outbound_q(q);
			} else {
				/*
				* One siga-w per buffer required for unicast
				* HiperSockets.
				*/
				q->u.out.use_enh_siga = 0;
				while (count--)
					qdio_kick_outbound_q(q);
			}

		/* report CC=2 conditions synchronously */
		if (q->qdio_error)
			__qdio_outbound_processing(q);
		goto out;
	}

	if (need_siga_sync(q)) {
		qdio_siga_sync_q(q);
		goto out;
	}

	/* try to fast requeue buffers */
	get_buf_state(q, prev_buf(bufnr), &state, 0);
	if (state != SLSB_CU_OUTPUT_PRIMED)
		qdio_kick_outbound_q(q);
	else {
		DBF_DEV_EVENT(DBF_INFO, q->irq_ptr, "fast-req");
		qdio_perf_stat_inc(&perf_stats.fast_requeue);
	}
out:
	/* Fixme: could wait forever if called from process context */
	tasklet_schedule(&q->tasklet);
}

/**
 * do_QDIO - process input or output buffers
 * @cdev: associated ccw_device for the qdio subchannel
 * @callflags: input or output and special flags from the program
 * @q_nr: queue number
 * @bufnr: buffer number
 * @count: how many buffers to process
 */
int do_QDIO(struct ccw_device *cdev, unsigned int callflags,
	    int q_nr, int bufnr, int count)
{
	struct qdio_irq *irq_ptr;

	if ((bufnr > QDIO_MAX_BUFFERS_PER_Q) ||
	    (count > QDIO_MAX_BUFFERS_PER_Q) ||
	    (q_nr > QDIO_MAX_QUEUES_PER_IRQ))
		return -EINVAL;

	if (!count)
		return 0;

	irq_ptr = cdev->private->qdio_data;
	if (!irq_ptr)
		return -ENODEV;

	if (callflags & QDIO_FLAG_SYNC_INPUT)
		DBF_DEV_EVENT(DBF_INFO, irq_ptr, "doQDIO input");
	else
		DBF_DEV_EVENT(DBF_INFO, irq_ptr, "doQDIO output");
	DBF_DEV_EVENT(DBF_INFO, irq_ptr, "q:%1d flag:%4x", q_nr, callflags);
	DBF_DEV_EVENT(DBF_INFO, irq_ptr, "buf:%2d cnt:%3d", bufnr, count);

	if (irq_ptr->state != QDIO_IRQ_STATE_ACTIVE)
		return -EBUSY;

	if (callflags & QDIO_FLAG_SYNC_INPUT)
		handle_inbound(irq_ptr->input_qs[q_nr], callflags, bufnr,
			       count);
	else if (callflags & QDIO_FLAG_SYNC_OUTPUT)
		handle_outbound(irq_ptr->output_qs[q_nr], callflags, bufnr,
				count);
	else
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL_GPL(do_QDIO);

static int __init init_QDIO(void)
{
	int rc;

	rc = qdio_setup_init();
	if (rc)
		return rc;
	rc = tiqdio_allocate_memory();
	if (rc)
		goto out_cache;
	rc = qdio_debug_init();
	if (rc)
		goto out_ti;
	rc = qdio_setup_perf_stats();
	if (rc)
		goto out_debug;
	rc = tiqdio_register_thinints();
	if (rc)
		goto out_perf;
	return 0;

out_perf:
	qdio_remove_perf_stats();
out_debug:
	qdio_debug_exit();
out_ti:
	tiqdio_free_memory();
out_cache:
	qdio_setup_exit();
	return rc;
}

static void __exit exit_QDIO(void)
{
	tiqdio_unregister_thinints();
	tiqdio_free_memory();
	qdio_remove_perf_stats();
	qdio_debug_exit();
	qdio_setup_exit();
}

module_init(init_QDIO);
module_exit(exit_QDIO);
