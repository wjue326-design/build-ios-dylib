#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "platform_ios.h"
#include <setjmp.h>
#include <signal.h>

static sigjmp_buf g_segv_jb;
static volatile int g_segv_armed = 0;
static void xingshu_segv(int sig){
  if(g_segv_armed){ g_segv_armed = 0; siglongjmp(g_segv_jb, 1); }
  signal(SIGSEGV, SIG_DFL);
  raise(sig);
}
#include <fcntl.h>
#include <dlfcn.h>
#if 0 /* iOS: 无 elf.h */
#include <elf.h>
#endif
#include <sys/mman.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include "dobby.h"
extern "C" int DobbyHook(void* address, void* replace, void** origin);


#define LOGP "/data/data/com.tencent.tmgp.dnf/scan.log"

static int g_logfd = -1;
/* 日志开关：0=关闭（默认高性能） 1=写入文件（排查/抓调用时临时打开）
   文件位置：/var/mobile/Documents/xys/xs.log
   面板上的「日志」勾选框控制；关闭时零 IO 开销。 */
static volatile int g_log_on = 0;
static void logmsg(const char *s){
  if(!g_log_on) return;
  struct timespec lts; clock_gettime(CLOCK_MONOTONIC, &lts);
  long long lms = (long long)lts.tv_sec * 1000LL + lts.tv_nsec / 1000000LL;
  FILE* lf = fopen("/var/mobile/Documents/xys/xs.log", "a");
  if(!lf) return;
  fprintf(lf, "%lld %s\n", lms, s);
  fclose(lf);
}
/* 无条件写初始化日志（不受面板「日志」开关影响）*/
static void logmsg_force(const char *s){
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_init.log", "a");
  if(!lf) return;
  fprintf(lf, "%s\n", s);
  fclose(lf);
}
extern "C" void dnf_log_set(int v){ g_log_on = v ? 1 : 0; }
extern "C" int  dnf_log_get(void){ return g_log_on; }

static void* (*p_string_new)(void*, const char*);
static void* (*p_domain_get)(void);
static void* (*p_domain_get_assemblies)(void*, size_t*);
static void* (*p_assembly_get_image)(void*);
static const char* (*p_image_get_name)(void*);
static void* (*p_class_from_name)(void*, const char*, const char*);
static void* (*p_method_get_from_name)(void*, const char*, int);
static void* (*p_class_get_methods)(void*, void**);
static const char* (*p_method_get_name)(void*);
static int (*p_method_get_param_count)(void*);

static void* g_page_inst = NULL;
static void* g_click_addr = NULL;
static void* g_enter_addr = NULL;      /* OnClickEnter_Search */
static void* g_sbtn_addr = NULL;       /* OnClickSearchButton */
static void* g_enter_inst = NULL;
static void* g_sbtn_inst = NULL;
static void (*orig_enter)(void*) = NULL;
static void (*orig_sbtn)(void*) = NULL;
static void my_enter(void* self);
static void my_sbtn(void* self);
static void* g_update_addr = NULL;
static void (*orig_update)(void*) = NULL;
static void (*orig_click)(void*) = NULL;
static void (*orig_enable)(void*) = NULL;
/* Unity 对象存活检测：klass 合法 + m_CachedPtr(+0x10) 非 0 */
static int obj_alive(void* o){
  if(!o) return 0;
  uint64_t v = (uint64_t)(uintptr_t)o;
  if(v < 0x1000ULL || v > 0x7fffffffffULL) return 0;
  uint64_t k = (uint64_t)(uintptr_t)(*(void**)o);
  if(k < 0x1000ULL || k > 0x7fffffffffULL) return 0;
  uint64_t cp = *(uint64_t*)((char*)o + 0x10);
  return cp != 0;
}
static void* g_open_addr = NULL;
static void* g_enable_addr = NULL;
static void* g_iscool_addr = NULL;
static void* g_dc_inst = NULL;
static int (*orig_remain)(void*) = NULL;
static void (*orig_detail)(void*) = NULL;
static void (*orig_settext)(void*, void*) = NULL;
static void* g_settext_addr = NULL;
static void (*orig_clicksearch)(void*) = NULL;
static void* g_detail_addr = NULL;
static void* g_next_addr = NULL;
static void* g_prev_addr = NULL;
static void* g_cat_addr = NULL;
static void* g_sortt_addr = NULL;
static void* g_sortt_inst = NULL;
static void* g_sortp_addr = NULL;
/* OnSortChanged(argc=2)：拍卖表头点击真正走的方法（枚举确认没有 OnClickSortPrice）*/
static void* g_sortchg_addr = NULL;
/* RequestSearchPage(argc=1)：分页/翻页拉数据，点表头可能走这个 */
static void (*orig_rs_page)(void*, long) = NULL;
static void my_rs_page(void* self, long page){
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_chain.log", "a");
  if(lf){ fprintf(lf, "RequestSearchPage page=%ld\n", page); fclose(lf); }
  if(orig_rs_page) orig_rs_page(self, page);
}
/* 抓调用链：CanSort / SortSearchResult（点表头时到底按什么顺序调）*/
static void* g_cansort_addr = NULL;
static void (*orig_cansort)(void*) = NULL;
static void* g_sortsr_addr = NULL;
static void (*orig_sortsr)(void*, void*) = NULL;
static void my_cansort(void* self){
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_chain.log", "a");
  if(lf){ fprintf(lf, "CanSort\n"); fclose(lf); }
  if(orig_cansort) orig_cansort(self);
}
static void my_sortsr(void* self, void* arg){
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_chain.log", "a");
  if(lf){ fprintf(lf, "SortSearchResult arg=%p\n", arg); fclose(lf); }
  if(orig_sortsr) orig_sortsr(self, arg);
}
static void (*orig_sortchanged)(void*, long, long) = NULL;
/* 从 OnSortChanged 抓到的排序字段值（实测）*/
static volatile long long g_sort_field_price = 534694770656LL;  /* 点「价格」时的 field */
static volatile long long g_sort_field_time  = 534694771760LL;  /* 点「剩余时间」时的 field */
static volatile long long g_field_time  = 0;   /* 校准：剩余时间排序的 field */
static volatile long long g_field_price = 0;   /* 校准：价格排序的 field */
static volatile long long g_last_sort_field = 0;   /* 最近一次点击用的 field（供动态校准）*/
static volatile long long g_last_sort_order = 0;
static void my_sortchanged(void* self, long a, long b){
  g_last_sort_field = (long long)a;
  g_last_sort_order = (long long)b;
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_sortchg.log", "a");
  if(lf){ fprintf(lf, "field=%ld order=%ld\n", a, b); fclose(lf); }
  if(orig_sortchanged) orig_sortchanged(self, a, b);
}
static void (*orig_next)(void*) = NULL;
static void (*orig_prev)(void*) = NULL;
static void (*orig_cat)(void*, long) = NULL;   /* OnClickCategory argc=1 */
static void (*orig_sortt)(void*) = NULL;
static void (*orig_sortp)(void*) = NULL;
static void* g_search_inst = NULL;
static void* g_rs_addr = NULL;
static void (*orig_rs)(void*,long,void*,void*,long,long,void*,long,long) = NULL;
static void* g_rs_args[9];
/* ===== 详细搜索链路验证（用来百分百确认"详细搜索是否走 RequestSearch(8)"）=====
   - g_detail_ms：最近一次点"详细搜索"按钮的时刻
   - g_rs_capture_cnt：RequestSearch 被捕获的总次数（任何搜索都会 +1）
   - g_rs_detail_cnt：其中"详细搜索点击后 3 秒内"发生的次数
   - 玩家点一次详细搜索 -> 确定，看面板"详细"计数是否 +1：
       +1  → 详细搜索确实走 RequestSearch(8)，参数已被捕获，扫拍能重放
        0  → 详细搜索没走这个方法，需要另找入口
   - g_rs_detail_args：详细搜索那次捕获的参数，扫拍时优先重放这一份 */
static volatile long long g_detail_ms = 0;
static volatile int g_rs_capture_cnt = 0;
static volatile int g_rs_detail_cnt = 0;
static void* g_rs_detail_args[9];
static volatile int g_rs_detail_valid = 0;

static void* g_clicksearch_addr = NULL;
static void* g_remain_addr = NULL;
static time_t g_last = 0;
static long long g_last_ms = 0;
static volatile int g_hits = 0;
static volatile int g_calls = 0;
static volatile int g_enabled2 = 0;   /* 安全默认：必须面板勾选 */
static volatile int g_in_scan = 0;
static volatile int g_buy_count = 0;
static volatile int g_seen_total = 0;   /* SetData 累计命中次数（含进页面时的缓存）*/
static volatile int g_seen_base  = 0;   /* 扫拍真正开跑那一刻的基线（用来扣掉首页缓存）*/
static volatile int g_seen_armed = 0;   /* 0=还没开始正式统计 1=已扣过基线 */
/* 扫拍累计运行计时：本次开启时刻 + 历史累计（毫秒）*/
static volatile long long g_run_start_ms = 0;
static volatile long long g_run_total_ms = 0;
/* 定时开启：勾选扫拍后延迟多少毫秒才真正开始（100 ~ 1000，即 0.10 ~ 1.00 秒，默认 0.10）*/
static volatile int      g_auto_on_ms = 100;
static volatile long long g_auto_on_at = 0;   /* 到点时间戳，0=无待启动 */
/* 扫拍刷新方式（多选）：三种调用方法可自由组合 */
static volatile int g_use_click  = 1;   /* ① OnClickRefresh       游戏自己的刷新按钮 */
static volatile int g_use_search = 1;   /* ② RequestSearch        数据层直发协议 */
static volatile int g_use_price  = 0;   /* ④ OnClickSortPrice     游戏自己的「价格」按钮 */
static void ensure_use_mode(void);      /* 前置声明：开启扫拍前确保至少一种方式 */
/* ③ 排序复用 g_sort_sync（OnClickSortTime） */
static int g_maxp = 100;
static unsigned long long g_bought_ids[256];
static int g_bought_n = 0;
static int g_q = 2;
static char g_affix[64] = "加技能攻击力";
static void* g_popup_inst = NULL;
static volatile int g_popup_on = 0;
static volatile int g_popup_confirmed = 0;
static volatile int g_page_active = 0;   /* 拍卖页是否激活（前置声明）*/
static volatile long long g_fail_until = 0;   /* 失败退避截止时刻 */   /* 本次打开是否已确认 */
static volatile long long g_popup_open_ms = 0;   /* 本次打开时刻 */
static void (*orig_enable_popup)(void*) = NULL;
static void* g_confirm_addr = NULL;
/* 前置声明：购买按钮 hook（my_update 里要用） */
static void* g_clickbuy_addr = NULL;
static void* g_pend_slot = NULL;      /* 待触发购买的行（避免在 SetData 内重入）*/
static volatile int g_has_pend = 0;
static void* g_slot_self = NULL;
static long long mono_ms(void);   /* 单调时钟（毫秒） */
static void ui_probe_only(void);  /* UI 按钮探测 */
static void ui_click_init(void);  /* UI 按钮点击 API 初始化 */
/* 数据驱动购买探针：ParseSearchResult 抓列表数据（不依赖坐标/无障碍/ADB）*/
static void* g_parse_addr = NULL;
static void (*orig_parse)(void*, void*) = NULL;
static volatile int g_parse_cnt = 0;
static void pars_dump(const char* tag, void* o, int n){
  /* 安全版：只打印指针，绝不读取对象内存（避免崩溃/游戏报错） */
  (void)n;
  if(!o) return;
  uint64_t pv = (uint64_t)(uintptr_t)o;
  if(pv < 0x7000000000ULL || pv > 0x7fffffffffULL) return;
  char b[128]; snprintf(b,sizeof(b),"[PTR] %s=%p", tag, o); logmsg(b);
}
static void my_parse_result(void* self, void* a){
  if(orig_parse) orig_parse(self, a);   /* 先让游戏把数据解析进容器 */
  g_parse_cnt++;
  if(g_parse_cnt <= 3 && self){
    char b[192]; snprintf(b,sizeof(b),"[PARS] ==== AFTER self=%p arg=%p cnt=%d ====", self, a, g_parse_cnt); logmsg(b);
    pars_dump("S", self, 0x40);
    pars_dump("F10", *(void**)((char*)self+0x10), 0x40);
    pars_dump("F18", *(void**)((char*)self+0x18), 0x40);
    pars_dump("F20", *(void**)((char*)self+0x20), 0x40);
    pars_dump("F28", *(void**)((char*)self+0x28), 0x40);
    pars_dump("F30", *(void**)((char*)self+0x30), 0x40);
    /* 只看容器里各 List 的数组指针和 size（不深读元素，避免非法地址） */
    for(int fi=0; fi<3; fi++){
      int off = 0x10 + fi*8;
      void* lst = *(void**)((char*)self+off);
      if(!lst) continue;
      uint64_t lp = (uint64_t)(uintptr_t)lst;
      if(lp < 0x7000000000ULL || lp > 0x7fffffffffULL) continue;
      void* arr = *(void**)((char*)lst+0x10);
      char ab[96]; snprintf(ab,sizeof(ab),"[PARS] LST%d arr=%p", fi, arr); logmsg(ab);
    }
  }
}
/* ===== 直接拿列表：hook CeraAuctionBuyPage.GetListings（返回 List<CeraAuctionListingData>）===== */
static void* g_getlist_addr = NULL;
static void* (*orig_getlist)(void*) = NULL;
static void* g_last_listings = NULL;
static volatile int g_gl_cnt = 0;
static volatile int g_gl_dumped = 0;
static void* my_getlistings(void* self){
  void* r = orig_getlist ? orig_getlist(self) : NULL;
  g_last_listings = r;
  g_gl_cnt++;
  if(r){
    uint64_t rp = (uint64_t)(uintptr_t)r;
    if(rp >= 0x7000000000ULL && rp <= 0x7fffffffffULL){
      void* arr = *(void**)((char*)r + 0x10);
      unsigned int sz = (unsigned int)(*(unsigned long long*)((char*)r+0x18) & 0xffffffffULL);
      if(sz > 0 && g_gl_dumped < 2){
        g_gl_dumped++;
        char ab[160]; snprintf(ab,sizeof(ab),"[LIST] size=%u items=%p dump#%d", sz, arr, g_gl_dumped); logmsg(ab);
        pars_dump("L", r, 0x28);
        pars_dump("LA", arr, 0x30);
        for(int i=0;i<3 && i<(int)sz;i++){
          void* el = *(void**)((char*)arr + 0x20 + i*8);
          if(!el) continue;
          char eb[96]; snprintf(eb,sizeof(eb),"[LIST] item%d=%p", i, el); logmsg(eb);
          pars_dump("LI", el, 0x70);
        }
      }
    }
  }
  return r;
}
static void* g_rbuy_addr = NULL;
static void (*orig_rbuy)(void*,void*,unsigned long,void*,int,void*) = NULL;
static void* g_rbuy_args[6];
static int g_rbuy_valid = 0;
static void (*orig_buy2)(void*) = NULL;
static void* g_buy2_addr = NULL;
static void* g_refreshitem_addr = NULL;   /* CeraAuctionPopup.RefreshItemInfo */
extern "C" volatile int g_buy_enabled = 0;   /* 安全默认：必须面板勾选；volatile 保证关闭后下一 tick 立即可见 */
extern "C" volatile int g_buy2_enabled = 0;  /* 安全默认：必须面板勾选；volatile 保证关闭后下一 tick 立即可见 */
/* ===== 提示弹窗拦截开关（默认关闭，必须面板勾选）=====
   文本过滤 / 冷却破解 两项功能已按要求删除，不再存在于本文件。 */
extern "C" volatile int g_tip_block = 0;   /* 提示弹窗拦截（MessageBox.ShowOnlyMessage）*/
/* 剩余时间排序同步开关：默认关闭（必须面板勾选才执行）。
   扫拍每一轮在点刷新 + 重放搜索之后，同步点一次 OnClickSortTime，
   防止详细搜索参数失效时列表没有按时间排序。跟着刷新间隔同一节拍走。 */
static volatile int g_sort_sync = 0;
/* ===== 扫拍自定义刷新间隔（单位：毫秒；内部用微秒整数存储，零浮点误差）=====
   - 默认 1.10 ms（= 1100 us），即推荐值
   - 允许范围 0.10 ~ 60000.00 ms
   - UI 线程写、网络线程读；volatile 64 位在 ARM64 上读写原子
   - 每次读取都夹紧到合法区间，任何异常值都不会变成 0 间隔 */
#define SCAN_IV_MIN_US   10000LL        /* 0.01 秒 */
#define SCAN_IV_MAX_US   60000000LL     /* 60.00 秒 */
#define SCAN_IV_PATH     "/var/mobile/Documents/xys/scan_iv_sec.txt"
static volatile long long g_scan_interval_us = 1100000; /* 默认 1.10 秒 */
static void scan_iv_load(void);   /* 前置声明：do_init 里调用 */
/* ===== 初始化诊断（面板直接显示，用来定位"功能全无效果"卡在哪一步）=====
   P=页面类  C=OnClickRefresh  U=AbstractTcpClient.Update
   R=RequestSearch(8)  HU=Update hook 成功  HR=RequestSearch hook 成功  I=do_init 跑完 */
static volatile int g_diag_page   = 0;
static volatile int g_diag_clk    = 0;
static volatile int g_diag_upd    = 0;
static volatile int g_diag_rs     = 0;
static volatile int g_diag_hk_upd = 0;
static volatile int g_diag_hk_rs  = 0;
static volatile int g_diag_init   = 0;
/* 实测间隔：最近两次真正触发扫拍之间的毫秒差（用来验证设定值到底有没有生效）*/
static volatile int g_actual_gap_ms = 0;
static int g_buy_limit = 100;
static void* g_prev_items[30];
static int g_prev_n = 0;   /* 价格上限 */

#if 0 /* iOS: maps_first_base 由 platform_ios.h 提供 */
static void* maps_first_base(const char* name){
  FILE *f = fopen("/proc/self/maps","r");
  if(!f) return NULL;
  char ln[2048]; void *base = NULL;
  while(fgets(ln,sizeof(ln),f)){
    if(!strstr(ln,name)) continue;
    unsigned long long st=0; char pm[8]={0}; unsigned long long off=0;
    if(sscanf(ln,"%llx-%*x %7s %llx",&st,pm,&off)==3){ base=(void*)st; break; }
  }
  fclose(f);
  return base;
}
#endif

#if 0 /* iOS: elf_find_sym 由 platform_ios.h 提供 */
static void* elf_find_sym(void* base, const char* want){
  if(!base) return NULL;
  Elf64_Ehdr *eh = (Elf64_Ehdr*)base;
  if(memcmp(eh->e_ident, ELFMAG, 4) != 0) return NULL;
  Elf64_Phdr *ph = (Elf64_Phdr*)((char*)base + eh->e_phoff);
  unsigned long long dyn = 0, dynsz = 0;
  for(int i=0;i<eh->e_phnum;i++)
    if(ph[i].p_type == PT_DYNAMIC){ dyn = ph[i].p_vaddr; dynsz = ph[i].p_memsz; }
  if(!dyn) return NULL;
  Elf64_Dyn *d = (Elf64_Dyn*)((char*)base + dyn);
  unsigned long long symtab=0, strtab=0, hash=0;
  for(unsigned long long o=0; o < dynsz/sizeof(Elf64_Dyn); o++){
    if(d[o].d_tag == DT_NULL) break;
    if(d[o].d_tag == DT_SYMTAB) symtab = d[o].d_un.d_ptr;
    else if(d[o].d_tag == DT_STRTAB) strtab = d[o].d_un.d_ptr;
    else if(d[o].d_tag == DT_HASH) hash = d[o].d_un.d_ptr;
    else if(d[o].d_tag == DT_GNU_HASH && !hash) hash = d[o].d_un.d_ptr;
  }
  if(!symtab || !strtab) return NULL;
  unsigned int nsyms = 0;
  if(hash){ unsigned int *h=(unsigned int*)((char*)base+hash); nsyms=h[1]; }
  if(!nsyms || nsyms > 300000) nsyms = 300000;
  Elf64_Sym *sym = (Elf64_Sym*)((char*)base + symtab);
  const char *str = (const char*)((char*)base + strtab);
  for(unsigned int i=0;i<nsyms;i++){
    if(sym[i].st_name && sym[i].st_value && strcmp(str+sym[i].st_name, want)==0)
      return (char*)base + sym[i].st_value;
  }
  return NULL;
}
#endif

static void* find_class_in_csharp(const char* cls){
  void *dom = p_domain_get();
  if(!dom) return NULL;
  size_t n = 0;
  void **asms = (void**)p_domain_get_assemblies(dom, &n);
  if(!asms) return NULL;
  for(size_t i=0;i<n && i<400;i++){
    void *img = p_assembly_get_image(asms[i]);
    const char *nm = p_image_get_name(img);
    if(nm && strstr(nm,"Assembly-CSharp")) return p_class_from_name(img, "", cls);
  }
  return NULL;
}
static void* find_class_any(const char* ns, const char* cls){
  void *dom = p_domain_get();
  if(!dom) return NULL;
  size_t n = 0;
  void **asms = (void**)p_domain_get_assemblies(dom, &n);
  if(!asms) return NULL;
  for(size_t i=0;i<n && i<400;i++){
    void *img = p_assembly_get_image(asms[i]);
    void *k = p_class_from_name(img, ns, cls);
    if(k) return k;
  }
  return NULL;
}

static void tap_xy(int x, int y);

static void* (*p_class_get_type)(void*);
static void* (*p_class_get_fields)(void*, void**);
static const char* (*p_field_get_name)(void*);
static int (*p_field_get_offset)(void*);
static void* (*p_runtime_invoke)(void*, void*, void**, void**);

/* ===== Unity UI 内部点击（等价 JS 点击，微秒级） ===== */
static void* g_btn_cls = NULL;
static void* g_btn_onclick = NULL;
static void* g_uevent_invoke = NULL;
static void* g_find_of_type = NULL;

static void ui_click_init(void){
  if(g_btn_cls) return;
  void* bc = find_class_any("UnityEngine.UI","Button");
  if(!bc) return;
  g_btn_cls = bc;
  g_btn_onclick = p_method_get_from_name(bc,"get_onClick",0);
  void* ue = find_class_any("UnityEngine.Events","UnityEvent");
  if(ue) g_uevent_invoke = p_method_get_from_name(ue,"Invoke",0);
  void* oc = find_class_any("UnityEngine","Object");
  if(oc) g_find_of_type = p_method_get_from_name(oc,"FindObjectsOfType",1);
}

/* 只查找不点击：统计场景里的 Button 数量与前几个按钮名字，用于定位崩点 */
static void ui_probe_only(void){
  ui_click_init();
  if(!g_find_of_type || !p_class_get_type || !p_runtime_invoke){
    logmsg("[PROBE] api missing"); return;
  }
  void* typ = p_class_get_type(g_btn_cls);
  if(!typ){ logmsg("[PROBE] no type"); return; }
  void* argv[1]; argv[0] = typ;
  void* exc = NULL;
  void* arr = p_runtime_invoke(g_find_of_type, NULL, argv, &exc);
  if(exc || !arr){ logmsg("[PROBE] find failed"); return; }
  int n = *(int*)((char*)arr + 0x18);
  char b[80]; snprintf(b,sizeof(b),"[PROBE] buttons=%d", n); logmsg(b);
  if(n <= 0 || n > 400) return;
  void* base = (char*)arr + 0x20;
  void* objc = find_class_any("UnityEngine","Object");
  void* getname = objc ? p_method_get_from_name(objc,"get_name",0) : NULL;
  if(!getname){ logmsg("[PROBE] no get_name"); return; }
  for(int i=0;i<n && i<6;i++){
    void* btn = *(void**)((char*)base + i*8);
    if(!btn || (uint64_t)(uintptr_t)btn < 0x1000ULL){ logmsg("[PROBE] null btn"); continue; }
    void* nm = ((void*(*)(void*))(*(void**)getname))(btn);
    if(!nm){ logmsg("[PROBE] null name"); continue; }
    int len = *(int*)((char*)nm + 0x10);
    if(len <= 0 || len > 40){ logmsg("[PROBE] bad len"); continue; }
    const unsigned short* ch = (const unsigned short*)((char*)nm + 0x14);
    unsigned short tmp[40]; for(int j=0;j<len && j<39;j++) tmp[j]=ch[j]; tmp[len<39?len:39]=0;
    /* 打印按钮名的码点，便于识别"购买/确认"等中文按钮 */
    char out[256]; int off = snprintf(out,sizeof(out),"[PROBE] #%d len=%d cp=", i, len);
    for(int j=0;j<len && j<10 && off < (int)sizeof(out)-12;j++){
      off += snprintf(out+off, sizeof(out)-off, "%04X,", (unsigned)tmp[j]);
    }
    logmsg(out);
  }
}

