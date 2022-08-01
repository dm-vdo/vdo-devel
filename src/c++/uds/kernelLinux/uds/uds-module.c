// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include <linux/module.h>

#include "buffer.h"
#include "errors.h"
#include "funnel-queue.h"
#include "logger.h"
#include "memory-alloc.h"
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
EXPORT_SYMBOL_GPL(uds_get_index_parameters);
EXPORT_SYMBOL_GPL(uds_get_index_stats);
EXPORT_SYMBOL_GPL(uds_open_index);
EXPORT_SYMBOL_GPL(uds_resume_index_session);
EXPORT_SYMBOL_GPL(uds_start_chunk_operation);
EXPORT_SYMBOL_GPL(uds_suspend_index_session);

EXPORT_SYMBOL_GPL(__uds_log_message);
EXPORT_SYMBOL_GPL(__uds_log_strerror);
EXPORT_SYMBOL_GPL(available_space);
EXPORT_SYMBOL_GPL(buffer_length);
EXPORT_SYMBOL_GPL(buffer_used);
EXPORT_SYMBOL_GPL(clear_buffer);
EXPORT_SYMBOL_GPL(compact_buffer);
EXPORT_SYMBOL_GPL(content_length);
EXPORT_SYMBOL_GPL(copy_bytes);
EXPORT_SYMBOL_GPL(current_time_us);
EXPORT_SYMBOL_GPL(ensure_available_space);
EXPORT_SYMBOL_GPL(equal_buffers);
EXPORT_SYMBOL_GPL(free_buffer);
EXPORT_SYMBOL_GPL(free_funnel_queue);
EXPORT_SYMBOL_GPL(funnel_queue_poll);
EXPORT_SYMBOL_GPL(get_boolean);
EXPORT_SYMBOL_GPL(get_buffer_contents);
EXPORT_SYMBOL_GPL(get_byte);
EXPORT_SYMBOL_GPL(get_bytes_from_buffer);
EXPORT_SYMBOL_GPL(get_uds_log_level);
EXPORT_SYMBOL_GPL(get_uds_memory_stats);
EXPORT_SYMBOL_GPL(get_uint16_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint16_les_from_buffer);
EXPORT_SYMBOL_GPL(get_uint32_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_le_from_buffer);
EXPORT_SYMBOL_GPL(get_uint64_les_from_buffer);
EXPORT_SYMBOL_GPL(has_same_bytes);
EXPORT_SYMBOL_GPL(is_funnel_queue_empty);
EXPORT_SYMBOL_GPL(make_buffer);
EXPORT_SYMBOL_GPL(make_funnel_queue);
EXPORT_SYMBOL_GPL(perform_once);
EXPORT_SYMBOL_GPL(put_boolean);
EXPORT_SYMBOL_GPL(put_buffer);
EXPORT_SYMBOL_GPL(put_byte);
EXPORT_SYMBOL_GPL(put_bytes);
EXPORT_SYMBOL_GPL(put_int64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint16_les_into_buffer);
EXPORT_SYMBOL_GPL(put_uint32_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_le_into_buffer);
EXPORT_SYMBOL_GPL(put_uint64_les_into_buffer);
EXPORT_SYMBOL_GPL(register_error_block);
EXPORT_SYMBOL_GPL(report_uds_memory_usage);
EXPORT_SYMBOL_GPL(reset_buffer_end);
EXPORT_SYMBOL_GPL(rewind_buffer);
EXPORT_SYMBOL_GPL(set_uds_log_level);
EXPORT_SYMBOL_GPL(skip_forward);
EXPORT_SYMBOL_GPL(uds_allocate_memory);
EXPORT_SYMBOL_GPL(uds_allocate_memory_nowait);
EXPORT_SYMBOL_GPL(uds_append_to_buffer);
EXPORT_SYMBOL_GPL(uds_assertion_failed);
EXPORT_SYMBOL_GPL(uds_duplicate_string);
EXPORT_SYMBOL_GPL(uds_fixed_sprintf);
EXPORT_SYMBOL_GPL(uds_free_memory);
EXPORT_SYMBOL_GPL(uds_get_thread_device_id);
EXPORT_SYMBOL_GPL(uds_initialize_thread_registry);
EXPORT_SYMBOL_GPL(uds_log_backtrace);
EXPORT_SYMBOL_GPL(uds_log_priority_to_string);
EXPORT_SYMBOL_GPL(uds_log_string_to_priority);
EXPORT_SYMBOL_GPL(uds_lookup_thread);
EXPORT_SYMBOL_GPL(uds_pause_for_logger);
EXPORT_SYMBOL_GPL(uds_reallocate_memory);
EXPORT_SYMBOL_GPL(uds_register_allocating_thread);
EXPORT_SYMBOL_GPL(uds_register_thread);
EXPORT_SYMBOL_GPL(uds_register_thread_device_id);
EXPORT_SYMBOL_GPL(uds_string_error);
EXPORT_SYMBOL_GPL(uds_string_error_name);
EXPORT_SYMBOL_GPL(uds_unregister_allocating_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread);
EXPORT_SYMBOL_GPL(uds_unregister_thread_device_id);
EXPORT_SYMBOL_GPL(uds_v_append_to_buffer);
EXPORT_SYMBOL_GPL(uds_vlog_strerror);
EXPORT_SYMBOL_GPL(uncompacted_amount);
EXPORT_SYMBOL_GPL(wrap_buffer);
EXPORT_SYMBOL_GPL(zero_bytes);


