/* FC3PerformancePatch - shared definitions.
 * Copyright (C) 2026 FC3PerformancePatch contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_H
#define FC3PP_H

#define FC3PP_NAME "FC3PerformancePatch"
#define FC3PP_VERSION "1.0.0"

#define FC3PP_INI_NAME L"FC3PerformancePatch.ini"
#define FC3PP_LOG_NAME L"FC3PerformancePatch.log"

/* A 32-bit process can only address the first 32 logical processors of its
 * processor group through the classic affinity APIs. */
#define FC3PP_MAX_CPUS 32

/* The engine's job manager caps itself at 15 cores x 2 threads. Never present
 * more than this many logical processors to the engine; it also keeps every
 * engine-visible affinity mask below bit 31, which is what makes the legacy
 * core-enumeration loops terminate. */
#define FC3PP_MAX_ENGINE_CPUS 30

#endif /* FC3PP_H */
