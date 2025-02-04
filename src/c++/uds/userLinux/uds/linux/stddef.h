/* SPDX-License-Identifier: GPL-2.0 */
/* Constructed from kernel include/linux/stddef.h and include/uapi/linux/stddef.h */

#ifndef UDS_LINUX_STDDEF_H
#define UDS_LINUX_STDDEF_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#define struct_group(NAME, MEMBERS...)		\
	union {					\
		struct { MEMBERS };		\
		struct { MEMBERS } NAME;	\
	}
#pragma GCC diagnostic pop

#endif /* UDS_LINUX_STDDEF_H */