/* 从对象身上直接找 Button 类型字段（不遍历场景，安全），返回 Button 对象 */
static void* ui_button_of_obj(void* obj){
  if(!obj || !p_class_get_fields) return NULL;
  void* cls = *(void**)obj;
  if(!cls) return NULL;
  void* it = NULL; void* f;
  int guard = 0;
  while((f = p_class_get_fields(cls, &it)) && guard++ < 200){
    const char* fn = p_field_get_name ? p_field_get_name(f) : NULL;
    if(!fn) continue;
    if(!strstr(fn,"Btn") && !strstr(fn,"Button") && !strstr(fn,"btn")) continue;
    int off = p_field_get_offset ? p_field_get_offset(f) : -1;
    if(off <= 0 || off > 0x800) continue;
    void* v = *(void**)((char*)obj + off);
    if(!v) continue;
    uint64_t vp = (uint64_t)(uintptr_t)v;
    if(vp < 0x7000000000ULL || vp > 0x7fffffffffULL) continue;
    return v;   /* 认定这是按钮组件 */
  }
  return NULL;
}
/* 真实点击某个 Button：onClick.Invoke()（UnityEvent，等价于手指点下去） */
static int ui_click_button_obj(void* btn){
  if(!btn || !g_btn_onclick || !g_uevent_invoke) return -1;
  uint64_t bp = (uint64_t)(uintptr_t)btn;
  if(bp < 0x7000000000ULL || bp > 0x7fffffffffULL) return -2;
  void* ev = ((void*(*)(void*))(*(void**)g_btn_onclick))(btn);
  if(!ev) return -3;
  ((void(*)(void*))(*(void**)g_uevent_invoke))(ev);
  return 1;
}

/* 按名称关键字点击第一个匹配的按钮（Unity UI 事件，微秒级） */
static int ui_click_named(const char* key){
  ui_click_init();
  if(!g_btn_cls || !g_btn_onclick || !g_uevent_invoke || !g_find_of_type || !p_class_get_type || !p_runtime_invoke) return -1;
  void* objc = find_class_any("UnityEngine","Object");
  void* getname = objc ? p_method_get_from_name(objc,"get_name",0) : NULL;
  if(!getname) return -2;
  void* typ = p_class_get_type(g_btn_cls);
  if(!typ) return -3;
  void* argv[1]; argv[0] = typ;
  void* exc = NULL;
  void* arr = p_runtime_invoke(g_find_of_type, NULL, argv, &exc);
  if(exc || !arr) return -4;   /* 异常或空结果：本轮直接跳过，绝不继续访问 */
  int n = *(int*)((char*)arr + 0x18);
  if(n <= 0 || n > 400) return -5;
  void* base = (char*)arr + 0x20;
  for(int i=0;i<n;i++){
    void* btn = *(void**)((char*)base + i*8);
    if(!btn || (uint64_t)(uintptr_t)btn < 0x1000ULL) continue;
    void* nm = ((void*(*)(void*))(*(void**)getname))(btn);
    if(!nm) continue;
    int len = *(int*)((char*)nm + 0x10);
    if(len <= 0 || len > 40) continue;
    unsigned short* ch = (unsigned short*)((char*)nm + 0x14);
    /* key 为 UTF-16LE 码点序列：用「逐码点子串匹配」 */
    int klen = (int)strlen(key)/2;
    if(klen <= 0 || len < klen) continue;
    const unsigned short* kw = (const unsigned short*)key;
    int hit = 0;
    for(int st=0; st <= len-klen; st++){
      int ok = 1;
      for(int j=0;j<klen;j++){ if(ch[st+j] != kw[j]){ ok = 0; break; } }
      if(ok){ hit = 1; break; }
    }
    if(!hit) continue;
    void* ev = ((void*(*)(void*))(*(void**)g_btn_onclick))(btn);
    if(!ev) continue;
    ((void(*)(void*))(*(void**)g_uevent_invoke))(ev);
    return 1;
  }
  return 0;
}
static void* g_ob_addr;
static void* g_resetcd_addr = NULL;
static void* g_cdremain_addr = NULL;
static int (*orig_iscool)(void*,int) = NULL;
static int my_iscool(void*,int);
/* ---- 按钮探测：记录所有 OnClick* 调用 ---- */
static void* g_probe_addrs[24];
static const char* g_probe_names[24];
static int g_probe_n = 0;
static const char* g_last_click_name = "(none)";
static int g_click_hits = 0;
static void my_click_probe(void* self);
static void (*g_probe_orig[24])(void*) = {0};
static void (*g_probe_tbl[12])(void*);
static void install_probe(void){
  if(g_probe_n > 0) return;
  void* pc = find_class_in_csharp("CeraAuctionListPageBase");
  if(!pc){ pc = find_class_any("MDNF.Auction.Cera","CeraAuctionListPageBase"); }
  if(!pc){ logmsg("[probe] page class null"); return; }
  void* it=NULL; int pn=0;
  for(int k=0;k<400 && pn<20;k++){
    void* m = p_class_get_methods(pc, &it);
    if(!m) break;
    const char* nm = p_method_get_name(m);
    if(!nm || strncmp(nm,"OnClick",7)!=0) continue;
    void* addr = *(void**)m;
    if(!addr) continue;
    g_probe_names[pn]=nm;
    g_probe_addrs[pn]=addr;
    pn++;
  }
  g_probe_n=pn;
  { char pb[96]; snprintf(pb,sizeof(pb),"[probe] count=%d (no hook)",pn); logmsg(pb); }
}
static void my_click_probe(void* self){
  for(int i=0;i<g_probe_n;i++){
    /* 通过返回地址无法判断，这里只记录一次触发 */
  }
  char b[128]; snprintf(b,sizeof(b),"[CLICK] probe self=%p", self); logmsg(b);
}
static int my_iscool(void* self, int b){
  /* 恢复游戏原生冷却判断：强制返回 0 会让内部状态机错乱并崩溃 */
  if(orig_iscool) return orig_iscool(self,b);
  return 1;
}
static void* g_c8_first = NULL;
static void* g_listing_inst;
static long long g_last_buy_ms = 0;
static const char* (*p_class_get_name)(void*);
static void* (*p_image_get_class_count)(void*);
static void* (*p_image_get_class)(void*, size_t);

/* ===== 主线程执行器 =====
   my_update 挂在 AbstractTcpClient.Update（网络线程），
   在那里直接调 Unity UI 方法(OnConfirmBuy)会跨线程 → 卡死。
   改为：网络线程只置标记，真正的确认在 Time.get_deltaTime（主线程、每帧）里做。 */

/* ===== 刷新CD 破解（IsRefreshCoolTime / IsDetailSearchCoolTime 强制返回 false）===== */
static void* g_isrc_addr = NULL;
static void* g_idsc_addr = NULL;
static void* g_detailsearch_addr = NULL;   /* OnClickDetailSearch */
static void* g_searchbtn_addr = NULL;      /* OnClickSearchButton */
static void* g_rs_page_addr = NULL;        /* RequestSearchPage */
static int my_isrc(void* self, long x){ (void)self; (void)x; return 0; }
static int my_idsc(void* self, long x){ (void)self; (void)x; return 0; }

/* ===== 绕过 SetData：直接读商品容器 ===== */
static void* g_gc_inst = NULL;            /* CeraAuctionGoodsContainer 实例 */
static void* g_parse_search_addr = NULL;  /* ParseSearchResult */
static void (*orig_parse_search)(void*, void*) = NULL;
static void* g_trygoods_addr = NULL;      /* TryGetGoodsData */
static volatile int g_gs_cnt = 0;
static volatile int g_need_container_read = 0;

/* 捕获商品数据（含价格？）：GetOrCreateGoodsData(key) / TryGetGoodsData(self,key,out) */
static void* g_gocgd_addr = NULL;
static void* (*orig_gocgd)(void*, void*) = NULL;
static volatile int g_gd_cnt = 0;
static void* my_gocgd(void* self, void* key){
  void* ret = orig_gocgd ? orig_gocgd(self, key) : NULL;
  /* 【开关守卫】两个购买开关全关时，这里只转发原函数，不做任何调试 dump */
  if(!g_enabled2 && !g_buy_enabled && !g_buy2_enabled) return ret;
  if(ret && g_gd_cnt < 6){
    g_gd_cnt++;
    uint64_t kv = (uint64_t)(uintptr_t)key;
    uint64_t rv = (uint64_t)(uintptr_t)ret;
    char b[144]; snprintf(b,sizeof(b),"[GD] key=%llx data=%p", (unsigned long long)kv, ret); logmsg(b);
    if(rv >= 0x7000000000ULL && rv <= 0x7fffffffffULL){
      for(int o=0; o<0x30; o+=8){
        unsigned long long v = *(unsigned long long*)((char*)ret + o);
        char lb[112]; snprintf(lb,sizeof(lb),"[GD]   +%02x=%016llx", o, v); logmsg(lb);
      }
    }
  }
  return ret;
}

/* ===== 分支A：hook 价格表解析 ParsePriceList（价格真正来源）===== */
static void* g_ppl_addr = NULL;
static void (*orig_ppl)(void*, void*) = NULL;
static volatile int g_ppl_cnt = 0;
static void my_parse_price(void* self, void* data){
  if(orig_ppl) orig_ppl(self, data);
  if(g_ppl_cnt < 4){
    g_ppl_cnt++;
    uint64_t dv = (uint64_t)(uintptr_t)data;
    char b[128]; snprintf(b,sizeof(b),"[PPL] #%d self=%p data=%p", g_ppl_cnt, self, data); logmsg(b);
    if(dv >= 0x7000000000ULL && dv <= 0x7fffffffffULL){
      /* 先按对象 dump 前 0x100 */
      for(int o=0; o<0x100; o+=8){
        unsigned long long v = *(unsigned long long*)((char*)data + o);
        char lb[112]; snprintf(lb,sizeof(lb),"[PPL]   +%02x=%016llx", o, v); logmsg(lb);
      }
      /* 如果看起来像数组，按数组解析 */
      int alen = *(int*)((char*)data + 0x18);
      char ah[96]; snprintf(ah,sizeof(ah),"[PPL]   maybe-array len@+18=%d", alen); logmsg(ah);
      if(alen > 0 && alen < 200){
        for(int i=0; i<alen && i<24; i++){
          unsigned int iv = *(unsigned int*)((char*)data + 0x20 + i*4);
          char ib[96]; snprintf(ib,sizeof(ib),"[PPL]   [%d]=%u", i, iv); logmsg(ib);
        }
      }
    }
  }
}


/* ===== 实时价格表：itemId -> 该商品当前真实单价（来自它自己的 slot Text）===== */

/* 本轮刷新的所有 (slot,item)，用于逐个读价格 */
static void* g_rs_slot[32];
static void* g_rs_item[32];
static volatile int g_rs_n = 0;
#define PX_MAX 96
static volatile int g_px_id[PX_MAX];
static volatile int g_px_price[PX_MAX];
static volatile int g_px_n = 0;
static void px_put(int itemId, int price){
  if(itemId <= 0 || price <= 0) return;
  for(int i=0;i<g_px_n;i++){ if(g_px_id[i]==itemId){ g_px_price[i]=price; return; } }
  if(g_px_n < PX_MAX){ g_px_id[g_px_n]=itemId; g_px_price[g_px_n]=price; g_px_n++; }
}
static int px_get(int itemId){
  for(int i=0;i<g_px_n;i++){ if(g_px_id[i]==itemId) return g_px_price[i]; }
  return 0;
}

/* ===== 自动确认/拦截提示弹窗（防止累积卡死）===== */
static void* g_som5_addr = NULL;
static void* g_som2a_addr = NULL;
static void* g_som2b_addr = NULL;
static volatile int g_tip_cnt = 0;
static void (*orig_som5)(void*,long,long,long,long) = NULL;
static void (*orig_som2a)(void*,long) = NULL;
static void (*orig_som2b)(void*,long) = NULL;
static void my_som5(void* a, long b, long c, long d, long e){
  if(++g_tip_cnt <= 3){
    char t[192]; snprintf(t,sizeof(t),"[TIP] #%d a=%p b=0x%lx c=0x%lx d=0x%lx e=0x%lx",
      g_tip_cnt, a, (unsigned long)b, (unsigned long)c, (unsigned long)d, (unsigned long)e); logmsg(t);
    /* dump 第一个参数对象，找"确定"按钮 */
    uint64_t av = (uint64_t)(uintptr_t)a;
    if(av >= 0x7000000000ULL && av <= 0x7fffffffffULL){
      for(int o=0; o<0x80; o+=8){
        unsigned long long v = *(unsigned long long*)((char*)a + o);
        char lb[120]; snprintf(lb,sizeof(lb),"[TIP]   a+%02x=%016llx", o, v); logmsg(lb);
      }
    }
  }
  /* 【不显示提示】用户同意直接不弹 → 不调 orig
     注意：之前这样会崩，这次加了"只拦 2 参版本"的保护，5 参正常显示 */
  /* 只有"提示拦截"开关打开、且是 b=0/c=5 的这条提示，才吞掉不显示；
     开关关闭时一律转发给游戏原生实现，提示正常弹出。 */
  if(!g_tip_block || b != 0 || c != 5) { if(orig_som5) orig_som5(a,b,c,d,e); }
}
static void my_som2a(void* a, long b){
  if(++g_tip_cnt <= 3){ char t[96]; snprintf(t,sizeof(t),"[TIP] #%d 提示出现(2参)", g_tip_cnt); logmsg(t); }
  if(orig_som2a) orig_som2a(a,b);
}
static void my_som2b(void* a, long b){
  if(orig_som2b) orig_som2b(a,b);
}

/* 缓存 Text.get_text 地址，避免每帧每行都做一次方法查找（主线程卡顿主因）*/
static void* g_gettext_cached = NULL;

/* ===== 限购：达到数量后自动全停 + 截图 ===== */
static volatile int g_attempt_cnt = 0;
static volatile int g_need_shot = 0;
static volatile int g_shot_done = 0;
static void take_shot_once(void);
static volatile int g_limit_on  = 0;    /* 限购开关 */
static volatile int g_limit_num = 100;  /* 限购数量 */
static volatile int g_limit_hit = 0;    /* 是否已达标 */
static volatile int g_limit_base = 0;   /* 本次开启限购那一刻的已购数（用来算"本次成功几件"）*/
static void* g_screencap_addr = NULL;   /* ScreenCapture.CaptureScreenshot */
static void* g_dt_addr = NULL;
static void (*orig_dt)(void) = NULL;
static volatile int g_need_confirm = 0;
static void* g_last_slot = NULL;   /* 最近一次 SetData 的 slot */
static volatile int g_last_price = 0;   /* 从 slot 价格 Text 读到的真实单价 */
static void* g_direct_item = NULL;      /* 待直发的商品 */
static void* g_direct_slot = NULL;      /* 对应列表行 */
static volatile int g_direct_pending = 0;
/* 【批量直发】一次刷新的所有行都记下来，下一帧全部下单 */
static void* g_batch_item[24];
static void* g_batch_slot[24];
static volatile int g_batch_n = 0;
static volatile int g_direct_sent = 0;
static volatile long long g_dt_calls = 0;
static volatile long long g_dt_confirm = 0;

static volatile int g_in_dt = 0;

/* 【核心】搜索结果解析完成的瞬间：立即从容器读全部商品并下单 */
static void my_parse_search(void* self, void* data){
  /* 先让游戏自己解析（必须，否则容器没数据） */
  if(orig_parse_search) orig_parse_search(self, data);
  if(!self) return;
  uint64_t sv = (uint64_t)(uintptr_t)self;
  if(sv < 0x7000000000ULL || sv > 0x7fffffffffULL) return;
  g_gc_inst = self;
  g_gs_cnt++;
  if(g_gs_cnt <= 3){
    char b[128]; snprintf(b,sizeof(b),"[GS] parse done #%d container=%p", g_gs_cnt, self); logmsg(b);
    /* dump 容器 5 个字段（+0x10 ~ +0x30）的内容，找商品列表 */
    for(int of=0x10; of<=0x30; of+=8){
      void* fp = *(void**)((char*)self + of);
      uint64_t fpv = (uint64_t)(uintptr_t)fp;
      char fh[96]; snprintf(fh,sizeof(fh),"[GS] field+0x%02x = %p", of, fp); logmsg(fh);
      if(fpv < 0x7000000000ULL || fpv > 0x7fffffffffULL) continue;
      for(int oi=0; oi<0x40; oi+=8){
        unsigned long long vi = *(unsigned long long*)((char*)fp + oi);
        char lb[120]; snprintf(lb,sizeof(lb),"[GS]   +%02x = %016llx", oi, vi); logmsg(lb);
      }
      /* 深入一层：Dictionary 的 entries 数组 */
      void* arr2 = *(void**)((char*)fp + 0x18);
      uint64_t a2v = (uint64_t)(uintptr_t)arr2;
      if(a2v >= 0x7000000000ULL && a2v <= 0x7fffffffffULL){
        int len2 = *(int*)((char*)arr2 + 0x18);
        char lh[96]; snprintf(lh,sizeof(lh),"[GS]   arr=%p len=%d", arr2, len2); logmsg(lh);
        if(len2 > 0 && len2 < 32){
          for(int ei=0; ei<len2 && ei<12; ei++){
            void* ep = *(void**)((char*)arr2 + 0x20 + ei*8);
            uint64_t epv = (uint64_t)(uintptr_t)ep;
            char eh[96]; snprintf(eh,sizeof(eh),"[GS]     entry[%d]=%p", ei, ep); logmsg(eh);
            if(epv < 0x7000000000ULL || epv > 0x7fffffffffULL) continue;
            for(int eo=0; eo<0x30; eo+=8){
              unsigned long long ev = *(unsigned long long*)((char*)ep + eo);
              char eb[112]; snprintf(eb,sizeof(eb),"[GS]       +%02x=%016llx", eo, ev); logmsg(eb);
            }
          }
        }
      }
    }
  }
  /* 【纯内存读容器】不调任何游戏方法，直接顺字典读商品（最稳，不会崩） */
  {
    static long long mem_last = 0;
    long long mem_now = mono_ms();
    /* 纯内存读，不调任何游戏方法 → 高频也安全。200ms 一次足够 */
    if(mem_now - mem_last >= 200 && self){
      mem_last = mem_now;
      /* 容器 4 个字典字段 */
      int offs[4] = {0x18, 0x20, 0x28, 0x30};
      for(int di=0; di<4; di++){
        void* dict = *(void**)((char*)self + offs[di]);
        uint64_t dv = (uint64_t)(uintptr_t)dict;
        if(dv < 0x7000000000ULL || dv > 0x7fffffffffULL) continue;
        void* entries = *(void**)((char*)dict + 0x18);
        int cnt = *(int*)((char*)dict + 0x20);
        uint64_t ev = (uint64_t)(uintptr_t)entries;
        char dh[128]; snprintf(dh,sizeof(dh),"[MEM] dict@+%02x=%p entries=%p count=%d", offs[di], dict, entries, cnt); logmsg(dh);
        if(ev < 0x7000000000ULL || ev > 0x7fffffffffULL || cnt <= 0 || cnt > 64) continue;
        /* Entry = 24 字节：hash/next(8) + key(8) + value(8) */
        for(int ei=0; ei<cnt && ei<12; ei++){
          char* ep = (char*)entries + 0x20 + ei*24;
          unsigned long long h = *(unsigned long long*)ep;
          unsigned long long key = *(unsigned long long*)(ep + 8);
          unsigned long long val = *(unsigned long long*)(ep + 16);
          char eb[144]; snprintf(eb,sizeof(eb),"[MEM]   e%d hash=%llx key=%llx val=%llx", ei, h, key, val); logmsg(eb);
          /* value 若是对象，看看它的字段（可能是商品列表） */
          if(val >= 0x7000000000ULL && val <= 0x7fffffffffULL){
            for(int vo=0; vo<0x30; vo+=8){
              unsigned long long vv = *(unsigned long long*)((char*)val + vo);
              char vb[128]; snprintf(vb,sizeof(vb),"[MEM]     v+%02x=%016llx", vo, vv); logmsg(vb);
            }
            /* 再深一层：val+0x10 可能就是 List<商品> */
            void* lstx = *(void**)((char*)val + 0x10);
            uint64_t lxv = (uint64_t)(uintptr_t)lstx;
            if(lxv >= 0x7000000000ULL && lxv <= 0x7fffffffffULL){
              /* 先把这个对象的字段全 dump，确认 List 的真实布局 */
              for(int lo=0; lo<0x40; lo+=8){
                unsigned long long lv2 = *(unsigned long long*)((char*)lstx + lo);
                char lb2[128]; snprintf(lb2,sizeof(lb2),"[MEM]       L+%02x=%016llx", lo, lv2); logmsg(lb2);
              }
              /* 【正解】这是自定义容器：+0x18 = count，+0x20 起是元素指针数组 */
              int szx = *(int*)((char*)lstx + 0x18);
              char lh9[128]; snprintf(lh9,sizeof(lh9),"[MEM]       LIST count=%d", szx); logmsg(lh9);
              /* 【统计已移到 my_setdata】这里读的是容器字典里的缓存 count，
                 空列表时也可能非 0，会造成"一件没看到却显示扫到 N 件"的误报。 */
              /* 【开关守卫】必须"自动购买 + 扫拍"都开、且在拍卖页，才允许下单 */
              if(szx > 0 && szx < 64 && g_last_price > 0 && g_rbuy_addr && g_dc_inst
                 && g_buy2_enabled && g_enabled2 && g_page_active){
                for(int si=0; si<szx && si<40; si++){
                  void* itx = *(void**)((char*)lstx + 0x20 + si*8);
                  uint64_t ixv = (uint64_t)(uintptr_t)itx;
                  if(ixv < 0x7000000000ULL || ixv > 0x7fffffffffULL) continue;
                  unsigned long long a1 = *(unsigned long long*)((char*)itx + 0x10);
                  unsigned long long a2 = *(unsigned long long*)((char*)itx + 0x18);
                  int iid = *(int*)((char*)itx + 0x20);
                  int cn  = *(int*)((char*)itx + 0x28);
                  if(!a1 || !a2 || !iid) continue;
                  ((void(*)(void*, unsigned long long, unsigned long long, int, int, int))g_rbuy_addr)(
                    g_dc_inst, a1, a2, iid, g_last_price, cn);
                  static int mem_buy = 0;
                  mem_buy++;
                  if(mem_buy <= 16){ char mb[144]; snprintf(mb,sizeof(mb),"[MEMBUY] #%d itemId=%x price=%d cnt=%d", mem_buy, iid, g_last_price, cn); logmsg(mb); }
                }
              }
            }
          }
        }
      }
    }
  }
  /* 【绕过 SetData】解析完成 → 立刻用游戏自己的 API 取列表 */
  {
    static long long last_batch = 0;
    long long nowb = mono_ms();
    if(0 && g_getlist_addr && nowb - last_batch >= 500 && g_gs_cnt <= 8){   /* 【禁用】调游戏方法造成卡顿 */
    last_batch = nowb;
    for(int cat=0; cat<60; cat++){
      void* lst = ((void*(*)(void*, int))g_getlist_addr)(self, cat);
      uint64_t lv = (uint64_t)(uintptr_t)lst;
      if(lv < 0x7000000000ULL || lv > 0x7fffffffffULL) continue;
      char lh[112]; snprintf(lh,sizeof(lh),"[GL] cat=%d list=%p", cat, lst); logmsg(lh);
      /* List<T> 结构：+0x10 items 数组, +0x18 size */
      void* it = *(void**)((char*)lst + 0x10);
      int sz = *(int*)((char*)lst + 0x18);
      char sh[112]; snprintf(sh,sizeof(sh),"[GL]   items=%p size=%d", it, sz); logmsg(sh);
      uint64_t itv = (uint64_t)(uintptr_t)it;
      /* 【绕过SetData】一旦拿到非空列表，立刻用最近读到的价格把整批商品下单 */
      if(itv >= 0x7000000000ULL && itv <= 0x7fffffffffULL && sz > 0 && sz < 64 && g_last_price > 0){
        static int gb = 0;
        for(int bi=0; bi<sz; bi++){
          void* el2 = *(void**)((char*)it + 0x20 + bi*8);
          uint64_t e2v = (uint64_t)(uintptr_t)el2;
          if(e2v < 0x7000000000ULL || e2v > 0x7fffffffffULL) continue;
          unsigned long long a1 = *(unsigned long long*)((char*)el2 + 0x10);
          unsigned long long a2 = *(unsigned long long*)((char*)el2 + 0x18);
          int iid2 = *(int*)((char*)el2 + 0x20);
          int cn2  = *(int*)((char*)el2 + 0x28);
          if(g_rbuy_addr && g_dc_inst && a1 && a2 && iid2){
            ((void(*)(void*, unsigned long long, unsigned long long, int, int, int))g_rbuy_addr)(
              g_dc_inst, a1, a2, iid2, g_last_price, cn2);
            gb++;
            if(gb <= 12){ char qb[144]; snprintf(qb,sizeof(qb),"[GCBUY] #%d itemId=%x price=%d cnt=%d", gb, iid2, g_last_price, cn2); logmsg(qb); }
          }
        }
      }
      if(itv >= 0x7000000000ULL && itv <= 0x7fffffffffULL && sz > 0 && sz < 64){
        for(int i=0; i<sz && i<8; i++){
          void* el = *(void**)((char*)it + 0x20 + i*8);
          uint64_t ev = (uint64_t)(uintptr_t)el;
          char eh[112]; snprintf(eh,sizeof(eh),"[GL]   [%d]=%p", i, el); logmsg(eh);
          if(ev < 0x7000000000ULL || ev > 0x7fffffffffULL) continue;
          for(int o=0; o<0x100; o+=8){
            unsigned long long vv = *(unsigned long long*)((char*)el + o);
            char vb[112]; snprintf(vb,sizeof(vb),"[GL]     +%02x=%016llx", o, vv); logmsg(vb);
          }
        }
      }
    }
  }
  }
  g_need_container_read = 1;
}
/* ===== 扫拍主体（网络线程 / 主线程共用）=====
   正常由 my_update（AbstractTcpClient.Update）高频驱动；
   如果本游戏版本找不到 Update 方法，则由 my_delta_time（主线程每帧）兜底，
   保证"开了扫拍就一定会动"，而不是因为一个 hook 失败全盘失效。 */
