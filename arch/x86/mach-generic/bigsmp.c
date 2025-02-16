/*
 * APIC driver for "bigsmp" XAPIC machines with more than 8 virtual CPUs.
 * Drives the local APIC in "clustered mode".
 */
#define APIC_DEFINITION 1
#include <linux/threads.h>
#include <linux/cpumask.h>
#include <asm/mpspec.h>
#include <asm/genapic.h>
#include <asm/fixmap.h>
#include <asm/apicdef.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/dmi.h>
#include <asm/bigsmp/apicdef.h>
#include <linux/smp.h>
#include <asm/bigsmp/apic.h>
#include <asm/bigsmp/ipi.h>
#include <asm/mach-default/mach_mpparse.h>
#include <asm/mach-default/mach_wakecpu.h>

static int dmi_bigsmp; /* can be set by dmi scanners */

static int hp_ht_bigsmp(const struct dmi_system_id *d)
{
	printk(KERN_NOTICE "%s detected: force use of apic=bigsmp\n", d->ident);
	dmi_bigsmp = 1;
	return 0;
}


static const struct dmi_system_id bigsmp_dmi_table[] = {
	{ hp_ht_bigsmp, "HP ProLiant DL760 G2",
	{ DMI_MATCH(DMI_BIOS_VENDOR, "HP"),
	DMI_MATCH(DMI_BIOS_VERSION, "P44-"),}
	},

	{ hp_ht_bigsmp, "HP ProLiant DL740",
	{ DMI_MATCH(DMI_BIOS_VENDOR, "HP"),
	DMI_MATCH(DMI_BIOS_VERSION, "P47-"),}
	},
	 { }
};

static cpumask_t vector_allocation_domain(int cpu)
{
        return cpumask_of_cpu(cpu);
}

static int probe_bigsmp(void)
{
	if (def_to_bigsmp)
		dmi_bigsmp = 1;
	else
		dmi_check_system(bigsmp_dmi_table);
	return dmi_bigsmp;
}

struct genapic apic_bigsmp = APIC_INIT("bigsmp", probe_bigsmp);
