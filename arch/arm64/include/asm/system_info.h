/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ARM_SYSTEM_INFO_H
#define __ASM_ARM_SYSTEM_INFO_H

#ifndef __ASSEMBLY__

/* information about the system we're running on */
#if defined(CONFIG_PLAT_RK3399_ODROIDN1)
extern const char *machine_name;
#endif
extern unsigned int system_rev;
extern unsigned int system_serial_low;
extern unsigned int system_serial_high;

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_ARM_SYSTEM_INFO_H */