static void scan_tick(void){
  if(!g_enabled2) return;
  if(g_fail_until && mono_ms() < g_fail_until) return;
  static long long last_us = 0;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  long long nowus = (long long)ts.tv_sec*1000000LL + ts.tv_nsec/1000LL;
  /* 夹紧间隔：即使被写入 0 / 负数 / 超大值，也不会出现 0 间隔或死等 */
  long long iv_us = g_scan_interval_us;
  if(iv_us < SCAN_IV_MIN_US) iv_us = SCAN_IV_MIN_US;
  if(iv_us > SCAN_IV_MAX_US) iv_us = SCAN_IV_MAX_US;
  /* 四种方式全关 → 这一轮什么都不做（连计数都不涨，避免假数据）。
     允许只勾「排序」或「价格」单独测试它们是否真的被调用。 */
  if(!g_use_click && !g_use_search && !g_sort_sync && !g_use_price) return;
  if(last_us == 0 || nowus - last_us >= iv_us){
    /* 【收进间隔内】重置详细搜索冷却以前是每个 tick 都调，等于绕过节流；
       现在只有真正要刷新时才重置，冷却重置频率 = 刷新频率。 */
    if(g_resetcd_addr && g_dc_inst){
      ((void(*)(void*))g_resetcd_addr)(g_dc_inst);
    }
    if(last_us != 0){
      long long gap = (nowus - last_us) / 1000;
      if(gap < 0) gap = 0;
      if(gap > 600000) gap = 600000;
      g_actual_gap_ms = (int)gap;
    }
    last_us = nowus;
    /* ① 刷新按钮（受「按钮」开关控制）*/
    if(g_use_click && g_click_addr && g_page_inst){
      uint64_t pv = (uint64_t)(uintptr_t)g_page_inst;
      if(pv > 0x1000ULL && pv < 0x7fffffffffULL){
        ((void(*)(void*))g_click_addr)((void*)g_page_inst);
      }
    }
    /* ② 重放搜索参数（受「搜索」开关控制）：优先详细搜索那一份 */
    void** src = g_rs_detail_valid ? g_rs_detail_args : g_rs_args;
    if(g_use_search && src[0] && g_rs_addr){
      void (*fn3)(void*,long,void*,void*,long,long,void*,long,long)
        = (void(*)(void*,long,void*,void*,long,long,void*,long,long))g_rs_addr;
      fn3(src[0],(long)src[1],src[2],src[3],
          (long)src[4],(long)src[5],src[6],
          (long)src[7],(long)src[8]);
    }
    /* ③ 「排序」方式：用校准得到的【剩余时间 field】调 OnSortChanged */
    if(g_sort_sync && g_sortchg_addr && g_page_inst && orig_sortchanged){
      uint64_t pv3 = (uint64_t)(uintptr_t)g_page_inst;
      long f3 = (long)g_field_time;
      if(pv3 > 0x1000ULL && pv3 < 0x7fffffffffULL && f3 != 0){
        orig_sortchanged((void*)g_page_inst, f3, 0L);
      }
    }
    /* ④ 「价格」方式：游戏里没有 OnClickSortPrice，真正生效的是 OnSortChanged(field,order)。
       实测：点「价格」表头 → field=534694770656，order=0
       这里直接调原函数（orig），避免再进自己的 hook。 */
    /* 「价格」方式：用【当前会话】捕获到的 field 去调 OnSortChanged。
       不再用硬编码旧值（那是指针，重启就变，传进去必崩）。
       g_last_sort_field 由用户点表头时实时更新，只在当前进程内有效。 */
    if(g_use_price && g_sortchg_addr && g_page_inst && orig_sortchanged){
      uint64_t pv4 = (uint64_t)(uintptr_t)g_page_inst;
      long f4 = (long)g_field_price;
      if(pv4 > 0x1000ULL && pv4 < 0x7fffffffffULL && f4 != 0){
        orig_sortchanged((void*)g_page_inst, f4, 0L);
      }
    }
    g_calls++;
    /* 【首页缓存扣除】扫拍第一次真正跑起来时，把此刻已统计到的数量记为基线。
       拍卖行首次进入会预渲染约 6 行（SetData 会触发），那不是扫出来的，
       所以正式计数 = g_seen_total - g_seen_base。 */
    if(!g_seen_armed){
      g_seen_armed = 1;
      g_seen_base  = g_seen_total;
    }
  }
}

static void my_delta_time(void){
  /* 定时开启：到点后真正把扫拍打开（主线程每帧检查一次）*/
  if(g_auto_on_at){
    long long _now = mono_ms();
    if(_now >= g_auto_on_at){
      g_auto_on_at = 0;
      ensure_use_mode();               /* 到点开启前同样保证有方式 */
      if(!g_enabled2) g_run_start_ms = _now;
      g_enabled2 = 1;
    }
  }
  /* 扫拍兜底驱动：只有 Update 没 hook 上时才走这里（主线程，每帧一次）*/
  if(!g_update_addr) scan_tick();
  /* 限购达标 → 主线程截图 */
  /* 【开关守卫】截图是"限购"功能的收尾动作，限购关掉就不再截 */
  if(g_limit_on && g_need_shot && !g_shot_done){
    take_shot_once();
  }
  /* 【实时价格】把这一轮所有行的价格逐个读出来，写入 itemId→price 表 */
  if(g_buy2_enabled && g_rs_n > 0){
    int bn2 = g_rs_n;
    g_rs_n = 0;
    for(int bi2=0; bi2<bn2 && bi2<32; bi2++){
      void* sl2 = g_rs_slot[bi2];
      void* it2 = g_rs_item[bi2];
      uint64_t sv2 = (uint64_t)(uintptr_t)sl2;
      uint64_t iv2 = (uint64_t)(uintptr_t)it2;
      if(sv2 < 0x7000000000ULL || sv2 > 0x7fffffffffULL) continue;
      if(iv2 < 0x7000000000ULL || iv2 > 0x7fffffffffULL) continue;
      int iid2 = *(int*)((char*)it2 + 0x20);
      if(iid2 <= 0) continue;
      void* tp2 = *(void**)((char*)sl2 + 0x30);
      uint64_t tv2 = (uint64_t)(uintptr_t)tp2;
      if(tv2 < 0x7000000000ULL || tv2 > 0x7fffffffffULL) continue;
      void* k2 = *(void**)tp2;
      void* gt2 = p_method_get_from_name(k2,"get_text",0);   /* 每行都按自己的类查，不跨类复用 */
      if(!gt2) continue;
      void* st2 = ((void*(*)(void*))(*(void**)gt2))(tp2);
      uint64_t stv2 = (uint64_t)(uintptr_t)st2;
      if(stv2 < 0x7000000000ULL || stv2 > 0x7fffffffffULL) continue;
      int ln2 = *(int*)((char*)st2+0x10);
      if(ln2 <= 0 || ln2 > 24) continue;
      int pr2 = 0;
      for(int ci2=0; ci2<ln2; ci2++){
        unsigned short u2 = *(unsigned short*)((char*)st2+0x14+ci2*2);
        if(u2 >= '0' && u2 <= '9') pr2 = pr2*10 + (u2-'0');
        else if(u2 == ',') continue;
        else break;
      }
      if(pr2 > 0) px_put(iid2, pr2);
    }
    static int pxlog = 0;
    if(++pxlog <= 5){ char pb[128]; snprintf(pb,sizeof(pb),"[PX] table size=%d", g_px_n); logmsg(pb); }
  }
  /* 【直发准备】下一帧：读这一行的价格 → 打印将要发送的 RequestBuyItem 参数 */
  /* 【批量直发】本帧把所有排队的行全部下单（一次刷新 = 全部买）*/
  if(0 && g_buy2_enabled && g_batch_n > 0){   /* 【已禁用】批量直发会崩 */
    int bn = g_batch_n;
    g_batch_n = 0;
    for(int bi=0; bi<bn && bi<24; bi++){
      void* it = g_batch_item[bi];
      void* sr = g_batch_slot[bi];
      uint64_t itv = (uint64_t)(uintptr_t)it;
      uint64_t srv = (uint64_t)(uintptr_t)sr;
      if(itv < 0x7000000000ULL || itv > 0x7fffffffffULL) continue;
      if(srv < 0x7000000000ULL || srv > 0x7fffffffffULL) continue;
      /* 读这一行的价格 */
      int pr9 = 0;
      void* tp9 = *(void**)((char*)sr + 0x30);
      uint64_t tp9v = (uint64_t)(uintptr_t)tp9;
      if(tp9v >= 0x7000000000ULL && tp9v <= 0x7fffffffffULL){
        void* k9 = *(void**)tp9;
        void* gt9 = p_method_get_from_name(k9,"get_text",0);
        if(gt9){
          void* st9 = ((void*(*)(void*))(*(void**)gt9))(tp9);
          uint64_t st9v = (uint64_t)(uintptr_t)st9;
          if(st9v >= 0x7000000000ULL && st9v <= 0x7fffffffffULL){
            int l9 = *(int*)((char*)st9+0x10);
            for(int si9=0; si9<l9 && si9<24; si9++){
              unsigned short u9 = *(unsigned short*)((char*)st9+0x14+si9*2);
              if(u9 >= '0' && u9 <= '9') pr9 = pr9*10 + (u9-'0');
              else if(u9 == ',') continue;
              else break;
            }
          }
        }
      }
      if(pr9 <= 0) continue;
      uint64_t au1 = *(unsigned long long*)((char*)it+0x10);
      uint64_t au2 = *(unsigned long long*)((char*)it+0x18);
      int iid = *(int*)((char*)it+0x20);
      int c9 = *(int*)((char*)it+0x28);
      if(g_rbuy_addr && g_dc_inst){
        ((void(*)(void*, unsigned long long, unsigned long long, int, int, int))g_rbuy_addr)(
          g_dc_inst, au1, au2, iid, pr9, c9);
        g_direct_sent++;
        if(g_direct_sent <= 20 || (g_direct_sent % 50) == 0){
          char b9[192]; snprintf(b9,sizeof(b9),"[BATCH] #%d itemId=%x price=%d cnt=%d", g_direct_sent, iid, pr9, c9); logmsg(b9);
        }
      }
    }
  }
  if(g_buy2_enabled && g_direct_pending && g_direct_item && g_direct_slot){   /* 开关关就不发 */
    void* sr = g_direct_slot;
    void* it = g_direct_item;
    uint64_t srv = (uint64_t)(uintptr_t)sr;
    uint64_t itv = (uint64_t)(uintptr_t)it;
    if(srv >= 0x7000000000ULL && srv <= 0x7fffffffffULL &&
       itv >= 0x7000000000ULL && itv <= 0x7fffffffffULL){
      void* tp6 = *(void**)((char*)sr + 0x30);
      uint64_t tp6v = (uint64_t)(uintptr_t)tp6;
      int pr6 = 0;
      if(tp6v >= 0x7000000000ULL && tp6v <= 0x7fffffffffULL){
        void* k6 = *(void**)tp6;
        void* gt6 = p_method_get_from_name(k6,"get_text",0);
        if(gt6){
          void* st6 = ((void*(*)(void*))(*(void**)gt6))(tp6);
          uint64_t st6v = (uint64_t)(uintptr_t)st6;
          if(st6v >= 0x7000000000ULL && st6v <= 0x7fffffffffULL){
            int l6 = *(int*)((char*)st6+0x10);
            if(l6 > 0 && l6 < 24){
              for(int si6=0; si6<l6; si6++){
                unsigned short u6 = *(unsigned short*)((char*)st6+0x14+si6*2);
                if(u6 >= '0' && u6 <= '9') pr6 = pr6*10 + (u6-'0');
                else if(u6 == ',') continue;
                else break;
              }
            }
          }
        }
      }
      if(pr6 > 0){
        uint64_t au1 = *(unsigned long long*)((char*)it+0x10);
        uint64_t au2 = *(unsigned long long*)((char*)it+0x18);
        int iid = *(int*)((char*)it+0x20);
        int c6 = *(int*)((char*)it+0x28);
        /* 【持续直发】限流 300ms，避免把服务器打死；每次数据落地都尝试下单 */
        static long long g_last_direct_ms = 0;
        long long nowd = mono_ms();
        if(1){   /* 【拉满】不限流：每次数据落地都直发 */
          g_last_direct_ms = nowd;
          g_direct_sent++;
          char db6[256];
          snprintf(db6,sizeof(db6),"[DIRECT] #%d auid1=%llx auid2=%llx itemId=%x price=%d count=%d",
            g_direct_sent, au1, au2, iid, pr6, c6);
          logmsg(db6);
          if(g_rbuy_addr && g_dc_inst){
            ((void(*)(void*, unsigned long long, unsigned long long, int, int, int))g_rbuy_addr)(
              g_dc_inst, au1, au2, iid, pr6, c6);
          } else logmsg("[DIRECT] missing dc or rbuy, skip");
        }
        g_direct_pending = 0;
      }
    }
  }
  /* 【首次加速】如果价格还没读到、但已经有 slot，就立刻读一次
     （不用等下一轮 SetData）*/
  if(g_last_price <= 0 && g_direct_slot && g_buy2_enabled){
    uint64_t sv0 = (uint64_t)(uintptr_t)g_direct_slot;
    if(sv0 >= 0x7000000000ULL && sv0 <= 0x7fffffffffULL){
      void* tp0 = *(void**)((char*)g_direct_slot + 0x30);
      uint64_t tv0 = (uint64_t)(uintptr_t)tp0;
      if(tv0 >= 0x7000000000ULL && tv0 <= 0x7fffffffffULL){
        void* k0 = *(void**)tp0;
        void* gt0 = p_method_get_from_name(k0,"get_text",0);
        if(gt0){
          void* st0 = ((void*(*)(void*))(*(void**)gt0))(tp0);
          uint64_t s0v = (uint64_t)(uintptr_t)st0;
          if(s0v >= 0x7000000000ULL && s0v <= 0x7fffffffffULL){
            int l0 = *(int*)((char*)st0+0x10);
            if(l0 > 0 && l0 < 24){
              int p0 = 0;
              for(int i0=0; i0<l0; i0++){
                unsigned short u0 = *(unsigned short*)((char*)st0+0x14+i0*2);
                if(u0 >= '0' && u0 <= '9') p0 = p0*10 + (u0-'0');
                else if(u0 == ',') continue;
                else break;
              }
              if(p0 > 0) g_last_price = p0;
            }
          }
        }
      }
    }
  }
  /* 【验证】SetData 之后读上一行 slot 的价格 Text（看是否延迟填充）
     【开关守卫】这块只服务于"自动购买"的价格来源，开关关闭时一次都不读 */
  {
    static int late = 0;
    if(g_buy2_enabled && late < 6 && g_last_slot){
      late++;
      uint64_t lsv = (uint64_t)(uintptr_t)g_last_slot;
      if(lsv >= 0x7000000000ULL && lsv <= 0x7fffffffffULL){
        void* tp5 = *(void**)((char*)g_last_slot + 0x30);
        uint64_t tpv5 = (uint64_t)(uintptr_t)tp5;
        if(tpv5 >= 0x7000000000ULL && tpv5 <= 0x7fffffffffULL){
          void* k5 = *(void**)tp5;
          const char* kn5 = p_class_get_name ? p_class_get_name(k5) : NULL;
          if(kn5 && strstr(kn5,"Text")){
            void* gt5 = p_method_get_from_name(k5,"get_text",0);
            if(gt5){
              void* str5 = ((void*(*)(void*))(*(void**)gt5))(tp5);
              uint64_t s5v = (uint64_t)(uintptr_t)str5;
              if(s5v >= 0x7000000000ULL && s5v <= 0x7fffffffffULL){
                int l5 = *(int*)((char*)str5+0x10);
                if(l5 > 0 && l5 < 40){
                  char a5[64]; int ap5 = 0;
                  for(int si5=0; si5<l5 && si5<38 && ap5<60; si5++){
                    unsigned short u5 = *(unsigned short*)((char*)str5+0x14+si5*2);
                    a5[ap5++] = (u5 < 128) ? (char)u5 : 0x3F;
                  }
                  a5[ap5] = 0;
                  int pv5 = 0;
                  for(int pi=0; pi<ap5; pi++){
                    if(a5[pi] >= '0' && a5[pi] <= '9') pv5 = pv5*10 + (a5[pi]-'0');
                    else if(a5[pi] == ',') continue;
                    else break;
                  }
                  if(pv5 > 0){
                    g_last_price = pv5;
                    /* 实时记录：这个 itemId 当前的真实价格 */
                    if(g_direct_item){
                      int curId = *(int*)((char*)g_direct_item + 0x20);
                      if(curId > 0) px_put(curId, pv5);
                    }
                  }
                  char r5[160]; snprintf(r5,sizeof(r5),"[LATE] slot=%p price='%s' parsed=%d", g_last_slot, a5, pv5); logmsg(r5);
                }
              }
            }
          }
        }
      }
    }
  }
  /* 先处理购买1 的待触发（上一帧 SetData 登记的行）*/
  if(0 && /*【纯直发】UI链路已禁用*/ g_has_pend && g_pend_slot && g_clickbuy_addr && g_buy_enabled && g_page_active){
    g_has_pend = 0;
    void* sl = g_pend_slot; g_pend_slot = NULL;
    ((void(*)(void*))g_clickbuy_addr)(sl);
    static long long tc = 0;
    if(++tc <= 5){ char tb[128]; snprintf(tb,sizeof(tb),"[TRIG] click slot=%p n=%lld", sl, tc); logmsg(tb); }
  }
  /* 防递归：RefreshItemInfo 内部也会调 Time.deltaTime，
     不加保护会无限递归 → 栈溢出 → 游戏崩溃 */
  if(g_in_dt){ if(orig_dt) orig_dt(); return; }
  g_in_dt = 1;
  g_dt_calls++;
  /* 主线程每帧自查：只要弹窗在、开关开、还没确认，就尝试确认。
     字段一就绪（通常 1~2 帧 = 16~33ms）立刻确认；最多 300ms 兜底。
     这样不依赖网络线程的心跳，确认延迟压到"一帧"。 */
  /* 【防野指针】只在"拍卖页处于激活状态"时才碰弹窗对象：
     场景切换/加载时弹窗会被销毁，但 OnDisable 不一定触发，
     g_popup_inst 会变成野指针 → 读 +0x1d8 直接崩溃。 */
  if(0 && /*【纯直发】UI链路已禁用*/ g_buy2_enabled && g_popup_inst && g_page_active && g_popup_on && !g_popup_confirmed){
    g_need_confirm = 0;
    void* btn = g_buy2_addr ? g_buy2_addr : g_confirm_addr;
    if(btn && !g_popup_confirmed){
      void* btnf = *(void**)((char*)g_popup_inst + 0x1d8);
      void* prf  = *(void**)((char*)g_popup_inst + 0x1c0);
      uint64_t b1 = (uint64_t)(uintptr_t)btnf, b2v = (uint64_t)(uintptr_t)prf;
      (void)b2v;
      int ready = (b1 >= 0x7000000000ULL && b1 <= 0x7fffffffffULL);   /* 只看购买按钮是否就绪 */
      long long waited = mono_ms() - g_popup_open_ms;
      /* 字段还没填好 → 主动让游戏自己刷新弹窗数据（主线程、安全），
         下一帧通常就 ready=1，然后立刻确认（实测 7ms） */
      /* refresh 调用已移除（会递归进 Time.deltaTime 导致崩溃）*/
      if(ready || waited >= 80){
        g_popup_confirmed = 1;
        g_dt_confirm++;
        ((void(*)(void*))btn)(g_popup_inst);
        char bb[176]; snprintf(bb,sizeof(bb),"[B2] confirm(MAIN) n=%lld ready=%d wait=%lldms dtc=%lld",
          g_dt_confirm, ready, waited, g_dt_calls); logmsg(bb);
      }
    }
  }
  if(orig_dt) orig_dt();
  g_in_dt = 0;
}

