/* version.dll forwarders.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PROXY_H
#define FC3PP_PROXY_H

/* Loads the system version.dll and resolves every forwarded export. Safe to
 * call repeatedly and from several threads; must not be called from DllMain. */
void fc3pp_proxy_resolve(void);

/* Names of the exports this proxy provides. */
const char *const *proxy_export_names(unsigned *count);

/* Non-zero once the real library has been loaded. */
int proxy_is_resolved(void);

void proxy_shutdown(void);

#endif /* FC3PP_PROXY_H */
