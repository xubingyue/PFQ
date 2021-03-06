/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#ifndef _PF_Q_LINUX_H_
#define _PF_Q_LINUX_H_

#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/filter.h>
#include <linux/skbuff.h>

#define Q_VERSION               "4.0"

#else  /* user space */

#define __user

#include <linux/filter.h>
#include <linux/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>

static inline void barrier() { asm volatile ("" ::: "memory"); }

#if defined(__LP64__) /* 64 bit */

static inline void mb()  { asm volatile ("mfence" ::: "memory"); }
static inline void rmb() { asm volatile ("lfence" ::: "memory"); }
static inline void wmb() { asm volatile ("sfence" ::: "memory"); }

#else /* 32-bit */

static inline void mb()  { asm volatile ("lock; addl $0,0(%%esp)" ::: "memory"); }
static inline void rmb() { asm volatile ("lock; addl $0,0(%%esp)" ::: "memory"); }
static inline void wmb() { asm volatile ("lock; addl $0,0(%%esp)" ::: "memory"); }

#endif

static inline void smp_mb()  { mb();      }
static inline void smp_rmb() { barrier(); }
static inline void smp_wmb() { barrier(); }


#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)

#endif /* __KERNEL__ */

#define Q_MAX_COUNTERS           	64
#define Q_MAX_PERSISTENT 		1024
#define Q_MAX_TX_QUEUES 		4

/* Common header */

#define PF_Q    27          /* packet q domain */

struct pfq_pkthdr
{
        uint64_t data;          /* state from pfq_cb */

        union
        {
                unsigned long long tv64;
                struct {
                        uint32_t    sec;
                        uint32_t    nsec;
                } tv;               /* note: compact timespec for 64 bits arch. */
        } tstamp;

        int         if_index;   /* interface index */
        int         gid;        /* group id */

        uint16_t    len;        /* length of the packet (off wire) */
        uint16_t    caplen;     /* bytes captured */

        union
        {
                struct
                {
                        uint16_t vlan_vid:12,   /* 8021q vlan id */
                                 reserved:1,    /* 8021q reserved bit */
                                 vlan_prio:3;   /* 8021q vlan priority */
                } vlan;

                uint16_t     vlan_tci;
        } un;

        uint8_t     hw_queue;   /* 256 queues per device */
        uint8_t     commit;

}; /* __attribute__((packed)); */



struct pfq_pkthdr_tx
{
	uint16_t len;
	uint16_t res;
};


struct pfq_tx_queue_hdr
{
        struct
        {
                volatile unsigned int index;
                unsigned int cache;
        } producer __attribute__((aligned(64)));

        struct
        {
                volatile unsigned int index;
                unsigned int cache;
        } consumer __attribute__((aligned(64)));

        unsigned int size_mask;        /* number of slots */
        unsigned int max_len;          /* max length of packet */
        unsigned int size;             /* number of slots (power of two) */
        unsigned int slot_size;        /* sizeof(pfq_pkthdr) + max_len + sizeof(skb_shinfo) */

} __attribute__((aligned(64)));


struct pfq_rx_queue_hdr
{
        volatile unsigned int   data;
        unsigned int            size;       /* number of slots */
        unsigned int            slot_size;  /* sizeof(pfq_pkthdr) + max_len + sizeof(skb_shinfo) */

} __attribute__((aligned(64)));


#define MPDB_QUEUE_INDEX(data)     (((data) & 0xff000000U) >> 24)
#define MPDB_QUEUE_LEN(data)       ((data) & 0x00ffffffU)


/* Queues Headers */

struct pfq_queue_hdr
{
        struct pfq_rx_queue_hdr rx;
        struct pfq_tx_queue_hdr tx[Q_MAX_TX_QUEUES];
};


/* slots size... */

#define MPDB_QUEUE_SLOT_SIZE(x)    ALIGN(sizeof(struct pfq_pkthdr) + x, 64)
#define SPSC_QUEUE_SLOT_SIZE(x)    ALIGN(sizeof(struct pfq_pkthdr_tx) + x, 64)


static inline
unsigned int pfq_spsc_next_index(struct pfq_tx_queue_hdr *q, unsigned int n)
{
        return (n + 1) & q->size_mask;
}