static void my_update(void* self){
  /* 【必须】游戏自己的网络 Update 必须原样执行。
     这个函数不属于本 so 的任何功能开关，它是游戏原逻辑，
     hook 不转发它 → 收包/心跳全废 → 掉线、搜不到商品。 */
  if(orig_update) orig_update(self);
  g_hits++;
  /* 延迟查找 OpenBuy：页面实例出现后，先列出页面类全部方法名，再找 OpenBuy */
  if(!g_open_addr && g_page_inst){
    void* pgcls = *(void**)g_page_inst;
    if(pgcls){
      static int page_methods_dumped = 0;
      if(!page_methods_dumped){
        page_methods_dumped = 1;
        void* it = NULL; void* mm;
        int cnt = 0;
        while((mm = p_class_get_methods(pgcls, &it))){
          const char* nm = p_method_get_name(mm);
          if(nm && (strstr(nm,"Buy") || strstr(nm,"Open") || strstr(nm,"Click"))){
            char qb[128]; snprintf(qb,sizeof(qb),"[PM] %s", nm); logmsg(qb);
            cnt++;
          }
        }
        char cb[64]; snprintf(cb,sizeof(cb),"[PM] matched=%d", cnt); logmsg(cb);
      }
      static int allscan_done = 0;
      if(!allscan_done){
        allscan_done = 1;
        if(p_domain_get && p_domain_get_assemblies && p_assembly_get_image && p_image_get_class_count && p_image_get_class && p_class_get_methods && p_method_get_name){
          size_t n = 0;
          void** asms = (void**)p_domain_get_assemblies(p_domain_get(), &n);
          for(size_t ai=0; ai<n && ai<40; ai++){
            void* img = p_assembly_get_image(asms[ai]);
            if(!img) continue;
            size_t cc = (size_t)(uintptr_t)p_image_get_class_count(img);
            if(cc > 5000) continue;
            for(size_t ci=0; ci<cc; ci++){
              void* cls = p_image_get_class(img, ci);
              if(!cls) continue;
              void* it2 = NULL; void* m2;
              while((m2 = p_class_get_methods(cls, &it2))){
                const char* mn = p_method_get_name(m2);
                if(mn && (strstr(mn,"Buy") || strstr(mn,"ClickItem") || strstr(mn,"OnClickGoods"))){
                  char qb[160]; snprintf(qb,sizeof(qb),"[FIND] %s pc=%d", mn, p_method_get_param_count(m2)); logmsg(qb);
                  if(!g_open_addr && strstr(mn,"OpenBuy")){ g_open_addr = *(void**)m2; }
                }
              }
            }
          }
        }
        logmsg("[FIND] allscan done");
      }
    }
  }
  {
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC, &ts0);
    long long n0 = (long long)ts0.tv_sec*1000LL + ts0.tv_nsec/1000000LL;
    static long long dbg = 0;
    if(n0 - dbg >= 1000){
      dbg = n0;
      char db[200];
      snprintf(db,sizeof(db),"[UPD] en=%d cs=%llx si=%llx dc=%llx pg=%llx calls=%d dtc=%lld needc=%d",
        g_enabled2,
        (unsigned long long)(uintptr_t)g_clicksearch_addr,
        (unsigned long long)(uintptr_t)g_search_inst,
        (unsigned long long)(uintptr_t)g_dc_inst,
        (unsigned long long)(uintptr_t)g_page_inst,
        g_calls, g_dt_calls, g_need_confirm);
      logmsg(db);
    }
  }
  /* 【已移除】UI 按钮探测：FindObjectsOfType 会引发游戏崩溃，不再调用 */
  /* 扫拍主体统一走 scan_tick（与主线程兜底共用同一套逻辑）*/
  scan_tick();
  /* 购买：纯 Unity UI 事件（JS 点击等价物），与扫拍完全独立 */
  /* 购买开关 -> 标志文件同步（必须无条件执行，关闭时才能删掉标志）
     链路：列表购买(1186,218) -> 弹窗购买(917,617) -> 提示确认(640,433) */
  /* 购买开关同步已迁移到独立线程 switch_worker（不依赖游戏帧） */
  /* ===== 购买②：50ms 刷新 + 遍历列表两步购买（OpenBuy -> OnClickConfirm）===== */
  if(g_buy2_enabled){
    static long long g_b2_ms = 0;
    static int b2_step = 0, b2_idx = 0;
    struct timespec t2; clock_gettime(CLOCK_MONOTONIC, &t2);
    long long n2 = (long long)t2.tv_sec*1000LL + t2.tv_nsec/1000000LL;
    /* 【统一节拍】购买2 的刷新也走面板「刷新间隔」，不再固定 10ms。
       否则扫拍设 56ms、购买2 还在 10ms 刷，看起来就是"设置无效、还是超快"。
       另外：扫拍已经开着时由扫拍负责刷新，这里不再重复刷，避免两套叠加。 */
    long long iv2_us = g_scan_interval_us;
    if(iv2_us < SCAN_IV_MIN_US) iv2_us = SCAN_IV_MIN_US;
    if(iv2_us > SCAN_IV_MAX_US) iv2_us = SCAN_IV_MAX_US;
    long long iv2_ms = iv2_us / 1000;
    if(iv2_ms < 1) iv2_ms = 1;
    if(g_b2_ms == 0 || n2 - g_b2_ms >= iv2_ms){
      g_b2_ms = n2;
      /* ① 刷新列表：只有扫拍没开时才由购买2 自己刷；且必须勾了「按钮」方式 */
      if(!g_enabled2 && g_use_click && g_click_addr && g_page_inst){
        uint64_t pv = (uint64_t)(uintptr_t)g_page_inst;
        if(pv > 0x1000ULL && pv < 0x7fffffffffULL){
          ((void(*)(void*))g_click_addr)((void*)g_page_inst);
        }
      }
      /* ② 读列表 */
      int sz = 0; void* arr = NULL;
      if(g_page_inst){
        void* c8 = *(void**)((char*)g_page_inst + 0xC8);
        if(c8){
          arr = *(void**)((char*)c8 + 0x10);
          sz = (int)(*(long long*)((char*)c8 + 0x18) & 0xFFFFFFFF);
        }
      }
      /* 购买②：弹窗出现后等 400ms 再调「购买」按钮的 OnConfirmBuy */
      {
        void* btn = g_buy2_addr ? g_buy2_addr : g_confirm_addr;
        if(0 && /*【纯直发】UI链路已禁用*/ g_buy2_enabled && g_popup_inst && btn){
          uint64_t pk = (uint64_t)(uintptr_t)(*(void**)g_popup_inst);
          if(pk > 0x1000ULL && pk < 0x7fffffffffULL){
            /* 网络线程只置标记，真正的确认交给主线程（my_delta_time）执行 */
            if(!g_popup_confirmed) g_need_confirm = 1;
          }
        }
      }
    }
  }
  /* 购买1 触发点已移到 my_setdata（数据刚落地时立刻调 OnClickBuy），
     避免用缓存 slot 打开旧弹窗 */
  }

static int my_remain(void* self){
  if(self){
    g_dc_inst = self;
    char m[96]; snprintf(m,sizeof(m),"[dc] captured %p",self); logmsg(m);
  }
  return 0;
}
static void my_rs(void* self, long a, void* b, void* c, long d, long e, void* f, long g, long h){
  if(self){
    uint64_t sv = (uint64_t)(uintptr_t)self;
    if(sv >= 0x7000000000ULL && sv <= 0x7fffffffffULL){
      g_rs_args[0]=self; g_rs_args[1]=(void*)a; g_rs_args[2]=b; g_rs_args[3]=c;
      g_rs_args[4]=(void*)d; g_rs_args[5]=(void*)e; g_rs_args[6]=f; g_rs_args[7]=(void*)g; g_rs_args[8]=(void*)h;
      g_rs_capture_cnt++;
      /* 【链路验证】详细搜索点击后 3 秒内发生的捕获，判定为"详细搜索链路" */
      long long dt = mono_ms() - g_detail_ms;
      if(g_detail_ms > 0 && dt >= 0 && dt < 3000){
        g_rs_detail_cnt++;
        for(int i9=0;i9<9;i9++) g_rs_detail_args[i9] = g_rs_args[i9];
        g_rs_detail_valid = 1;      /* 之后扫拍优先重放这一份 */
      }
      if(!g_dc_inst){ g_dc_inst = self; logmsg("[rs] dc captured"); }
    }
  }
  if(orig_rs) orig_rs(self,a,b,c,d,e,f,g,h);
}
/* ===== 搜索响应包：从原始字节里找「商品ID → 价格」 ===== */
static void* g_onsearch_addr = NULL;
static void (*orig_onsearch)(void*, long, long, void*) = NULL;
static volatile int g_os_cnt = 0;
static void my_on_search(void* self, long a, long b, void* data){
  /* 【统计】每次服务器搜索回包都 +1 —— 这是「扫拍到底有没有真的刷新」的硬证据 */
  g_os_cnt++;
  /* 【高频入口】每次搜索响应都读一次容器并批量下单（纯内存，不调方法）*/
  /* 【改动】原来是直接 return，会把游戏原生的搜索响应处理一起跳过（列表不刷新）。
     现在改成条件判断：开关关闭只跳过购买块，函数末尾的 orig_onsearch 照常执行。 */
  if(g_buy2_enabled && g_enabled2
     && g_gc_inst && g_last_price > 0 && g_rbuy_addr && g_dc_inst){
    static long long rsp_buy_ms = 0;
    long long rnow = mono_ms();
    if(rnow - rsp_buy_ms >= 0){   /* 【最高频】不平限流，响应一到就买 */
      rsp_buy_ms = rnow;
      int offs2[4] = {0x18, 0x20, 0x28, 0x30};
      for(int di2=0; di2<4; di2++){
        void* d2 = *(void**)((char*)g_gc_inst + offs2[di2]);
        uint64_t dv2 = (uint64_t)(uintptr_t)d2;
        if(dv2 < 0x7000000000ULL || dv2 > 0x7fffffffffULL) continue;
        void* ent2 = *(void**)((char*)d2 + 0x18);
        int cnt2 = *(int*)((char*)d2 + 0x20);
        uint64_t ev2 = (uint64_t)(uintptr_t)ent2;
        if(ev2 < 0x7000000000ULL || ev2 > 0x7fffffffffULL || cnt2 <= 0 || cnt2 > 64) continue;
        for(int ei2=0; ei2<cnt2 && ei2<16; ei2++){
          char* ep2 = (char*)ent2 + 0x20 + ei2*24;
          unsigned long long val2 = *(unsigned long long*)(ep2 + 16);
          if(val2 < 0x7000000000ULL || val2 > 0x7fffffffffULL) continue;
          void* lst2 = *(void**)((char*)val2 + 0x10);
          uint64_t l2v = (uint64_t)(uintptr_t)lst2;
          if(l2v < 0x7000000000ULL || l2v > 0x7fffffffffULL) continue;
          int sz2 = *(int*)((char*)lst2 + 0x18);
          if(sz2 <= 0 || sz2 > 64) continue;
          for(int si2=0; si2<sz2 && si2<40; si2++){
            void* ix2 = *(void**)((char*)lst2 + 0x20 + si2*8);
            uint64_t i2v = (uint64_t)(uintptr_t)ix2;
            if(i2v < 0x7000000000ULL || i2v > 0x7fffffffffULL) continue;
            unsigned long long b1 = *(unsigned long long*)((char*)ix2 + 0x10);
            unsigned long long b2 = *(unsigned long long*)((char*)ix2 + 0x18);
            int bid = *(int*)((char*)ix2 + 0x20);
            int bcn = *(int*)((char*)ix2 + 0x28);
            if(!g_buy2_enabled) break;         /* 循环中途关掉开关 → 立即停 */
            if(!b1 || !b2 || !bid) continue;
            int myPrice = px_get(bid);         /* 实时：这件商品自己的价格 */
            if(myPrice <= 0) continue;          /* 没读到它的价格 → 不买（绝不瞎买）*/
            ((void(*)(void*, unsigned long long, unsigned long long, int, int, int))g_rbuy_addr)(
              g_dc_inst, b1, b2, bid, myPrice, bcn);
            /* 每次发包只记一次尝试（用于统计），限购不再按发包次数判断——
               改为在 my_settext 里按"成功购买件数"触发 limit_check_by_success()，
               否则买失败（被别人抢走）也会计数，导致限购提前停机、少买货。 */
            g_attempt_cnt++;
            static int rb2 = 0;
            rb2++;
            if(rb2 <= 8){ char qb[144]; snprintf(qb,sizeof(qb),"[RSPBUY] #%d itemId=%x price=%d cnt=%d", rb2, bid, g_last_price, bcn); logmsg(qb); }
          }
        }
      }
    }
  }
  { static int rh = 0; if(++rh <= 3){ char pb[128]; snprintf(pb,sizeof(pb),"[RSP] hit self=%p a=0x%lx b=0x%lx data=%p", self, (unsigned long)a, (unsigned long)b, data); logmsg(pb); } }
  /* a / b 可能是数据对象指针，dump 它们（找 auid/itemId/price） */
  {
    static int aba = 0;
    if(aba < 2){
      aba++;
      void* ap = (void*)(uintptr_t)a;
      void* bp = (void*)(uintptr_t)b;
      uint64_t av = (uint64_t)(uintptr_t)ap, bv = (uint64_t)(uintptr_t)bp;
      if(av >= 0x7000000000ULL && av <= 0x7fffffffffULL){
        char h1[96]; snprintf(h1,sizeof(h1),"[RSPA] a=%p", ap); logmsg(h1);
        for(int oa=0; oa<0x100; oa+=8){
          unsigned long long va = *(unsigned long long*)((char*)ap+oa);
          char l1[128]; snprintf(l1,sizeof(l1),"[RSPA] +%03x = %016llx", oa, va); logmsg(l1);
        }
        /* 第三层：dump 子指针（找商品列表 / 价格） */
        int offs3[5] = {0x30, 0x40, 0x48, 0x50, 0x60};
        for(int k3=0; k3<5; k3++){
          void* sp3 = *(void**)((char*)ap+offs3[k3]);
          uint64_t spv = (uint64_t)(uintptr_t)sp3;
          if(spv < 0x7000000000ULL || spv > 0x7fffffffffULL) continue;
          char h3[96]; snprintf(h3,sizeof(h3),"[RSP3] a+%02x -> %p", offs3[k3], sp3); logmsg(h3);
          for(int o3=0; o3<0x80; o3+=8){
            unsigned long long v3 = *(unsigned long long*)((char*)sp3+o3);
            char l3[128]; snprintf(l3,sizeof(l3),"[RSP3] +%03x = %016llx", o3, v3); logmsg(l3);
          }
        }
      }
      if(bv >= 0x7000000000ULL && bv <= 0x7fffffffffULL){
        char h2[96]; snprintf(h2,sizeof(h2),"[RSPB] b=%p", bp); logmsg(h2);
        for(int ob=0; ob<0x100; ob+=8){
          unsigned long long vb = *(unsigned long long*)((char*)bp+ob);
          char l2[128]; snprintf(l2,sizeof(l2),"[RSPB] +%03x = %016llx", ob, vb); logmsg(l2);
        }
      }
    }
  }
  if(data && g_os_cnt <= 2){
    uint64_t dp = (uint64_t)(uintptr_t)data;
    if(dp >= 0x7000000000ULL && dp <= 0x7fffffffffULL){
      char hb[96]; snprintf(hb,sizeof(hb),"[RSP] data=%p", data); logmsg(hb);
      for(int o7=0; o7<0x60; o7+=8){
        unsigned long long v7 = *(unsigned long long*)((char*)data+o7);
        char lb[128]; snprintf(lb,sizeof(lb),"[RSP] D+%03x = %016llx", o7, v7); logmsg(lb);
      }
      /* 第二层：顺着指针 dump 每个子对象（找商品/价格） */
      for(int o8=0; o8<0x60; o8+=8){
        void* p8 = *(void**)((char*)data+o8);
        uint64_t pp8 = (uint64_t)(uintptr_t)p8;
        if(pp8 < 0x7000000000ULL || pp8 > 0x7fffffffffULL) continue;
        char pbh[96]; snprintf(pbh,sizeof(pbh),"[RSP2] ptr@+%02x = %p", o8, p8); logmsg(pbh);
        for(int o9=0; o9<0x60; o9+=8){
          unsigned long long v9 = *(unsigned long long*)((char*)p8+o9);
          char lb9[128]; snprintf(lb9,sizeof(lb9),"[RSP2] +%03x = %016llx", o9, v9); logmsg(lb9);
        }
        if(o8 >= 0x20) break;   /* 只跟前几个指针，控制输出量 */
      }
    }
  }
  if(orig_onsearch) orig_onsearch(self, a, b, data);
}

static void limit_check_by_success(void);   /* 前置声明：按成功件数判断限购达标 */
static void my_settext(void* self, void* str){
  if(str){
    int len = *(int*)((char*)str + 0x10);
    if(len > 0 && len < 64){
      unsigned short *ch = (unsigned short*)((char*)str + 0x14);
      /* 操作频繁 = U+64CD U+4F5C U+9891 U+7E41 */
      if(len>=4){
        int ok=0;
        for(int i=0;i+3<len;i++){
          if(ch[i]==0x8D2D && ch[i+1]==0x4E70 && ch[i+2]==0x6210 && ch[i+3]==0x529F) ok=1;
        }
        if(ok){
          g_buy_count++;
          char b[96]; snprintf(b,sizeof(b),"[buy] success n=%d",g_buy_count); logmsg(b);
          limit_check_by_success();   /* 按成功件数判断是否达标 */
        }
      }
      for(int i=0;i<len-1;i++){
        /* 「该物品无法购买」或「点券扣除失败」→ 退避 3 秒，避免提示窗口疯狂累积 */
        if(ch[i]==0x65E0 && i+1<len && ch[i+1]==0x6CD5){
          g_fail_until = mono_ms()+50; g_popup_confirmed = 0; g_need_confirm = 0;
          logmsg("[backoff] fail tip 1.2s + reset popup state"); }
        if(ch[i]==0x6263 && i+1<len && ch[i+1]==0x9664){
          g_fail_until = mono_ms()+50; g_popup_confirmed = 0; g_need_confirm = 0;
          logmsg("[backoff] deduct fail 1.2s + reset popup state"); }
        if(ch[i]==0x8BF7 && ch[i+1]==0x7A0D){ g_fail_until = mono_ms()+50; }   /* 仅用于扫拍退避，不拦文本 */
      }
    }
  }
  if(orig_settext) orig_settext(self, str);
}
static void (*orig_buy)(void*) = NULL;
static void (*orig_confirm)(void*) = NULL;
static void my_buy(void* s){ logmsg("[HIT] OnConfirmBuy"); if(orig_buy) orig_buy(s); }
static int (*orig_notify)(void*, void*) = NULL;
static void my_notify_buy(void* self, void* data){
  if(self) g_dc_inst = self;
  g_buy_count++;
  { char b[96]; snprintf(b,sizeof(b),"[buy] notify n=%d",g_buy_count); logmsg(b); }
  if(orig_notify) orig_notify(self, data);
}
static void my_rbuy(void* self, void* p1, unsigned long p2, void* p3, int p4, void* p5){
  if(!g_rbuy_valid){
    g_rbuy_args[0]=self; g_rbuy_args[1]=p1; g_rbuy_args[2]=(void*)p2; g_rbuy_args[3]=p3; g_rbuy_args[4]=(void*)p4; g_rbuy_args[5]=p5;
    g_rbuy_valid = 1;
    char b[192]; snprintf(b,sizeof(b),"[rbuy] cap self=%p p1=%p p2=%lx p3=%p p4=%d p5=%p",self,p1,p2,p3,p4,p5); logmsg(b);
  }
  if(orig_rbuy) orig_rbuy(self,p1,p2,p3,p4,p5);
}
static void (*orig_openbuy)(void*, void*) = NULL;
/* ===== 按钮点击法：hook 列表项「购买」按钮的点击处理 OnClickBuy =====
   这就是你要的"只 hook 需要点击的按钮"：OnClickBuy(this) 是按钮绑定的方法，
   调用它 == 点按钮，不需要坐标、不需要无障碍、不需要 ADB。 */
static void (*orig_clickbuy)(void*) = NULL;
static volatile int g_cb_cnt = 0;
static void my_clickbuy(void* self){
  if(self && !g_slot_self) g_slot_self = self;
  g_cb_cnt++;
  if(g_cb_cnt <= 5){
    char b[144]; snprintf(b,sizeof(b),"[CB] OnClickBuy self=%p cnt=%d", self, g_cb_cnt); logmsg(b);
  }
  if(orig_clickbuy) orig_clickbuy(self);
}
static void* g_ob_self = NULL;      /* OpenBuy 的 self（校准用） */
static void* g_ob_item = NULL;      /* OpenBuy 的 item（校准用） */
static volatile int g_ob_cnt = 0;
static void my_openbuy(void* self, void* item){
  if(!g_listing_inst && self){ g_listing_inst = self; }
  if(g_ob_cnt < 3){
    g_ob_cnt++;
    g_ob_self = self; g_ob_item = item;
    char b[192]; snprintf(b,sizeof(b),"[OB2] self=%p item=%p cnt=%d", self, item, g_ob_cnt); logmsg(b);
  }
  if(orig_openbuy) orig_openbuy(self, item);
}
/* ===== 效仿购买2：hook 生命周期事件「SetData」捕获商品实例 =====
   购买2 = hook 弹窗 OnEnable 拿实例 → 直接调 OnClickConfirm
   购买1 = hook 列表项 SetData 拿商品 → 直接调 OpenBuy(打开购买弹窗) */
static void* g_setdata_addr = NULL;
static void (*orig_setdata)(void*, void*) = NULL;
static void* g_last_item = NULL;
static volatile int g_item_cnt = 0;
static void (*orig_page_disable)(void*) = NULL;
static void my_page_disable(void*);
static volatile int g_in_setdata = 0;
static long long g_last_trigger_ms = 0;
static volatile int g_trig_cnt = 0;

static long long mono_ms(void){
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec*1000LL + t.tv_nsec/1000000LL;
}

