// XSAPI.h —— 内核导出函数声明 + 统一 JSON 入口
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/* 生命周期 */
void     dnf_scan_init(void);
void     dnf_scan_set_enabled(int v);
int      dnf_diag_bits(void);

/* 节拍 */
void     dnf_scan_set_interval_sec(double sec);
double   dnf_scan_get_interval_sec(void);
void     dnf_scan_set_interval_ms(double ms);
double   dnf_scan_get_interval_ms(void);
void     dnf_auto_on_set_ms(int ms);
int      dnf_auto_on_get_ms(void);
long long dnf_auto_on_remain_ms(void);
long long dnf_run_ms(void);
int      dnf_actual_gap_ms(void);
int      dnf_scan_count(void);

/* 四方式 */
void     dnf_use_click_set(int v);
int      dnf_use_click_get(void);
void     dnf_use_search_set(int v);
int      dnf_use_search_get(void);
void     dnf_sort_sync_set(int v);
int      dnf_sort_sync_get(void);
void     dnf_use_price_set(int v);
int      dnf_use_price_get(void);

/* 表头校准 */
void     dnf_snap_time(void);
void     dnf_snap_price(void);
long long dnf_get_field_time(void);
long long dnf_get_field_price(void);

/* 购买 */
void     dnf_buy_set(int v);
void     dnf_buy2_set(int v);

/* 限购 */
void     dnf_set_limit(int n);
void     dnf_limit_set(int v);
int      dnf_limit_hit(void);
int      dnf_limit_num(void);
int      dnf_attempt_count(void);

/* 统计 */
int      dnf_seen_count(void);
int      dnf_buy_count(void);
int      dnf_parse_cnt(void);
int      dnf_on_search_cnt(void);
int      dnf_rs_capture_cnt(void);
int      dnf_rs_detail_cnt(void);
int      dnf_rs_detail_valid(void);
int      dnf_rs_page_found(void);
void     dnf_rs_reset(void);

/* 其它 */
void     dnf_tip_set(int v);
void     dnf_log_set(int v);
int      dnf_log_get(void);
void     dnf_scan_set_delay_ms(int ms);      /* 新增: 定时开启(与 auto_on 同义, 便于面板直接传) */

/* ===== 统一 JSON 入口（H5GG 桥 与 内嵌面板 共用）=====
 * 入参: {"cmd":"...","data":...}
 * 出参: {"ok":true,"data":...} / {"ok":false,"err":"..."}
 * 返回的指针由调用方 copy 后 free（实现用 malloc）。
 */
char*    xs_api_call(const char* json);

/* 崩溃/进度日志（写到 Documents/xys/crash.log） */
void     xs_crash_note(const char* tag, const char* detail);

/* 启动 dylib 里的悬浮面板（UIKit + 内嵌 HTML） */
void     xs_panel_show(void);

#ifdef __cplusplus
}
#endif
