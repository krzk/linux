/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_CLK_IMX_H
#define __LINUX_CLK_IMX_H

int mx21_clocks_init(unsigned long lref, unsigned long href);
int mx27_clocks_init(unsigned long fref);
int mx31_clocks_init(unsigned long fref);
int mx35_clocks_init(void);

#endif /* __LINUX_CLK_IMX_H */