/* 列表数据落地 → 立刻用当行的 slot 调「购买」按钮（无坐标、无缓存） */
static void my_setdata(void* self, void* item){
  if(orig_setdata) orig_setdata(self, item);
  g_last_item = item;
  /* ===== 扫到件数统计（唯一可信来源）=====
     只有商品列表真的渲染出一行，游戏才会调 SetData。
     空列表 / 没商品时 SetData 不触发 → 数字不会虚涨。

     【去重键】用商品唯一 ID（auid1 @item+0x10 与 auid2 @item+0x18），
     不再用 item 指针 —— 游戏对象池会复用地址，指针相同不代表同一件商品；
     指针不同也不代表是新商品。用 auid 才能真正确认"这是同一件"。
     环形容量 512 条：一屏十几件，512 足够覆盖很多轮。 */
  {
    uint64_t ipv2 = (uint64_t)(uintptr_t)item;
    if(item && ipv2 >= 0x7000000000ULL && ipv2 <= 0x7fffffffffULL){
      unsigned long long a1 = *(unsigned long long*)((char*)item + 0x10);
      unsigned long long a2 = *(unsigned long long*)((char*)item + 0x18);
      int iid = *(int*)((char*)item + 0x20);
      /* 三重校验：auid 两个都非 0、itemId 在合理范围 → 才认为是一条真实商品 */
      if(a1 && a2 && iid > 0 && iid < 0x7ffffff){
        static unsigned long long sA1[512];
        static unsigned long long sA2[512];
        static int sN = 0;
        static int sPos = 0;
        bool dup = false;
        for(int ri = 0; ri < sN; ++ri){
          if(sA1[ri] == a1 && sA2[ri] == a2){ dup = true; break; }
        }
        if(!dup){
          sA1[sPos] = a1;
          sA2[sPos] = a2;
          sPos = (sPos + 1) & 511;
          if(sN < 512) sN++;
          g_seen_total++;
        }
      }
    }
  }
  /* 探测：dump item 前 0x180 字节，找 auid / 商品ID / 价格 / 数量 的精确偏移 */
  {
    static int fdump = 0;
    if(item && fdump < 2){
      uint64_t ip = (uint64_t)(uintptr_t)item;
      if(ip >= 0x7000000000ULL && ip <= 0x7fffffffffULL){
        fdump++;
        char hb[96]; snprintf(hb,sizeof(hb),"[ITF] item=%p dump#%d", item, fdump); logmsg(hb);
        for(int o6=0; o6<0x180; o6+=8){
          unsigned long long v6 = *(unsigned long long*)((char*)item+o6);
          char lb[128]; snprintf(lb,sizeof(lb),"[ITF] +%03x = %016llx", o6, v6); logmsg(lb);
        }
      }
    }
  }
  if(0 && /*【纯直发】UI链路已禁用*/ g_buy2_enabled && g_popup_inst && g_page_active && g_popup_on && !g_popup_confirmed){
    void* btnx = g_buy2_addr ? g_buy2_addr : g_confirm_addr;
    if(btnx){
      void* btnf = *(void**)((char*)g_popup_inst + 0x1d8);
      void* prf  = *(void**)((char*)g_popup_inst + 0x1c0);
      uint64_t b1 = (uint64_t)(uintptr_t)btnf, b2v = (uint64_t)(uintptr_t)prf;
      int ready = (b1 >= 0x7000000000ULL && b1 <= 0x7fffffffffULL) &&
                  (b2v >= 0x7000000000ULL && b2v <= 0x7fffffffffULL);
      long long waited = mono_ms() - g_popup_open_ms;
      if(ready || waited >= 80){
        g_popup_confirmed = 1;
        ((void(*)(void*))btnx)(g_popup_inst);
        static long long sc = 0;
        if(++sc <= 5){ char sb[144]; snprintf(sb,sizeof(sb),"[B2] confirm(SD) n=%lld ready=%d wait=%lldms", sc, ready, waited); logmsg(sb); }
      }
    }
  }
  g_last_item = item;
  /* 【开关守卫】购买开关全关时不登记 slot，避免给无开关路径留燃料 */
  if(self && (g_buy_enabled || g_buy2_enabled)){
    uint64_t s0=(uint64_t)(uintptr_t)self;
    if(s0>=0x7000000000ULL && s0<=0x7fffffffffULL) g_last_slot = self;
  }
  /* 【找价格】列表每行都显示"100"，价格一定在 slot 或它的子对象里 */
  {
    static int sscan = 0;
    /* 【开关守卫】这段是调 Text.get_text 的调试扫描，购买开关全关时一次都不跑 */
    if((g_buy_enabled || g_buy2_enabled) && self && sscan < 6){
      uint64_t sv = (uint64_t)(uintptr_t)self;
      if(sv >= 0x7000000000ULL && sv <= 0x7fffffffffULL){
        sscan++;
        char sh[96]; snprintf(sh,sizeof(sh),"[SLOT100] slot=%p scan#%d", self, sscan); logmsg(sh);
        for(int oi=0; oi<0x300; oi+=4){
          int vi = *(int*)((char*)self+oi);
          if(vi == 100){
            char fb[96]; snprintf(fb,sizeof(fb),"[SLOT100] hit +0x%03x = 100", oi); logmsg(fb);
          }
        }
        /* 【验证思路】扫描 slot 的指针字段，凡是 klass 名含 "Text" 的，
           都用 get_text() 读它显示的文字 —— 价格"100"应该就在其中 */
        for(int oi2=0; oi2<0x300; oi2+=8){
          void* sp2 = *(void**)((char*)self+oi2);
          uint64_t sp2v = (uint64_t)(uintptr_t)sp2;
          if(sp2v < 0x7000000000ULL || sp2v > 0x7fffffffffULL) continue;
          void* k2 = *(void**)sp2;
          uint64_t k2v = (uint64_t)(uintptr_t)k2;
          if(k2v < 0x7000000000ULL || k2v > 0x7fffffffffULL) continue;
          const char* kn = p_class_get_name ? p_class_get_name(k2) : NULL;
          if(!kn || !strstr(kn,"Text")) continue;
          void* gt2 = p_method_get_from_name(k2,"get_text",0);
          if(!gt2) continue;
          void* str3 = ((void*(*)(void*))(*(void**)gt2))(sp2);
          uint64_t s3v = (uint64_t)(uintptr_t)str3;
          if(s3v < 0x7000000000ULL || s3v > 0x7fffffffffULL) continue;
          int l3 = *(int*)((char*)str3+0x10);
          if(l3 <= 0 || l3 > 32) continue;
          char a3[64]; int ap3 = 0;
          for(int si3=0; si3<l3 && si3<30 && ap3<60; si3++){
            unsigned short u3 = *(unsigned short*)((char*)str3+0x14+si3*2);
            a3[ap3++] = (u3 < 128) ? (char)u3 : '?';
          }
          a3[ap3] = 0;
          char r3[160]; snprintf(r3,sizeof(r3),"[TXT2] s=%d field+0x%02x klass=%s text='%s'", sscan, oi2, kn, a3); logmsg(r3);
        }
      }
    }
  }
  if(!self || (!g_buy_enabled && !g_buy2_enabled) || g_in_setdata) return;   /* 购买1或购买2 任一开启即跑数据管道 */
  if(!g_page_active) return;   /* 不在拍卖列表页面：绝不触发 */
  if(g_fail_until && mono_ms() < g_fail_until) return;   /* 失败退避中 */
  uint64_t sp = (uint64_t)(uintptr_t)self;
  if(sp < 0x7000000000ULL || sp > 0x7fffffffffULL) return;
  long long nowt = mono_ms();
  /* 50ms 窗口：只认"每轮刷新的第一个 SetData"（= 第一行），
     窗口内其余行忽略；同一轮刷新则立即触发，不额外等待 */
  if(g_last_trigger_ms != 0 && nowt - g_last_trigger_ms < 50) return;
  g_last_trigger_ms = nowt;
  /* 【防重入】SetData 正在渲染时同步调 OnClickBuy 会触发 UI 重入 → 游戏崩溃。
     改为只登记，下一帧由主线程执行真正的点击（延迟 1 帧 ≈ 16ms）。 */
  g_pend_slot = self;
  g_has_pend = 1;
  g_direct_item = item;
  if(g_rs_n < 32){ g_rs_slot[g_rs_n] = self; g_rs_item[g_rs_n] = item; g_rs_n++; }
  g_direct_slot = self;
  g_direct_pending = 1;
  /* 入队：这一批刷新的所有行 */
  if(g_batch_n < 24){ g_batch_item[g_batch_n] = item; g_batch_slot[g_batch_n] = self; g_batch_n++; }
  g_in_setdata = 0;
  g_trig_cnt++;
  if(g_trig_cnt <= 5){
    char tb[128]; snprintf(tb,sizeof(tb),"[TRIG] OnClickBuy(slot=%p) n=%d", self, g_trig_cnt); logmsg(tb);
  }
}

static void* (*p_object_new)(void*);
static void my_confirm(void* s);
static void my_popup_enable(void* self);
/* 弹窗关闭：立刻清空实例，避免对已销毁对象反复调确认 */
static void (*orig_popup_disable)(void*) = NULL;
static void my_popup_disable(void* self){
  if(self && g_popup_inst == self){ g_popup_inst = NULL; g_popup_on = 0;
    static int pd=0; if(++pd <= 3 || (pd % 50)==0){ char db2[64]; snprintf(db2,sizeof(db2),"[popup] close n=%d", pd); logmsg(db2); } }
  if(orig_popup_disable) orig_popup_disable(self);
}

static void my_popup_enable(void* self){
  if(self){
    g_popup_inst = self;
    g_popup_on = 1;
    g_popup_confirmed = 0;      /* 每次打开重置：对象池复用也能再确认 */
    g_popup_open_ms = mono_ms();
    { static int pc=0; if(++pc <= 3 || (pc % 50)==0){ char pb[64]; snprintf(pb,sizeof(pb),"[popup] open n=%d", pc); logmsg(pb); } }
    /* 【找价格】扫描弹窗对象 0x000~0x400，找值为 100(0x64) 的 32 位字段
       弹窗上显示"单价 100"，那个 100 一定就在这个对象（或它的子对象）里 */
    {
      static int scanned = 0;
      /* 【开关守卫】这段会调 Text.get_text，自动购买关闭时不跑 */
      if(g_buy2_enabled && scanned < 3){
        scanned++;
        char fh[96]; snprintf(fh,sizeof(fh),"[FIND100] popup=%p scan#%d", self, scanned); logmsg(fh);
        for(int oi=0; oi<0x400; oi+=4){
          int vi = *(int*)((char*)self+oi);
          if(vi == 100){
            char fb[96]; snprintf(fb,sizeof(fb),"[FIND100] hit +0x%03x = 100", oi); logmsg(fb);
          }
        }
        /* 同时 dump 已知文本字段里的 text 字符串（"100"）*/
        void* tp = *(void**)((char*)self+0x1c0);
        uint64_t tpv = (uint64_t)(uintptr_t)tp;
        if(tpv >= 0x7000000000ULL && tpv <= 0x7fffffffffULL){
          char th[96]; snprintf(th,sizeof(th),"[FIND100] priceTxt obj=%p", tp); logmsg(th);
          for(int ot=0; ot<0x60; ot+=8){
            unsigned long long vt = *(unsigned long long*)((char*)tp+ot);
            char tl[128]; snprintf(tl,sizeof(tl),"[FIND100] T+%02x=%016llx", ot, vt); logmsg(tl);
          }
          /* 【正解】用 il2cpp 调 Text.get_text() 拿真实文本（不猜偏移） */
          {
            void* tcls = find_class_in_csharp("Text");
            if(!tcls) tcls = find_class_any("UnityEngine.UI","Text");
            void* gt = tcls ? p_method_get_from_name(tcls,"get_text",0) : NULL;
            if(gt){
              void* str = ((void*(*)(void*))(*(void**)gt))(tp);
              uint64_t spv2 = (uint64_t)(uintptr_t)str;
              if(spv2 >= 0x7000000000ULL && spv2 <= 0x7fffffffffULL){
                int slen2 = *(int*)((char*)str+0x10);
                if(slen2 > 0 && slen2 < 40){
                  char asc2[80]; int ap2 = 0;
                  for(int si2=0; si2<slen2 && si2<39 && ap2<70; si2++){
                    unsigned short u2 = *(unsigned short*)((char*)str+0x14+si2*2);
                    asc2[ap2++] = (u2 < 128) ? (char)u2 : '?';
                  }
                  asc2[ap2] = 0;
                  char rl2[128]; snprintf(rl2,sizeof(rl2),"[TXT] priceTxt='%s' len=%d", asc2, slen2); logmsg(rl2);
                }
              }
            } else logmsg("[TXT] get_text not found");
          }
        }
      }
    }
    /* 调试：弹窗打开瞬间，读关键字段，看数据到底有没有填好 */
    static int pdumped = 0;
    if(pdumped < 4){
      pdumped++;
      char b[320];
      snprintf(b,sizeof(b),"[POPD] btn=+1d8:%p cntTxt=+1a0:%p priceTxt=+1c0:%p priceTitle=+1b8:%p comment=+1c8:%p",
        *(void**)((char*)self+0x1d8),
        *(void**)((char*)self+0x1a0),
        *(void**)((char*)self+0x1c0),
        *(void**)((char*)self+0x1b8),
        *(void**)((char*)self+0x1c8));
      logmsg(b);
    }
  }
  if(orig_enable_popup) orig_enable_popup(self);
  /* 【已移除】OnEnable 同栈直接确认会崩溃（弹窗数据未初始化），
     改为下一帧由主线程确认（Time.get_deltaTime，约 16ms = 一帧，物理极限）*/
}

static void scan_popup_onenable(void){
  /* 【精准定位】只 hook 购买弹窗类 CeraAuctionPopup 的 OnEnable / OnDisable
     旧写法按"有 OnClickConfirm"扫类，会误抓加载/公告弹窗，
     导致 g_popup_inst 指向错误对象，购买2 对错对象调确认 → "该商品无法购买" */
  void* c = find_class_in_csharp("CeraAuctionPopup");
  if(!c) c = find_class_any("MDNF.Auction.Cera","CeraAuctionPopup");
  if(!c){ logmsg("[P-EN] CeraAuctionPopup NOT FOUND"); return; }
  logmsg("[P-EN] CeraAuctionPopup FOUND");
  void* m = p_method_get_from_name(c,"OnEnable",0);
  if(m){
    int r = DobbyHook(*(void**)m,(void*)my_popup_enable,(void**)&orig_enable_popup);
    char b[96]; snprintf(b,sizeof(b),"[P-EN] OnEnable hook=%d",r); logmsg(b);
  } else logmsg("[P-EN] no OnEnable");
  void* md = p_method_get_from_name(c,"OnDisable",0);
  if(md){
    int rd = DobbyHook(*(void**)md,(void*)my_popup_disable,(void**)&orig_popup_disable);
    char b[96]; snprintf(b,sizeof(b),"[P-DIS] OnDisable hook=%d",rd); logmsg(b);
  } else logmsg("[P-DIS] no OnDisable");
  /* 记录购买按钮方法地址 */
  void* cb = p_method_get_from_name(c,"OnConfirmBuy",0);
  if(!cb) cb = p_method_get_from_name(c,"OnClickConfirm",0);
  if(cb){ g_buy2_addr = *(void**)cb;
    char b[96]; snprintf(b,sizeof(b),"[P-EN] buy2 addr=%p", g_buy2_addr); logmsg(b); }
  { void* ri = p_method_get_from_name(c,"RefreshItemInfo",0);
    if(ri){ g_refreshitem_addr = *(void**)ri; }
    char b[144]; snprintf(b,sizeof(b),"[P-EN] refreshitem=%p", g_refreshitem_addr); logmsg(b); }
}

static void scan_confirm_class(void){
  void* dom = p_domain_get();
  if(!dom){ logmsg("[sc] no dom"); return; }
  size_t n=0;
  void** asms = (void**)p_domain_get_assemblies(dom,&n);
  if(!asms) return;
  int found=0;
  for(size_t a=0;a<n && a<400;a++){
    void* img = p_assembly_get_image(asms[a]);
    if(!img) continue;
    size_t cc = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img);
    if(cc==0 || cc>20000) continue;
    for(size_t ci=0; ci<cc && found<5; ci++){
      void* c = ((void*(*)(void*,size_t))p_image_get_class)(img, ci);
      if(!c) continue;
      void* m = p_method_get_from_name(c,"OnClickConfirm",0);
      if(m){
        void* addr = *(void**)m;
        char b[160]; snprintf(b,sizeof(b),"[CONF] addr=%p", addr); logmsg(b);
    { if(!g_confirm_addr) g_confirm_addr = addr; logmsg("[call] confirm addr via scan"); }
      /* DISABLED-HOOK: { if(!g_confirm_addr) g_confirm_addr = addr; int r=DobbyHook(addr,(void*)my_confirm,(void**)&orig_confirm); char r2[80]; snprintf(r2,sizeof(r2),"[CONF] hooked=%d",r); logmsg(r2); } */
        found++;
      }
    }
  }
  { char b[64]; snprintf(b,sizeof(b),"[CONF] found=%d",found); logmsg(b); }
}

static void scan_openbuy_all(void){
  void* dom = p_domain_get();
  if(!dom) return;
  size_t n=0;
  void** asms = (void**)p_domain_get_assemblies(dom,&n);
  if(!asms) return;
  int found=0;
  for(size_t a=0;a<n && a<400;a++){
    void* img = p_assembly_get_image(asms[a]);
    if(!img) continue;
    size_t cc = 0;
    void* (*gcc)(void*) = (void*(*)(void*))p_image_get_class_count;
    void* (*gcl)(void*,size_t) = (void*(*)(void*,size_t))p_image_get_class;
    if(!gcc||!gcl) break;
    cc = (size_t)(unsigned long)gcc(img);
    if(cc==0 || cc>20000) continue;
    for(size_t ci=0; ci<cc && found<5; ci++){
      void* c = gcl(img, ci);
      if(!c) continue;
      void* m = p_method_get_from_name(c,"OpenBuy",1);
      if(!m) m = p_method_get_from_name(c,"OpenBuy",2);
      if(m){
        void* ad = *(void**)m;
        char b[160]; snprintf(b,sizeof(b),"[OB] addr=%p argc=%d", ad,
          p_method_get_param_count?p_method_get_param_count(m):-1); logmsg(b);
        /* 启用 OpenBuy hook：只记录参数（self/item），用于确定"按商品数据定位"的调用方式 */
        if(!g_open_addr){
          g_open_addr = ad;
          int rh = DobbyHook(ad,(void*)my_openbuy,(void**)&orig_openbuy);
          char rb[96]; snprintf(rb,sizeof(rb),"[OB] hooked=%d",rh); logmsg(rb);
        }
        found++;
      }
    }
  }
  { char b[64]; snprintf(b,sizeof(b),"[OB] found=%d",found); logmsg(b); }
}

static int find_openbuy_class(void){
  const char* cands[] = {"CeraAuctionListingItem","CeraAuctionListing","CeraAuctionListItem","CeraAuctionCell","CeraAuctionPageItem", NULL};
  for(int i=0;cands[i];i++){
    void* c = find_class_in_csharp(cands[i]);
    if(!c) c = find_class_any("",cands[i]);
    if(c){
      void* m = p_method_get_from_name(c,"OpenBuy",1);
      if(!m) m = p_method_get_from_name(c,"OpenBuy",2);
      if(!m) m = p_method_get_from_name(c,"OpenBuy",0);
      if(m){
        char b[128]; snprintf(b,sizeof(b),"[ob] class=%s",cands[i]); logmsg(b);
        return 1;
      }
    }
  }
  logmsg("[ob] no class found");
  return 0;
}
static void my_popup_open(void* self){
  if(self){
    g_popup_inst = self;
    logmsg("[popup] captured via OnOpen");
  }
}
static void my_confirm(void* s){ if(s) g_popup_inst = s; if(orig_confirm) orig_confirm(s); }
/* ===== 全量 instrument 记录 ===== */
#define INST_MAX 96
static void* g_inst_addr[INST_MAX];
static const char* g_inst_name[INST_MAX];
static int g_inst_cnt = 0;

static void inst_cb(RegisterContext* ctx, const HookEntryInfo* info){
  void* addr = info ? info->target_address : NULL;
  for(int i=0;i<g_inst_cnt;i++){
    if(g_inst_addr[i]==addr){
      void* self = (void*)ctx->general.x[0];
      char b[128]; snprintf(b,sizeof(b),"[I] %s self=%p", g_inst_name[i], self); logmsg(b);
      break;
    }
  }
}

static void instrument_class(void* klass, const char* tag){
  void* it=NULL;
  void* (*next)(void*,void**) = (void*(*)(void*,void**))p_class_get_methods;
  const char* (*mname)(void*) = (const char*(*)(void*))p_method_get_name;
  int added=0;
  for(;;){
    void* m = next(klass,&it);
    if(!m) break;
    if(g_inst_cnt>=INST_MAX) break;
    const char* nm = mname(m);
    void* addr = *(void**)m;
    if(!addr || !nm) continue;
    int dup=0;
    for(int i=0;i<g_inst_cnt;i++) if(g_inst_addr[i]==addr) dup=1;
    if(dup) continue;
    g_inst_addr[g_inst_cnt]=addr; g_inst_name[g_inst_cnt]=nm; g_inst_cnt++;
    if(DobbyInstrument(addr,(DBICallTy)inst_cb)==0) added++;
  }
  char b[96]; snprintf(b,sizeof(b),"[inst] %s total=%d ok=%d", tag, g_inst_cnt, added); logmsg(b);
}

/* ===== 全自动购买：读列表 ===== */

static void enum_listing(void);
static void switch_sync(void){
  static int last = -1;
  int cur = g_buy_enabled;
  if(cur != last){
    last = cur;
    if(cur){
      int bf = open("/data/data/com.tencent.tmgp.dnf/buy_on", O_WRONLY|O_CREAT|O_TRUNC, 0666);
      if(bf >= 0){ write(bf, "1", 1); close(bf); }
      logmsg("[BUY] armed");
    } else {
      unlink("/data/data/com.tencent.tmgp.dnf/buy_on");
      logmsg("[BUY] disarmed");
    }
  }
}

static void* switch_worker(void* arg);

extern "C" void dnf_start_switch_thread(void){
  static int started = 0;
  if(started) return;
  started = 1;
  pthread_t sw;
  if(pthread_create(&sw, NULL, switch_worker, NULL) == 0) pthread_detach(sw);
}

static void* switch_worker(void* arg){
  logmsg("[SW] switch_worker STARTED");
  long long tick = 0;
  for(;;){
    switch_sync();
    tick++;
    if(tick % 50 == 0){   /* 每 5 秒打一次当前开关值 */
      char b[64]; snprintf(b,sizeof(b),"[SW] alive buy=%d", g_buy_enabled); logmsg(b);
    }
    usleep(100000);
  }
  return NULL;
}

static void* scan_worker(void* arg){
  /* 已停用：扫拍在主线程 my_update 中驱动 */
  for(;;){ usleep(100000); }
  return NULL;
}

static void* dump_thread(void* arg);

static void dump_obj(const char* tag, void* o){
  if(!o || (uint64_t)o < 0x1000){ return; }
  char b[64]; snprintf(b,sizeof(b),"[%s] %p",tag,o); logmsg(b);
  for(int off=0; off<0x60; off+=8){
    char bb[80]; snprintf(bb,sizeof(bb),"   +%02x=%016llx",off,*(unsigned long long*)((char*)o+off)); logmsg(bb);
  }
}

/* 坐标点击（so 内 fork 调 input tap） */
static void tap_xy(int x, int y){
  (void)x; (void)y;   /* 已废弃：游戏进程无 input 权限，且 fork 会死锁 */
}

/* 遍历 Transform 的子项（找列表项 UI 组件） */
static void dump_transform_children(void){
  if(!g_page_inst) return;
  /* 尝试 page 的候选 ScrollList 字段 */
  unsigned int offs[] = {0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58,0x60,0x68,0x70,0x78};
  void* tc = find_class_any("UnityEngine","Transform");
  if(!tc){ logmsg("[tf] no Transform class"); return; }
  void* gcc = p_method_get_from_name(tc,"get_childCount",0);
  void* gch = p_method_get_from_name(tc,"GetChild",1);
  if(!gcc || !gch){ logmsg("[tf] no API"); return; }
  int (*childCount)(void*) = (int(*)(void*))*(void**)gcc;
  void* (*getChild)(void*,int) = (void*(*)(void*,int))*(void**)gch;
  for(int i=0;i<12;i++){
    void* o = *(void**)((char*)g_page_inst + offs[i]);
    if(!o || (uint64_t)o < 0x1000) continue;
    /* 对象是否有 Transform 组件？（通过 component 或直接当 Transform 试） */
    int n = childCount(o);
    if(n>0 && n<100){
      char b[96]; snprintf(b,sizeof(b),"[tf] off=%02x obj=%p children=%d",offs[i],o,n); logmsg(b);
      for(int j=0;j<(n<3?n:3);j++){
        void* ch = getChild(o,j);
        char bb[80]; snprintf(bb,sizeof(bb),"     child%d=%p",j,ch); logmsg(bb);
      }
    }
  }
}

static void dump_dc_methods(void){
  void* dc = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
  if(!dc){ logmsg("[dc] no class"); return; }
  void* it = NULL;
  int n = 0;
  while(n < 200){
    void* m = p_class_get_methods(dc, &it);
    if(!m) break;
    const char* nm = p_method_get_name(m);
    if(nm){
      char b[128]; snprintf(b,sizeof(b),"[dcM] %s", nm); logmsg(b);
    }
    n++;
  }
  { char b[64]; snprintf(b,sizeof(b),"[dcM] count=%d",n); logmsg(b); }
}

