/* SPDX-License-Identifier: MIT */
/*
 * netp.h — netp backend public test helper.
 *
 * Ported from intp-comparison:
 *   variants/v2.1-c-abi-cgroup/include/backend.h @ 2e87b30fc762
 *   (the "Public helpers shared with the unit tests" declaration of
 *   netp_resolve_speed, moved here so -Wmissing-prototypes passes).
 */

#ifndef INTP_BACKEND_NETP_H
#define INTP_BACKEND_NETP_H

/* Public helper shared with the unit tests -- safe to call at startup. */
long netp_resolve_speed(const char *iface, int *assumed_out);

#endif /* INTP_BACKEND_NETP_H */
