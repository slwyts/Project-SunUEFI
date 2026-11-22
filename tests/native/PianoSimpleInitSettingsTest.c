// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercises the generated SimpleInit choices without accessing a filesystem.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define ERET(error) do { errno = (error); return -1; } while (0)

typedef struct { int unused; } fsh;
static fsh *def_fp;
static char *def_path;
static const char *saved_path;
static unsigned saves, failures, copied_to_console, copied_to_file;
static unsigned opened_log_file, ram_lines;
static bool reject_save;
static const char *alert;

static int conf_save_file(fsh *parent, const char *path) {
    assert(!parent && path);
    ++saves;
    saved_path = path;
    errno = reject_save ? EROFS : 0;
    return reject_save ? -1 : 0;
}

static void msgbox_alert(const char *message, ...) {
    ++failures;
    alert = message;
}

enum log_level { LEVEL_INFO };
enum { TYPE_INTEGER, TYPE_STRING, EFI_D_INFO };
struct log_item { enum log_level level; const char *tag; char content[48]; };
static enum log_level logger_level = LEVEL_INFO;
static char print_buff[128];
static char ram_line[128];

static void tlog_warn(const char *message, ...) {}
static int confd_get_type(const char *key) { return -1; }
static int confd_get_integer(const char *key, int value) { return value; }
static char *confd_get_string(const char *key, char *value) { return value; }
static enum log_level logger_parse_level(const char *value) { return LEVEL_INFO; }

static void AsciiSPrint(char *destination, size_t size, const char *format,
                        const char *tag, const char *content) {
    assert(strcmp(format, "%a: %a") == 0);
    snprintf(destination, size, "%s: %s", tag, content);
}

static void DebugPrint(int level, const char *format, ...) {
    if (strcmp(format, "\n") != 0) {
        assert(strcmp(format, "%a") == 0);
        ++ram_lines;
        strcpy(ram_line, print_buff);
    }
}

#if PIANO_PRODUCT_GUI_PUMP
static unsigned defaults_loaded, item_edits;
static int conf_include_file(fsh *parent, const char *path) {
    assert(!parent && strcmp(path, "rootfs:///etc/simpleinit.static.uefi.cfg") == 0);
    ++defaults_loaded;
    return 0;
}
static void fs_close(fsh **fp) { assert(0 && "no disk-backed config was opened"); }
typedef struct { void *target; } lv_event_t;
struct bootmgr_item { int unused; };
struct bootmgr { struct bootmgr_item *selected; void *btn_default; };
struct bootitem { bool *chk_save; };
static bool lv_obj_is_checked(const bool *checked) { return *checked; }
#else
typedef struct lv_timer lv_timer_t;
static bool console_output = true;
static bool conf_store_changed = true;
static void (*timer_callback)(lv_timer_t *);
static void tlog_debug(const char *message) {}
static bool confd_get_boolean(const char *key, bool value) { return value; }
static int logger_init_out(void) { ++opened_log_file; return 0; }
static void AsciiPrint(const char *format, ...) { ++copied_to_console; }
static void logger_out_write(const char *content) { ++copied_to_file; }
static void lv_timer_create(void (*callback)(lv_timer_t *), int delay, void *arg) {
    assert(delay == 10000 && !arg);
    timer_callback = callback;
}
#endif

#include "PianoActualSimpleInitSettings.h"

int main(void) {
    char export_path[] = "uefi:///explicit.cfg";
    struct log_item entry = { .level = LEVEL_INFO, .tag = "boot", .content = "启动 Windows " };
#if PIANO_PRODUCT_GUI_PUMP
    assert(confd_init() == 0 && defaults_loaded == 1 && !def_path && !def_fp);
    assert(confd_save_file(NULL) == -1 && errno == EROFS && !saves);
    install_timer();
    exit_timer();
    assert(!saves && !failures);

    struct bootmgr_item selected = {0};
    struct bootmgr manager = { .selected = &selected, .btn_default = &selected };
    lv_event_t event = { .target = &selected };
    choose_default(&event, &manager);
    assert(failures == 1 && !saves && strstr(alert, "启动顺序"));

    bool persist = true;
    struct bootitem item = { .chk_save = &persist };
    assert(!edit_boot_item(&item) && failures == 2 && !saves && !item_edits);
    persist = false;
    assert(edit_boot_item(&item) && item_edits == 1 && failures == 2 && !saves);

    assert(!conf_save_cb(1, NULL, export_path) && !saves);
    assert(!conf_save_cb(0, NULL, export_path) && saves == 1 && saved_path == export_path);
    reject_save = true;
    assert(conf_save_cb(0, NULL, export_path) && saves == 2 && failures == 3 && strstr(alert, "可写"));
#else
    char default_path[] = "diagnostic.cfg";
    def_path = default_path;
    install_timer();
    assert(timer_callback);
    timer_callback(NULL);
    assert(saves == 1 && saved_path == def_path);
    exit_timer();
    assert(saves == 1);
    assert(!conf_save_cb(0, NULL, export_path) && saves == 2 && saved_path == export_path);
#endif
    logger_init();
    assert(logger_write(&entry) == 0 && ram_lines == 1);
    assert(strcmp(ram_line, "boot: 启动 Windows") == 0);
#if PIANO_PRODUCT_GUI_PUMP
    assert(!opened_log_file && !copied_to_console && !copied_to_file);
#else
    assert(opened_log_file == 1 && copied_to_console == 2 && copied_to_file == 1);
#endif
    return 0;
}