#ifdef TEST_INTERNAL
#include "chapter-index.h"
#include "dory.h"
#include "hash-utils.h"
#include "index.h"
#include "index-session.h"
#include "io-factory.h"
#include "open-chapter.h"
#include "random.h"
#include "request-queue.h"
#include "volume.h"

EXPORT_SYMBOL_GPL(assert_page_in_cache);
EXPORT_SYMBOL_GPL(chapter_index_discard_count);
EXPORT_SYMBOL_GPL(chapter_index_empty_count);
EXPORT_SYMBOL_GPL(chapter_index_overflow_count);
EXPORT_SYMBOL_GPL(chapters_replayed);
EXPORT_SYMBOL_GPL(chapters_written);
EXPORT_SYMBOL_GPL(close_open_chapter);
EXPORT_SYMBOL_GPL(compute_bits);
EXPORT_SYMBOL_GPL(compute_delta_index_save_bytes);
EXPORT_SYMBOL_GPL(compute_index_page_map_save_size);
EXPORT_SYMBOL_GPL(compute_volume_index_save_blocks);
EXPORT_SYMBOL_GPL(destroy_volume_page);
EXPORT_SYMBOL_GPL(discard_index_state_data);
EXPORT_SYMBOL_GPL(discard_open_chapter);
EXPORT_SYMBOL_GPL(dory_forgetful);
EXPORT_SYMBOL_GPL(empty_open_chapter_index);
EXPORT_SYMBOL_GPL(encode_record_page);
EXPORT_SYMBOL_GPL(enqueue_page_read);
EXPORT_SYMBOL_GPL(enqueue_read);
EXPORT_SYMBOL_GPL(enqueue_request);
EXPORT_SYMBOL_GPL(event_count_broadcast);
EXPORT_SYMBOL_GPL(event_count_cancel);
EXPORT_SYMBOL_GPL(event_count_prepare);
EXPORT_SYMBOL_GPL(event_count_wait);
EXPORT_SYMBOL_GPL(extend_delta_zone);
EXPORT_SYMBOL_GPL(find_index_page_number);
EXPORT_SYMBOL_GPL(find_invalidate_and_make_least_recent);
EXPORT_SYMBOL_GPL(find_volume_chapter_boundaries_impl);
EXPORT_SYMBOL_GPL(finish_restoring_delta_index);
EXPORT_SYMBOL_GPL(finish_saving_delta_index);
EXPORT_SYMBOL_GPL(flush_buffered_writer);
EXPORT_SYMBOL_GPL(forget_chapter);
EXPORT_SYMBOL_GPL(free_buffered_reader);
EXPORT_SYMBOL_GPL(free_buffered_writer);
EXPORT_SYMBOL_GPL(free_configuration);
EXPORT_SYMBOL_GPL(free_event_count);
EXPORT_SYMBOL_GPL(free_geometry);
EXPORT_SYMBOL_GPL(free_index);
EXPORT_SYMBOL_GPL(free_index_page_map);
EXPORT_SYMBOL_GPL(free_open_chapter);
EXPORT_SYMBOL_GPL(free_open_chapter_index);
EXPORT_SYMBOL_GPL(free_page_cache);
EXPORT_SYMBOL_GPL(free_radix_sorter);
EXPORT_SYMBOL_GPL(free_uds_index_layout);
EXPORT_SYMBOL_GPL(free_volume);
EXPORT_SYMBOL_GPL(get_delta_entry_value);
EXPORT_SYMBOL_GPL(get_delta_index_bits_used);
EXPORT_SYMBOL_GPL(get_delta_index_entry);
EXPORT_SYMBOL_GPL(get_delta_index_stats);
EXPORT_SYMBOL_GPL(get_index_stats);
EXPORT_SYMBOL_GPL(get_page_from_cache);
EXPORT_SYMBOL_GPL(get_sparse_cache_counters);
EXPORT_SYMBOL_GPL(get_volume_index_combined_stats);
EXPORT_SYMBOL_GPL(get_volume_page);
EXPORT_SYMBOL_GPL(get_volume_page_locked);
EXPORT_SYMBOL_GPL(get_volume_page_protected);
EXPORT_SYMBOL_GPL(has_sparse_chapters);
EXPORT_SYMBOL_GPL(initialize_chapter_index_page);
EXPORT_SYMBOL_GPL(initialize_delta_index);
EXPORT_SYMBOL_GPL(initialize_delta_zone);
EXPORT_SYMBOL_GPL(initialize_delta_zone_page);
EXPORT_SYMBOL_GPL(initialize_volume_page);
EXPORT_SYMBOL_GPL(invalidate_page_cache_for_chapter);
EXPORT_SYMBOL_GPL(invalidate_sparse_cache);
EXPORT_SYMBOL_GPL(is_chapter_sparse);
EXPORT_SYMBOL_GPL(load_open_chapter);
EXPORT_SYMBOL_GPL(load_volume_index);
EXPORT_SYMBOL_GPL(log_uds_memory_allocations);
EXPORT_SYMBOL_GPL(make_configuration);
EXPORT_SYMBOL_GPL(make_event_count);
EXPORT_SYMBOL_GPL(make_geometry);
EXPORT_SYMBOL_GPL(make_index);
EXPORT_SYMBOL_GPL(make_index_page_map);
EXPORT_SYMBOL_GPL(make_open_chapter);
EXPORT_SYMBOL_GPL(make_open_chapter_index);
EXPORT_SYMBOL_GPL(make_page_cache);
EXPORT_SYMBOL_GPL(make_page_most_recent);
EXPORT_SYMBOL_GPL(make_radix_sorter);
EXPORT_SYMBOL_GPL(make_uds_bufio);
EXPORT_SYMBOL_GPL(make_uds_index_layout);
EXPORT_SYMBOL_GPL(make_uds_io_factory);
EXPORT_SYMBOL_GPL(make_uds_request_queue);
EXPORT_SYMBOL_GPL(make_volume);
EXPORT_SYMBOL_GPL(make_volume_index);
EXPORT_SYMBOL_GPL(map_to_physical_chapter);
EXPORT_SYMBOL_GPL(map_to_physical_page);
EXPORT_SYMBOL_GPL(min_volume_index_delta_lists);
EXPORT_SYMBOL_GPL(move_bits);
EXPORT_SYMBOL_GPL(next_delta_index_entry);
EXPORT_SYMBOL_GPL(open_uds_buffered_reader);
EXPORT_SYMBOL_GPL(open_uds_buffered_writer);
EXPORT_SYMBOL_GPL(pack_open_chapter_index_page);
EXPORT_SYMBOL_GPL(put_delta_index_entry);
EXPORT_SYMBOL_GPL(put_open_chapter);
EXPORT_SYMBOL_GPL(put_open_chapter_index_record);
EXPORT_SYMBOL_GPL(put_page_in_cache);
EXPORT_SYMBOL_GPL(put_uds_io_factory);
EXPORT_SYMBOL_GPL(put_volume_index_record);
EXPORT_SYMBOL_GPL(radix_sort);
EXPORT_SYMBOL_GPL(random_in_range);
EXPORT_SYMBOL_GPL(read_from_buffered_reader);
EXPORT_SYMBOL_GPL(read_index_page_map);
EXPORT_SYMBOL_GPL(read_volume_page);
EXPORT_SYMBOL_GPL(rel_time_to_string);
EXPORT_SYMBOL_GPL(remove_delta_index_entry);
EXPORT_SYMBOL_GPL(remove_from_open_chapter);
EXPORT_SYMBOL_GPL(remove_volume_index_record);
EXPORT_SYMBOL_GPL(reset_open_chapter);
EXPORT_SYMBOL_GPL(save_index);
EXPORT_SYMBOL_GPL(save_open_chapter);
EXPORT_SYMBOL_GPL(save_volume_index);
EXPORT_SYMBOL_GPL(saves_begun);
EXPORT_SYMBOL_GPL(search_chapter_index_page);
EXPORT_SYMBOL_GPL(search_open_chapter);
EXPORT_SYMBOL_GPL(search_record_page);
EXPORT_SYMBOL_GPL(search_volume_page_cache);
EXPORT_SYMBOL_GPL(select_victim_in_cache);
EXPORT_SYMBOL_GPL(set_delta_entry_value);
EXPORT_SYMBOL_GPL(set_request_restarter);
EXPORT_SYMBOL_GPL(set_volume_index_record_chapter);
EXPORT_SYMBOL_GPL(sleep_for);
EXPORT_SYMBOL_GPL(space_remaining_in_write_buffer);
EXPORT_SYMBOL_GPL(start_delta_index_search);
EXPORT_SYMBOL_GPL(start_restoring_delta_index);
EXPORT_SYMBOL_GPL(start_saving_delta_index);
EXPORT_SYMBOL_GPL(swap_delta_index_page_endianness);
EXPORT_SYMBOL_GPL(sync_volume_store);
EXPORT_SYMBOL_GPL(track_uds_memory_allocations);
EXPORT_SYMBOL_GPL(uds_alloc_sprintf);
EXPORT_SYMBOL_GPL(uds_allocate_memory_counter);
EXPORT_SYMBOL_GPL(uds_allocation_error_injection);
EXPORT_SYMBOL_GPL(uds_apply_to_threads);
EXPORT_SYMBOL_GPL(uds_broadcast_cond);
EXPORT_SYMBOL_GPL(uds_create_thread);
EXPORT_SYMBOL_GPL(uds_destroy_barrier);
EXPORT_SYMBOL_GPL(uds_destroy_cond);
EXPORT_SYMBOL_GPL(uds_enter_barrier);
EXPORT_SYMBOL_GPL(uds_get_num_cores);
EXPORT_SYMBOL_GPL(uds_get_thread_id);
EXPORT_SYMBOL_GPL(uds_init_cond);
EXPORT_SYMBOL_GPL(uds_initialize_barrier);
EXPORT_SYMBOL_GPL(uds_join_threads);
EXPORT_SYMBOL_GPL(uds_log_embedded_message);
EXPORT_SYMBOL_GPL(uds_request_queue_enqueue);
EXPORT_SYMBOL_GPL(uds_request_queue_finish);
EXPORT_SYMBOL_GPL(uds_signal_cond);
EXPORT_SYMBOL_GPL(uds_thread_exit);
EXPORT_SYMBOL_GPL(uds_timed_wait_cond);
EXPORT_SYMBOL_GPL(uds_wait_cond);
EXPORT_SYMBOL_GPL(uds_wrap_vsnprintf);
EXPORT_SYMBOL_GPL(uds_yield_scheduler);
EXPORT_SYMBOL_GPL(uninitialize_delta_index);
EXPORT_SYMBOL_GPL(uninitialize_delta_zone);
EXPORT_SYMBOL_GPL(update_index_page_map);
EXPORT_SYMBOL_GPL(verify_buffered_data);
EXPORT_SYMBOL_GPL(wait_for_idle_index);
EXPORT_SYMBOL_GPL(write_chapter);
EXPORT_SYMBOL_GPL(write_guard_delta_list);
EXPORT_SYMBOL_GPL(write_index_page_map);
EXPORT_SYMBOL_GPL(write_index_pages);
EXPORT_SYMBOL_GPL(write_record_pages);
EXPORT_SYMBOL_GPL(write_to_buffered_writer);
EXPORT_SYMBOL_GPL(write_zeros_to_buffered_writer);
#endif /* TEST_INTERNAL */


MODULE_DESCRIPTION("deduplication engine");
MODULE_AUTHOR("Red Hat, Inc.");
MODULE_LICENSE("GPL");
MODULE_VERSION(CURRENT_VERSION);
