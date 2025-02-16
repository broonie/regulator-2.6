#ifndef _S390_BYTEORDER_H
#define _S390_BYTEORDER_H

/*
 *  include/asm-s390/byteorder.h
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com)
 */

#include <asm/types.h>

#define __BIG_ENDIAN

#ifndef __s390x__
# define __SWAB_64_THRU_32__
#endif

#ifdef __s390x__
static inline __u64 __arch_swab64p(const __u64 *x)
{
	__u64 result;

	asm volatile("lrvg %0,%1" : "=d" (result) : "m" (*x));
	return result;
}
#define __arch_swab64p __arch_swab64p

static inline __u64 __arch_swab64(__u64 x)
{
	__u64 result;

	asm volatile("lrvgr %0,%1" : "=d" (result) : "d" (x));
	return result;
}
#define __arch_swab64 __arch_swab64

static inline void __arch_swab64s(__u64 *x)
{
	*x = __arch_swab64p(x);
}
#define __arch_swab64s __arch_swab64s
#endif /* __s390x__ */

static inline __u32 __arch_swab32p(const __u32 *x)
{
	__u32 result;
	
	asm volatile(
#ifndef __s390x__
		"	icm	%0,8,3(%1)\n"
		"	icm	%0,4,2(%1)\n"
		"	icm	%0,2,1(%1)\n"
		"	ic	%0,0(%1)"
		: "=&d" (result) : "a" (x), "m" (*x) : "cc");
#else /* __s390x__ */
		"	lrv	%0,%1"
		: "=d" (result) : "m" (*x));
#endif /* __s390x__ */
	return result;
}
#define __arch_swab32p __arch_swab32p

#ifdef __s390x__
static inline __u32 __arch_swab32(__u32 x)
{
	__u32 result;
	
	asm volatile("lrvr  %0,%1" : "=d" (result) : "d" (x));
	return result;
}
#define __arch_swab32 __arch_swab32
#endif /* __s390x__ */

static inline __u16 __arch_swab16p(const __u16 *x)
{
	__u16 result;
	
	asm volatile(
#ifndef __s390x__
		"	icm	%0,2,1(%1)\n"
		"	ic	%0,0(%1)\n"
		: "=&d" (result) : "a" (x), "m" (*x) : "cc");
#else /* __s390x__ */
		"	lrvh	%0,%1"
		: "=d" (result) : "m" (*x));
#endif /* __s390x__ */
	return result;
}
#define __arch_swab16p __arch_swab16p

#include <linux/byteorder.h>

#endif /* _S390_BYTEORDER_H */