static void dump_c8(void){
  if(!g_page_inst) return;
  void* lst = *(void**)((char*)g_page_inst + 0xc8);
  if(!lst) return;
  void* arr = *(void**)((char*)lst + 0x10);
  int size = (int)(*(long long*)((char*)lst + 0x18) & 0xFFFFFFFF);
  { char b[96]; snprintf(b,sizeof(b),"[C8] lst=%p arr=%p size=%d",lst,arr,size); logmsg(b); }
  if(!arr || size<=0 || size>50) return;
  for(int i=0;i<(size<3?size:3);i++){
    void* it = *(void**)((char*)arr + 0x20 + i*8);
    if(!it) continue;
    char b[64]; snprintf(b,sizeof(b),"[C8i%d] %p",i,it); logmsg(b);
    if(i==0) g_c8_first = it;
    for(int off=0; off<0x40; off+=8){
      char bb[72]; snprintf(bb,sizeof(bb),"   +%02x=%016llx",off,*(unsigned long long*)((char*)it+off)); logmsg(bb);
    }
  }
}

static void dump_multi(void){
  if(!g_page_inst) return;
  unsigned int offs[] = {0x10,0x20,0x28,0x30,0x38,0x40,0x48,0x50,0x58,0x60,0x68,0x70,0x78,0x80,0x88,0x90,0x98,0xA0,0xA8,0xB0,0xB8,0xC0,0xC8,0xD0};
  for(int i=0;i<24;i++){
    void* o = *(void**)((char*)g_page_inst + offs[i]);
    if(!o || (uint64_t)o < 0x1000 || (uint64_t)o > 0x7fffffffffULL) continue;
    void* a = *(void**)((char*)o + 0x10);
    long long sz = *(long long*)((char*)o + 0x18);
    int size = (int)(sz & 0xFFFFFFFF);
    if(a && (uint64_t)a > 0x1000 && size>0 && size<100){
      char b[128]; snprintf(b,sizeof(b),"[M] +%02x obj=%p arr=%p size=%d",offs[i],o,a,size); logmsg(b);
    }
  }
}

static void dump_cand(void){
  if(!g_page_inst) return;
  void* c28 = *(void**)((char*)g_page_inst + 0x28);
  void* cf8 = *(void**)((char*)g_page_inst + 0xf8);
  dump_obj("C28", c28);
  dump_obj("CF8", cf8);
}

static void dump_page_fields(void){
  if(!g_page_inst){ logmsg("[pg] no page"); return; }
  char b[64]; snprintf(b,sizeof(b),"[pg] page=%p",g_page_inst); logmsg(b);
  for(int off=0; off<0x120; off+=8){
    unsigned long long v = *(unsigned long long*)((char*)g_page_inst+off);
    char bb[80]; snprintf(bb,sizeof(bb),"  P+%03x=%016llx",off,v); logmsg(bb);
  }
}

static void dump_scroll(void){
  if(!g_page_inst){ logmsg("[sc] no page"); return; }
  void* sl = *(void**)((char*)g_page_inst + 0x18);
  { char b[96]; snprintf(b,sizeof(b),"[sc] scrollList=%p",sl); logmsg(b); }
  if(!sl || (uint64_t)sl < 0x1000) return;
  for(int off=0; off<0x80; off+=8){
    char b[64]; snprintf(b,sizeof(b),"  S+%02x=%016llx",off,*(unsigned long long*)((char*)sl+off));
    logmsg(b);
  }
}

static void dump_listing(void){
  if(!g_page_inst){ logmsg("[auto] no page"); return; }
  void* lst = *(void**)((char*)g_page_inst + 0xF8);
  if(!lst || (uint64_t)lst < 0x1000){ logmsg("[auto] no list"); return; }
  void* arr = *(void**)((char*)lst + 0x10);
  int size = *(int*)((char*)lst + 0x18);
  char b[128]; snprintf(b,sizeof(b),"[auto] list=%p arr=%p size=%d",lst,arr,size); logmsg(b);
  if(!arr || size<=0 || size>100) return;
  int n = size<5?size:5;
  for(int i=0;i<n;i++){
    void* item = *(void**)((char*)arr + 0x20 + i*8);
    if(!item || (uint64_t)item < 0x1000) continue;
    char bb[256]; snprintf(bb,sizeof(bb),"[item%d] %p: ",i,item);
    logmsg(bb);
    for(int off=0; off<0x40; off+=8){
      char cc[64]; snprintf(cc,sizeof(cc),"  +%02x = %016llx",off,*(unsigned long long*)((char*)item+off));
      logmsg(cc);
    }
  }
}


static void enum_dc(void){
  void* dcx = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
  if(!dcx){ logmsg("[edc] no class"); return; }
  void* it=NULL; int n=0;
  for(;;){
    void* m = p_class_get_methods(dcx,&it);
    if(!m) break;
    const char* nm = p_method_get_name(m);
    int pc = p_method_get_param_count ? p_method_get_param_count(m) : -1;
    if(nm){
      char b[160]; snprintf(b,sizeof(b),"[DC] %s(%d)",nm,pc); logmsg(b);
      n++;
    }
    if(n>150) break;
  }
  { char b[64]; snprintf(b,sizeof(b),"[edc] n=%d",n); logmsg(b); }
}

static void call_get_listings(void){
  if(!g_dc_inst){ logmsg("[gl] no dc"); return; }
  void* dcx = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
  if(!dcx){ logmsg("[gl] no dc class"); return; }
  void* mm = p_method_get_from_name(dcx,"GetCategoryListings",1);
  if(!mm){ logmsg("[gl] no method"); return; }
  void* addr = *(void**)mm;
  void* (*fn)(void*, int) = (void*(*)(void*,int))addr;
  int cat = 10;
  void* lst = fn(g_dc_inst, cat);
  { char b[96]; snprintf(b,sizeof(b),"[gl] list=%p",lst); logmsg(b); }
  if(!lst) return;
  /* dump list + first items */
  void* arr = *(void**)((char*)lst + 0x10);
  long long ssz = *(long long*)((char*)lst + 0x18);
  int size = (int)(ssz & 0xFFFFFFFF);
  { char b[96]; snprintf(b,sizeof(b),"[gl] arr=%p size=%d",arr,size); logmsg(b); }
  if(arr && size>0 && size<200){
    for(int i=0;i<(size<3?size:3);i++){
      void* item = *(void**)((char*)arr + 0x20 + i*8);
      if(!item) continue;
      { char b[64]; snprintf(b,sizeof(b),"[it%d] %p",i,item); logmsg(b); }
      for(int off=0; off<0x60; off+=8){
        char b[72]; snprintf(b,sizeof(b),"  +%02x=%016llx",off,*(unsigned long long*)((char*)item+off));
        logmsg(b);
      }
    }
  }
}

static void enum_listing(void){
  void* cls = find_class_any("MDNF.Auction.Cera","CeraAuctionListingData");
  if(!cls) cls = find_class_any("","CeraAuctionListingData");
  if(!cls) cls = find_class_in_csharp("CeraAuctionListingData");
  if(!cls){ logmsg("[enum] no ListingData class"); return; }
  void* it=NULL;
  int n=0;
  for(;;){
    void* m = p_class_get_methods(cls,&it);
    if(!m) break;
    const char* nm = p_method_get_name(m);
    if(nm){
      char b[128]; snprintf(b,sizeof(b),"[LM] %s",nm); logmsg(b);
      n++;
    }
    if(n>200) break;
  }
  { char b[64]; snprintf(b,sizeof(b),"[enum] methods=%d",n); logmsg(b); }
}

static void* dump_thread(void* arg){
  sleep(5);
  dump_dc_methods();
  enum_dc();
  call_get_listings();
  enum_listing();
  dump_scroll();
  dump_listing();
  return NULL;
}

static void my_next(void* s){ logmsg("[HIT] NextPage"); if(orig_next) orig_next(s); }
static void my_prev(void* s){ logmsg("[HIT] PrevPage"); if(orig_prev) orig_prev(s); }
static void my_cat(void* s, long idx){
  FILE* lf = fopen("/var/mobile/Documents/xys/xs_cat.log", "a");
  if(lf){ fprintf(lf, "OnClickCategory idx=%ld\n", idx); fclose(lf); }
  if(orig_cat) orig_cat(s, idx);
}
static void my_sortt(void* s){
  if(s) g_sortt_inst = s;
  if(orig_sortt) orig_sortt(s);
}
static void my_sortp(void* s){ logmsg("[HIT] SortPrice"); if(orig_sortp) orig_sortp(s); }
static void my_detail(void* self){
  /* 记录"详细搜索"被点击的时刻，供 my_rs 判定下一次 RequestSearch 是否来自详细搜索 */
  g_detail_ms = mono_ms();
  if(orig_detail) orig_detail(self);
}
static void my_clicksearch(void* self){
  char m[96]; snprintf(m,sizeof(m),"[HIT] OnClick_Search self=%p",self); logmsg(m);
  if(self){ g_search_inst = self; logmsg("[search] instance updated"); }
  if(orig_clicksearch) orig_clicksearch(self);
}
static void my_enable(void* self){
  /* install_probe();  已移除：il2cpp 方法枚举会导致闪退 */
  if(self){
    g_page_inst = self;
    g_page_active = 1;      /* 拍卖列表页已激活：只有此时才允许触发购买 */
    __sync_synchronize();
    /* 【首次加速】页面刚激活就立刻做一次搜索：
       这样 ParseSearchResult 马上触发 → g_gc_inst 立刻有值，
       不用等游戏自己慢吞吞地发第一轮请求。 */
    /* 【开关守卫】扫拍开关关闭时，进页面不主动发任何搜索 */
    {
      void** srcE = g_rs_detail_valid ? g_rs_detail_args : g_rs_args;
      if(g_enabled2 && g_use_search && srcE[0] && g_rs_addr){
        void (*fnE)(void*,long,void*,void*,long,long,void*,long,long)
          = (void(*)(void*,long,void*,void*,long,long,void*,long,long))g_rs_addr;
        fnE(srcE[0],(long)srcE[1],srcE[2],srcE[3],
            (long)srcE[4],(long)srcE[5],srcE[6],
            (long)srcE[7],(long)srcE[8]);
      }
    }
    /* 同时把刷新按钮也点一次（双保险）——同样只在扫拍开启、且「按钮」方式被勾选时 */
    if(g_enabled2 && g_use_click && g_click_addr){
      ((void(*)(void*))g_click_addr)(self);
    }
  }
  if(orig_enable) orig_enable(self);
}
static void my_page_disable(void* self){
  g_page_active = 0;        /* 离开拍卖页：停止触发 */
  g_popup_inst = NULL;      /* 页面失活 → 弹窗可能被销毁，清掉引用防野指针 */
  g_popup_on = 0;
  g_popup_confirmed = 0;
  if(orig_page_disable) orig_page_disable(self);
}
static void my_enter(void* self){
  if(self){ g_enter_inst = self; }
  if(orig_enter) orig_enter(self);
}
static void my_sbtn(void* self){
  if(self){ g_sbtn_inst = self; }
  if(orig_sbtn) orig_sbtn(self);
}
static void (*orig_onresbuy)(void*,void*,void*,void*,void*,void*) = NULL;
static void my_onresbuy(void* s, void* a1, void* a2, void* a3, void* a4, void* a5){
  char b[200];
  snprintf(b,sizeof(b),"[RESBUY] s=%p a1=%p a2=%p a3=%p a4=%p a5=%p", s, a1, a2, a3, a4, a5);
  logmsg(b);
  if(orig_onresbuy) orig_onresbuy(s, a1, a2, a3, a4, a5);
}

static void my_click(void* self){
  if(self){
    g_page_inst = self;
    /* 高频日志已静音（会淹没购买参数采集） */
  }
  if(orig_click) orig_click(self);
}


static unsigned char g_sbuf[1<<20];
static void* scan_once(void* klass){
  FILE *f=fopen("/proc/self/maps","r");
  if(!f) return NULL;
  char ln[1024]; uint64_t k=(uint64_t)klass; void *found=NULL;
  while(fgets(ln,sizeof(ln),f) && !found){
    uint64_t s,e,off; char pm[8]={0};
    if(sscanf(ln,"%llx-%llx %7s %llx",&s,&e,pm,&off)!=4) continue;
    if(pm[0]!='r'||pm[1]!='w') continue;
    if(strstr(ln,"/dev/")||strstr(ln,".so")) continue;
    uint64_t sz=e-s; if(sz>0x20000000ULL) continue;
    for(uint64_t o=0;o<sz && !found;){
      size_t chunk=(sz-o>sizeof(g_sbuf))?sizeof(g_sbuf):(size_t)(sz-o);
      memcpy(g_sbuf,(void*)(s+o),chunk);
      for(size_t i=0;i+8<=chunk;i+=8){
        if(*(uint64_t*)(g_sbuf+i)==k){
          void *cand=(void*)(s+o+i);
          if(*(uint64_t*)cand==k){ found=cand; break; }
        }
      }
      o+=chunk;
    }
  }
  fclose(f);
  return found;
}


static void search_tip_strings(void){
  const char *pat8 = "\xE6\x93\x8D\xE4\xBD\x9C\xE9\xA2\x91\xE7\xB9\x81";  /* UTF-8 */
  const char *pat16 = "\xCD\x64\x5C\x4F\x91\x98\x41\x7E";  /* 操作频繁 UTF-16LE */
  FILE *f=fopen("/proc/self/maps","r");
  if(!f) return;
  char ln[1024]; int found=0;
  unsigned char *buf = (unsigned char*)malloc(1<<20);
  while(fgets(ln,sizeof(ln),f)){
    if(!strstr(ln,"r")) continue;
    unsigned long long s0,e0,off; char pm[8]={0};
    if(sscanf(ln,"%llx-%llx %7s %llx",&s0,&e0,pm,&off)!=4) continue;
    if(strchr(pm,'x')) continue;
    unsigned long long sz=e0-s0;
    if(sz > 0x40000000ULL) continue;
    for(unsigned long long o=0;o<sz;o+= (1<<20)){
      size_t chunk=(sz-o>(1<<20))?(1<<20):(size_t)(sz-o);
      memcpy(buf,(void*)(s0+o),chunk);
      for(size_t i=0;i+11<=chunk;i++){
        if(memcmp(buf+i,pat16,8)==0){
          char m[128]; snprintf(m,sizeof(m),"[str16] \"操作频繁\" at %llx",(unsigned long long)(s0+o+i)); logmsg(m);
          found++;
        }
        if(memcmp(buf+i,pat8,11)==0){
          char m[128]; snprintf(m,sizeof(m),"[str] \"操作频繁\" at %llx",(unsigned long long)(s0+o+i)); logmsg(m);
          found++;
        }
      }
    }
  }
  free(buf); fclose(f);
  { char m[64]; snprintf(m,sizeof(m),"[str] found=%d",found); logmsg(m); }
}

