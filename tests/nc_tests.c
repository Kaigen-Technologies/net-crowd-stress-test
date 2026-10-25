#include "tests/test_framework.h"
#include "lib/string.h"
#include "net_crowd/nc_names.h"

typedef struct NcNameCase {
  u32 seed;
  const char *name;
} NcNameCase;

// golden names: generation is integer-only, so every platform must print these exact strings
hz_global const NcNameCase nc_name_cases[] = {
    {0u, "BraveAlpaca0"},          {1u, "CalmAlpaca0"},          {4095u, "DapperZebra0"},
    {0x12345678u, "WittyIguana565"}, {0x9e3779b9u, "ZanyOsprey55"}, {0xffffffffu, "DapperZebra575"},
};

void test_nc_names(void) {
  TempAllocator ta = tctx_temp_allocator_begin(NULL);
  char *name = ALLOC_ARRAY(&ta.allocator, char, NC_NAME_MAX);
  for (u32 i = 0; i < (u32)ARRAY_SIZE(nc_name_cases); i++) {
    nc_name(nc_name_cases[i].seed, name, NC_NAME_MAX);
    log_info("nc name % = %", fmt_hex(nc_name_cases[i].seed), fmt_cstr(name));
    test_check_true(cstr_equal(name, nc_name_cases[i].name));
  }
  tctx_temp_allocator_end(ta);
}

// the project's test composition root: hzbuild test run --filter nc_
void test_app_register(void) { REGISTER_TEST(test_nc_names); }
