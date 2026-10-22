/* SPDX-License-Identifier: MIT
 * Zero-valued storage for the optional sheng components the boot-progress page reads.
 * This board has no touch/cpufreq/display/GPU component yet; the page then reports zeros.
 * The declarations match package/BootProgress.c exactly (same types and array bounds) so a
 * later real provider can replace this file without changing the page.
 * Every definition is weak: a real provider linked next to it (the display archive's NativeLegacyDisplay,
 * LegacyDisplayProbe, NativeHidInput and TouchState define many of these) takes precedence.
 */
#include <stdint.h>
#pragma clang attribute push (__attribute__((weak)), apply_to = any(function, variable))
void boot_ui_trace_snapshot(unsigned a) { (void)a; }
char boot_ui_trace_rows[14][120];
char boot_ui_trace_label[120];
unsigned boot_ui_trace_ready;
volatile unsigned boot_ui_trace_phase;
volatile unsigned boot_ui_trace_index;
void boot_process_capture(void) {  }
void boot_process_snapshot(void) {  }
void sheng_cpufreq_poll(void) {  }
char boot_process_rows[10][120];
unsigned boot_ui_trace_role_rows(unsigned a,char b[][120],unsigned c,unsigned d) { (void)a;(void)b;(void)c;(void)d;return 0; }
char native_touch_rows[4][120];
char native_touch_trace[8][120];
char tb128fu_gpu_rows[16][120];
volatile unsigned tb128fu_gpu_stage_req;
char native_cpufreq_row[120];
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8];
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[12];
volatile int native_touch_ys[12];
volatile uint32_t native_cpufreq_stage;
volatile unsigned native_touch_stage_req;
volatile unsigned boot_process_passes;
volatile unsigned boot_process_seen;
volatile unsigned boot_process_attempts;
volatile unsigned boot_process_busy;
volatile uint32_t native_legacy_stage;
volatile uint32_t native_legacy_pm_stage;
volatile uint32_t native_legacy_pm_calls;
volatile uint32_t native_legacy_pm_callbacks;
volatile uint32_t native_legacy_swap_validation_calls;
volatile uint32_t native_legacy_swap_validation_result;
volatile uint32_t native_legacy_swap_validation_reason;
volatile uint32_t native_legacy_swap_accepted_count;
volatile uint32_t native_legacy_swap_rejected_count;
volatile uint64_t native_legacy_rej_surface[7];
volatile uint32_t native_legacy_rej_reason;
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3];
volatile uint32_t native_legacy_rej_size[2];
char native_legacy_rej_class[32];
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[32];
volatile uint32_t native_legacy_frame_time[32];
volatile uint64_t native_legacy_frame_stats[32][5];
volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4];
volatile uint64_t native_legacy_frame_color_other[8];
volatile uint32_t native_legacy_transfer_calls;
volatile uint32_t native_legacy_retirement_calls;
volatile uint32_t native_legacy_guard_slot;
volatile uint32_t native_legacy_prepare_calls;
volatile uint32_t native_legacy_prepare_reason;
volatile uint32_t native_legacy_prepare_attempts;
volatile uint32_t native_legacy_client_calls;
volatile uint32_t native_legacy_client_type;
volatile int native_legacy_client_pid;
volatile int native_legacy_client_status;
char native_legacy_client_class[64];
volatile uint64_t native_legacy_gop_virtual;
volatile uint64_t native_legacy_gop_physical;
volatile uint64_t native_legacy_gop_bytes;
volatile int native_legacy_transfer_status;
volatile uint64_t native_legacy_transfer_bytes;
volatile uint64_t native_legacy_copy_n;
volatile uint64_t native_gop_gpu_offers;
volatile uint64_t native_gop_gpu_completions;
volatile uint64_t native_gop_gpu_failures;
volatile uint64_t native_gop_gpu_abandoned;
volatile uint32_t native_legacy_gpu_fallbacks;
volatile uint32_t native_legacy_gpu_last_status;
volatile uint64_t native_legacy_copy_last;
volatile uint64_t native_legacy_copy_max;
volatile uint64_t native_legacy_copy_sum;
volatile uint64_t native_legacy_copy_bytes_last;
volatile uint64_t native_legacy_copy_bytes_sum;
volatile uint64_t native_adreno_cache_stats[5];
volatile uint32_t native_adreno_coherence[8];
volatile uint32_t native_adreno_wc_distrusted;
volatile uint32_t native_adreno_wc_audit[6];
volatile uint64_t native_adreno_reassert_ticks;
volatile uint64_t native_adreno_audit_ticks;
volatile uint32_t native_adreno_audit_attr;

/* Read by package/BootLog.c (the UFS diagnostic log). Same declarations as there; zeros and empty reports. The reset backend
 * (boot_reset_probe, boot_reset_bootloader) is package/BootReset.c. */
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
unsigned boot_ui_trace_hot_rows(char a[][120], unsigned b) { (void)a;(void)b;return 0; }
unsigned native_legacy_clients_dump(char *a, unsigned b) { (void)a;(void)b;return 0; }
unsigned native_legacy_prepare_report(char *a, unsigned b) { (void)a;(void)b;return 0; }
unsigned native_legacy_properties_now(char *a, unsigned b) { (void)a;(void)b;return 0; }
unsigned native_legacy_state_words(char *a, unsigned b) { (void)a;(void)b;return 0; }
unsigned native_legacy_swap_timeline(char *a, unsigned b) { (void)a;(void)b;return 0; }
int (*native_adreno_cached_read32)(uint64_t, uint32_t *);
int (*native_adreno_dram_read32)(uint64_t, uint32_t *);
struct native_adreno_client_record native_adreno_client_log[8];
struct native_adreno_asker_record native_adreno_metal_askers[12];
char native_adreno_metal_rows[48][96];
char native_gpu_coherence_row[120];
uint32_t native_gpu_perf_vote[2];
volatile uint32_t native_adreno_coherent_skips[4];
volatile uint32_t native_adreno_export_stats[4];
volatile uint64_t native_adreno_fence_phys;
volatile uint32_t native_adreno_fence_wait[8];
volatile uint32_t native_adreno_ib_log[4][4 + 28];
volatile uint32_t native_adreno_ib_log_count;
volatile uint64_t native_adreno_import_diag[10];
volatile uint64_t native_adreno_import_fail_pa, native_adreno_import_fail_offset;
volatile uint32_t native_adreno_import_stage;
volatile uint32_t native_adreno_io_coherent;
volatile uint32_t native_adreno_map_fail[7][2];
volatile uint32_t native_adreno_method_stats[20][4];
volatile uint32_t native_adreno_pool_stats[8];
volatile uint32_t native_adreno_pool_status;
volatile uint32_t native_adreno_pool_wc;
volatile uint32_t native_adreno_ring_log[4][4 + 112];
volatile uint32_t native_adreno_ring_log_count;
volatile uint32_t native_adreno_view_diag[6];
volatile uint64_t native_adreno_vm_phase_us[2][8];
volatile uint64_t native_adreno_wait_outer[4];
volatile uint64_t native_gop_wait_stats[7];
volatile uint64_t native_gpu_idle_sleeps;
volatile uint64_t native_gpu_idle_spins;
/* NativeHidInput 28f9b46 registers its button callback with a board button driver (power, volume up/down). This board has none
 * yet; the kernel is linked statically, where an undefined weak reference does not link. */
void native_buttons_set_sink(void (*a)(void *, uint32_t), void *b) { (void)a;(void)b; }
volatile uint32_t native_touch_edge_stats[4];
volatile int32_t native_touch_extent[4];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_strokes[16][7];
#pragma clang attribute pop