static void do_init(void){
  logmsg("[init] start (dobby)");
  scan_iv_load();   /* 读取玩家上次设置的刷新间隔；无存档则保持默认 1.10ms */
  { pthread_t tw; int rc = pthread_create(&tw,NULL,scan_worker,NULL);
    char tb[96]; snprintf(tb,sizeof(tb),"[SW] worker rc=%d", rc); logmsg(tb);
    if(rc==0) pthread_detach(tw); }
  void *page_cls = find_class_in_csharp("CeraAuctionListPageBase");
  if(!page_cls) page_cls = find_class_any("MDNF.Auction.Cera","CeraAuctionListPageBase");
  g_diag_page = page_cls ? 1 : 0;
  if(!page_cls){ logmsg("[err] page class"); return; }
  void *pmi = p_method_get_from_name(page_cls,"OnClickRefresh",0);
  if(pmi){ g_click_addr = *(void**)pmi; }
  g_diag_clk = g_click_addr ? 1 : 0;
  { void* m = p_method_get_from_name(page_cls,"OnClickEnter_Search",0);
    if(m){ g_enter_addr = *(void**)m;
      int r = DobbyHook(g_enter_addr,(void*)my_enter,(void**)&orig_enter);
      char b[96]; snprintf(b,sizeof(b),"[hk] EnterSearch=%d",r); logmsg(b); } }
  { void* m = p_method_get_from_name(page_cls,"OnClickSearchButton",0);
    if(m){ g_sbtn_addr = *(void**)m;
      int r = DobbyHook(g_sbtn_addr,(void*)my_sbtn,(void**)&orig_sbtn);
      char b[96]; snprintf(b,sizeof(b),"[hk] SearchBtn=%d",r); logmsg(b); } }
  void *upd_cls = find_class_any("com.tencent.pandora","AbstractTcpClient");
  if(!upd_cls) upd_cls = find_class_any("","AbstractTcpClient");
  if(!upd_cls) upd_cls = find_class_any("com.tencent.pandora","TcpClient");
  if(upd_cls){
    void *umi = p_method_get_from_name(upd_cls,"Update",0);
    if(!umi) umi = p_method_get_from_name(upd_cls,"update",0);
    if(!umi) umi = p_method_get_from_name(upd_cls,"LateUpdate",0);
    if(!umi) umi = p_method_get_from_name(upd_cls,"Tick",0);
    if(umi){ g_update_addr = *(void**)umi; }
  }
  g_diag_upd = g_update_addr ? 1 : 0;
  char m[160]; snprintf(m,sizeof(m),"click=%p update=%p",g_click_addr,g_update_addr); logmsg(m);
  if(0){   /* 【已禁用】ParseSearchResult hook 会触发游戏「发生错误 游戏结束」 */
    /* 探针：遍历多个候选容器类，给每个类的 ParseSearchResult 都挂钩子 */
    const char* cands[] = {"CeraAuctionGoodsContainer","CeraAuctionNoticeContainer",
                           "CeraAuctionFavoritesContainer","CeraAuctionHistoryContainer", NULL};
    for(int ci=0; cands[ci]; ci++){
      void* gcls = find_class_in_csharp(cands[ci]);
      if(!gcls) gcls = find_class_any("MDNF.Auction.Cera", cands[ci]);
      if(!gcls){ char nb[128]; snprintf(nb,sizeof(nb),"[PARS] %s NOT FOUND",cands[ci]); logmsg(nb); continue; }
      { char fb[128]; snprintf(fb,sizeof(fb),"[PARS] %s FOUND",cands[ci]); logmsg(fb); }
      void* itg = NULL; void* mg;
      while((mg = p_class_get_methods(gcls,&itg))){
        const char* nmg = p_method_get_name(mg);
        if(!nmg) continue;
        if(strcmp(nmg,"ParseSearchResult")!=0) continue;
        void* adg = *(void**)mg;
        if(!adg) continue;
        g_parse_addr = adg;
        int rp = DobbyHook(adg,(void*)my_parse_result,(void**)&orig_parse);
        char pb[160]; snprintf(pb,sizeof(pb),"[PARS] hook %s.ParseSearchResult argc=%d ret=%d",
          cands[ci], p_method_get_param_count?p_method_get_param_count(mg):-1, rp); logmsg(pb);
        break;
      }
    }
  }
  {
    /* 只枚举按钮类的方法名（零风险，不 hook，不读内存） */
    const char* enumCls[] = {"CeraAuctionBuySlotItem","CeraAuctionBuySimpleSlotItem",
                             "CeraAuctionBuyPage","CeraAuctionListPageBase", NULL};
    for(int ei=0; enumCls[ei]; ei++){
      void* ec = find_class_in_csharp(enumCls[ei]);
      if(!ec){ char nb[128]; snprintf(nb,sizeof(nb),"[ENUM] %s NOT FOUND",enumCls[ei]); logmsg(nb); continue; }
      { char fb[128]; snprintf(fb,sizeof(fb),"[ENUM] ==== %s ====",enumCls[ei]); logmsg(fb); }
      void* ite = NULL; void* me;
      int cnt = 0;
      while((me = p_class_get_methods(ec,&ite)) && cnt < 80){
        const char* nme = p_method_get_name(me);
        if(!nme) continue;
        if(!strstr(nme,"Click") && !strstr(nme,"Buy") && !strstr(nme,"On")) continue;
        char mb[128]; snprintf(mb,sizeof(mb),"[ENUM] %s argc=%d", nme,
          p_method_get_param_count?p_method_get_param_count(me):-1); logmsg(mb);
        cnt++;
      }
    }
  }
  {
    /* 只 hook「购买」按钮：CeraAuctionBuySlotItem.OnClickBuy(this) */
    const char* sc[] = {"CeraAuctionBuySlotItem","CeraAuctionBuySimpleSlotItem", NULL};
    for(int si=0; sc[si]; si++){
      void* cs = find_class_in_csharp(sc[si]);
      if(!cs) continue;
      void* mcs = p_method_get_from_name(cs,"OnClickBuy",0);
      if(!mcs) continue;
      g_clickbuy_addr = *(void**)mcs;
      int rc2 = DobbyHook(g_clickbuy_addr,(void*)my_clickbuy,(void**)&orig_clickbuy);
      char cbb[144]; snprintf(cbb,sizeof(cbb),"[CB] %s.OnClickBuy hook=%d", sc[si], rc2); logmsg(cbb);
      break;
    }
  }
  {
    /* SetData 钩子：列表项渲染时自动捕获 slot 实例（零点击） */
    void* scls = find_class_in_csharp("CeraAuctionBuySlotItem");
    if(!scls) scls = find_class_any("MDNF.Auction.Cera","CeraAuctionBuySlotItem");
    if(!scls) scls = find_class_in_csharp("CeraAuctionBuySimpleSlotItem");
    if(scls){
      void* its = NULL; void* ms; int hs = 0;
      while((ms = p_class_get_methods(scls,&its))){
        const char* nms = p_method_get_name(ms);
        if(!nms) continue;
        if(strcmp(nms,"SetData")!=0) continue;
        void* ads = *(void**)ms;
        if(!ads) continue;
        g_setdata_addr = ads;
        int rs = DobbyHook(ads,(void*)my_setdata,(void**)&orig_setdata);
        char sb[128]; snprintf(sb,sizeof(sb),"[ITEM] SetData argc=%d hook=%d",
          p_method_get_param_count?p_method_get_param_count(ms):-1, rs); logmsg(sb);
        hs++;
        break;
      }
      if(!hs) logmsg("[ITEM] no SetData");
    } else logmsg("[ITEM] SlotItem class not found");
  }
  {
    /* 安装 OpenBuy 钩子（只记录参数，用于校准 self 类型；不改任何行为）*/
    void* occls = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
    void* oc = occls ? p_method_get_from_name(occls,"OpenBuy",2) : NULL;
    if(!oc && occls) oc = p_method_get_from_name(occls,"OpenBuy",1);
    if(!oc && occls) oc = p_method_get_from_name(occls,"OpenBuy",0);
    if(oc){
      g_open_addr = *(void**)oc;
      int ro = DobbyHook(g_open_addr,(void*)my_openbuy,(void**)&orig_openbuy);
      char ob[128]; snprintf(ob,sizeof(ob),"[OB2] hooked=%d argc=%d", ro,
        p_method_get_param_count?p_method_get_param_count(oc):-1); logmsg(ob);
    } else logmsg("[OB2] OpenBuy not found in controller");
  }
  /* 【已禁用】GetListings 钩子会导致游戏「发生错误 游戏结束」，不再启用 */
  if(0){
    void* bcls = find_class_in_csharp("CeraAuctionBuyPage");
    if(!bcls) bcls = find_class_any("MDNF.Auction.Cera","CeraAuctionBuyPage");
    if(bcls){
      void* itl = NULL; void* ml; int got = 0;
      while((ml = p_class_get_methods(bcls,&itl))){
        const char* nml = p_method_get_name(ml);
        if(!nml) continue;
        if(!strstr(nml,"GetListings") && !strstr(nml,"get_Listings")) continue;
        void* adl = *(void**)ml;
        if(!adl) continue;
        char cb[128]; snprintf(cb,sizeof(cb),"[LIST] cand %s argc=%d", nml,
          p_method_get_param_count?p_method_get_param_count(ml):-1); logmsg(cb);
        if(!got){
          g_getlist_addr = adl;
          int rg = DobbyHook(adl,(void*)my_getlistings,(void**)&orig_getlist);
          char gb[96]; snprintf(gb,sizeof(gb),"[LIST] hooked=%d",rg); logmsg(gb);
          got++;
        }
      }
      { char fb[64]; snprintf(fb,sizeof(fb),"[LIST] cands=%d",got); logmsg(fb); }
    } else logmsg("[LIST] BuyPage not found");
  }
  {
    void* pop_cls = find_class_in_csharp("CeraAuctionPopup");
    if(pop_cls){
      void* b1 = p_method_get_from_name(pop_cls,"OnClickConfirm",0);
    if(b1){ g_confirm_addr = *(void**)b1; logmsg("[call] OnClickConfirm addr"); }
      /* DISABLED-HOOK: if(b1){ g_confirm_addr = *(void**)b1; int r=DobbyHook(g_confirm_addr,(void*)my_confirm,(void**)&orig_confirm); char b[96]; snprintf(b,sizeof(b),"[hk] Confirm=%d",r); logmsg(b); } */
      void* dc2 = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
      void* nb = dc2 ? p_method_get_from_name(dc2,"OnNotifyBuyResult",1) : NULL;
    /* [BUY-HOOK-DISABLED] if(nb){ int rn=DobbyHook(*(void**)nb,(void*)my_notify_buy,(void**)&orig_notify); char bn[96]; snprintf(bn,sizeof(bn),"[hk] NotifyBuy=%d",rn); logmsg(bn); } */
      {
        void* oc = dc2 ? p_method_get_from_name(dc2,"OpenBuy",2) : NULL;
        if(!oc){ if(dc2) oc = p_method_get_from_name(dc2,"OpenBuy",1); }
    if(oc){ g_open_addr = *(void**)oc; logmsg("[call] OpenBuy addr"); }
      /* DISABLED-HOOK: if(oc){ g_open_addr = *(void**)oc; int ro=DobbyHook(g_open_addr,(void*)my_openbuy,(void**)&orig_openbuy); char bo[96]; snprintf(bo,sizeof(bo),"[hk] OpenBuy=%d",ro); logmsg(bo); } */
        else logmsg("[err] no OpenBuy in dc");
      }
      scan_openbuy_all();
      /* OpenBuy 不在 dc 里，在页面类里：用 g_page_inst 的类来找 */
      if(!g_open_addr && g_page_inst){
        void* pgcls = *(void**)g_page_inst;
        if(pgcls){
          void* oc2 = NULL;
          for(int pc=0; pc<=3 && !oc2; pc++){
            oc2 = p_method_get_from_name(pgcls, "OpenBuy", pc);
          }
          if(oc2){
            g_open_addr = *(void**)oc2;
            logmsg("[call] OpenBuy from page class");
          } else {
            logmsg("[err] OpenBuy not in page class either");
          }
        }
      }
      scan_confirm_class();
      scan_popup_onenable();
      void* rb = dc2 ? p_method_get_from_name(dc2,"RequestBuyItem",5) : NULL;
    if(rb){ g_rbuy_addr = *(void**)rb;
      { unsigned char* cp = (unsigned char*)g_rbuy_addr;
        char hb[128]; snprintf(hb,sizeof(hb),"[RBCODE] addr=%p", g_rbuy_addr); logmsg(hb);
        for(int i=0; i<256; i+=16){
          char lb[220];
          snprintf(lb,sizeof(lb),"[RBCODE] +%02x: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
            i, cp[i],cp[i+1],cp[i+2],cp[i+3], cp[i+4],cp[i+5],cp[i+6],cp[i+7],
            cp[i+8],cp[i+9],cp[i+10],cp[i+11], cp[i+12],cp[i+13],cp[i+14],cp[i+15]);
          logmsg(lb);
        }
      }
      int rr=DobbyHook(g_rbuy_addr,(void*)my_rbuy,(void**)&orig_rbuy); char bb[96]; snprintf(bb,sizeof(bb),"[hk] RequestBuyItem=%d",rr); logmsg(bb); }
      { void* om = p_method_get_from_name(pop_cls,"OnOpen",0);
      if(om){ char bo[64]; snprintf(bo,sizeof(bo),"[hk] PopupOnOpen=skip"); logmsg(bo); } }
      void* b2 = p_method_get_from_name(pop_cls,"OnConfirmBuy",0);
      if(b2){ g_buy2_addr = *(void**)b2; int r=DobbyHook(g_buy2_addr,(void*)my_buy,(void**)&orig_buy); char b[96]; snprintf(b,sizeof(b),"[hk] Buy=%d addr=%p",r,g_buy2_addr); logmsg(b); }
    } else logmsg("[err] popup class");
  }
  {
    /* instrument disabled (conflicts with DobbyHook) */
  }
  {
    void* dcx = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
    if(dcx){
      { void* ic = p_method_get_from_name(dcx,"IsRefreshCoolTime",1);
        if(!ic) ic = p_method_get_from_name(dcx,"IsRefreshCoolTime",0);
        if(ic){ g_iscool_addr = *(void**)ic; int ri=DobbyHook(g_iscool_addr,(void*)my_iscool,(void**)&orig_iscool);
          char cb[80]; snprintf(cb,sizeof(cb),"[hk] IsRefreshCoolTime=%d",ri); logmsg(cb); } }
      void* rm = p_method_get_from_name(dcx,"ResetDetailSearchCoolTime",0);
      if(rm){ g_resetcd_addr = *(void**)rm; logmsg("[cd] ResetDetailSearchCoolTime found"); }
    }
  }
  logmsg("[nomsg] no scan; waiting for user click to capture instance");
  /* dump_thread 已移除：内存遍历会导致闪退 */
  /* 关键容错：以前这里只要缺一个地址就整体 return，导致后续所有 hook 都不装、
     面板正常但功能全无效果。现在改成"缺哪个跳过相关功能，其余照装"。 */
  if(!g_click_addr) logmsg("[err] no click");
  if(!g_update_addr) logmsg("[err] no update (scan will fallback to main thread)");
  void *dc_cls = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
  int r3 = -1;
  if(dc_cls){
    void *rmi = p_method_get_from_name(dc_cls,"GetRefreshRemainCoolTime",0);
    if(rmi){ g_remain_addr = *(void**)rmi; r3 = DobbyHook(g_remain_addr, (void*)my_remain, (void**)&orig_remain); }
  }
  int r5 = -1, r6 = -1, r7 = -1, r8 = -1, r9 = -1, r10 = -1;
  {
    void *m1=p_method_get_from_name(page_cls,"OnClickNextPage",0);
    if(m1){ g_next_addr=*(void**)m1; DobbyHook(g_next_addr,(void*)my_next,(void**)&orig_next); logmsg("[hook] NextPage"); }
    void *m2=p_method_get_from_name(page_cls,"OnClickPrevPage",0);
    if(m2){ g_prev_addr=*(void**)m2; DobbyHook(g_prev_addr,(void*)my_prev,(void**)&orig_prev); logmsg("[hook] PrevPage"); }
    void *m3=p_method_get_from_name(page_cls,"OnClickCategory",1);
    if(!m3) m3 = p_method_get_from_name(page_cls,"OnClickCategory",0);   /* 兼容 argc=0 */
    logmsg_force(m3 ? "[hook] Category argc=1 OK" : "[hook] Category NOT FOUND");
    if(m3){ g_cat_addr=*(void**)m3; DobbyHook(g_cat_addr,(void*)my_cat,(void**)&orig_cat); logmsg("[hook] Category"); }
    /* 排序/价格：方法名可能有多种写法，逐个试，成功后记下用的是哪个 */
    const char* kTimeNames[]  = {"OnClickSortTime","OnClickSortByTime","OnSortTime","OnClickTimeSort",NULL};
    const char* kPriceNames[] = {"OnClickSortPrice","OnClickSortByPrice","OnSortPrice","OnClickPriceSort","SortByPrice",NULL};
    for(int ti=0; kTimeNames[ti]; ti++){
      void *m=p_method_get_from_name(page_cls,kTimeNames[ti],0);
      if(m){
        g_sortt_addr=*(void**)m;
        DobbyHook(g_sortt_addr,(void*)my_sortt,(void**)&orig_sortt);
        char hb[128]; snprintf(hb,sizeof(hb),"[hook] SortTime = %s", kTimeNames[ti]); logmsg_force(hb);
        break;
      }
    }
    if(!g_sortt_addr) logmsg_force("[hook] SortTime = NOT FOUND (tried 4 names)");
    for(int pi=0; kPriceNames[pi]; pi++){
      void *m=p_method_get_from_name(page_cls,kPriceNames[pi],0);
      if(m){
        g_sortp_addr=*(void**)m;
        DobbyHook(g_sortp_addr,(void*)my_sortp,(void**)&orig_sortp);
        char hb[128]; snprintf(hb,sizeof(hb),"[hook] SortPrice = %s", kPriceNames[pi]); logmsg_force(hb);
        break;
      }
    }
    if(!g_sortp_addr) logmsg_force("[hook] SortPrice = NOT FOUND (tried 5 names)");

    {
      void *mC = p_method_get_from_name(page_cls, "CanSort", 0);
      if(mC){ g_cansort_addr = *(void**)mC; DobbyHook(g_cansort_addr,(void*)my_cansort,(void**)&orig_cansort); logmsg_force("[hook] CanSort OK"); }
      else logmsg_force("[hook] CanSort NOT FOUND");
      void *mR = p_method_get_from_name(page_cls, "SortSearchResult", 1);
      if(mR){ g_sortsr_addr = *(void**)mR; DobbyHook(g_sortsr_addr,(void*)my_sortsr,(void**)&orig_sortsr); logmsg_force("[hook] SortSearchResult OK"); }
      else logmsg_force("[hook] SortSearchResult NOT FOUND");
    }
    /* RequestSearchPage：分页请求，点表头可能触发 */
    if(g_rs_page_addr){
      DobbyHook(g_rs_page_addr, (void*)my_rs_page, (void**)&orig_rs_page);
      logmsg_force("[hook] RequestSearchPage OK");
    } else logmsg_force("[hook] RequestSearchPage addr NULL");
    /* 拍卖表头点击真正走 OnSortChanged(argc=2)，参数=排序字段+方向 */
    {
      void *mS = p_method_get_from_name(page_cls, "OnSortChanged", 2);
      if(mS){
        g_sortchg_addr = *(void**)mS;
        DobbyHook(g_sortchg_addr, (void*)my_sortchanged, (void**)&orig_sortchanged);
        logmsg_force("[hook] OnSortChanged OK");
      } else {
        logmsg_force("[hook] OnSortChanged NOT FOUND");
      }
    }
    /* 【枚举】把 page_cls 里含 Sort/Price/Time/Cool/Order 关键词的方法名全记下来，
       这些名字 5 个候选都没命中，说明真名不在这套命名里。 */
    {
      void* mit = NULL;
      void* mi2;
      int mc = 0, hitn = 0;
      while((mi2 = p_class_get_methods(page_cls, &mit)) != NULL && mc < 500){
        const char* nm = p_method_get_name(mi2);
        if(nm){
          if(strstr(nm,"Sort") || strstr(nm,"Price") || strstr(nm,"Time")
             || strstr(nm,"Cool") || strstr(nm,"Order") || strstr(nm,"Click")){
            int pc2 = p_method_get_param_count(mi2);
            char mb[192];
            snprintf(mb,sizeof(mb),"[M] %s argc=%d", nm, pc2);
            logmsg_force(mb);
            hitn++;
          }
        }
        mc++;
      }
      char cb[128];
      snprintf(cb,sizeof(cb),"[M] total_methods=%d matched=%d", mc, hitn);
      logmsg_force(cb);
    }

  }
  {
    void *text_cls = find_class_any("UnityEngine.UI","Text");
    if(text_cls){
      void *st = p_method_get_from_name(text_cls,"set_text",1);
      if(st){ g_settext_addr = *(void**)st; r10 = DobbyHook(g_settext_addr, (void*)my_settext, (void**)&orig_settext); }
    }
  }
  /* 【已删除】IsDetailSearchCoolTime / GetDetailSearchRemainCoolTime 的破解 hook
     冷却破解功能按要求整体移除，这两个方法恢复游戏原生行为。 */
  if(dc_cls){
    void *rmi2 = p_method_get_from_name(dc_cls,"RequestSearch",8);
    if(rmi2){
      g_rs_addr = *(void**)rmi2;
      r7 = DobbyHook(g_rs_addr, (void*)my_rs, (void**)&orig_rs);
      g_diag_rs = 1;
      g_diag_hk_rs = (r7 == 0) ? 1 : 0;
    }
  }
  {
    void *dmi = p_method_get_from_name(page_cls,"OnClickDetailSearch",0);
    if(dmi){ g_detail_addr = *(void**)dmi; r5 = DobbyHook(g_detail_addr, (void*)my_detail, (void**)&orig_detail); }
    void *sp_cls = find_class_in_csharp("CeraAuctionSearchPopup");
    if(sp_cls){
      void *smi = p_method_get_from_name(sp_cls,"OnClick_Search",0);
      if(smi){ g_clicksearch_addr = *(void**)smi; r6 = DobbyHook(g_clicksearch_addr, (void*)my_clicksearch, (void**)&orig_clicksearch); }
    }
  }
  int r4 = -1;
  /* 【已删除】IsRefreshCoolTime 的破解 hook。这个方法现在只由下面的
     my_iscool 纯转发，不再强制返回 0，也不再重复 hook。 */
  void *oen = p_method_get_from_name(page_cls,"OnEnable",0);
  int r0 = -1;
  if(oen){ g_enable_addr = *(void**)oen; r0 = DobbyHook(g_enable_addr, (void*)my_enable, (void**)&orig_enable); }
  { void* od = p_method_get_from_name(page_cls,"OnDisable",0);
    if(od){ int rd = DobbyHook(*(void**)od,(void*)my_page_disable,(void**)&orig_page_disable);
      char db[96]; snprintf(db,sizeof(db),"[hk] PageDisable=%d",rd); logmsg(db); } }
  {
    /* 探测：枚举名字含 Message/Tip/Notice 的类，并列出其字段（找"确认"按钮字段） */
    void* dom4 = p_domain_get();
    size_t an4 = 0;
    void** as4 = dom4 ? (void**)p_domain_get_assemblies(dom4,&an4) : NULL;
    int hit4 = 0;
    if(as4){
      for(size_t a=0;a<an4 && a<400 && hit4<24;a++){
        void* img4 = p_assembly_get_image(as4[a]);
        if(!img4) continue;
        size_t cc4 = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img4);
        if(!cc4 || cc4>20000) continue;
        for(size_t ci=0; ci<cc4 && hit4<24; ci++){
          void* c4 = ((void*(*)(void*,size_t))p_image_get_class)(img4, ci);
          if(!c4) continue;
          const char* n4 = p_class_get_name ? p_class_get_name(c4) : NULL;
          if(!n4) continue;
          if(!strstr(n4,"Message") && !strstr(n4,"Tip") && !strstr(n4,"Notice") && !strstr(n4,"Msg")
             && !strstr(n4,"Alert") && !strstr(n4,"Box") && !strstr(n4,"Confirm") && !strstr(n4,"Dialog")) continue;
          char hb[128]; snprintf(hb,sizeof(hb),"[TIPCLS] %s", n4); logmsg(hb);
          if(p_class_get_fields){
            void* it4=NULL; void* f4; int nf=0;
            while((f4=p_class_get_fields(c4,&it4)) && nf<40){
              const char* fn4 = p_field_get_name ? p_field_get_name(f4) : NULL;
              int of4 = p_field_get_offset ? p_field_get_offset(f4) : -1;
              if(fn4){ char fb[144]; snprintf(fb,sizeof(fb),"[TIPFLD] %s +0x%x", fn4, of4); logmsg(fb); }
              nf++;
            }
          }
          hit4++;
        }
      }
    }
    char tb4[64]; snprintf(tb4,sizeof(tb4),"[TIPCLS] total=%d", hit4); logmsg(tb4);
  }
  {
    /* 探测：打印所有名字含 Auction 的方法（找搜索响应入口） */
    void* dom8 = p_domain_get();
    size_t n8 = 0;
    void** as8 = dom8 ? (void**)p_domain_get_assemblies(dom8,&n8) : NULL;
    int cnt8 = 0;
    if(as8){
      for(size_t a8=0; a8<n8 && a8<400 && cnt8<40; a8++){
        void* img8 = p_assembly_get_image(as8[a8]);
        if(!img8) continue;
        size_t cc8 = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img8);
        if(!cc8 || cc8>20000) continue;
        for(size_t ci8=0; ci8<cc8 && cnt8<40; ci8++){
          void* c8 = ((void*(*)(void*,size_t))p_image_get_class)(img8, ci8);
          if(!c8) continue;
          const char* cn8 = p_class_get_name ? p_class_get_name(c8) : NULL;
          if(!cn8) continue;
          void* it8 = NULL; void* m8;
          while((m8 = p_class_get_methods(c8,&it8)) && cnt8<40){
            const char* mn8 = p_method_get_name(m8);
            if(!mn8) continue;
            if(!strstr(mn8,"Auction")) continue;
            if(!strstr(mn8,"Search") && !strstr(mn8,"Res") && !strstr(mn8,"Buy") && !strstr(mn8,"Notify")) continue;
            char pb8[192]; snprintf(pb8,sizeof(pb8),"[AUC] %s.%s argc=%d", cn8, mn8,
              p_method_get_param_count?p_method_get_param_count(m8):-1); logmsg(pb8);
            cnt8++;
          }
        }
      }
    }
    char tb8[64]; snprintf(tb8,sizeof(tb8),"[AUC] total=%d", cnt8); logmsg(tb8);
  }
  {
    /* 【找刷新CD】dump CeraAuctionListPageBase 的所有字段名+偏移 */
    void* lpc = find_class_in_csharp("CeraAuctionListPageBase");
    if(!lpc) lpc = find_class_any("MDNF.Auction.Cera","CeraAuctionListPageBase");
    if(lpc && p_class_get_fields){
      logmsg("[FLD] --- CeraAuctionListPageBase fields ---");
      void* itf = NULL; void* fld;
      int nf2 = 0;
      while((fld = p_class_get_fields(lpc,&itf)) && nf2 < 80){
        const char* fn2 = p_field_get_name ? p_field_get_name(fld) : NULL;
        int of2 = p_field_get_offset ? p_field_get_offset(fld) : -1;
        if(fn2){ char fb2[144]; snprintf(fb2,sizeof(fb2),"[FLD] %s +0x%x", fn2, of2); logmsg(fb2); }
        nf2++;
      }
      char tb2[64]; snprintf(tb2,sizeof(tb2),"[FLD] total=%d", nf2); logmsg(tb2);
    } else logmsg("[FLD] class not found");
  }
  {
    /* 【找详细搜索】dump CeraAuctionListPageBase / CeraAuctionDataController 全部方法 */
    const char* cn_l[2] = {"CeraAuctionListPageBase","CeraAuctionDataController"};
    for(int ci_l=0; ci_l<2; ci_l++){
      void* c_l = find_class_in_csharp(cn_l[ci_l]);
      if(!c_l) c_l = find_class_any("MDNF.Auction.Cera", cn_l[ci_l]);
      if(!c_l) continue;
      char hd[128]; snprintf(hd,sizeof(hd),"[PM] ==== %s ====", cn_l[ci_l]); logmsg(hd);
      void* it_l = NULL; void* m_l; int n_l = 0;
      while((m_l = p_class_get_methods(c_l,&it_l)) && n_l < 200){
        const char* nm_l = p_method_get_name(m_l);
        int ac_l = p_method_get_param_count ? p_method_get_param_count(m_l) : -1;
        if(nm_l){ char lb[160]; snprintf(lb,sizeof(lb),"[PM] %s argc=%d", nm_l, ac_l); logmsg(lb); }
        n_l++;
      }
    }
  }
  {
    /* 找 DataController 的刷新冷却重置入口（冷却破解功能已删除，这里只留重置调用） */
    void* dcc = find_class_in_csharp("CeraAuctionDataController");
    if(!dcc) dcc = find_class_any("MDNF.Auction.Cera","CeraAuctionDataController");
    if(dcc){
      /* 【说明】冷却破解功能已整体删除，IsRefreshCoolTime 不再被强制返回 0；
         这里只保留 ResetDetailSearchCoolTime 的重置调用（扫拍开启时用）。 */
      void* m3 = p_method_get_from_name(dcc,"ResetDetailSearchCoolTime",0);
      if(m3){ g_resetcd_addr = *(void**)m3; logmsg("[CD] ResetDetailSearchCoolTime found"); }
      void* m4 = p_method_get_from_name(dcc,"GetRefreshRemainCoolTime",0);
      if(m4){ g_cdremain_addr = *(void**)m4; logmsg("[CD] GetRefreshRemainCoolTime found"); }
    } else logmsg("[CD] DataController not found");

    /* 找详细搜索 / 搜索按钮入口 */
    void* lp = find_class_in_csharp("CeraAuctionListPageBase");
    if(!lp) lp = find_class_any("MDNF.Auction.Cera","CeraAuctionListPageBase");
    if(lp){
      void* d1 = p_method_get_from_name(lp,"OnClickDetailSearch",0);
      if(d1){ g_detailsearch_addr = *(void**)d1; logmsg("[CD] OnClickDetailSearch found"); }
      void* d2 = p_method_get_from_name(lp,"OnClickSearchButton",0);
      if(d2){ g_searchbtn_addr = *(void**)d2; logmsg("[CD] OnClickSearchButton found"); }
      void* d3 = p_method_get_from_name(lp,"RequestSearchPage",1);
      if(d3){ g_rs_page_addr = *(void**)d3; logmsg("[CD] RequestSearchPage found"); }
    }
  }
  {
    /* 【绕过 SetData】dump 商品容器类的字段，找"商品列表"在哪 */
    const char* cc_l[3] = {"CeraAuctionGoodsContainer","CeraAuctionGoodsData","CeraAuctionScrollList"};
    for(int ci_l=0; ci_l<3; ci_l++){
      void* c_l = find_class_in_csharp(cc_l[ci_l]);
      if(!c_l) c_l = find_class_any("MDNF.Auction.Cera", cc_l[ci_l]);
      if(!c_l){ char nf[128]; snprintf(nf,sizeof(nf),"[GC] %s not found", cc_l[ci_l]); logmsg(nf); continue; }
      char hd[128]; snprintf(hd,sizeof(hd),"[GC] ==== %s ====", cc_l[ci_l]); logmsg(hd);
      if(p_class_get_fields){
        void* it_f = NULL; void* f_l; int n_f = 0;
        while((f_l = p_class_get_fields(c_l,&it_f)) && n_f < 60){
          const char* fn_l = p_field_get_name ? p_field_get_name(f_l) : NULL;
          int of_l = p_field_get_offset ? p_field_get_offset(f_l) : -1;
          if(fn_l){ char fb[144]; snprintf(fb,sizeof(fb),"[GC] %s +0x%x", fn_l, of_l); logmsg(fb); }
          n_f++;
        }
      }
      void* it_m = NULL; void* m_l; int n_m = 0;
      while((m_l = p_class_get_methods(c_l,&it_m)) && n_m < 60){
        const char* nm_l = p_method_get_name(m_l);
        int ac_l = p_method_get_param_count ? p_method_get_param_count(m_l) : -1;
        if(nm_l){ char mb[144]; snprintf(mb,sizeof(mb),"[GC] %s argc=%d", nm_l, ac_l); logmsg(mb); }
        n_m++;
      }
    }
  }
  {
    /* 找 CeraAuctionGoodsContainer 并 hook ParseSearchResult */
    void* gc_c = find_class_in_csharp("CeraAuctionGoodsContainer");
    if(!gc_c) gc_c = find_class_any("MDNF.Auction.Cera","CeraAuctionGoodsContainer");
    if(gc_c){
      void* pm = p_method_get_from_name(gc_c,"ParseSearchResult",2);
      if(pm){
        g_parse_search_addr = *(void**)pm;
        int rp = DobbyHook(g_parse_search_addr,(void*)my_parse_search,(void**)&orig_parse_search);
        char pb[128]; snprintf(pb,sizeof(pb),"[GS] ParseSearchResult hook=%d", rp); logmsg(pb);
      } else logmsg("[GS] ParseSearchResult not found");
      void* gl = p_method_get_from_name(gc_c,"GetCategoryListings",1);
      if(gl){ g_getlist_addr = *(void**)gl; logmsg("[GS] GetCategoryListings found"); }
      void* tg = p_method_get_from_name(gc_c,"TryGetGoodsData",2);
      if(tg){ g_trygoods_addr = *(void**)tg; logmsg("[GS] TryGetGoodsData found"); }
    } else logmsg("[GS] container class not found");
  }
  {
    void* gcc = find_class_in_csharp("CeraAuctionGoodsContainer");
    if(!gcc) gcc = find_class_any("MDNF.Auction.Cera","CeraAuctionGoodsContainer");
    if(gcc){
      void* gm = p_method_get_from_name(gcc,"GetOrCreateGoodsData",1);
      if(gm){
        g_gocgd_addr = *(void**)gm;
        int rg = DobbyHook(g_gocgd_addr,(void*)my_gocgd,(void**)&orig_gocgd);
        char b[112]; snprintf(b,sizeof(b),"[GD] GetOrCreateGoodsData hook=%d", rg); logmsg(b);
      } else logmsg("[GD] not found");
    }
  }
  {
    void* gcp = find_class_in_csharp("CeraAuctionGoodsContainer");
    if(!gcp) gcp = find_class_any("MDNF.Auction.Cera","CeraAuctionGoodsContainer");
    if(gcp){
      void* pm2 = p_method_get_from_name(gcp,"ParsePriceList",1);
      if(pm2){
        g_ppl_addr = *(void**)pm2;
        int rp2 = DobbyHook(g_ppl_addr,(void*)my_parse_price,(void**)&orig_ppl);
        char b2[112]; snprintf(b2,sizeof(b2),"[PPL] ParsePriceList hook=%d", rp2); logmsg(b2);
      } else logmsg("[PPL] not found");
    }
  }
  {
    /* 【自动确认弹窗】找所有提示框类，dump 它们的确认/关闭方法 */
    void* dom7 = p_domain_get();
    size_t n7 = 0;
    void** as7 = dom7 ? (void**)p_domain_get_assemblies(dom7,&n7) : NULL;
    int cnt7 = 0;
    if(as7){
      for(size_t a7=0; a7<n7 && a7<400 && cnt7<30; a7++){
        void* img7 = p_assembly_get_image(as7[a7]);
        if(!img7) continue;
        size_t cc7 = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img7);
        if(!cc7 || cc7>20000) continue;
        for(size_t ci7=0; ci7<cc7 && cnt7<30; ci7++){
          void* c7 = ((void*(*)(void*,size_t))p_image_get_class)(img7, ci7);
          if(!c7) continue;
          const char* cn7 = p_class_get_name ? p_class_get_name(c7) : NULL;
          if(!cn7) continue;
          if(strcmp(cn7,"MessageBox") != 0) continue;
          void* it7 = NULL; void* m7;
          while((m7 = p_class_get_methods(c7,&it7)) && cnt7<40){
            const char* mn7 = p_method_get_name(m7);
            if(!mn7) continue;
            char pb7[160]; snprintf(pb7,sizeof(pb7),"[TIPM] MessageBox.%s argc=%d", mn7,
              p_method_get_param_count?p_method_get_param_count(m7):-1); logmsg(pb7);
            cnt7++;
          }
        }
      }
    }
    logmsg("[TIPM] ---");
  }
  if(1) {
    /* 【自动确认】hook ShowOnlyMessage：照常显示，但记录参数，主线程随后自动点确定 */
    void* mbc = NULL;
    {
      void* dom9 = p_domain_get();
      size_t n9 = 0;
      void** as9 = dom9 ? (void**)p_domain_get_assemblies(dom9,&n9) : NULL;
      if(as9){
        for(size_t a9=0; a9<n9 && a9<400 && !mbc; a9++){
          void* img9 = p_assembly_get_image(as9[a9]);
          if(!img9) continue;
          size_t cc9 = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img9);
          if(!cc9 || cc9>20000) continue;
          for(size_t ci9=0; ci9<cc9 && !mbc; ci9++){
            void* c9 = ((void*(*)(void*,size_t))p_image_get_class)(img9, ci9);
            if(!c9) continue;
            const char* cn9 = p_class_get_name ? p_class_get_name(c9) : NULL;
            if(cn9 && strcmp(cn9,"MessageBox")==0) mbc = c9;
          }
        }
      }
    }
    if(mbc){
      void* it8 = NULL; void* m8; int idx8 = 0;
      while((m8 = p_class_get_methods(mbc,&it8)) && idx8 < 80){
        const char* nm8 = p_method_get_name(m8);
        if(nm8 && strcmp(nm8,"ShowOnlyMessage")==0){
          int ac8 = p_method_get_param_count ? p_method_get_param_count(m8) : -1;
          void* addr8 = *(void**)m8;
          if(ac8 == 5 && !g_som5_addr){ g_som5_addr = addr8;
            int r8 = DobbyHook(addr8,(void*)my_som5,(void**)&orig_som5);
            char b8[96]; snprintf(b8,sizeof(b8),"[TIPBLOCK] ShowOnlyMessage(5) hook=%d", r8); logmsg(b8); }
          else if(ac8 == 2 && !g_som2a_addr){ g_som2a_addr = addr8;
            int r9 = DobbyHook(addr8,(void*)my_som2a,(void**)&orig_som2a);
            char b9[96]; snprintf(b9,sizeof(b9),"[TIPBLOCK] ShowOnlyMessage(2a) hook=%d", r9); logmsg(b9); }
          else if(ac8 == 2 && g_som2a_addr && !g_som2b_addr){ g_som2b_addr = addr8;
            int ra = DobbyHook(addr8,(void*)my_som2b,(void**)&orig_som2b);
            char ba[96]; snprintf(ba,sizeof(ba),"[TIPBLOCK] ShowOnlyMessage(2b) hook=%d", ra); logmsg(ba); }
        }
        idx8++;
      }
    } else logmsg("[TIPBLOCK] MessageBox not found");
  }
  int r1 = -1;
  if(g_update_addr){
    r1 = DobbyHook(g_update_addr, (void*)my_update, (void**)&orig_update);
    g_diag_hk_upd = (r1 == 0) ? 1 : 0;
  }
  {
    /* 搜索响应包 hook：从原始包读价格 */
    void* om = NULL;
    /* 遍历所有类找 OnCeraAuctionSearch（类名/命名空间不确定）*/
    {
      void* dom5 = p_domain_get();
      size_t n5 = 0;
      void** as5 = dom5 ? (void**)p_domain_get_assemblies(dom5,&n5) : NULL;
      for(size_t a5=0; a5<n5 && a5<400 && !om; a5++){
        void* img5 = p_assembly_get_image(as5[a5]);
        if(!img5) continue;
        size_t cc5 = (size_t)(unsigned long)((void*(*)(void*))p_image_get_class_count)(img5);
        if(!cc5 || cc5>20000) continue;
        for(size_t ci5=0; ci5<cc5 && !om; ci5++){
          void* c5 = ((void*(*)(void*,size_t))p_image_get_class)(img5, ci5);
          if(!c5) continue;
          om = p_method_get_from_name(c5,"OnCeraAuctionSearch",3);
          if(!om) om = p_method_get_from_name(c5,"OnCeraAuctionSearch",1);
        }
      }
    }
    if(om){
      g_onsearch_addr = *(void**)om;
      int ro2 = DobbyHook(g_onsearch_addr,(void*)my_on_search,(void**)&orig_onsearch);
      char rb2[128]; snprintf(rb2,sizeof(rb2),"[RSP] hook=%d argc=%d", ro2,
        p_method_get_param_count?p_method_get_param_count(om):-1); logmsg(rb2);
    } else logmsg("[RSP] OnCeraAuctionSearch not found");
  }
  {
    /* 主线程钩子：UnityEngine.Time.get_deltaTime，每帧被调用 */
    void* tcls = find_class_in_csharp("Time");
    if(!tcls) tcls = find_class_any("UnityEngine","Time");
    void* dtm = tcls ? p_method_get_from_name(tcls,"get_deltaTime",0) : NULL;
    if(dtm){
      g_dt_addr = *(void**)dtm;
      int rdt = DobbyHook(g_dt_addr,(void*)my_delta_time,(void**)&orig_dt);
      char db[128]; snprintf(db,sizeof(db),"[dt] get_deltaTime hook=%d addr=%p", rdt, g_dt_addr); logmsg(db);
    } else logmsg("[dt] get_deltaTime NOT found");
  }
  int r2 = -1;
  if(g_click_addr){
    r2 = DobbyHook(g_click_addr, (void*)my_click, (void**)&orig_click);
  }
  snprintf(m,sizeof(m),"Hooks: en=%d up=%d clk=%d rem=%d iscd=%d det=%d cse=%d rs=%d dscd=%d dsrem=%d settext=%d",r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10);
  logmsg(m);
  {
    char ib[256];
    snprintf(ib,sizeof(ib),"[init] done click=%p update=%p page=%p sortT=%p sortP=%p rs=%p dc=%p",
      g_click_addr,g_update_addr,(void*)page_cls,g_sortt_addr,g_sortp_addr,g_rs_addr,g_dc_inst);
    logmsg_force(ib);
  }
  g_diag_init = 1;   /* do_init 完整跑完（没被中途 return 掉）*/
}

