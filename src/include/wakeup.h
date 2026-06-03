#ifndef WAKEUP_H
#define WAKEUP_H

#include "types.h"

/*
 * Wakeup sources — Power Management wakeup event tracking.
 *
 * Each wakeup-capable device driver registers a wakeup source that
 * tracks how many wakeup events it has delivered.  The PM subsystem
 * uses this information to decide whether it is safe to suspend:
 * if any wakeup event is currently being processed, suspend is
 * deferred to avoid losing the event.
 *
 * Reference: Linux kernel drivers/base/power/wakeup.c
 *
 * Usage:
 *   int src = wakeup_source_register("rtc0");
 *   wakeup_source_event(src);   // count a wakeup event
 *   uint64_t cnt = wakeup_source_count(src);
 *   wakeup_source_unregister(src);
 *
 *   // Before suspend: check if any events are in flight
 *   if (wakeup_get_active_count() > 0) {
 *       // defer suspend, events still being processed
 *   }
 *
 *   // After resume: clear the pending flag
 *   wakeup_clear_active();
 */

/* Maximum number of wakeup sources the system can track. */
#define WAKEUP_SRC_MAX  32

/* Maximum length of a wakeup source name (including NUL terminator). */
#define WAKEUP_NAME_MAX 48

/**
 * wakeup_source_register - Register a wakeup source by name.
 * @name: Human-readable name (e.g. "rtc0", "usb-hcd", "power-button").
 *
 * Returns a non-negative source ID on success, or -1 on failure.
 */
int wakeup_source_register(const char *name);

/**
 * wakeup_source_unregister - Unregister a previously registered source.
 * @id: Source ID returned by wakeup_source_register().
 */
void wakeup_source_unregister(int id);

/**
 * wakeup_source_event - Record that a wakeup event has occurred.
 * @id: Source ID.
 *
 * Increments the event counter for this source and marks the source
 * as having an event in-flight (which prevents suspend until
 * wakeup_clear_active() is called).
 */
void wakeup_source_event(int id);

/**
 * wakeup_source_count - Read the total number of wakeup events for a source.
 * @id: Source ID.
 *
 * Returns the event count, or 0 if @id is invalid.
 */
uint64_t wakeup_source_count(int id);

/**
 * wakeup_source_name - Get the name of a wakeup source.
 * @id: Source ID.
 *
 * Returns a pointer to the name string, or NULL if @id is invalid.
 */
const char *wakeup_source_name(int id);

/**
 * wakeup_source_is_active - Check if a source has events in-flight.
 * @id: Source ID.
 *
 * Returns 1 if the source has an unprocessed wakeup event, 0 otherwise.
 */
int wakeup_source_is_active(int id);

/**
 * wakeup_get_active_count - Total number of sources with events in-flight.
 *
 * Returns the count of wakeup sources that currently have an unprocessed
 * event.  The PM suspend path checks this before entering a sleep state;
 * if non-zero, suspend is deferred.
 */
int wakeup_get_active_count(void);

/**
 * wakeup_clear_active - Clear all in-flight wakeup event flags.
 *
 * Called after resume to acknowledge all pending wakeup events
 * and re-arm the counter for the next suspend cycle.
 */
void wakeup_clear_active(void);

/**
 * wakeup_print_sources - Print all registered wakeup sources to kernel log.
 *
 * Used by /sys/power/wakeup_count read and for debugging.
 */
void wakeup_print_sources(void);

/**
 * wakeup_init - Initialise the wakeup sources subsystem.
 *
 * Called once at boot from the PM init path.
 */
void wakeup_init(void);

#endif /* WAKEUP_H */