/* producer */

static inline
int pfq_spsc_write_avail(struct pfq_tx_queue_hdr *q)
{
        if (unlikely(q->producer.cache == 0)) {
                /* consumer - producer - 1 + size */
                q->producer.cache = (q->consumer.index - q->producer.index + q->size_mask) & q->size_mask;
        }

        return (int)q->producer.cache;
}


static inline
int pfq_spsc_write_index(struct pfq_tx_queue_hdr *q)
{
        if (pfq_spsc_write_avail(q) == 0)
                return -1;

        return (int)q->producer.index;
}


static inline
void pfq_spsc_write_commit_n(struct pfq_tx_queue_hdr *q, unsigned int n)
{
        smp_wmb();

        if (unlikely(n > q->producer.cache))
                n = q->producer.cache;

        q->producer.index = (q->producer.index + n) & q->size_mask;
        q->producer.cache -= n;

        smp_wmb();
}


static inline
void pfq_spsc_write_commit(struct pfq_tx_queue_hdr *q)
{
        pfq_spsc_write_commit_n(q,1);
}

/* consumer */

static inline
int pfq_spsc_read_avail(struct pfq_tx_queue_hdr *q)
{
        if(unlikely(q->consumer.cache == 0))
                q->consumer.cache = (q->producer.index - q->consumer.index + q->size) & q->size_mask;

        return (int)q->consumer.cache;
}


static inline
int pfq_spsc_read_index(struct pfq_tx_queue_hdr *q)
{
        if (pfq_spsc_read_avail(q) == 0)
                return -1;

        return (int)q->consumer.index;
}


static inline
void pfq_spsc_read_commit_n(struct pfq_tx_queue_hdr *q, unsigned int n)
{
        smp_wmb();

        if (unlikely(n > q->consumer.cache))
                n = q->consumer.cache;

        q->consumer.index  = (q->consumer.index + n) & q->size_mask;
        q->consumer.cache -= n;

        smp_wmb();
}


static inline
void pfq_spsc_read_commit(struct pfq_tx_queue_hdr *q)
{
        pfq_spsc_read_commit_n(q,1);
}


/*
   +------------------+---------------------+          +---------------------+          +---------------------+
   | pfq_queue_hdr    | pfq_pkthdr | packet | ...      | pfq_pkthdr | packet |...       | pfq_pkthdr | packet | ...
   +------------------+---------------------+          +---------------------+          +---------------------+
   +                             +                             +                            +
   | <------+ queue rx  +------> |  <----+ queue rx +------>   |  <----+ queue tx +------>  |  <----+ queue tx +------>
   +                             +                             +                            +
   */


/* PFQ socket options */

#define Q_SO_DISABLE 			0
#define Q_SO_ENABLE 			1

#define Q_SO_SET_RX_TSTAMP          	2
#define Q_SO_SET_RX_CAPLEN          	3
#define Q_SO_SET_RX_SLOTS           	4
#define Q_SO_SET_RX_OFFSET          	5
#define Q_SO_SET_TX_MAXLEN          	6
#define Q_SO_SET_TX_SLOTS           	7

#define Q_SO_GROUP_BIND		    	8
#define Q_SO_GROUP_UNBIND 	    	9
#define Q_SO_GROUP_JOIN             	10
#define Q_SO_GROUP_LEAVE            	11
#define Q_SO_GROUP_FPROG            	12      /* Berkeley packet filter */
#define Q_SO_GROUP_VLAN_FILT_TOGGLE 	13      /* enable/disable VLAN filters */
#define Q_SO_GROUP_VLAN_FILT        	14      /* enable/disable VLAN ID filters */
#define Q_SO_GROUP_FUNCTION     	15

#define Q_SO_EGRESS_BIND         	16
#define Q_SO_EGRESS_UNBIND         	17

#define Q_SO_GET_ID                 	20
#define Q_SO_GET_STATUS             	21      /* 1 = enabled, 0 = disabled */
#define Q_SO_GET_STATS              	22
#define Q_SO_GET_SHMEM_SIZE          	23      /* size of the shared memory in (bytes) */
#define Q_SO_GET_RX_TSTAMP          	24
#define Q_SO_GET_RX_CAPLEN          	25
#define Q_SO_GET_RX_SLOTS           	26
#define Q_SO_GET_RX_OFFSET          	27
#define Q_SO_GET_TX_MAXLEN          	28
#define Q_SO_GET_TX_SLOTS           	29
#define Q_SO_GET_GROUPS             	30
#define Q_SO_GET_GROUP_STATS        	31
#define Q_SO_GET_GROUP_COUNTERS     	32