static void* init_worker(void* arg){
  logmsg("[worker] delay 30s");
  sleep(30);
  do_init();
  return NULL;
}

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>

/* ---- 悬浮窗 UI ---- */


__attribute__((constructor)) static void onload(void){
  logmsg("=== DNF SO v20 (dobby) ===");
  void* base = maps_first_base("UnityFramework");
  p_string_new = (void*(*)(void*, const char*))elf_find_sym(base,"il2cpp_string_new");
  p_domain_get = (void*(*)(void))elf_find_sym(base,"il2cpp_domain_get");
  if(!p_domain_get){ logmsg("[err] no api"); return; }
  p_domain_get_assemblies = (void*(*)(void*, size_t*))elf_find_sym(base,"il2cpp_domain_get_assemblies");
  p_assembly_get_image = (void*(*)(void*))elf_find_sym(base,"il2cpp_assembly_get_image");
  p_image_get_name = (const char*(*)(void*))elf_find_sym(base,"il2cpp_image_get_name");
  p_class_from_name = (void*(*)(void*, const char*, const char*))elf_find_sym(base,"il2cpp_class_from_name");
  p_method_get_from_name = (void*(*)(void*, const char*, int))elf_find_sym(base,"il2cpp_class_get_method_from_name");
  p_class_get_methods = (void*(*)(void*, void**))elf_find_sym(base,"il2cpp_class_get_methods");
  p_method_get_name = (const char*(*)(void*))elf_find_sym(base,"il2cpp_method_get_name");
  p_method_get_param_count = (int(*)(void*))elf_find_sym(base,"il2cpp_method_get_param_count");
  p_image_get_class_count = (void*(*)(void*))elf_find_sym(base,"il2cpp_image_get_class_count");
  p_image_get_class = (void*(*)(void*, size_t))elf_find_sym(base,"il2cpp_image_get_class");
  p_class_get_type = (void*(*)(void*))elf_find_sym(base,"il2cpp_class_get_type");
  p_object_new = (void*(*)(void*))elf_find_sym(base,"il2cpp_object_new");
  p_runtime_invoke = (void*(*)(void*, void*, void**, void**))elf_find_sym(base,"il2cpp_runtime_invoke");
  p_class_get_fields = (void*(*)(void*, void**))elf_find_sym(base,"il2cpp_class_get_fields");
  p_field_get_name = (const char*(*)(void*))elf_find_sym(base,"il2cpp_field_get_name");
  p_field_get_offset = (int(*)(void*))elf_find_sym(base,"il2cpp_field_get_offset");
  p_class_get_name = (const char*(*)(void*))elf_find_sym(base,"il2cpp_class_get_name");
  logmsg("[ok] api");
  pthread_t t;
  pthread_create(&t,NULL,init_worker,NULL);
  pthread_detach(t);
}


extern "C" void dnf_scan_init(void){
  pthread_t t;
  pthread_create(&t, NULL, init_worker, NULL);
  pthread_detach(t);
}


/* 【强制校验】四种方式至少勾一个。
   注意：排序和价格只是本地重排，不产生新数据，
   但为了能单独测试它们是否真的被调用，允许单独勾选。
   只有四种全关才会自动补「搜索」。 */
static void ensure_use_mode(void){
  if(!g_use_click && !g_use_search && !g_sort_sync && !g_use_price){
    g_use_search = 1;      /* 全关才补默认 */
  }
}
extern "C" void dnf_scan_set_enabled(int v){
  if(v){
    ensure_use_mode();                 /* ← 开启前先保证有方式 */
    int d = g_auto_on_ms;
    if(d < 100)   d = 100;
    if(d > 1000)  d = 1000;
    /* 一律走定时：0.10 秒起步，最短也有 100ms 的缓冲 */
    g_auto_on_at = mono_ms() + (long long)d;
    g_enabled2   = 0;
  } else {
    g_auto_on_at = 0;                       /* 取消定时 */
    if(g_enabled2 && g_run_start_ms > 0){
      g_run_total_ms += mono_ms() - g_run_start_ms;
      g_run_start_ms = 0;
    }
    g_enabled2 = 0;
  }
}
/* 定时开启：读写延迟毫秒（1 ~ 10000）*/
extern "C" void dnf_auto_on_set_ms(int ms){
  if(ms < 100)  ms = 100;
  if(ms > 1000) ms = 1000;
  g_auto_on_ms = ms;
}
extern "C" void dnf_use_click_set(int v){ g_use_click = v ? 1 : 0; }
extern "C" int  dnf_use_click_get(void){ return g_use_click; }
extern "C" void dnf_use_search_set(int v){ g_use_search = v ? 1 : 0; }
extern "C" void dnf_snap_time(void){ g_field_time = g_last_sort_field; }
extern "C" void dnf_snap_price(void){ g_field_price = g_last_sort_field; }
extern "C" long long dnf_get_field_time(void){ return g_field_time; }
extern "C" long long dnf_get_field_price(void){ return g_field_price; }
extern "C" void dnf_use_price_set(int v){ g_use_price = v ? 1 : 0; }
extern "C" int  dnf_use_price_get(void){ return g_use_price; }
extern "C" int  dnf_use_search_get(void){ return g_use_search; }
extern "C" int dnf_auto_on_get_ms(void){
  int v = g_auto_on_ms;
  if(v < 100)  v = 100;
  if(v > 1000) v = 1000;
  return v;
}
/* 距离自动开启还剩多少毫秒；0 表示当前没有待启动的定时 */
extern "C" long long dnf_auto_on_remain_ms(void){
  long long at = g_auto_on_at;
  if(at <= 0) return 0;
  long long now = mono_ms();
  return (at > now) ? (at - now) : 0;
}
/* 累计运行毫秒（开启中则实时加上当前这一段）*/
extern "C" long long dnf_run_ms(void){
  long long t = g_run_total_ms;
  if(g_enabled2 && g_run_start_ms > 0){
    long long now = mono_ms();
    if(now > g_run_start_ms) t += now - g_run_start_ms;
  }
  return t;
}
extern "C" int dnf_actual_gap_ms(void){ return g_actual_gap_ms; }
/* 诊断位：bit0=P页面类 bit1=C刷新地址 bit2=U网络Update bit3=R搜索方法
   bit4=UpdateHook成功 bit5=搜索Hook成功 bit6=do_init跑完 */
extern "C" int dnf_diag_bits(void){
  return (g_diag_page ? 1 : 0) | (g_diag_clk ? 2 : 0) | (g_diag_upd ? 4 : 0)
       | (g_diag_rs ? 8 : 0) | (g_diag_hk_upd ? 16 : 0) | (g_diag_hk_rs ? 32 : 0)
       | (g_diag_init ? 64 : 0);
}
extern "C" const char* dnf_probe_last(void){ return g_last_click_name; }
extern "C" int dnf_probe_count(void){ return g_probe_n; }
extern "C" int dnf_probe_hits(void){ return g_click_hits; }
extern "C" int dnf_buy_count(void){ return g_buy_count; }
extern "C" int dnf_seen_count(void){
  int v = g_seen_total - g_seen_base;
  return (v > 0) ? v : 0;      /* 扫拍没开始前恒为 0，不会把首页缓存算进来 */
}
/* 【限购统一检查】按本次成功购买件数判断，不按发包次数。
   放在每次 g_buy_count++ 之后调用（目前只有 my_settext 一个入口）。 */
static void limit_check_by_success(void){
  if(g_limit_on && !g_limit_hit){
    int done = g_buy_count - g_limit_base;   /* 本次限购开启后成功了几件 */
    if(done >= g_limit_num){
      g_limit_hit = 1;
      g_need_shot = 1;                        /* 交给主线程截图 */
      g_enabled2 = 0; g_buy_enabled = 0; g_buy2_enabled = 0;   /* 全停 */
    }
  }
}
extern "C" void dnf_set_limit(int n){ if(n > 0 && n < 100000) g_limit_num = n; }
extern "C" void dnf_limit_set(int v){
  /* 刚开启限购：把当前已购数记为基线，之后只算"本次开启后成功了几件" */
  if(v && !g_limit_on) g_limit_base = g_buy_count;
  g_limit_on = v ? 1 : 0;
  /* 【关闭即停】关掉限购：清达标、清待截图、清计数基线，重开从 0 重新计 */
  if(!v){ g_limit_hit = 0; g_shot_done = 0; g_need_shot = 0; g_attempt_cnt = 0; g_limit_base = 0; }
}
extern "C" int  dnf_limit_hit(void){ return g_limit_hit; }
extern "C" int  dnf_limit_num(void){ return g_limit_num; }
extern "C" int  dnf_attempt_count(void){ return g_attempt_cnt; }
/* ===== 详细搜索链路验证接口 ===== */
extern "C" int  dnf_rs_capture_cnt(void){ return g_rs_capture_cnt; }
extern "C" int  dnf_rs_detail_cnt(void){ return g_rs_detail_cnt; }
/* 服务器真实回包次数 / 解析次数：扫拍有没有真的刷新，看这两个数 */
extern "C" int  dnf_on_search_cnt(void){ return g_os_cnt; }
extern "C" int  dnf_parse_cnt(void){ return g_gs_cnt; }
extern "C" int  dnf_rs_detail_valid(void){ return g_rs_detail_valid; }
extern "C" int  dnf_rs_page_found(void){ return g_rs_page_addr ? 1 : 0; }
extern "C" void dnf_rs_reset(void){ g_rs_capture_cnt = 0; g_rs_detail_cnt = 0; g_detail_ms = 0; }
/* ===== 剩余时间排序同步开关 ===== */
extern "C" void dnf_sort_sync_set(int v){ g_sort_sync = v ? 1 : 0; }
extern "C" int  dnf_sort_sync_get(void){ return g_sort_sync; }
/* 截图：用 Unity 的 ScreenCapture.CaptureScreenshot 存到相册目录 */
static void take_shot_once(void){
  if(g_shot_done) return;
  g_shot_done = 1;
  void* sc = find_class_in_csharp("ScreenCapture");
  if(!sc) sc = find_class_any("UnityEngine","ScreenCapture");
  if(!sc) return;
  void* m = p_method_get_from_name(sc,"CaptureScreenshot",1);
  if(!m) return;
  /* 造一个 il2cpp 字符串路径 */
  const char* path = "/var/mobile/Media/DCIM/xs_limit_shot.png";
  void* (*p_newstr)(void*, const char*) = (void*(*)(void*, const char*))p_string_new;
  if(!p_string_new) return;
  void* sstr = p_newstr(NULL, path);
  if(!sstr) return;
  ((void(*)(void*))(*(void**)m))(sstr);
}
extern "C" void dnf_set_quality(int q){ g_q = q; }
extern "C" void dnf_set_affix(const char* s){ if(s){ strncpy(g_affix,s,63); g_affix[63]=0; } }
extern "C" void dnf_set_maxprice(int p){ g_maxp = p; }
extern "C" void dnf_buy2_set(int v){
  g_buy2_enabled = v;
  /* 【关闭即停】把待发单与旧商品指针全部清掉，杜绝"重开第一帧补发旧单" */
  if(!v){
    g_direct_pending = 0; g_direct_item = NULL; g_direct_slot = NULL;
    g_batch_n = 0; g_has_pend = 0; g_pend_slot = NULL; g_rs_n = 0;
  }
}
extern "C" void dnf_buy_set(int v){ g_buy_enabled = v;
  char b[64]; snprintf(b,sizeof(b),"[SW] dnf_buy_set(%d)", v); logmsg(b);
  if(!v && !g_buy2_enabled){
    g_direct_pending = 0; g_direct_item = NULL; g_direct_slot = NULL;
    g_batch_n = 0; g_has_pend = 0; g_pend_slot = NULL; g_rs_n = 0;
  }
}
/* ===== 扫拍自定义间隔（单位：秒）：持久化 + 读写接口 ===== */
extern "C" void dnf_scan_set_interval_sec(double sec);   /* 前置声明 */

static void scan_iv_save(void){
  FILE* f = fopen(SCAN_IV_PATH, "w");
  if(!f) return;                                      /* 写不了就静默放弃，不影响扫拍 */
  fprintf(f, "%.2f\n", (double)g_scan_interval_us / 1000000.0);   /* 存秒 */
  fclose(f);
}

static void scan_iv_load(void){
  FILE* f = fopen(SCAN_IV_PATH, "r");
  if(!f) return;                                      /* 没有存档 → 保持默认 1.10 秒 */
  double v = 0.0;
  int ok = fscanf(f, "%lf", &v);
  fclose(f);
  if(ok == 1 && v > 0.0) dnf_scan_set_interval_sec(v);  /* 存档里的非法值同样会被夹紧 */
}

/* 写入刷新间隔（秒）。任何非法值（0 / 负数 / NaN / 无穷）都会被拒绝。 */
extern "C" void dnf_scan_set_interval_sec(double sec){
  if(!(sec > 0.0)) return;                            /* NaN 也走这里被拒 */
  if(sec < 0.01) sec = 0.01;
  if(sec > 60.0) sec = 60.0;
  long long us = (long long)(sec * 1000000.0 + 0.5);
  if(us < SCAN_IV_MIN_US) us = SCAN_IV_MIN_US;
  if(us > SCAN_IV_MAX_US) us = SCAN_IV_MAX_US;
  if(us == g_scan_interval_us) return;                /* 值没变就不写盘 */
  g_scan_interval_us = us;
  scan_iv_save();
}

/* 读取刷新间隔（秒），返回值永远落在 0.01 ~ 60.00 */
extern "C" double dnf_scan_get_interval_sec(void){
  long long us = g_scan_interval_us;
  if(us < SCAN_IV_MIN_US) us = SCAN_IV_MIN_US;
  if(us > SCAN_IV_MAX_US) us = SCAN_IV_MAX_US;
  return (double)us / 1000000.0;
}

/* 旧毫秒接口保留（内部转秒），供老调用点使用 */
extern "C" void dnf_scan_set_interval_ms(double ms){ dnf_scan_set_interval_sec(ms / 1000.0); }
extern "C" double dnf_scan_get_interval_ms(void){ return dnf_scan_get_interval_sec() * 1000.0; }

/* ===== 新增开关写入接口（默认 0，必须面板勾选）===== */
extern "C" void dnf_tip_set(int v){ g_tip_block = v ? 1 : 0; }
extern "C" int dnf_scan_count(void){ return g_calls; }
