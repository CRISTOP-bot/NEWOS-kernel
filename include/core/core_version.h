/* NEWOS release identity.
 *
 * The build passes NEWOS_VERSION / NEWOS_VERSION_ID from the repository's
 * VERSION file (see Makefile), so there is exactly one place to bump the
 * system version. The fallbacks only matter for single-file host builds.
 */
#ifndef CORE_VERSION_H
#define CORE_VERSION_H

#ifndef NEWOS_VERSION
#define NEWOS_VERSION "0.0.0"
#endif

#ifndef NEWOS_VERSION_ID
#define NEWOS_VERSION_ID NEWOS_VERSION
#endif

#endif /* CORE_VERSION_H */
