// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2023 Red Hat
 */

#include <linux/module.h>

#include "errors.h"
#include "event-count.h"
#include "funnel-queue.h"
#include "logger.h"
#include "memory-alloc.h"
#include "string-utils.h"
#include "thread-device.h"
#include "time-utils.h"
#include "uds.h"
#include "uds-sysfs.h"
#include "uds-threads.h"

static int __init dedupe_init(void)
{
	uds_initialize_thread_device_registry();
	uds_memory_init();
	uds_log_info("loaded version %s", CURRENT_VERSION);
	uds_init_sysfs();
	return 0;
}

static void __exit dedupe_exit(void)
{
	uds_put_sysfs();
	uds_memory_exit();
	uds_log_info("unloaded version %s", CURRENT_VERSION);
}

module_init(dedupe_init);
module_exit(dedupe_exit);

EXPORT_SYMBOL_GPL(uds_close_index);
EXPORT_SYMBOL_GPL(uds_compute_index_size);
EXPORT_SYMBOL_GPL(uds_create_index_session);
EXPORT_SYMBOL_GPL(uds_destroy_index_session);
EXPORT_SYMBOL_GPL(uds_flush_index_session);
EXPORT_SYMBOL_GPL(uds_get_index_session_stats);
EXPORT_SYMBOL_GPL(uds_launch_request);
EXPORT_SYMBOL_GPL(uds_open_index);
EXPORT_SYMBOL_GPL(uds_resume_index_session);
EXPORT_SYMBOL_GPL(uds_suspend_index_session);

EXPORT_SYMBOL_GPL(__uds_log_message);
EXPORT_SYMBOL_GPL(__uds_log_strerror);
EXPORT_SYMBOL_GPL(uds_allocate_memory);
EXPORT_SYMBOL_GPL(uds_allocate_memory_nowait);
EXPORT_SYMBOL_GPL(uds_append_to_buffer);
EXPORT_SYMBOL_GPL(uds_assertion_failed);
EXPORT_SYMBOL_GPL(uds_duplicate_string);
EXPORT_SYMBOL_GPL(uds_free_funnel_queue);
EXPORT_SYMBOL_GPL(uds_free_memory);
EXPORT_SYMBOL_GPL(uds_funnel_queue_poll);
EXPORT_SYMBOL_GPL(uds_get_log_level);
EXPORT_SYMBOL_GPL(uds_get_memory_stats);
EXPORT_SYMBOL_GPL(uds_get_thread_device_id);
EXPORT_SYMBOL_GPL(uds_initialize_thread_registry);
EXPORT_SYMBOL_GPL(uds_is_funnel_queue_empty);
EXPORT_SYMBOL_GPL(uds_log_backtrace);
EXPORT_SYMBOL_GPL(uds_log_priority_to_string);
EXPORT_SYMBOL_GPL(uds_log_string_to_priority);
EXPORT_SYMBOL_GPL(uds_lookup_thread);
EXPORT_SYMBOL_GPL(uds_make_funnel_queue);
EXPORT_SYMBOL_GPL(uds_perform_once);
EXPORT_SYMBOL_GPL(uds_reallocate_memory);
EXPORT_SYMBOL_GPL(uds_register_allocating_thread);
EXPORT_SYMBOL_GPL(uds_register_error_block);
EXPORT_SYMBOL_GPL(uds_register_thread);
EXPORT_SYMBOL_GPL(uds_register_thread_device_id);
EXPORT_SYMBOL_GPL(uds_report_memory_usage);
EXPORT_SYMBOL_GPL(uds_set_log_level);
EXPORT_SYMBOL_GPL(uds_string_error);
EXPORT_SYMBOL_GPL(uds_string_error_name);
EXPORT_SYMBOL_GPL(uds_unregister_allocating_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread_device_id);

#ifdef TEST_INTERNAL
#include "chapter-index.h"
#include "dory.h"
#include "funnel-requestqueue.h"
#include "hash-utils.h"
#include "index.h"
#include "index-session.h"
#include "io-factory.h"
#include "murmurhash3.h"
#include "open-chapter.h"
#include "volume.h"
#include "volume-index.h"

