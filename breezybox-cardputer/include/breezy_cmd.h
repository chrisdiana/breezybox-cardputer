#pragma once

#include "esp_console.h"

// Command handlers - called by esp_console
int cmd_echo(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_free(int argc, char **argv);
int cmd_sh(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_chmod(int argc, char **argv);
int cmd_ln(int argc, char **argv);
int cmd_find(int argc, char **argv);
int cmd_grep(int argc, char **argv);
int cmd_sed(int argc, char **argv);
int cmd_sort(int argc, char **argv);
int cmd_uniq(int argc, char **argv);
int cmd_cut(int argc, char **argv);
int cmd_tr(int argc, char **argv);
int cmd_which(int argc, char **argv);
int cmd_type(int argc, char **argv);
int cmd_ps(int argc, char **argv);
int cmd_kill(int argc, char **argv);
int cmd_uname(int argc, char **argv);
int cmd_env(int argc, char **argv);
int cmd_export(int argc, char **argv);
int cmd_unset(int argc, char **argv);
int cmd_history(int argc, char **argv);
int cmd_source(int argc, char **argv);
int cmd_true(int argc, char **argv);
int cmd_false(int argc, char **argv);
int cmd_test(int argc, char **argv);
int cmd_sync(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_cp(int argc, char **argv);
int cmd_mv(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_du(int argc, char **argv);
int cmd_date(int argc, char **argv);
int cmd_sleep(int argc, char **argv);
int cmd_eget(int argc, char **argv);
int cmd_ping(int argc, char **argv);
int cmd_lua(int argc, char **argv);
int cmd_ssh(int argc, char **argv);
int cmd_sshcfg(int argc, char **argv);
int cmd_scp(int argc, char **argv);
int cmd_wifi(int argc, char **argv);
int cmd_httpd(int argc, char **argv);
int cmd_head(int argc, char **argv);
int cmd_tail(int argc, char **argv);
int cmd_more(int argc, char **argv);
int cmd_wc(int argc, char **argv);
