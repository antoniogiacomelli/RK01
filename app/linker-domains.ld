/* SPDX-License-Identifier: Apache-2.0 */
/******************************************************************************/
/*                                                                            */
/* RK01 - Bounded Responses. Bounded domains.                                  */
/* VERSION: V0.1.0                                                            */
/* (C) 2026 Antonio Giacomelli <dev@kernel0.org>                               */
/*                                                                            */
/******************************************************************************/

/*
 * Reserved application domain linker hook.
 *
 * The intended RK01 image direction is ThreadX-inspired: domain images should
 * be described by the linker with bounded code/rodata, data/BSS, entry
 * metadata and MPU geometry. V0.1.0 has the writable-domain/SVC side of that
 * model, but public examples still use RK_DECLARE_DOMAIN() C-declared RAM. No
 * per-domain linker symbols are required in this release.
 */
