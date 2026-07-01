/* thread_affinity.h -- Nintendo Switch three-core scheduling policy */

#ifndef __THREAD_AFFINITY_H__
#define __THREAD_AFFINITY_H__

void thread_affinity_init(void);

void thread_affinity_pin_source_thread(void);
void thread_affinity_pin_background_thread(void);
void thread_affinity_pin_render_thread(void);

#endif