EXPORT_SYMBOL_GPL(begin_pending_search);
EXPORT_SYMBOL_GPL(chapter_index_discard_count);
EXPORT_SYMBOL_GPL(chapter_index_empty_count);
EXPORT_SYMBOL_GPL(chapter_index_overflow_count);
EXPORT_SYMBOL_GPL(chapters_replayed);
EXPORT_SYMBOL_GPL(chapters_written);
EXPORT_SYMBOL_GPL(discard_index_state_data);
EXPORT_SYMBOL_GPL(dory_forgetful);
EXPORT_SYMBOL_GPL(encode_record_page);
EXPORT_SYMBOL_GPL(end_pending_search);
EXPORT_SYMBOL_GPL(enqueue_page_read);
EXPORT_SYMBOL_GPL(enqueue_read);
EXPORT_SYMBOL_GPL(event_count_broadcast);
EXPORT_SYMBOL_GPL(event_count_cancel);
EXPORT_SYMBOL_GPL(event_count_prepare);
EXPORT_SYMBOL_GPL(event_count_wait);
EXPORT_SYMBOL_GPL(extend_delta_zone);
EXPORT_SYMBOL_GPL(find_chapter_limits);
EXPORT_SYMBOL_GPL(free_event_count);
EXPORT_SYMBOL_GPL(get_dlist_number);
EXPORT_SYMBOL_GPL(get_page_from_cache);
EXPORT_SYMBOL_GPL(get_sub_index);
EXPORT_SYMBOL_GPL(get_volume_index_memory_used);
EXPORT_SYMBOL_GPL(get_volume_index_separate_stats);
EXPORT_SYMBOL_GPL(get_volume_page_locked);
EXPORT_SYMBOL_GPL(get_volume_page_protected);
EXPORT_SYMBOL_GPL(initialize_page_cache);
EXPORT_SYMBOL_GPL(invalidate_page);
EXPORT_SYMBOL_GPL(log_uds_memory_allocations);
EXPORT_SYMBOL_GPL(make_event_count);
EXPORT_SYMBOL_GPL(make_page_most_recent);
EXPORT_SYMBOL_GPL(map_to_physical_page);
EXPORT_SYMBOL_GPL(min_volume_index_delta_lists);
EXPORT_SYMBOL_GPL(move_bits);
EXPORT_SYMBOL_GPL(murmurhash3_128);
EXPORT_SYMBOL_GPL(put_page_in_cache);
EXPORT_SYMBOL_GPL(saves_begun);
EXPORT_SYMBOL_GPL(search_record_page);
EXPORT_SYMBOL_GPL(select_victim_in_cache);
EXPORT_SYMBOL_GPL(set_chapter_tester);
EXPORT_SYMBOL_GPL(set_request_restarter);
EXPORT_SYMBOL_GPL(swap_delta_index_page_endianness);
EXPORT_SYMBOL_GPL(test_page_count);
EXPORT_SYMBOL_GPL(test_pages);
EXPORT_SYMBOL_GPL(track_uds_memory_allocations);
EXPORT_SYMBOL_GPL(uds_alloc_sprintf);
EXPORT_SYMBOL_GPL(uds_allocate_memory_counter);
EXPORT_SYMBOL_GPL(uds_allocation_error_injection);
EXPORT_SYMBOL_GPL(uds_apply_to_threads);
EXPORT_SYMBOL_GPL(uds_broadcast_cond);
EXPORT_SYMBOL_GPL(uds_close_open_chapter);
EXPORT_SYMBOL_GPL(uds_compute_delta_index_save_bytes);
EXPORT_SYMBOL_GPL(uds_compute_index_page_map_save_size);
EXPORT_SYMBOL_GPL(uds_compute_saved_open_chapter_size);
EXPORT_SYMBOL_GPL(uds_compute_volume_index_save_blocks);
EXPORT_SYMBOL_GPL(uds_create_thread);
EXPORT_SYMBOL_GPL(uds_destroy_barrier);
EXPORT_SYMBOL_GPL(uds_destroy_cond);
EXPORT_SYMBOL_GPL(uds_discard_open_chapter);
EXPORT_SYMBOL_GPL(uds_empty_open_chapter_index);
EXPORT_SYMBOL_GPL(uds_enqueue_request);
EXPORT_SYMBOL_GPL(uds_enter_barrier);
EXPORT_SYMBOL_GPL(uds_find_index_page_number);
EXPORT_SYMBOL_GPL(uds_finish_restoring_delta_index);
EXPORT_SYMBOL_GPL(uds_finish_saving_delta_index);
EXPORT_SYMBOL_GPL(uds_fixed_sprintf);
EXPORT_SYMBOL_GPL(uds_flush_buffered_writer);
EXPORT_SYMBOL_GPL(uds_forget_chapter);
EXPORT_SYMBOL_GPL(uds_free_buffered_reader);
EXPORT_SYMBOL_GPL(uds_free_buffered_writer);
EXPORT_SYMBOL_GPL(uds_free_configuration);
EXPORT_SYMBOL_GPL(uds_free_geometry);
EXPORT_SYMBOL_GPL(uds_free_index);
EXPORT_SYMBOL_GPL(uds_free_index_layout);
EXPORT_SYMBOL_GPL(uds_free_index_page_map);
EXPORT_SYMBOL_GPL(uds_free_open_chapter);
EXPORT_SYMBOL_GPL(uds_free_open_chapter_index);
EXPORT_SYMBOL_GPL(uds_free_radix_sorter);
EXPORT_SYMBOL_GPL(uds_free_volume);
EXPORT_SYMBOL_GPL(uds_free_volume_index);
EXPORT_SYMBOL_GPL(uds_get_delta_entry_value);
EXPORT_SYMBOL_GPL(uds_get_delta_index_entry);
EXPORT_SYMBOL_GPL(uds_get_delta_index_stats);
EXPORT_SYMBOL_GPL(uds_get_index_parameters);
EXPORT_SYMBOL_GPL(uds_get_index_stats);
EXPORT_SYMBOL_GPL(uds_get_volume_index_record);
EXPORT_SYMBOL_GPL(uds_get_volume_index_stats);
EXPORT_SYMBOL_GPL(uds_get_volume_index_zone);
EXPORT_SYMBOL_GPL(uds_get_volume_record_page);
EXPORT_SYMBOL_GPL(uds_has_sparse_chapters);
EXPORT_SYMBOL_GPL(uds_init_cond);
EXPORT_SYMBOL_GPL(uds_initialize_barrier);
EXPORT_SYMBOL_GPL(uds_initialize_chapter_index_page);
EXPORT_SYMBOL_GPL(uds_initialize_delta_index);
EXPORT_SYMBOL_GPL(uds_invalidate_sparse_cache);
EXPORT_SYMBOL_GPL(uds_is_chapter_sparse);
EXPORT_SYMBOL_GPL(uds_is_volume_index_sample);
EXPORT_SYMBOL_GPL(uds_join_threads);
EXPORT_SYMBOL_GPL(uds_load_open_chapter);
EXPORT_SYMBOL_GPL(uds_load_volume_index);
EXPORT_SYMBOL_GPL(uds_log_embedded_message);
EXPORT_SYMBOL_GPL(uds_lookup_volume_index_name);
EXPORT_SYMBOL_GPL(uds_make_buffered_reader);
EXPORT_SYMBOL_GPL(uds_make_buffered_writer);
EXPORT_SYMBOL_GPL(uds_make_bufio);
EXPORT_SYMBOL_GPL(uds_make_configuration);
EXPORT_SYMBOL_GPL(uds_make_geometry);
EXPORT_SYMBOL_GPL(uds_make_index);
EXPORT_SYMBOL_GPL(uds_make_index_layout);
EXPORT_SYMBOL_GPL(uds_make_index_page_map);
EXPORT_SYMBOL_GPL(uds_make_io_factory);
EXPORT_SYMBOL_GPL(uds_make_open_chapter);
EXPORT_SYMBOL_GPL(uds_make_open_chapter_index);
EXPORT_SYMBOL_GPL(uds_make_radix_sorter);
EXPORT_SYMBOL_GPL(uds_make_request_queue);
EXPORT_SYMBOL_GPL(uds_make_volume);
EXPORT_SYMBOL_GPL(uds_make_volume_index);
EXPORT_SYMBOL_GPL(uds_map_to_physical_chapter);
EXPORT_SYMBOL_GPL(uds_next_delta_index_entry);
EXPORT_SYMBOL_GPL(uds_pack_open_chapter_index_page);
EXPORT_SYMBOL_GPL(uds_put_delta_index_entry);
EXPORT_SYMBOL_GPL(uds_put_io_factory);
EXPORT_SYMBOL_GPL(uds_put_open_chapter);
EXPORT_SYMBOL_GPL(uds_put_open_chapter_index_record);
EXPORT_SYMBOL_GPL(uds_put_volume_index_record);
EXPORT_SYMBOL_GPL(uds_radix_sort);
EXPORT_SYMBOL_GPL(uds_read_from_buffered_reader);
EXPORT_SYMBOL_GPL(uds_read_index_page_map);
EXPORT_SYMBOL_GPL(uds_remove_delta_index_entry);
EXPORT_SYMBOL_GPL(uds_remove_from_open_chapter);
EXPORT_SYMBOL_GPL(uds_remove_volume_index_record);
EXPORT_SYMBOL_GPL(uds_request_queue_enqueue);
EXPORT_SYMBOL_GPL(uds_request_queue_finish);
EXPORT_SYMBOL_GPL(uds_reset_open_chapter);
EXPORT_SYMBOL_GPL(uds_save_index);
EXPORT_SYMBOL_GPL(uds_save_open_chapter);
EXPORT_SYMBOL_GPL(uds_save_volume_index);
EXPORT_SYMBOL_GPL(uds_search_chapter_index_page);
EXPORT_SYMBOL_GPL(uds_search_open_chapter);
EXPORT_SYMBOL_GPL(uds_search_volume_page_cache);
EXPORT_SYMBOL_GPL(uds_set_delta_entry_value);
EXPORT_SYMBOL_GPL(uds_set_volume_index_open_chapter);
EXPORT_SYMBOL_GPL(uds_set_volume_index_record_chapter);
EXPORT_SYMBOL_GPL(uds_set_volume_index_zone_open_chapter);
EXPORT_SYMBOL_GPL(uds_signal_cond);
EXPORT_SYMBOL_GPL(uds_start_delta_index_search);
EXPORT_SYMBOL_GPL(uds_start_restoring_delta_index);
EXPORT_SYMBOL_GPL(uds_start_saving_delta_index);
EXPORT_SYMBOL_GPL(uds_thread_exit);
EXPORT_SYMBOL_GPL(uds_timed_wait_cond);
EXPORT_SYMBOL_GPL(uds_uninitialize_delta_index);
EXPORT_SYMBOL_GPL(uds_update_index_page_map);
EXPORT_SYMBOL_GPL(uds_verify_buffered_data);
EXPORT_SYMBOL_GPL(uds_wait_cond);
EXPORT_SYMBOL_GPL(uds_wait_for_idle_index);
EXPORT_SYMBOL_GPL(uds_write_chapter);
EXPORT_SYMBOL_GPL(uds_write_guard_delta_list);
EXPORT_SYMBOL_GPL(uds_write_index_page_map);
EXPORT_SYMBOL_GPL(uds_write_to_buffered_writer);
EXPORT_SYMBOL_GPL(uninitialize_page_cache);
#endif /* TEST_INTERNAL */


MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
