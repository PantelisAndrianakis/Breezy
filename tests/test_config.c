#include "test_framework.h"
#include "config.h"

static void test_parse_libs(void)
{
	LinkConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	config_parse_links("[link]\nlibs = [\"m\", \"mysqlclient\"]\n", &cfg);
	ASSERT_INT(cfg.nlibs, 2);
	ASSERT_STR(cfg.libs[0], "m");
	ASSERT_STR(cfg.libs[1], "mysqlclient");
}

static void test_parse_lib_paths(void)
{
	LinkConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	config_parse_links("[link]\nlib_paths = [\"C:/libs\"]\nlibs = [\"z\"]\n", &cfg);
	ASSERT_INT(cfg.nlib_paths, 1);
	ASSERT_STR(cfg.lib_paths[0], "C:/libs");
	ASSERT_INT(cfg.nlibs, 1);
	ASSERT_STR(cfg.libs[0], "z");
}

static void test_ignores_other_sections_and_comments(void)
{
	LinkConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	config_parse_links(
		"# a comment\n"
		"[build]\n"
		"libs = [\"ignored\"]\n"
		"[link]\n"
		"# link libs:\n"
		"libs = [\"ssl\"]\n", &cfg);
	ASSERT_INT(cfg.nlibs, 1);
	ASSERT_STR(cfg.libs[0], "ssl");
}

static void test_empty_is_noop(void)
{
	LinkConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	config_parse_links("", &cfg);
	ASSERT_INT(cfg.nlibs, 0);
	ASSERT_INT(cfg.nlib_paths, 0);
}

static void test_ir_flag_defaults_off(void)
{
	/* With BZY_IR unset, the experimental IR backend is disabled. */
	ASSERT_INT(bzy_ir_enabled(), 0);
}

int main(void)
{
	RUN(test_parse_libs);
	RUN(test_parse_lib_paths);
	RUN(test_ignores_other_sections_and_comments);
	RUN(test_empty_is_noop);
	RUN(test_ir_flag_defaults_off);
	SUMMARY();
	return 0;
}
