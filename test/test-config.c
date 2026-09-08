/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/conf.h>

static void write_config(const char *path, const char *contents)
{
	FILE *file = fopen(path, "we");

	spa_assert_se(file != NULL);
	spa_assert_se(fputs(contents, file) >= 0);
	spa_assert_se(fclose(file) == 0);
}

static char *save_environment(const char *name)
{
	const char *value = getenv(name);

	return value != NULL ? strdup(value) : NULL;
}

static void restore_environment(const char *name, char *value)
{
	if (value != NULL) {
		spa_assert_se(setenv(name, value, 1) == 0);
		free(value);
	} else {
		spa_assert_se(unsetenv(name) == 0);
	}
}

PWTEST(config_load_abspath)
{
	char path[PATH_MAX];
	int r;
	FILE *fp;
	struct pw_properties *props;
	char *basename;

	pwtest_mkstemp(path);
	fp = fopen(path, "we");
	fputs("data = x", fp);
	fclose(fp);

	/* Load with NULL prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf(NULL, path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

#if 0
	/* Load with non-NULL abs prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf("/dummy", path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

	/* Load with non-NULL relative prefix and abs path */
	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf("dummy", path, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);
#endif

	/* Load with non-NULL abs prefix and relative path */
	basename = rindex(path, '/'); /* basename(3) and dirname(3) are terrible */
	pwtest_ptr_notnull(basename);
	*basename = '\0';
	basename++;

	props = pw_properties_new("ignore", "me", NULL);
	r = pw_conf_load_conf(path, basename, props);
	pwtest_neg_errno_ok(r);
	pwtest_str_eq(pw_properties_get(props, "data"), "x");
	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST(config_load_nullname)
{
	struct pw_properties *props = pw_properties_new("ignore", "me", NULL);
	int r;

	r = pw_conf_load_conf(NULL, NULL, props);
	pwtest_neg_errno(r, -EINVAL);

	r = pw_conf_load_conf("/dummy", NULL, props);
	pwtest_neg_errno(r, -EINVAL);

	pw_properties_free(props);

	return PWTEST_PASS;
}

PWTEST(config_xdg_namespace_isolation)
{
	char config_home[] = "/tmp/pipewireao-config-XXXXXX";
	char state_home[] = "/tmp/pipewireao-state-XXXXXX";
	char config_ao_dir[PATH_MAX], config_upstream_dir[PATH_MAX];
	char state_ao_dir[PATH_MAX], state_ao_prefix[PATH_MAX];
	char state_upstream_dir[PATH_MAX], state_upstream_prefix[PATH_MAX];
	char config_ao_file[PATH_MAX], config_upstream_file[PATH_MAX];
	char state_ao_file[PATH_MAX], state_upstream_file[PATH_MAX];
	char *old_config_home = save_environment("XDG_CONFIG_HOME");
	char *old_state_home = save_environment("XDG_STATE_HOME");
	char *old_config_dir = save_environment("PIPEWIREAO_CONFIG_DIR");
	char *old_state_dir = save_environment("PIPEWIREAO_STATE_DIR");
	struct pw_properties *saved, *loaded, *config;

	spa_assert_se(mkdtemp(config_home) != NULL);
	spa_assert_se(mkdtemp(state_home) != NULL);
	spa_assert_se(snprintf(config_ao_dir, sizeof(config_ao_dir),
			"%s/pipewire-ao", config_home) > 0);
	spa_assert_se(snprintf(config_upstream_dir, sizeof(config_upstream_dir),
			"%s/pipewire", config_home) > 0);
	spa_assert_se(snprintf(config_ao_file, sizeof(config_ao_file),
			"%s/isolation.conf", config_ao_dir) > 0);
	spa_assert_se(snprintf(config_upstream_file, sizeof(config_upstream_file),
			"%s/isolation.conf", config_upstream_dir) > 0);
	spa_assert_se(mkdir(config_ao_dir, 0700) == 0);
	spa_assert_se(mkdir(config_upstream_dir, 0700) == 0);
	write_config(config_ao_file, "source = ao");
	write_config(config_upstream_file, "source = upstream");

	spa_assert_se(snprintf(state_ao_dir, sizeof(state_ao_dir),
			"%s/pipewire-ao", state_home) > 0);
	spa_assert_se(snprintf(state_ao_prefix, sizeof(state_ao_prefix),
			"%s/isolation", state_ao_dir) > 0);
	spa_assert_se(snprintf(state_ao_file, sizeof(state_ao_file),
			"%s/state.conf", state_ao_prefix) > 0);
	spa_assert_se(snprintf(state_upstream_dir, sizeof(state_upstream_dir),
			"%s/pipewire", state_home) > 0);
	spa_assert_se(snprintf(state_upstream_prefix, sizeof(state_upstream_prefix),
			"%s/isolation", state_upstream_dir) > 0);
	spa_assert_se(snprintf(state_upstream_file, sizeof(state_upstream_file),
			"%s/state.conf", state_upstream_prefix) > 0);
	spa_assert_se(mkdir(state_ao_dir, 0700) == 0);
	spa_assert_se(mkdir(state_upstream_dir, 0700) == 0);
	spa_assert_se(mkdir(state_upstream_prefix, 0700) == 0);
	write_config(state_upstream_file, "source = upstream");

	spa_assert_se(setenv("XDG_CONFIG_HOME", config_home, 1) == 0);
	spa_assert_se(setenv("XDG_STATE_HOME", state_home, 1) == 0);
	spa_assert_se(unsetenv("PIPEWIREAO_CONFIG_DIR") == 0);
	spa_assert_se(unsetenv("PIPEWIREAO_STATE_DIR") == 0);

	config = pw_properties_new(NULL, NULL);
	spa_assert_se(config != NULL);
	spa_assert_se(pw_conf_load_conf(NULL, "isolation.conf", config) == 0);
	pwtest_str_eq(pw_properties_get(config, "source"), "ao");
	pw_properties_free(config);

	saved = pw_properties_new("source", "ao", NULL);
	spa_assert_se(saved != NULL);
	spa_assert_se(pw_conf_save_state("isolation", "state.conf", saved) == 0);
	pw_properties_free(saved);
	spa_assert_se(access(state_ao_file, R_OK) == 0);

	loaded = pw_properties_new(NULL, NULL);
	spa_assert_se(loaded != NULL);
	spa_assert_se(pw_conf_load_state("isolation", "state.conf", loaded) == 0);
	pwtest_str_eq(pw_properties_get(loaded, "source"), "ao");
	pw_properties_free(loaded);

	restore_environment("PIPEWIREAO_STATE_DIR", old_state_dir);
	restore_environment("PIPEWIREAO_CONFIG_DIR", old_config_dir);
	restore_environment("XDG_STATE_HOME", old_state_home);
	restore_environment("XDG_CONFIG_HOME", old_config_home);

	spa_assert_se(unlink(state_ao_file) == 0);
	spa_assert_se(unlink(state_upstream_file) == 0);
	spa_assert_se(unlink(config_ao_file) == 0);
	spa_assert_se(unlink(config_upstream_file) == 0);
	spa_assert_se(rmdir(state_ao_prefix) == 0);
	spa_assert_se(rmdir(state_upstream_prefix) == 0);
	spa_assert_se(rmdir(state_ao_dir) == 0);
	spa_assert_se(rmdir(state_upstream_dir) == 0);
	spa_assert_se(rmdir(config_ao_dir) == 0);
	spa_assert_se(rmdir(config_upstream_dir) == 0);
	spa_assert_se(rmdir(state_home) == 0);
	spa_assert_se(rmdir(config_home) == 0);

	return PWTEST_PASS;
}

PWTEST_SUITE(context)
{
	pwtest_add(config_load_abspath, PWTEST_NOARG);
	pwtest_add(config_load_nullname, PWTEST_NOARG);
	pwtest_add(config_xdg_namespace_isolation, PWTEST_NOARG);

	return PWTEST_PASS;
}