#define Q_SO_TX_BIND         		33
#define Q_SO_TX_UNBIND 			34
#define Q_SO_TX_FLUSH			35


/* general placeholders */

#define Q_ANY_DEVICE         -1
#define Q_ANY_QUEUE          -1
#define Q_ANY_GROUP          -1

#define Q_NO_KTHREAD         -1
#define Q_ANY_CPU	     65535


/* timestamp */

#define Q_TSTAMP_OFF          0       /* default */
#define Q_TSTAMP_ON           1

/* vlan */

#define Q_VLAN_PRIO_MASK     0xe000
#define Q_VLAN_VID_MASK      0x0fff
#define Q_VLAN_TAG_PRESENT   0x1000

#define Q_VLAN_UNTAG         0
#define Q_VLAN_ANYTAG       -1

/* group policies */

#define Q_POLICY_GROUP_UNDEFINED       0
#define Q_POLICY_GROUP_PRIVATE         1
#define Q_POLICY_GROUP_RESTRICTED      2
#define Q_POLICY_GROUP_SHARED          3

/* group class type */

#define Q_CLASS(n)              (1UL << (n))
#define Q_CLASS_MAX             (sizeof(unsigned long)<<3)

#define Q_CLASS_DEFAULT         Q_CLASS(0)
#define Q_CLASS_USER_PLANE 	Q_CLASS(1)
#define Q_CLASS_CONTROL_PLANE   Q_CLASS(2)
#define Q_CLASS_CONTROL		Q_CLASS(Q_CLASS_MAX-1) 			/* reserved for management */
#define Q_CLASS_ANY             (((unsigned long)-1) ^ Q_CLASS_CONTROL) /* any class except management */


/*
 * Functional argument:
 *
 * pod	  	-> (ptr/value, sizeof,  -1 )
 * pod array    -> (ptr,       sizeof,  len)
 * string 	-> (ptr,       0     ,  -1 )
 * expression 	-> (0,         index ,  -1 )
 *
 */

struct pfq_functional_arg_descr
{
	const void __user *	addr;
	size_t 			size;
	size_t 			nelem;   /* > 1 is an array */
};


/*
 * Functional descriptor:
 */

struct pfq_functional_descr
{
        const char __user *     	symbol;
	struct pfq_functional_arg_descr arg[8];
        size_t 				next;
};


struct pfq_computation_descr
{
        size_t                          size;
        size_t                          entry_point;
        struct pfq_functional_descr     fun[];
};


/*  sock options helper structures */


struct pfq_vlan_toggle
{
        int gid;
        int vid;
        int toggle;
};

struct pfq_binding
{
        union {
        	int gid;
        	int cpu;
	};

        int if_index;
        int hw_queue;
};

struct pfq_group_join
{
        int gid;
        int policy;
        unsigned long class_mask;
};

struct pfq_group_computation
{
        int gid;
        struct pfq_computation_descr __user *prog;
};


struct pfq_group_context
{
        void __user *context;
        size_t       size;      /* sizeof(context) */
        int gid;
        int level;
};

/* pfq_fprog: per-group sock_fprog */

struct pfq_fprog
{
        int gid;
        struct sock_fprog fcode;
};


/* pfq statistics for socket and groups */

struct pfq_stats
{
        unsigned long int recv;   	/* received by the group */
        unsigned long int lost;   	/* queue is full, packet lost... */
        unsigned long int drop;   	/* by filter                     */

        unsigned long int sent;   	/* sent by the driver */
        unsigned long int disc;   	/* discarded by the driver */

	unsigned long int frwd;      	/* forwarded to devices */
	unsigned long int kern;		/* forwarded to kernel  */
};


/* pfq counters for groups */

struct pfq_counters
{
        unsigned long int counter[Q_MAX_COUNTERS];
};

#endif /* _PF_Q_LINUX_H_ */
